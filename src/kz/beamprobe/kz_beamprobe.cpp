/*
 * [СПАЙК beam-probe] Выбрасываемый код. Задача — ОТВЕТ на вопрос, а не фича.
 *
 * Вопрос: умеет ли CS2 рисовать луч штатной СЕРВЕРНОЙ сущностью (без систем частиц и без
 * своего воркшоп-аддона). Сейчас !lead собран из отрезков info_particle_system со стоковым
 * эффектом ui_annotation_line_segment.vpcf, у которого на каждой вершине спрайт-точка.
 *
 * Пробник ничего в дефолтах не меняет и в !lead не лезет. Он только:
 *   1) спрашивает ЖИВУЮ схему сервера, существуют ли классы лучей (CBeam и родня) и какие
 *      у них поля — это единственное доказательство «класс в CS2 есть», которое можно
 *      получить не гадая: game/server/EnvBeam.cpp в hl2sdk-cs2 — мёртвое дерево Source 1
 *      (LINK_ENTITY_TO_CLASS/DECLARE_SERVERCLASS, оно не компилируется в этот плагин);
 *   2) пытается создать сущность по списку имён классов и печатает, что получилось;
 *   3) выставляет поля луча (концы, ширина, цвет) по схеме — только те, что реально нашлись,
 *      и только если ширина поля совпала с тем, что пишем;
 *   4) сносит за собой: у каждой пробной сущности есть срок жизни, плюс есть clear.
 *
 * Всё печатается машинно-читаемыми строками с префиксом "beamprobe " в серверную консоль:
 * команда серверная (RCON), ответ уедет в лог и в ответ RCON.
 */

#include "common.h"
#include "utils/utils.h"
#include "utils/schema.h"
#include "utils/ctimer.h"
#include "utils/interfaces.h"
#include "kz/kz.h"
#include "sdk/entity/cbasemodelentity.h"
#include "entitykeyvalues.h"
#include "entity2/entityclass.h"

#include <cstdlib>

#include "tier0/memdbgon.h"

#define KZ_BEAMPROBE_TARGETNAME  "cyb_beamprobe"
#define KZ_BEAMPROBE_MAX_ENTS    32
#define KZ_BEAMPROBE_MAX_FIELDS  512
#define KZ_BEAMPROBE_DEF_LIFE    60.0f
#define KZ_BEAMPROBE_DEF_WIDTH   6.0f
#define KZ_BEAMPROBE_BEAM_LENGTH 192.0f

// Имена ЭНТИТИ-классов (для CreateEntityByName). Порядок важен только для читаемости вывода.
// info_particle_system в конце — контроль: он заведомо создаётся, и если не создался он,
// сломан сам пробник, а не гипотеза про лучи.
//
// ЗАПРЕЩЁННЫЙ КАНДИДАТ: `env_laser`. Спавн его 11.09 ПОВЕСИЛ ГЛАВНЫЙ ПОТОК сервера —
// канарейку пришлось пересоздавать. В список не возвращать ни при каких условиях; если
// когда-нибудь понадобится лазер, сперва выясняется, почему он вешает поток, и только на
// отдельном стенде, а не на канарейке с игроками.
static_global const char *g_probeEntityClasses[] = {
	"beam", "env_beam", "env_sprite", "beam_spotlight", "env_beam_spotlight", "path_particle_rope", "info_particle_system",
};

// Классы, которые пробник не создаёт НИКОГДА, чем бы их ни попросили. Список отдельный от
// списка кандидатов намеренно: убрать имя из кандидатов мало — его можно назвать руками
// аргументом команды, а цена ошибки здесь уже заплачена (вис главного потока).
static_global const char *g_probeBannedClasses[] = {
	"env_laser",
};

static_global bool IsProbeClassBanned(const char *classname)
{
	for (u32 i = 0; i < KZ_ARRAYSIZE(g_probeBannedClasses); i++)
	{
		if (KZ_STREQI(classname, g_probeBannedClasses[i]))
		{
			return true;
		}
	}
	return false;
}

// Имена классов ЖИВОЙ СХЕМЫ сервера (для schema::GetClassFields). CParticleSystem и
// CBaseModelEntity — контроль: они точно есть, и found=1 у них доказывает, что запрос рабочий.
static_global const char *g_probeSchemaClasses[] = {
	"CBeam",      "CEnvBeam",          "CEnvLaser",       "CBeamSpotlight",  "CSprite",
	"CEnvSprite", "CPathParticleRope", "CEnvLaserTarget", "CParticleSystem", "CBaseModelEntity",
};
// CEnvLaser выше остаётся ТОЛЬКО в списке схемы: schema-ветка ничего не создаёт, она лишь
// читает описание класса. Спавнить его запрещено — см. g_probeBannedClasses.

