/*
	Зоны KZ-карт из БД платформы: загрузка набора на старте карты и спавн триггеров.
	Условие «bbox до DispatchSpawn» и его доказательство — в kz_zones.h.
*/

#include "kz_zones.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/option/kz_option.h"
#include "utils/utils.h"
#include "utils/http.h"
#include "utils/ctimer.h"

#include "sdk/entity/cbasetrigger.h"
#include "sdk/entity/cbasemodelentity.h"
#include "sdk/ccollisionproperty.h"
#include "entity2/entitykeyvalues.h"
#include "entity2/entitysystem.h"
#include "tier1/keyvalues3.h"

#include <string>
#include <vector>

// Имя, по которому свои зоны отличаются от родных триггеров карты (снятие, отладка).
#define KZ_CYBER_ZONE_NAME "cyb_map_zone"

static_global struct
{
	std::string mapName;
	std::vector<KzCyberZone> zones;
	CUtlVector<CEntityHandle> spawned;
	i32 revision;
	bool loaded;
	// Мир доделан движком (прошёл round_start). До этого спавнить бесполезно: движковая
	// очистка на рестарте раунда идёт ПОСЛЕ события round_prestart и сносит наши энтити.
	bool worldReady;
	// Снимок хендлов на round_prestart. Отдельное поле, а не spawned: тот обнуляет DespawnAll
	// внутри ApplyLoadedZones, и аудит выживаемости молчал бы ровно в интересующем случае —
	// ответ api приходит в те же кадры, что события раунда.
	CUtlVector<CEntityHandle> preRoundSpawned;
	// Растёт на каждой загрузке карты. Отложенные таймеры сверяются с ним и молча выходят,
	// если карта уже сменилась: флаг preserveMapChange=false в этом форке НИЧЕГО не гарантирует —
	// RemoveNonPersistentTimers() объявлена и определена, но ниоткуда не вызывается
	// (то же предупреждение — в src/kz/invisible/kz_invisible.cpp:295).
	u32 mapGeneration;
} g_cybZones;

static_function const char *ZoneTypeName(KzCyberZoneType type)
{
	switch (type)
	{
		case KZ_CYBER_ZONE_START:
			return "start";
		case KZ_CYBER_ZONE_END:
			return "end";
		case KZ_CYBER_ZONE_MODIFIER:
			return "modifier";
		default:
			return "unknown";
	}
}

bool KZ::zones::IsValidZoneMapName(const std::string &name)
{
	if (name.empty() || name.size() > 64)
	{
		return false;
	}
	for (char c : name)
	{
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
		{
			return false;
		}
	}
	return true;
}

bool KZ::zones::ParseZoneType(const char *raw, KzCyberZoneType &out)
{
	if (!raw)
	{
		return false;
	}
	if (KZ_STREQ(raw, "start"))
	{
		out = KZ_CYBER_ZONE_START;
		return true;
	}
	if (KZ_STREQ(raw, "end"))
	{
		out = KZ_CYBER_ZONE_END;
		return true;
	}
	if (KZ_STREQ(raw, "modifier"))
	{
		out = KZ_CYBER_ZONE_MODIFIER;
		return true;
	}
	return false;
}

const char *KZ::zones::ZoneTypeToString(KzCyberZoneType type)
{
	return ZoneTypeName(type);
}

