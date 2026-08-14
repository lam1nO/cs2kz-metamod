/*
	СПАЙК LAM-20 (шаг 0) — ВЫБРАСЫВАЕМЫЙ КОД, в релиз не идёт.

	Проверяем ровно один вопрос: увидит ли движковая трассировка касаний
	(INavPhysicsInterface::TraceShape + CTraceFilterHitAllTriggers, kz_trigger.cpp)
	trigger_multiple, который заспавнен в рантайме и объём которого задан программно
	(m_vecMins/m_vecMaxs + SOLID_BBOX + CollisionRulesChanged), а не взят из VPhysX-меша
	энтити карты.

	ОТКАТ СПАЙКА = `git revert` коммита целиком, УДАЛЕНИЯ ЭТОГО ФАЙЛА НЕДОСТАТОЧНО.
	Спайк живёт в четырёх местах: этот файл, снятый гейт окна раунда и фасад
	SpikeSetAllowSpawnAnytime/SpikeIsActive в src/kz/mappingapi/kz_mappingapi.{h,cpp},
	лог касания в src/kz/trigger/kz_trigger.cpp, лог фактора в src/kz/trigger/mapping_api.cpp.

	Команды (скрытые, доступны только SPIKE_ALLOWED_STEAMID):
	  !spikezone [множитель] [0|1|2] — заспавнить зону-бустер (KZTRIGGER_MODIFIER) вокруг игрока.
									   Третий аргумент — когда задавать bbox относительно
									   DispatchSpawn: 0 = после (дефолт), 1 = до, 2 = и до, и после.
									   Порядок вынесен в аргумент, чтобы отрицательный ответ спайка
									   нельзя было объяснить «а если бы задали bbox раньше».
	  !spikeprobe                    — прогнать ту же трассировку вручную и сказать, видна ли зона
	  !spikeclear                    — снести заспавненные зоны

	Команда сервера (RCON, без живого игрока):
	  kz_spike_selftest [0|1|2]      — поставить зону у спавн-поинта карты, прогнать по ней
									   трассировку коробкой игрока и сразу снять зону.
									   Отвечает на ядро вопроса спайка headless: verdict=
									   trace_sees_zone | trace_blind. Живой заход нужен только
									   для подтверждения StartTouch и самого прыжка.
*/

#include "kz/kz.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/trigger/kz_trigger.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "sdk/entity/cbasetrigger.h"
#include "sdk/entity/cbasemodelentity.h"
#include "sdk/ccollisionproperty.h"
#include "sdk/navphysicsinterface.h"
#include "entity2/entitykeyvalues.h"
#include "entity2/entitysystem.h"

#include <stdlib.h>

// Спайк-команды дают произвольный множитель прыжка кому угодно на публичной канарейке:
// это и гриф чужих ранов, и «рекорд» с бустом в общем по сети рекордном тракте
// (удаление рекорда — ручная процедура в Postgres). Поэтому allowlist из одного steam_id.
#define SPIKE_ALLOWED_STEAMID 76561199702618240ull

#define SPIKE_ZONE_NAME    "cyb_spike_zone"
#define SPIKE_ZONE_HALF_XY 64.0f
#define SPIKE_ZONE_HEIGHT  128.0f
#define SPIKE_MAX_ZONES    16
#define SPIKE_JUMP_FACTOR  2.0f
// Потолок g_mappingApi.triggers — CUtlVectorFixed<KzTrigger, 2048> без проверки ёмкости
// в AddToTail: Grow() у фиксированной памяти в релизе no-op, то есть запись за границу.
// Штатно это недостижимо (регистрация заперта окном раунда), а спайк гейт снимает —
// поэтому останавливаемся с запасом сами.
#define SPIKE_MAX_MAPI_TRIGGERS 2000

// Хендлы наших зон. Трогается только из команд (не из хуков движения).
static_global CUtlVectorFixed<CEntityHandle, SPIKE_MAX_ZONES> g_spikeZones;