// Поля ищем ТОЛЬКО по настоящей цепочке C++-классов созданной сущности (её отдаёт движок,
// см. CollectClassChain). Список «правдоподобных» классов тут был бы миной: офсет поля CBeam,
// записанный в энтити, которая не CBeam, попадает в чужую память этой энтити.
#define KZ_BEAMPROBE_MAX_CHAIN 12

struct ProbeEnt
{
	CEntityHandle handle;
	f64 deadlineRealtime;
};

// Состояние пробника. Живёт только в контексте консольной команды и таймера уборки —
// в хуках движения не читается и не пишется.
static_global ProbeEnt g_probeEnts[KZ_BEAMPROBE_MAX_ENTS];
static_global int g_probeEntCount = 0;
static_global bool g_sweepTimerRunning = false;

struct ResolvedField
{
	const char *declaredIn;
	uint32_t offset;
	int size;
	bool networked;
};

// Ищем поле по цепочке классов. Буфер статический: 512 записей на стеке в колбэке консольной
// команды не нужны, а параллельных вызовов у консольной команды не бывает.
static_global bool ResolveField(const char *const *classes, int classCount, const char *fieldName, ResolvedField &out)
{
	static schema::FieldDesc fields[KZ_BEAMPROBE_MAX_FIELDS];
	for (int c = 0; c < classCount; c++)
	{
		int count = schema::GetClassFields(classes[c], fields, KZ_BEAMPROBE_MAX_FIELDS);
		for (int i = 0; i < count; i++)
		{
			if (!fields[i].name || V_stricmp(fields[i].name, fieldName) != 0)
			{
				continue;
			}
			out.declaredIn = classes[c];
			out.offset = fields[i].offset;
			out.size = fields[i].size;
			out.networked = fields[i].networked;
			return true;
		}
	}
	return false;
}

// Ровно то, что делает Set() в SCHEMA_FIELD: сначала цепочка, иначе сама сущность.
static_global void MarkChanged(CBaseEntity *ent, const ResolvedField &field)
{
	if (!field.networked)
	{
		return;
	}
	int16_t chain = schema::FindChainOffset(field.declaredIn, hash_32_fnv1a_const(field.declaredIn));
	if (chain != 0)
	{
		ChainNetworkStateChanged(reinterpret_cast<uintptr_t>(ent) + chain, field.offset);
	}
	else
	{
		EntityNetworkStateChanged(reinterpret_cast<uintptr_t>(ent), field.offset);
	}
}

template<typename T>
static_global bool ProbeSetTyped(CBaseEntity *ent, const char *tag, const char *const *classes, int classCount, const char *fieldName, T value)
{
	ResolvedField field;
	if (!ResolveField(classes, classCount, fieldName, field))
	{
		Msg("beamprobe field class=%s name=%s set=missing\n", tag, fieldName);
		return false;
	}
	// Ширину поля не угадываем: запись 4 байт в однобайтовое поле затёрла бы соседей.
	if (field.size != (int)sizeof(T))
	{
		Msg("beamprobe field class=%s name=%s set=size_mismatch declared_in=%s field_size=%i want=%i\n", tag, fieldName, field.declaredIn, field.size,
			(int)sizeof(T));
		return false;
	}
	*reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(ent) + field.offset) = value;
	MarkChanged(ent, field);
	Msg("beamprobe field class=%s name=%s set=ok declared_in=%s offset=0x%X size=%i networked=%i\n", tag, fieldName, field.declaredIn, field.offset,
		field.size, field.networked);
	return true;
}