// Спавн одной зоны. Возвращает false, если движок энтити не отдал или снёс её на спавне.
static_function bool SpawnZone(const KzCyberZone &zone)
{
	// Регистрации в Mapping API снимаются только на round_prestart, а правка зоны
	// переспавнивает весь набор — за раунд их можно накопить. Вектор фиксированный, и
	// AddToTail за границей пишет в соседние поля, поэтому упираемся заранее и с reason.
	const i32 registered = KZ::mapapi::RegisteredTriggerCount();
	const i32 capacity = KZ::mapapi::MaxRegisteredTriggers();
	if (registered >= capacity - KZ_ZONES_PER_MAP_LIMIT)
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] zone_spawn_failed id=%s reason=trigger_table_full registered=%d capacity=%d\n", zone.id,
					 registered, capacity);
		return false;
	}

	// Дескриптор KZ_NO_MAPAPI_COURSE_DESCRIPTOR существует ТОЛЬКО на картах без Mapping API —
	// там его заводит сам форк. На карте с курсами такой зоны нет, и каждое касание било бы
	// Mapi_Error, а тот раз в минуту высыпает ошибки в общий чат всем игрокам. Проверяем ДО
	// создания энтити и keyvalues: иначе выход из середины функции оставлял бы их течь.
	if ((zone.type == KZ_CYBER_ZONE_START || zone.type == KZ_CYBER_ZONE_END) && KZ::mapapi::MapApiVersion() != KZ_NO_MAPAPI_VERSION)
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_spawn_skipped id=%s type=%s reason=map_has_mapping_api\n", zone.id, ZoneTypeName(zone.type));
		return false;
	}

	CBaseTrigger *trigger = utils::CreateEntityByName<CBaseTrigger>("trigger_multiple");
	if (!trigger)
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] zone_spawn_failed id=%s reason=create_entity_failed\n", zone.id);
		return false;
	}

	// Центр бокса: origin энтити. Габариты задаём относительно него, поэтому bbox симметричен.
	const Vector origin((zone.mins.x + zone.maxs.x) * 0.5f, (zone.mins.y + zone.maxs.y) * 0.5f, (zone.mins.z + zone.maxs.z) * 0.5f);
	const Vector half((zone.maxs.x - zone.mins.x) * 0.5f, (zone.maxs.y - zone.mins.y) * 0.5f, (zone.maxs.z - zone.mins.z) * 0.5f);

	// КРИТИЧНО: объём задаётся ДО DispatchSpawn. Обоснование и данные замера — в kz_zones.h.
	CCollisionProperty *collision = trigger->m_pCollision();
	if (!collision)
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] zone_spawn_failed id=%s reason=no_collision_property\n", zone.id);
		g_pKZUtils->RemoveEntity(trigger);
		return false;
	}
	collision->m_vecMins(Vector(-half.x, -half.y, -half.z));
	collision->m_vecMaxs(Vector(half.x, half.y, half.z));
	collision->m_nSolidType(SOLID_BBOX);
	collision->m_usSolidFlags((uint8)(FSOLID_NOT_SOLID | FSOLID_TRIGGER));
	collision->m_CollisionGroup((uint8)COLLISION_GROUP_TRIGGER);
	// Фильтр касаний ищет слой CONTENTS_TRIGGER (1<<2) — энтити обязана объявить себя им.
	collision->m_collisionAttribute().m_nInteractsAs(collision->m_collisionAttribute().m_nInteractsAs() | (1ull << LAYER_INDEX_CONTENTS_TRIGGER));
	collision->m_collisionAttribute().m_nCollisionGroup((uint8)COLLISION_GROUP_TRIGGER);

	CEntityKeyValues *kv = new CEntityKeyValues();
	kv->SetString("targetname", KZ_CYBER_ZONE_NAME);
	kv->SetVector("origin", origin);
	kv->SetQAngle("angles", vec3_angle);
	// spawnflags бит 0 — «срабатывать на игроков»: без него PassesTriggerFilters вернёт false
	// и зона не подействует, даже если касание случится.
	kv->SetInt("spawnflags", 1);
	kv->SetBool("StartDisabled", false);

	switch (zone.type)
	{
		case KZ_CYBER_ZONE_START:
		case KZ_CYBER_ZONE_END:
		{
			kv->SetInt("timer_trigger_type", zone.type == KZ_CYBER_ZONE_START ? KZTRIGGER_ZONE_START : KZTRIGGER_ZONE_END);
			// Первая итерация — только main-курс, и он существует лишь на картах без Mapping API
			// (проверено выше, до создания энтити): там дефолтный курс заводит сам форк.
			kv->SetString("timer_zone_course_descriptor", KZ_NO_MAPAPI_COURSE_DESCRIPTOR);
			break;
		}
		case KZ_CYBER_ZONE_MODIFIER:
		{
			kv->SetInt("timer_trigger_type", KZTRIGGER_MODIFIER);
			kv->SetFloat("timer_modifier_jump_impulse", zone.jumpFactor);
			break;
		}
		default:
			// Недостижимо: тип заполняется только тремя значениями, и все три покрыты выше.
			// Освобождаем kv явно ради формальной чистоты выхода.
			kv->Release();
			g_pKZUtils->RemoveEntity(trigger);
			return false;
	}

	const CEntityHandle handle = trigger->GetRefEHandle();
	trigger->DispatchSpawn(kv);

	CEntityInstance *inst = GameEntitySystem() ? GameEntitySystem()->GetEntityInstance(handle) : nullptr;
	if (!inst)
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] zone_spawn_failed id=%s reason=removed_on_spawn\n", zone.id);
		return false;
	}
	g_cybZones.spawned.AddToTail(handle);
	return true;
}