// Порядок задания bbox относительно DispatchSpawn.
enum SpikeBoxOrder
{
	SPIKE_BOX_POST = 0,
	SPIKE_BOX_PRE = 1,
	SPIKE_BOX_BOTH = 2,
};

static_function bool Spike_IsAllowed(KZPlayer *player)
{
	if (player->GetSteamId64(false) == SPIKE_ALLOWED_STEAMID)
	{
		return true;
	}
	KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] spike_cmd_rejected steam_id=%llu reason=not_allowed\n", player->GetSteamId64(false));
	return false;
}

// Просто «жива ли энтити по хендлу», без суждения о том, чья она.
static_function CBaseEntity *Spike_ResolveByHandle(const CEntityHandle &handle)
{
	CEntityInstance *inst = GameEntitySystem() ? GameEntitySystem()->GetEntityInstance(handle) : nullptr;
	return inst ? static_cast<CBaseEntity *>(inst) : nullptr;
}

static_function bool Spike_NameMatches(CBaseEntity *ent)
{
	return ent && ent->m_pEntity && ent->m_pEntity->NameMatches(SPIKE_ZONE_NAME);
}

// Резолв с проверкой, что это ИМЕННО наша зона. Нужен там, где хендл мог протухнуть:
// после смены карты энтити-система пересоздаётся и серийники перезапускаются, поэтому
// старый хендл теоретически резолвится в чужую энтити (триггер карты, павн игрока).
// В пути «сразу после DispatchSpawn» эта проверка НЕ используется намеренно: targetname
// проставляется движком из keyvalues, и его пропажа выглядела бы как «энтити снесли», то
// есть давала бы ложный отрицательный ответ спайка. Там резолв идёт по хендлу, а
// совпадение имени пишется отдельным полем name_ok.
static_function CBaseEntity *Spike_ResolveOwnZone(const CEntityHandle &handle)
{
	CBaseEntity *ent = Spike_ResolveByHandle(handle);
	return Spike_NameMatches(ent) ? ent : nullptr;
}

// Выбрасывает протухшие хендлы и возвращает число ЖИВЫХ зон. Вектор переживает смену карты,
// а энтити — нет, поэтому голый Count() врёт дважды: включает гейт спайк-логов при нуле живых
// зон (и тогда spike_jump_factor_applied начинает писаться всем игрокам на штатных
// modifier-зонах карты) и упирает !spikezone в reason=zone_limit на мёртвых записях.
static_function i32 Spike_PruneAndCountLiveZones()
{
	FOR_EACH_VEC_BACK(g_spikeZones, i)
	{
		if (!Spike_ResolveOwnZone(g_spikeZones[i]))
		{
			g_spikeZones.Remove(i);
		}
	}
	return g_spikeZones.Count();
}

static_function void Spike_Clear()
{
	i32 removed = 0;
	i32 stale = 0;
	FOR_EACH_VEC(g_spikeZones, i)
	{
		CBaseEntity *ent = Spike_ResolveOwnZone(g_spikeZones[i]);
		if (ent)
		{
			g_pKZUtils->RemoveEntity(ent);
			removed++;
		}
		else
		{
			stale++;
		}
	}
	g_spikeZones.RemoveAll();
	// Зон нет — гасим и гейт спайк-логов в горячем коде касаний, не дожидаясь round_prestart.
	KZ::mapapi::SpikeSetActive(false);
	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] spike_zone_clear removed=%d stale=%d\n", removed, stale);
}