// Целые поля пишем по объявленной ширине (1/2/4/8): m_nBeamType и подобные у Valve бывают
// однобайтовыми, и шаблон выше на них честно ругался бы size_mismatch.
static_global bool ProbeSetInt(CBaseEntity *ent, const char *tag, const char *const *classes, int classCount, const char *fieldName, i64 value)
{
	ResolvedField field;
	if (!ResolveField(classes, classCount, fieldName, field))
	{
		Msg("beamprobe field class=%s name=%s set=missing\n", tag, fieldName);
		return false;
	}
	uintptr_t addr = reinterpret_cast<uintptr_t>(ent) + field.offset;
	switch (field.size)
	{
		case 1:
			*reinterpret_cast<i8 *>(addr) = (i8)value;
			break;
		case 2:
			*reinterpret_cast<i16 *>(addr) = (i16)value;
			break;
		case 4:
			*reinterpret_cast<i32 *>(addr) = (i32)value;
			break;
		case 8:
			*reinterpret_cast<i64 *>(addr) = value;
			break;
		default:
			Msg("beamprobe field class=%s name=%s set=bad_size declared_in=%s field_size=%i\n", tag, fieldName, field.declaredIn, field.size);
			return false;
	}
	MarkChanged(ent, field);
	Msg("beamprobe field class=%s name=%s set=ok declared_in=%s offset=0x%X size=%i networked=%i value=%lli\n", tag, fieldName, field.declaredIn,
		field.offset, field.size, field.networked, (long long)value);
	return true;
}

// Убирает пробные сущности: force — все, иначе только те, чей срок вышел.
// Хендл сам по себе ничего не гарантирует: список переживает смену карты (обработчика у него
// нет), а индексы энтити переиспользуются — поэтому перед сносом сверяем targetname, как
// RemoveLeadSegment (src/kz/lead/kz_lead.cpp:247). Не прошедшая сверку запись — не наша,
// её просто выбрасываем из списка, ничего не трогая.
static_global int SweepProbeEnts(bool force)
{
	f64 now = g_pKZUtils->GetServerGlobals()->realtime;
	int removed = 0;
	int kept = 0;
	for (int i = 0; i < g_probeEntCount; i++)
	{
		CEntityInstance *ent = GameEntitySystem() ? GameEntitySystem()->GetEntityInstance(g_probeEnts[i].handle) : nullptr;
		if (!ent || !ent->m_pEntity || !ent->m_pEntity->NameMatches(KZ_BEAMPROBE_TARGETNAME))
		{
			continue;
		}
		if (!force && now < g_probeEnts[i].deadlineRealtime)
		{
			g_probeEnts[kept++] = g_probeEnts[i];
			continue;
		}
		g_pKZUtils->RemoveEntity(ent);
		removed++;
	}
	g_probeEntCount = kept;
	return removed;
}

static_global f64 BeamProbeSweepTimer()
{
	int removed = SweepProbeEnts(false);
	if (removed > 0 || g_probeEntCount == 0)
	{
		Msg("beamprobe sweep removed=%i alive=%i\n", removed, g_probeEntCount);
	}
	if (g_probeEntCount > 0)
	{
		return 1.0;
	}
	g_sweepTimerRunning = false;
	return 0.0;
}

// false — взять сущность под уборку не вышло; звать её надо так, чтобы сущность тут же ушла:
// не взятая под уборку проба не снимается ни сроком, ни clear и живёт до смены карты.
static_global bool TrackProbeEnt(CBaseEntity *ent, f32 lifetime)
{
	if (g_probeEntCount >= KZ_BEAMPROBE_MAX_ENTS)
	{
		Msg("beamprobe track result=full max=%i\n", KZ_BEAMPROBE_MAX_ENTS);
		return false;
	}
	g_probeEnts[g_probeEntCount].handle = ent->GetRefEHandle();
	g_probeEnts[g_probeEntCount].deadlineRealtime = g_pKZUtils->GetServerGlobals()->realtime + lifetime;
	g_probeEntCount++;
	if (!g_sweepTimerRunning)
	{
		// Непереживающий смену карты таймер: на смене карты сущности исчезают вместе с миром,
		// а persistent-таймеры форка после смены карты спят (известная грабля).
		StartTimer(BeamProbeSweepTimer, 1.0, false, true);
		g_sweepTimerRunning = true;
	}
	return true;
}

// Живой игрок, рядом с которым ставим пробу (slot < 0 — первый подходящий).
static_global KZPlayer *FindProbeTarget(int wantedSlot)
{
	for (int i = 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (!player || !player->IsInGame() || player->IsFakeClient())
		{
			continue;
		}
		if (wantedSlot >= 0 && player->GetPlayerSlot().Get() != wantedSlot)
		{
			continue;
		}
		if (!player->GetPlayerPawn())
		{
			continue;
		}
		return player;
	}
	return nullptr;
}