static_function void DespawnAll()
{
	FOR_EACH_VEC(g_cybZones.spawned, i)
	{
		CEntityInstance *inst = GameEntitySystem() ? GameEntitySystem()->GetEntityInstance(g_cybZones.spawned[i]) : nullptr;
		if (inst && inst->m_pEntity && inst->m_pEntity->NameMatches(KZ_CYBER_ZONE_NAME))
		{
			g_pKZUtils->RemoveEntity(inst);
		}
	}
	g_cybZones.spawned.RemoveAll();
}

static_function void ApplyLoadedZones(const char *reason);

static_function bool ReadVector(KeyValues3 *obj, const char *key, Vector &out)
{
	KeyValues3 *v = obj ? obj->FindMember(key) : nullptr;
	if (!v || v->GetType() != KV3_TYPE_TABLE)
	{
		return false;
	}
	KeyValues3 *x = v->FindMember("x");
	KeyValues3 *y = v->FindMember("y");
	KeyValues3 *z = v->FindMember("z");
	if (!x || !y || !z)
	{
		return false;
	}
	out.x = (f32)x->GetDouble(0.0);
	out.y = (f32)y->GetDouble(0.0);
	out.z = (f32)z->GetDouble(0.0);
	return true;
}

// Разбор ответа api. Битую зону пропускаем поимённо, весь набор из-за неё не бракуем:
// на канарейке лучше применить три зоны из четырёх и сказать про четвёртую, чем молча
// оставить карту сломанной.
static_function void IngestZones(const char *body, const std::string &mapName)
{
	KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
	CUtlString error = "";
	LoadKV3FromJSON(&kv, &error, body, "");
	if (!error.IsEmpty())
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zones_load_failed map=%s reason=bad_json err=%s\n", mapName.c_str(), error.Get());
		return;
	}

	KeyValues3 *revision = kv.FindMember("revision");
	KeyValues3 *arr = kv.FindMember("zones");
	if (!arr || arr->GetType() != KV3_TYPE_ARRAY)
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zones_load_failed map=%s reason=no_zones_array\n", mapName.c_str());
		return;
	}

	g_cybZones.zones.clear();
	g_cybZones.revision = revision ? (i32)revision->GetDouble(0.0) : 0;

	const i32 count = arr->GetArrayElementCount();
	i32 skipped = 0;
	for (i32 i = 0; i < count && (i32)g_cybZones.zones.size() < KZ_ZONES_PER_MAP_LIMIT; i++)
	{
		KeyValues3 *el = arr->GetArrayElement(i);
		if (!el || el->GetType() != KV3_TYPE_TABLE)
		{
			skipped++;
			continue;
		}
		KzCyberZone zone {};
		KeyValues3 *idMember = el->FindMember("id");
		KeyValues3 *typeMember = el->FindMember("triggerType");
		if (!idMember || !typeMember || !KZ::zones::ParseZoneType(typeMember->GetString(""), zone.type))
		{
			skipped++;
			continue;
		}
		V_snprintf(zone.id, sizeof(zone.id), "%s", idMember->GetString(""));
		if (!ReadVector(el, "mins", zone.mins) || !ReadVector(el, "maxs", zone.maxs))
		{
			skipped++;
			continue;
		}
		if (zone.type == KZ_CYBER_ZONE_MODIFIER)
		{
			KeyValues3 *params = el->FindMember("params");
			KeyValues3 *factor = params ? params->FindMember("jumpFactor") : nullptr;
			zone.jumpFactor = factor ? (f32)factor->GetDouble(1.0) : 1.0f;
			if (zone.jumpFactor <= 0.0f)
			{
				skipped++;
				continue;
			}
		}
		g_cybZones.zones.push_back(zone);
	}

	g_cybZones.loaded = true;
	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zones_loaded map=%s count=%d skipped=%d revision=%d\n", mapName.c_str(), (i32)g_cybZones.zones.size(),
				skipped, g_cybZones.revision);

	// Спавним по конъюнкции «набор загружен И мир готов», порядок произвольный: события раунда
	// на kz-инстансе случаются один раз за карту и в те же кадры, что ответ api. Решение
	// принимает ApplyLoadedZones — она одна знает про готовность мира; вторая проверка здесь
	// была бы вторым местом, где правило может разъехаться.
	ApplyLoadedZones("api_response");

	// Сторож на «round_start не пришёл». Единственный путь спавна висит на одном событии, и
	// его неприход дал бы ровно тот тихий отказ, который чинит этот коммит: зоны не работают,
	// а признак — ОТСУТСТВИЕ строки в логе. Ищем не отсутствие, а явный warn с reason.
	// Поколение карты — параметром таймера: preserveMapChange=false защиты НЕ даёт
	// (RemoveNonPersistentTimers() в форке ниоткуда не зовётся), поэтому сторож, вооружённый на
	// карте A, доживёт до карты B. Без сверки он стрелял бы на здоровой карте в окне «ответ api
	// пришёл, round_start ещё нет» — а сторож, кричащий на исправной карте, обесценивает себя
	// ровно как молчащая диагностика. Идиома та же, что previewGeneration в редакторе.
	StartTimer<u32>(
		[](u32 generation) -> f64
		{
			if (generation != g_cybZones.mapGeneration)
			{
				return -1.0; // карта сменилась — это сторож от прошлой
			}
			if (g_cybZones.loaded && !g_cybZones.zones.empty() && !g_cybZones.worldReady)
			{
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zones_not_applied map=%s count=%d reason=world_never_ready\n", g_cybZones.mapName.c_str(),
							(i32)g_cybZones.zones.size());
			}
			return -1.0;
		},
		g_cybZones.mapGeneration, KZ_ZONES_WORLD_READY_TIMEOUT, false);
}