// phase — только для лога. Без него PRE-режим при отсутствующем m_pCollision молча
// становился no-op'ом, неотличимым в итоговом логе от «движок перезаписал bbox на спавне»,
// и три значения order переставали разделять гипотезы — то есть спайк терял смысл.
static_function void Spike_ApplyBox(CBaseTrigger *trigger, const Vector &mins, const Vector &maxs, const char *phase)
{
	CCollisionProperty *collision = trigger->m_pCollision();
	if (!collision)
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] spike_box_apply phase=%s collision=0 solid_after=-1\n", phase);
		return;
	}
	collision->m_vecMins(mins);
	collision->m_vecMaxs(maxs);
	collision->m_nSolidType(SOLID_BBOX);
	collision->m_usSolidFlags((uint8)(FSOLID_NOT_SOLID | FSOLID_TRIGGER));
	collision->m_CollisionGroup((uint8)COLLISION_GROUP_TRIGGER);
	// Фильтр касаний (CTraceFilterHitAllTriggers) ищет m_nInteractsWith = CONTENTS_TRIGGER (1<<2),
	// значит энтити обязана объявить себя этим слоем.
	collision->m_collisionAttribute().m_nInteractsAs(collision->m_collisionAttribute().m_nInteractsAs() | (1ull << LAYER_INDEX_CONTENTS_TRIGGER));
	collision->m_collisionAttribute().m_nCollisionGroup((uint8)COLLISION_GROUP_TRIGGER);
	// solid_after читается обратно из схемы: показывает, что запись вообще легла.
	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] spike_box_apply phase=%s collision=1 solid_after=%d interacts_as=0x%llx\n", phase,
				(int)collision->m_nSolidType(), (unsigned long long)collision->m_collisionAttribute().m_nInteractsAs());
}