static_global void DumpSchemaClasses()
{
	static schema::FieldDesc fields[KZ_BEAMPROBE_MAX_FIELDS];
	for (u32 c = 0; c < KZ_ARRAYSIZE(g_probeSchemaClasses); c++)
	{
		const char *className = g_probeSchemaClasses[c];
		int size = 0;
		int fieldCount = 0;
		bool found = schema::GetClassLayout(className, size, fieldCount);
		Msg("beamprobe schema class=%s found=%i size=%i own_fields=%i\n", className, found ? 1 : 0, size, fieldCount);
		if (!found)
		{
			continue;
		}
		int count = schema::GetClassFields(className, fields, KZ_BEAMPROBE_MAX_FIELDS);
		for (int i = 0; i < count; i++)
		{
			Msg("beamprobe schema_field class=%s name=%s type=%s offset=0x%X size=%i networked=%i\n", className, fields[i].name,
				fields[i].typeName ? fields[i].typeName : "?", fields[i].offset, fields[i].size, fields[i].networked ? 1 : 0);
		}
	}
}

// Настоящая цепочка C++-классов сущности по данным движка: env_beam -> CEnvBeam -> CBeam -> ...
// Это же и ответ на вопрос спайка: если у созданной сущности в цепочке стоит CBeam, класс луча
// в CS2 живой, а не наследие Source 1.
static_global int CollectClassChain(CBaseEntity *ent, const char **out, int maxCount)
{
	if (!ent->m_pEntity || !ent->m_pEntity->m_pClass)
	{
		return 0;
	}
	int count = 0;
	for (CEntityClassInfo *info = ent->m_pEntity->m_pClass->m_pClassInfo; info && count < maxCount; info = info->m_pBaseClassInfo)
	{
		if (!info->m_pszCPPClassname)
		{
			break;
		}
		out[count++] = info->m_pszCPPClassname;
	}
	return count;
}