// Снять всё своё и поставить набор заново. reason — только для лога.
static_function void ApplyLoadedZones(const char *reason)
{
	DespawnAll();
	if (!g_cybZones.loaded || g_cybZones.zones.empty())
	{
		return;
	}
	// Единственная точка спавна, и она же единственная, кто знает про готовность мира: в
	// неготовый мир энтити создавать бессмысленно — движковая очистка их снесёт.
	if (!g_cybZones.worldReady)
	{
		KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zones_deferred map=%s count=%d reason=world_not_ready caller=%s\n", g_cybZones.mapName.c_str(),
					(i32)g_cybZones.zones.size(), reason);
		return;
	}
	KZ::mapapi::BeginExternalTriggerSpawn();
	i32 ok = 0;
	for (const KzCyberZone &zone : g_cybZones.zones)
	{
		if (SpawnZone(zone))
		{
			ok++;
		}
	}
	KZ::mapapi::EndExternalTriggerSpawn();
	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zones_applied map=%s count=%d of=%d revision=%d reason=%s\n", g_cybZones.mapName.c_str(), ok,
				(i32)g_cybZones.zones.size(), g_cybZones.revision, reason);
}

void KZ::zones::ResetEditors()
{
	// Граница и оверлоад связаны — менять одно без другого нельзя.
	// Здесь взят ToPlayer(CPlayerSlot): он внутри делает index = slot.Get() + 1 по массиву
	// players[MAXPLAYERS + 1] (валидны 0..MAXPLAYERS), поэтому верхняя граница строго
	// `i < MAXPLAYERS`. При `<=` вышло бы чтение players[MAXPLAYERS + 1] — за концом
	// аллокации, с разыменованием мусорного указателя; assert внутри в релизе выкомпилен.
	// Индексный оверлоад ToPlayer(u32) тоже существует (kz.h, движение; определён в
	// kz_manager.cpp как players[index] напрямую), и для НЕГО верна другая граница —
	// `<= MAXPLAYERS`, как в kz_goto.cpp. Не переносить границу из соседнего файла, не
	// посмотрев, какой оверлоад там вызывается.
	for (i32 i = 0; i < MAXPLAYERS; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(i));
		if (player && player->zonesService)
		{
			player->zonesService->OnMapChanged();
		}
	}
}