// Ядро спавна: origin приходит либо от игрока (!spikezone), либо от спавн-поинта карты
// (серверная самопроверка). Возвращает живую зону или nullptr; steamId — только для логов
// (0 = вызов с консоли сервера).
static_function CBaseTrigger *Spike_SpawnZoneAt(const Vector &origin, f32 jumpFactor, SpikeBoxOrder order, u64 steamId,
												const KzTrigger **registeredOut)
{
	if (registeredOut)
	{
		*registeredOut = nullptr;
	}

	// Считаем ЖИВЫЕ зоны, а не записи: мёртвые хендлы после смены карты иначе съедали бы лимит.
	if (Spike_PruneAndCountLiveZones() >= SPIKE_MAX_ZONES)
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] spike_zone_spawn_failed steam_id=%llu reason=zone_limit\n", steamId);
		return nullptr;
	}

	// !spikeclear не чистит g_mappingApi.triggers (это делает только round_prestart),
	// поэтому цикл «наставить-снести» копит записи — считаем их, а не живые зоны.
	if (KZ::mapapi::SpikeTriggerCount() >= SPIKE_MAX_MAPI_TRIGGERS)
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] spike_zone_spawn_failed steam_id=%llu reason=mapi_triggers_full count=%d\n", steamId,
					KZ::mapapi::SpikeTriggerCount());
		return nullptr;
	}

	CBaseTrigger *trigger = utils::CreateEntityByName<CBaseTrigger>("trigger_multiple");
	if (!trigger)
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] spike_zone_spawn_failed steam_id=%llu reason=create_entity_failed\n", steamId);
		return nullptr;
	}

	const Vector mins(-SPIKE_ZONE_HALF_XY, -SPIKE_ZONE_HALF_XY, 0.0f);
	const Vector maxs(SPIKE_ZONE_HALF_XY, SPIKE_ZONE_HALF_XY, SPIKE_ZONE_HEIGHT);

	// Две одинаковые пачки keyvalues: первую забирает движок в DispatchSpawn (после чего
	// читать её нельзя), вторая — на случай, если хук CEntitySystem::Spawn не сработал и
	// регистрацию в Mapping API придётся позвать руками. Вторая пачка течёт ВСЕГДА, на
	// каждый вызов команды и независимо от того, понадобился ли фолбэк: осознанная цена
	// выбрасываемого кода, в вариант A её тащить нельзя.
	CEntityKeyValues *kv = new CEntityKeyValues();
	CEntityKeyValues *kvFallback = new CEntityKeyValues();
	CEntityKeyValues *kvs[2] = {kv, kvFallback};
	for (CEntityKeyValues *k : kvs)
	{
		k->SetString("targetname", SPIKE_ZONE_NAME);
		k->SetVector("origin", origin);
		k->SetQAngle("angles", vec3_angle);
		// Mapping API читает эти ключи в Mapi_OnTriggerMultipleSpawn.
		k->SetInt("timer_trigger_type", KZTRIGGER_MODIFIER);
		k->SetFloat("timer_modifier_jump_impulse", jumpFactor);
		// spawnflags бит 0 — «срабатывать на игроков»: без него PassesTriggerFilters вернёт false
		// и модификатор не применится, даже если касание случится.
		k->SetInt("spawnflags", 1);
		k->SetBool("StartDisabled", false);
		k->SetFloat("wait", 0.1f);
	}

	if (order == SPIKE_BOX_PRE || order == SPIKE_BOX_BOTH)
	{
		Spike_ApplyBox(trigger, mins, maxs, "pre");
	}

	// Хендл берём ДО спавна: trigger_multiple без модели движок вправе снести прямо
	// внутри Spawn, и дальнейшая работа по указателю была бы обращением к освобождённой памяти.
	const CEntityHandle handle = trigger->GetRefEHandle();

	// Гейт kz_mappingapi.cpp:186 пропускает регистрацию только в окне round_prestart→round_start.
	// Для спайка снимаем его на время своего спавна.
	KZ::mapapi::SpikeSetAllowSpawnAnytime(true);
	trigger->DispatchSpawn(kv);
	KZ::mapapi::SpikeSetAllowSpawnAnytime(false);

	// Резолв по ХЕНДЛУ, без сверки имени: targetname проставляется движком из keyvalues,
	// и его пропажа не должна выглядеть как «энтити снесли» — иначе спайк выдаст ложное «нет».
	CBaseEntity *resolved = Spike_ResolveByHandle(handle);
	if (!resolved)
	{
		// Это тоже результат спайка: движок не держит бесмодельный trigger_multiple.
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] spike_zone_spawn_failed steam_id=%llu reason=removed_on_spawn order=%d\n", steamId, (int)order);
		return nullptr;
	}
	const bool nameOk = Spike_NameMatches(resolved);
	trigger = static_cast<CBaseTrigger *>(resolved);

	if (order == SPIKE_BOX_POST || order == SPIKE_BOX_BOTH)
	{
		Spike_ApplyBox(trigger, mins, maxs, "post");
	}
	trigger->CollisionRulesChanged();
	// Телепорт в ту же точку — чтобы движок перелинковал энтити с новыми габаритами.
	trigger->Teleport(&origin, &vec3_angle, &vec3_origin);

	g_spikeZones.AddToTail(handle);
	KZ::mapapi::SpikeSetActive(true);

	const KzTrigger *registered = KZ::mapapi::GetKzTrigger(trigger);
	bool viaFallback = false;
	if (!registered)
	{
		// Хук CEntitySystem::Spawn нашего DispatchSpawn не увидел — зовём разбор keyvalues руками,
		// чтобы вопрос спайка (видит ли трассировка объём) не смешивался с вопросом регистрации.
		EntitySpawnInfo_t info {};
		info.m_pEntity = trigger->m_pEntity;
		info.m_pKeyValues = kvFallback;
		KZ::mapapi::SpikeSetAllowSpawnAnytime(true);
		KZ::mapapi::OnSpawn(1, &info);
		KZ::mapapi::SpikeSetAllowSpawnAnytime(false);
		registered = KZ::mapapi::GetKzTrigger(trigger);
		viaFallback = registered != nullptr;
	}

	CCollisionProperty *collision = trigger->m_pCollision();
	KZ_LOG_INFO(LogChannel::MappingAPI,
				"[cyb] spike_zone_spawn steam_id=%llu ent=%d order=%d origin=(%.0f %.0f %.0f) mins=(%.0f %.0f %.0f) maxs=(%.0f %.0f %.0f) "
				"solid=%d flags=0x%02x interacts_as=0x%llx name_ok=%d registered=%d via_fallback=%d mapi_triggers=%d jump_factor=%.2f\n",
				steamId, trigger->entindex(), (int)order, origin.x, origin.y, origin.z, mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z,
				collision ? (int)collision->m_nSolidType() : -1, collision ? (unsigned)collision->m_usSolidFlags() : 0u,
				collision ? (unsigned long long)collision->m_collisionAttribute().m_nInteractsAs() : 0ull, nameOk ? 1 : 0, registered ? 1 : 0,
				viaFallback ? 1 : 0, KZ::mapapi::SpikeTriggerCount(), registered ? registered->modifier.jumpFactor : -1.0f);

	if (registeredOut)
	{
		*registeredOut = registered;
	}
	return trigger;
}