static_global bool SpawnProbeBeam(const char *classname, const Vector &start, const Vector &end, f32 width, const char *material, f32 lifetime)
{
	CBaseModelEntity *ent = utils::CreateEntityByName<CBaseModelEntity>(classname);
	if (!ent)
	{
		Msg("beamprobe create class=%s ok=0 reason=no_factory\n", classname);
		return false;
	}
	CEntityHandle handle = ent->GetRefEHandle();
	Msg("beamprobe create class=%s ok=1 index=%i handle_valid=%i designer=%s\n", classname, ent->entindex(), handle.IsValid() ? 1 : 0,
		ent->m_pEntity ? ent->m_pEntity->GetClassname() : "?");

	const char *chain[KZ_BEAMPROBE_MAX_CHAIN];
	int chainCount = CollectClassChain(ent, chain, KZ_BEAMPROBE_MAX_CHAIN);
	for (int i = 0; i < chainCount; i++)
	{
		Msg("beamprobe cpp_class class=%s depth=%i name=%s\n", classname, i, chain[i]);
	}
	if (chainCount == 0)
	{
		// Без цепочки классов писать поля некуда: офсет из чужого класса — это порча памяти.
		Msg("beamprobe fields class=%s skipped=1 reason=no_class_chain\n", classname);
	}

	// Поля луча до DispatchSpawn. Пишем только то, что нашлось в СВОИХ классах этой сущности.
	if (chainCount > 0)
	{
		ProbeSetTyped<Vector>(ent, classname, chain, chainCount, "m_vecEndPos", end);
		ProbeSetTyped<float>(ent, classname, chain, chainCount, "m_fWidth", width);
		ProbeSetTyped<float>(ent, classname, chain, chainCount, "m_flWidth", width);
		ProbeSetTyped<float>(ent, classname, chain, chainCount, "m_fEndWidth", width);
		ProbeSetTyped<float>(ent, classname, chain, chainCount, "m_flEndWidth", width);
		ProbeSetTyped<float>(ent, classname, chain, chainCount, "m_fAmplitude", 0.0f);
		ProbeSetTyped<float>(ent, classname, chain, chainCount, "m_flAmplitude", 0.0f);
		ProbeSetTyped<float>(ent, classname, chain, chainCount, "m_flFrameRate", 0.0f);
		ProbeSetTyped<float>(ent, classname, chain, chainCount, "m_flHDRColorScale", 1.0f);
		ProbeSetTyped<float>(ent, classname, chain, chainCount, "m_flFadeLength", 0.0f);
		// 1 = BEAM_ENTPOINT в нумерации Source 1 («от сущности к точке»); BEAM_POINTS там 0.
		// Значение оставлено как было: им снят единственный живой результат пробы.
		ProbeSetInt(ent, classname, chain, chainCount, "m_nBeamType", 1);
		ProbeSetInt(ent, classname, chain, chainCount, "m_nBeamFlags", 0);
		ProbeSetInt(ent, classname, chain, chainCount, "m_nNumBeamEnts", 0);
		ProbeSetInt(ent, classname, chain, chainCount, "m_nHaloIndex", 0);
		ProbeSetInt(ent, classname, chain, chainCount, "m_bTurnedOff", 0);
		ProbeSetTyped<Color>(ent, classname, chain, chainCount, "m_clrRender", Color(0, 255, 255, 255));
	}

	CEntityKeyValues *kv = new CEntityKeyValues();
	kv->SetVector("origin", start);
	kv->SetString("targetname", KZ_BEAMPROBE_TARGETNAME);
	kv->SetString("rendercolor", "0 255 255");
	kv->SetInt("renderamt", 255);
	kv->SetFloat("width", width);
	kv->SetFloat("BoltWidth", width);
	kv->SetFloat("life", 0.0f);
	kv->SetInt("spawnflags", 1); // «start on» у env_beam в Source 1
	kv->SetBool("start_active", true);
	kv->SetString("effect_name", "particles/ui/hud/ui_map_def_utility_trail.vpcf"); // нужно только контрольному info_particle_system
	if (material && material[0])
	{
		// Какой ключ у луча в CS2 — неизвестно, поэтому кладём все правдоподобные:
		// незнакомый ключ энтити просто игнорирует.
		kv->SetString("texture", material);
		kv->SetString("material", material);
		kv->SetString("BeamTexture", material);
		if (V_strstr(material, ".vmdl"))
		{
			kv->SetString("model", material);
		}
	}
	ent->DispatchSpawn(kv);

	bool alive = handle.Get() != nullptr;
	Msg("beamprobe spawn class=%s alive=%i index=%i start=%.1f,%.1f,%.1f end=%.1f,%.1f,%.1f width=%.2f material=%s lifetime=%.0f\n", classname,
		alive ? 1 : 0, alive ? ent->entindex() : -1, start.x, start.y, start.z, end.x, end.y, end.z, width, material && material[0] ? material : "-",
		lifetime);
	if (!alive)
	{
		// Сущность может умереть прямо в DispatchSpawn, если классу не хватило ключей.
		return false;
	}

	// Позицию ставим ещё раз уже после спавна, а конец луча переписываем: спавн мог его сбросить.
	ent->Teleport(&start, nullptr, &vec3_origin);
	if (chainCount > 0)
	{
		ProbeSetTyped<Vector>(ent, classname, chain, chainCount, "m_vecEndPos", end);
	}

	// Видимость: quiet-фильтр (kz_quiet.cpp) перебирает ТОЛЬКО info_particle_system с меткой
	// m_iTeamNum == CUSTOM_PARTICLE_SYSTEM_TEAM и custom_hud_layout, поэтому пробную сущность
	// он не гасит — она уходит всем по общим правилам. Метку команды намеренно НЕ ставим:
	// на не-частице она ничего не значит, а на частице означала бы «не видит никто».
	Msg("beamprobe visibility class=%s quiet_filtered=0 team=%i effects=0x%X targetname=%s\n", classname, (int)ent->m_iTeamNum(),
		(unsigned)ent->m_fEffects(), KZ_BEAMPROBE_TARGETNAME);

	if (!TrackProbeEnt(ent, lifetime))
	{
		// Под уборку не взяли — сносим сами, иначе проба останется в мире до смены карты.
		g_pKZUtils->RemoveEntity(ent);
		Msg("beamprobe untracked class=%s removed=1 reason=tracker_full\n", classname);
		return false;
	}
	return true;
}