void KZ::zones::OnMapLoaded()
{
	KZ::zones::ResetEditors();
	g_cybZones.mapGeneration++;
	g_cybZones.worldReady = false;
	DespawnAll();
	g_cybZones.zones.clear();
	g_cybZones.revision = 0;
	g_cybZones.loaded = false;
	g_cybZones.mapName = g_pKZUtils->GetCurrentMapName().Get();

	const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
	if (!url || url[0] == '\0')
	{
		return; // платформенный источник выключен — играем на родных триггерах карты
	}
	const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");

	std::string fullUrl = url;
	if (!fullUrl.empty() && fullUrl.back() == '/')
	{
		fullUrl.pop_back();
	}
	fullUrl += "/ingest/v1/kz/zones";

	const std::string mapName = g_cybZones.mapName;

	HTTP::Request req(HTTP::Method::GET, fullUrl);
	req.SetQuery("map", mapName);
	if (token && token[0] != '\0')
	{
		req.SetHeader("Authorization", std::string("Bearer ") + token);
	}

	// clang-format off
	req.Send(
		[mapName](HTTP::Response resp)
		{
			// Карта могла смениться, пока запрос летел — чужие зоны на новой карте недопустимы.
			if (!KZ_STREQ(mapName.c_str(), g_pKZUtils->GetCurrentMapName().Get()))
			{
				return;
			}
			if (resp.status < 200 || resp.status >= 300)
			{
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zones_load_failed map=%s reason=http_%u\n", mapName.c_str(), (unsigned)resp.status);
				return;
			}
			std::optional<std::string> body = resp.Body();
			if (!body.has_value())
			{
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zones_load_failed map=%s reason=empty_body\n", mapName.c_str());
				return;
			}
			IngestZones(body->c_str(), mapName);
		},
		[mapName]()
		{
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zones_load_failed map=%s reason=network_error\n", mapName.c_str());
		});
	// clang-format on
}

void KZ::zones::OnRoundPreStart()
{
	// ЗДЕСЬ НЕ СПАВНИМ — и это главный урок бага cyb.118. Окно регистрации Mapping API
	// (round_prestart -> round_start) рассчитано на энтити, которые пересоздаёт САМ движок:
	// его очистка мира идёт ПОСЛЕ события round_prestart, поэтому всё, что мы успели создать
	// в обработчике, она сносит. Симптом был коварным: SpawnZone проверяет энтити сразу после
	// DispatchSpawn и честно рапортует zones_applied, а через мгновение энтити уже нет;
	// !zone list при этом показывает зону, потому что читает ДАННЫЕ, а не мир.
	// Снимаем копию хендлов: по ней на round_start считается аудит выживаемости. Держать её
	// отдельно обязательно — сам spawned к тому моменту может быть уже обнулён переспавном.
	g_cybZones.preRoundSpawned.RemoveAll();
	FOR_EACH_VEC(g_cybZones.spawned, i)
	{
		g_cybZones.preRoundSpawned.AddToTail(g_cybZones.spawned[i]);
	}
	g_cybZones.worldReady = false;
}