// Сколько зон нашего спайка видит трассировка из точки origin коробкой bounds.
// Ровно тот же вызов, что делает KZTriggerService::UpdateTriggerTouchList.
static_function i32 Spike_TraceOwnZones(const Vector &origin, const bbox_t &bounds, i32 *hitTotalOut)
{
	CTraceFilterHitAllTriggers filter;
	trace_t tr;
	INavPhysicsInterface::TraceShape(Ray_t(bounds.mins, bounds.maxs), origin, origin, &filter, &tr);

	i32 ours = 0;
	FOR_EACH_VEC(filter.hitTriggerHandles, i)
	{
		if (Spike_ResolveOwnZone(filter.hitTriggerHandles[i]))
		{
			ours++;
		}
	}
	if (hitTotalOut)
	{
		*hitTotalOut = filter.hitTriggerHandles.Count();
	}
	return ours;
}

static_function void Spike_SpawnZone(KZPlayer *player, f32 jumpFactor, SpikeBoxOrder order)
{
	if (!player->GetPlayerPawn())
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] spike_zone_spawn_failed steam_id=%llu reason=no_pawn\n", player->GetSteamId64(false));
		return;
	}

	Vector origin;
	player->GetOrigin(&origin);

	const KzTrigger *registered = nullptr;
	CBaseTrigger *zone = Spike_SpawnZoneAt(origin, jumpFactor, order, player->GetSteamId64(false), &registered);
	if (!zone)
	{
		player->PrintChat(true, false, "spike: zone spawn failed, see server log");
		return;
	}
	player->PrintChat(true, false, "spike: zone spawned (order=%d registered=%d), step in and jump", (int)order, registered ? 1 : 0);
}

// Ручной прогон той же трассировки, что делает UpdateTriggerTouchList: отвечает на вопрос
// «видит ли трассировка нашу энтити» отдельно от вопроса «проходит ли она фильтры касания».
static_function void Spike_Probe(KZPlayer *player)
{
	if (!player->GetPlayerPawn())
	{
		return;
	}
	Vector origin;
	player->GetOrigin(&origin);
	bbox_t bounds;
	player->GetBBoxBounds(&bounds);

	i32 hitTotal = 0;
	const i32 ours = Spike_TraceOwnZones(origin, bounds, &hitTotal);

	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] spike_probe steam_id=%llu hit_total=%d hit_ours=%d spawned=%d\n", player->GetSteamId64(false),
				hitTotal, ours, g_spikeZones.Count());
	player->PrintChat(true, false, "spike: trace sees %d triggers, %d of them ours", hitTotal, ours);
}

SCMD(kz_spikezone, SCFL_HIDDEN)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!Spike_IsAllowed(player))
	{
		return MRES_SUPERCEDE;
	}
	f32 factor = args->ArgC() > 1 ? (f32)atof(args->Arg(1)) : SPIKE_JUMP_FACTOR;
	if (factor <= 0.0f)
	{
		factor = SPIKE_JUMP_FACTOR;
	}
	i32 rawOrder = args->ArgC() > 2 ? atoi(args->Arg(2)) : (i32)SPIKE_BOX_POST;
	SpikeBoxOrder order = (rawOrder == 1) ? SPIKE_BOX_PRE : ((rawOrder == 2) ? SPIKE_BOX_BOTH : SPIKE_BOX_POST);
	Spike_SpawnZone(player, factor, order);
	return MRES_SUPERCEDE;
}

SCMD(kz_spikeprobe, SCFL_HIDDEN)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!Spike_IsAllowed(player))
	{
		return MRES_SUPERCEDE;
	}
	Spike_Probe(player);
	return MRES_SUPERCEDE;
}