CON_COMMAND_F(kz_beam_probe,
			  "[СПАЙК] Проверить, умеет ли CS2 рисовать луч штатной сущностью. Usage: kz_beam_probe schema | spawn [classname|all] "
			  "[lifetime] [width] [material] [slot] | clear",
			  FCVAR_NONE)
{
	// Серверная команда, игрокам не отдаём — канон форка (kz_invisible.cpp, kz_lead_refresh).
	if (utils::GetController(context.GetPlayerSlot()))
	{
		Msg("beamprobe denied reason=not_server slot=%d\n", context.GetPlayerSlot().Get());
		return;
	}
	if (!GameEntitySystem())
	{
		Msg("beamprobe denied reason=no_entity_system\n");
		return;
	}

	const char *sub = args.ArgC() >= 2 ? args.Arg(1) : "schema";

	if (KZ_STREQI(sub, "clear"))
	{
		int removed = SweepProbeEnts(true);
		Msg("beamprobe clear removed=%i\n", removed);
		return;
	}

	if (KZ_STREQI(sub, "schema"))
	{
		DumpSchemaClasses();
		Msg("beamprobe done action=schema classes=%i\n", (int)KZ_ARRAYSIZE(g_probeSchemaClasses));
		return;
	}

	if (!KZ_STREQI(sub, "spawn"))
	{
		Msg("beamprobe usage sub=schema|spawn|clear\n");
		return;
	}

	const char *wantClass = args.ArgC() >= 3 ? args.Arg(2) : "all";
	if (IsProbeClassBanned(wantClass))
	{
		Msg("beamprobe denied reason=class_banned class=%s note=hangs_main_thread\n", wantClass);
		return;
	}
	f32 lifetime = args.ArgC() >= 4 ? (f32)utils::StringToFloat(args.Arg(3)) : KZ_BEAMPROBE_DEF_LIFE;
	f32 width = args.ArgC() >= 5 ? (f32)utils::StringToFloat(args.Arg(4)) : KZ_BEAMPROBE_DEF_WIDTH;
	const char *material = args.ArgC() >= 6 ? args.Arg(5) : "";
	int slot = args.ArgC() >= 7 ? atoi(args.Arg(6)) : -1;
	if (lifetime <= 0.0f || lifetime > 600.0f)
	{
		lifetime = KZ_BEAMPROBE_DEF_LIFE;
	}
	if (width <= 0.0f || width > 128.0f)
	{
		width = KZ_BEAMPROBE_DEF_WIDTH;
	}

	KZPlayer *target = FindProbeTarget(slot);
	if (!target)
	{
		Msg("beamprobe denied reason=no_player slot=%i\n", slot);
		return;
	}
	// Инициализация и проверка пешки локальные: MovementPlayer::GetOrigin (mv_player.cpp:71)
	// при отсутствии пешки в origin не пишет вовсе, а гарантия из FindProbeTarget живёт в
	// другой функции и переживёт не всякую правку.
	if (!target->GetPlayerPawn())
	{
		Msg("beamprobe denied reason=no_pawn slot=%i\n", target->GetPlayerSlot().Get());
		return;
	}
	Vector base = vec3_origin;
	target->GetOrigin(&base);

	// Чистим протухшее до спавна: так же снимается флаг таймера, если его убила смена карты.
	SweepProbeEnts(false);
	if (g_probeEntCount == 0)
	{
		g_sweepTimerRunning = false;
	}

	int spawned = 0;
	int failed = 0;
	int index = 0;
	for (u32 i = 0; i < KZ_ARRAYSIZE(g_probeEntityClasses); i++)
	{
		const char *classname = g_probeEntityClasses[i];
		if (!KZ_STREQI(wantClass, "all") && !KZ_STREQI(wantClass, classname))
		{
			continue;
		}
		// Вторая застава на случай, если запрещённое имя когда-нибудь вернут в кандидаты.
		if (IsProbeClassBanned(classname))
		{
			Msg("beamprobe skip class=%s reason=banned note=hangs_main_thread\n", classname);
			continue;
		}
		// Каждый кандидат — свой вертикальный столб со сдвигом вбок, чтобы по месту было
		// видно, КАКОЙ класс нарисовался: порядок сдвигов печатается в этой же строке.
		Vector start = base + Vector(32.0f * (f32)index, 0.0f, 8.0f);
		Vector end = start + Vector(0.0f, 0.0f, KZ_BEAMPROBE_BEAM_LENGTH);
		Msg("beamprobe candidate idx=%i class=%s offset_x=%.0f\n", index, classname, 32.0f * (f32)index);
		if (SpawnProbeBeam(classname, start, end, width, material, lifetime))
		{
			spawned++;
		}
		else
		{
			failed++;
		}
		index++;
	}
	Msg("beamprobe done action=spawn spawned=%i failed=%i tracked=%i player=%s\n", spawned, failed, g_probeEntCount,
		target->GetController() ? target->GetController()->GetPlayerName() : "?");
}