void KZ::zones::OnRoundStart()
{
	// Аудит ДО любого вмешательства: сколько наших энтити пережило движковую очистку мира.
	// Пишется всегда, а не только при расхождении — именно отсутствие такой проверки стоило
	// нам бага «применил, но не работает»: мы верили строке лога, а не состоянию мира.
	i32 alive = 0;
	FOR_EACH_VEC(g_cybZones.preRoundSpawned, i)
	{
		CEntityInstance *inst = GameEntitySystem() ? GameEntitySystem()->GetEntityInstance(g_cybZones.preRoundSpawned[i]) : nullptr;
		if (inst && inst->m_pEntity && inst->m_pEntity->NameMatches(KZ_CYBER_ZONE_NAME))
		{
			alive++;
		}
	}
	// Печатается ВСЕГДА, включая of=0: «строки нет» — худший вид диагностики, именно на нём мы
	// и потеряли время с багом cyb.118.
	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zones_audit stage=round_start map=%s alive=%d of=%d\n", g_cybZones.mapName.c_str(), alive,
				g_cybZones.preRoundSpawned.Count());
	g_cybZones.preRoundSpawned.RemoveAll();

	// Мир доделан — теперь спавн доживает до игрока.
	g_cybZones.worldReady = true;
	ApplyLoadedZones("round_start");
}

bool KZ::zones::IsReady()
{
	return g_cybZones.loaded;
}

const char *KZ::zones::CurrentMapName()
{
	return g_cybZones.mapName.c_str();
}

const std::vector<KzCyberZone> &KZ::zones::Loaded()
{
	return g_cybZones.zones;
}

i32 KZ::zones::Revision()
{
	return g_cybZones.revision;
}

void KZ::zones::AddAndSpawn(const KzCyberZone &zone, i32 apiRevision)
{
	if ((i32)g_cybZones.zones.size() >= KZ_ZONES_PER_MAP_LIMIT)
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_add_rejected map=%s reason=zone_limit\n", g_cybZones.mapName.c_str());
		return;
	}
	g_cybZones.zones.push_back(zone);
	// revision — версия набора у api; свой счётчик только фолбэк, иначе !zone list врёт.
	g_cybZones.revision = apiRevision > 0 ? apiRevision : g_cybZones.revision + 1;

	// Поставленная зона обязана ожить без рестарта раунда: рестарт срубил бы раны всем на
	// сервере. Поэтому точечно открываем окно регистрации ровно на свой DispatchSpawn.
	// В неготовый мир не спавним: движковая очистка всё равно снесёт энтити, а игрок увидел бы
	// «зона поставлена» и не почувствовал её. Такую зону поставит round_start.
	bool spawned = false;
	if (g_cybZones.worldReady)
	{
		KZ::mapapi::BeginExternalTriggerSpawn();
		spawned = SpawnZone(g_cybZones.zones.back());
		KZ::mapapi::EndExternalTriggerSpawn();
	}
	else
	{
		// Через общий путь: он сам напишет reason=world_not_ready, и «решение принимает одна
		// точка» становится буквальным, а не почти-буквальным.
		ApplyLoadedZones("zone_added");
	}

	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zone_added steam_id=%llu map=%s id=%s type=%s spawned=%d revision=%d\n",
				g_cybZones.zones.back().createdBy, g_cybZones.mapName.c_str(), g_cybZones.zones.back().id, ZoneTypeName(g_cybZones.zones.back().type),
				spawned ? 1 : 0, g_cybZones.revision);
}

bool KZ::zones::RemoveById(const char *id, i32 apiRevision)
{
	for (auto it = g_cybZones.zones.begin(); it != g_cybZones.zones.end(); ++it)
	{
		if (!KZ_STREQ(it->id, id))
		{
			continue;
		}
		g_cybZones.zones.erase(it);
		// revision авторитетный из ответа api; свой инкремент только фолбэк.
		g_cybZones.revision = apiRevision > 0 ? apiRevision : g_cybZones.revision + 1;
		// Энтити снимаем разом и ставим набор заново: попадание «зона ↔ энтити» один-в-один
		// не гарантировано (движок мог снести энтити на спавне), а держать вторую таблицу
		// соответствий ради удаления одной зоны дороже, чем переспавнить набор.
		// Через общий путь — он один знает про готовность мира.
		ApplyLoadedZones("zone_removed");
		KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zone_removed map=%s id=%s revision=%d\n", g_cybZones.mapName.c_str(), id, g_cybZones.revision);
		return true;
	}
	return false;
}