SCMD(kz_spikeclear, SCFL_HIDDEN)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!Spike_IsAllowed(player))
	{
		return MRES_SUPERCEDE;
	}
	Spike_Clear();
	player->PrintChat(true, false, "spike: zones removed");
	return MRES_SUPERCEDE;
}

// Серверная самопроверка через RCON, без живого игрока: ставим зону у спавн-поинта карты и
// тут же прогоняем ТУ ЖЕ трассировку, что и UpdateTriggerTouchList, коробкой размером со
// стоящего игрока. Это и есть ядро вопроса спайка — «видит ли движковая трассировка
// программно заданный объём»; StartTouch и реальный прыжок проверяются живым заходом
// отдельно, но отрицательный ответ здесь закрывает вариант A уже без человека в игре.
CON_COMMAND_F(kz_spike_selftest, "LAM-20 spike: spawn a runtime trigger_multiple at a map spawn point and trace it. Args: [box order 0|1|2].",
			  FCVAR_NONE)
{
	if (utils::GetController(context.GetPlayerSlot()))
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] spike_selftest_rejected reason=not_server slot=%d\n", context.GetPlayerSlot().Get());
		return;
	}

	const i32 rawOrder = args.ArgC() > 1 ? atoi(args.Arg(1)) : (i32)SPIKE_BOX_POST;
	const SpikeBoxOrder order = (rawOrder == 1) ? SPIKE_BOX_PRE : ((rawOrder == 2) ? SPIKE_BOX_BOTH : SPIKE_BOX_POST);

	// Точка теста — любой спавн-поинт карты: гарантированно внутри геометрии и есть на любой карте.
	CBaseEntity *spawnPoint = utils::FindEntityByClassname(nullptr, "info_player_counterterrorist");
	if (!spawnPoint)
	{
		spawnPoint = utils::FindEntityByClassname(nullptr, "info_player_terrorist");
	}
	if (!spawnPoint || !spawnPoint->m_CBodyComponent() || !spawnPoint->m_CBodyComponent()->m_pSceneNode())
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] spike_selftest_failed reason=no_spawn_point\n");
		return;
	}
	const Vector origin = spawnPoint->m_CBodyComponent()->m_pSceneNode()->m_vecAbsOrigin();

	const KzTrigger *registered = nullptr;
	CBaseTrigger *zone = Spike_SpawnZoneAt(origin, SPIKE_JUMP_FACTOR, order, 0ull, &registered);
	if (!zone)
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] spike_selftest_failed reason=spawn_failed order=%d\n", (int)order);
		return;
	}

	// Габариты стоящего игрока — те же, что использует сам форк (utils.cpp:458).
	const bbox_t playerBounds = {{-16.0f, -16.0f, 0.0f}, {16.0f, 16.0f, 72.0f}};
	i32 hitTotal = 0;
	const i32 ours = Spike_TraceOwnZones(origin, playerBounds, &hitTotal);

	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] spike_selftest order=%d origin=(%.0f %.0f %.0f) registered=%d hit_total=%d hit_ours=%d verdict=%s\n",
				(int)order, origin.x, origin.y, origin.z, registered ? 1 : 0, hitTotal, ours, ours > 0 ? "trace_sees_zone" : "trace_blind");

	// Зона стоит на спавн-поинте: оставить её значило бы раздать бустер прыжка всем,
	// кто там появится. Снимаем сразу же — свои зоны игрока (!spikezone) не трогаем.
	const CEntityHandle handle = zone->GetRefEHandle();
	g_spikeZones.FindAndRemove(handle);
	g_pKZUtils->RemoveEntity(zone);
	// Снимаем зону в обход Spike_Clear, поэтому гасим гейт спайк-логов здесь же. Условие, а не
	// безусловный false: у игрока могут стоять свои зоны, поставленные через !spikezone.
	KZ::mapapi::SpikeSetActive(Spike_PruneAndCountLiveZones() > 0);
}
