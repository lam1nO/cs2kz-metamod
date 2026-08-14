/*
	Зоны KZ-карт из БД платформы: загрузка набора на старте карты и спавн триггеров.
	Условие «bbox до DispatchSpawn» и его доказательство — в kz_zones.h.
*/

#include "kz_zones.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/db/kz_db.h"
#include "kz/option/kz_option.h"
// Полные типы сервисов игрока: kz.h объявляет их только указателями, а нам нужно читать
// triggerTrackers и спрашивать IsInPrac при поиске задетых переприменением.
#include "kz/trigger/kz_trigger.h"
#include "kz/prac/kz_prac.h"
#include "kz/language/kz_language.h"
#include "utils/utils.h"
#include "utils/http.h"
#include "utils/ctimer.h"

#include "sdk/entity/cbasetrigger.h"
#include "sdk/entity/cbasemodelentity.h"
#include "sdk/ccollisionproperty.h"
#include "entity2/entitykeyvalues.h"
#include "entity2/entitysystem.h"
#include "tier1/keyvalues3.h"

#include <optional> // resp.Body() отдаёт std::optional (приезжает и из utils/http.h, но явно надёжнее)
#include <string>
#include <vector>

// Имя, по которому свои зоны отличаются от родных триггеров карты (снятие, отладка).
#define KZ_CYBER_ZONE_NAME "cyb_map_zone"

// Поставленная зона: что стоит в мире и из какой записи оно поставлено.
struct KzSpawnedZone
{
	CEntityHandle handle;
	KzCyberZone zone;
};

static_global struct
{
	std::string mapName;
	std::vector<KzCyberZone> zones;
	// Курсы карты по версии платформы. Приезжают тем же ответом, что и зоны, поэтому курс
	// гарантированно известен раньше зоны, которая на него ссылается.
	std::vector<KzCyberCourse> courses;
	// Хендл + ЗАПИСЬ, из которой зона поставлена. Запись нужна для diff-применения: без неё
	// пришлось бы сносить и ставить заново весь набор на каждую правку, а это доигрывает
	// EndTouch/StartTouch всем, кто стоит в зоне (стёртые чекпоинты и дёрганый таймер).
	std::vector<KzSpawnedZone> spawned;
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
	// Дескрипторы курсов, которые завели ИМЕННО МЫ на этой карте. Нужны поимённо, а не флагом:
	// досетап в локальной БД обязан подтверждаться по КАЖДОМУ нашему курсу. Проверка по одному
	// захардкоженному дескриптору давала два тихих отказа сразу — курс с другим именем не гасил
	// флаг никогда, а чужой "Default" с ненулевым id гасил его за наш курс, оставляя тому
	// localDatabaseID = 0, то есть раны в никуда.
	std::vector<std::string> createdCourses;
	// Ждём момента, когда карта будет заведена в БД, чтобы досетапить курс.
	bool localCoursesPending;
} g_cybZones;

// Завели ли МЫ курс с таким дескриптором на этой карте (определение ниже — им пользуется SpawnZone).
static_function bool WeCreatedCourse(const char *descriptorName);

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
		case KZ_CYBER_ZONE_STAGE:
			return "stage";
		case KZ_CYBER_ZONE_CHECKPOINT:
			return "checkpoint";
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
	if (KZ_STREQ(raw, "stage"))
	{
		out = KZ_CYBER_ZONE_STAGE;
		return true;
	}
	if (KZ_STREQ(raw, "checkpoint"))
	{
		out = KZ_CYBER_ZONE_CHECKPOINT;
		return true;
	}
	// "split" НЕ разбираем намеренно: api отбивает его четырёхсотым, и тащить непроверенный путь
	// в спавн незачем. Появится в api — добавится здесь одной веткой.
	return false;
}

const char *KZ::zones::ZoneTypeToString(KzCyberZoneType type)
{
	return ZoneTypeName(type);
}

// Спавн одной зоны. Возвращает false, если зону отбил гейт, движок не отдал энтити или снёс её
// на спавне. outReason (может быть nullptr) получает ту же машинно-читаемую причину, что уходит
// в лог: редактор показывает её автору зоны, иначе «поставлено» и «работает» расходятся молча.
static_function bool SpawnZone(const KzCyberZone &zone, const char **outReason = nullptr)
{
	auto fail = [&](const char *reason) -> bool
	{
		if (outReason)
		{
			*outReason = reason;
		}
		return false;
	};

	// Регистрации в Mapping API снимаются только на round_prestart, а правка зоны
	// переспавнивает весь набор — за раунд их можно накопить. Вектор фиксированный, и
	// AddToTail за границей пишет в соседние поля, поэтому упираемся заранее и с reason.
	const i32 registered = KZ::mapapi::RegisteredTriggerCount();
	const i32 capacity = KZ::mapapi::MaxRegisteredTriggers();
	if (registered >= capacity - KZ_ZONES_PER_MAP_LIMIT)
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] zone_spawn_failed id=%s reason=trigger_table_full registered=%d capacity=%d\n", zone.id,
					 registered, capacity);
		return fail("trigger_table_full");
	}

	// Курс зоны и гейт. Все проверки — ДО создания энтити и keyvalues: выход из середины функции
	// оставлял бы их течь.
	//
	// Дескриптор, с которым зона будет зарегистрирована, обязан быть КАНОНИЧЕСКИМ — ровно тем
	// написанием, что держит карта. Поиск курса идёт без учёта регистра, а подсчёт зон курса — с
	// учётом; зона с разошедшимся регистром запускает таймер, но не попадает в счётчики стейджей,
	// а те гейтят финиш. Отказ был бы полностью тихим.
	const char *courseDescriptor = nullptr;
	const bool needsCourse = zone.type != KZ_CYBER_ZONE_MODIFIER;
	if (needsCourse)
	{
		if (zone.courseDescriptor[0])
		{
			// Курс выбран администратором явно — этого достаточно, проверять владение НЕ нужно:
			// смысл операции и есть «переставить зоны чужому курсу». Требуем лишь существования
			// дескриптора, иначе каждое касание било бы Mapi_Error, а тот раз в минуту высыпает
			// накопленное в общий чат всем игрокам.
			courseDescriptor = KZ::mapapi::GetCanonicalCourseDescriptor(zone.courseDescriptor);
			if (!courseDescriptor)
			{
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_spawn_skipped id=%s type=%s course=%s reason=unknown_course\n", zone.id,
							ZoneTypeName(zone.type), zone.courseDescriptor);
				return fail("unknown_course");
			}
		}
		else
		{
			// Курс не выбран — прежнее поведение первой итерации: зона идёт в наш дефолтный курс,
			// и вот здесь владение проверять ОБЯЗАТЕЛЬНО. Имя "Default" ничем не зарезервировано,
			// маппер вправе назвать так свой курс, и гейт по одному лишь существованию имени
			// поставил бы нашу зону второй старт-зоной в чужой курс.
			if (!KZ::mapapi::IsPlatformOwnedCourse(KZ_NO_MAPAPI_COURSE_DESCRIPTOR))
			{
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_spawn_skipped id=%s type=%s reason=no_platform_course\n", zone.id,
							ZoneTypeName(zone.type));
				return fail("no_platform_course");
			}
			courseDescriptor = KZ::mapapi::GetCanonicalCourseDescriptor(KZ_NO_MAPAPI_COURSE_DESCRIPTOR);
			if (!courseDescriptor)
			{
				return fail("no_platform_course");
			}
		}
	}

	CBaseTrigger *trigger = utils::CreateEntityByName<CBaseTrigger>("trigger_multiple");
	if (!trigger)
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] zone_spawn_failed id=%s reason=create_entity_failed\n", zone.id);
		return fail("create_entity_failed");
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
		return fail("no_collision_property");
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
		case KZ_CYBER_ZONE_STAGE:
		case KZ_CYBER_ZONE_CHECKPOINT:
		{
			KzTriggerType triggerType = KZTRIGGER_ZONE_START;
			if (zone.type == KZ_CYBER_ZONE_END)
			{
				triggerType = KZTRIGGER_ZONE_END;
			}
			else if (zone.type == KZ_CYBER_ZONE_STAGE)
			{
				triggerType = KZTRIGGER_ZONE_STAGE;
			}
			else if (zone.type == KZ_CYBER_ZONE_CHECKPOINT)
			{
				triggerType = KZTRIGGER_ZONE_CHECKPOINT;
			}
			kv->SetInt("timer_trigger_type", triggerType);
			// Каноническое написание, а не строка из api — см. развёрнутое обоснование у гейта.
			kv->SetString("timer_zone_course_descriptor", courseDescriptor);
			// Номер приходит от api готовым; ключи разные у стейджа и чекпойнта, и перепутать их
			// значит получить зону, которую разбор Mapping API отобьёт как «номер 0».
			if (zone.type == KZ_CYBER_ZONE_STAGE)
			{
				kv->SetInt("timer_zone_stage_number", zone.stageNumber);
			}
			else if (zone.type == KZ_CYBER_ZONE_CHECKPOINT)
			{
				kv->SetInt("timer_zone_checkpoint_number", zone.stageNumber);
			}
			break;
		}
		case KZ_CYBER_ZONE_MODIFIER:
		{
			kv->SetInt("timer_trigger_type", KZTRIGGER_MODIFIER);
			kv->SetFloat("timer_modifier_jump_impulse", zone.jumpFactor);
			break;
		}
		default:
			// Недостижимо: ParseZoneType выставляет только покрытые выше значения (split он
			// намеренно не разбирает). Освобождаем kv явно ради чистоты выхода.
			kv->Release();
			g_pKZUtils->RemoveEntity(trigger);
			return fail("unknown_zone_type");
	}

	const CEntityHandle handle = trigger->GetRefEHandle();
	trigger->DispatchSpawn(kv);

	CEntityInstance *inst = GameEntitySystem() ? GameEntitySystem()->GetEntityInstance(handle) : nullptr;
	if (!inst)
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] zone_spawn_failed id=%s reason=removed_on_spawn\n", zone.id);
		return fail("removed_on_spawn");
	}

	// Живой энтити МАЛО: Mapi_OnTriggerMultipleSpawn выходит без регистрации на своих ошибочных
	// ветках (пустой дескриптор, номер зоны <= 0, закрытое окно регистрации). Незарегистрированная
	// энтити — это зона, которую видно и в которую можно войти, но для таймера её нет.
	// Раньше цена такой зоны была один раунд, теперь — до смены карты: diff увидит её живой,
	// сочтёт совпавшей и оставит стоять навсегда. Поэтому предпосылка «энтити жива ⇒ регистрация
	// жива» здесь ПРОВЕРЯЕТСЯ, а не предполагается.
	if (!KZ::mapapi::GetKzTrigger(trigger))
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] zone_spawn_failed id=%s type=%s reason=not_registered\n", zone.id, ZoneTypeName(zone.type));
		g_pKZUtils->RemoveEntity(inst);
		return fail("not_registered");
	}

	// Стартовая позиция курса — из объёма нашей старт-зоны. Родного info_teleport_destination
	// "timer_start" на этих картах нет, а без позиции !main и меню !courses показывают курс серым
	// «No Start Position For Course» и никуда не телепортируют.
	// Перебиваем позицию ТОЛЬКО у курса, который завели мы сами.
	// У чужого курса (родного или выбранного админом явно) позицию задаёт маппер через
	// info_teleport_destination "timer_start" — молча смещать точку рестарта всей карте мы не
	// вправе. Если её там нет, overwrite=false всё равно заполнит пустое место, то есть хуже не
	// станет никогда.
	// ЗНАЕМ И ПРИНИМАЕМ: у чужого курса с уже заданной позицией !main продолжит телепортировать
	// на СТАРУЮ точку, даже если старт-зону переставили. Отдельная операция, её никто не заказывал.
	if (zone.type == KZ_CYBER_ZONE_START)
	{
		// Владение проверяем по КОНКРЕТНОМУ курсу. Глобальный флаг «мы что-то завели» здесь врал:
		// на карте без Mapping API один own-курс из api взводил бы его, и безкурсовая старт-зона
		// перебила бы мапперский info_teleport_destination "timer_start" — то самое, что запрещает
		// комментарий выше.
		const bool overwrite = WeCreatedCourse(courseDescriptor);
		if (!KZ::mapapi::SetCourseStartPositionFromTrigger(courseDescriptor, trigger, overwrite))
		{
			// Не отказ спавна: зона работает, страдает только телепорт на старт.
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_start_position_failed id=%s course=%s reason=no_valid_position\n", zone.id,
						courseDescriptor);
		}
	}

	g_cybZones.spawned.push_back({handle, zone});
	return true;
}

// Дескриптор курса под наши start/end. Форк заводит KZ_NO_MAPAPI_COURSE_DESCRIPTOR сам только на
// картах без Mapping API (kz_mappingapi.cpp, ветка KZ_NO_MAPAPI_VERSION). Целевые карты Mapping
// API объявляют, а курсов не имеют — там дескриптор приходится заводить нам.
static_function void EnsurePlatformCourse()
{
	// Тот же критерий, что у гейта в SpawnZone: курс уже есть и он наш (или форковый на карте
	// без Mapping API). Спрашивать здесь просто «есть ли дескриптор с таким именем» нельзя —
	// мапперский курс, названный "Default", тогда считался бы нашим.
	if (KZ::mapapi::IsPlatformOwnedCourse(KZ_NO_MAPAPI_COURSE_DESCRIPTOR))
	{
		return;
	}
	// У карты есть свои курсы — свой главный курс поверх них не заводим: зона должна вставать
	// в выбранный админом курс карты, а выбор курса — задача B. Тихо выходим, а зону отобьёт
	// гейт в SpawnZone с reason=no_platform_course.
	if (KZ::course::GetCourseCount() > 0)
	{
		return;
	}
	const char *reason = KZ::mapapi::CreateExternalCourse(0, KZ_NO_MAPAPI_COURSE_NAME, KZ_NO_MAPAPI_COURSE_DESCRIPTOR);
	if (reason)
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] course_create_failed map=%s descriptor=%s reason=%s\n", g_cybZones.mapName.c_str(),
					 KZ_NO_MAPAPI_COURSE_DESCRIPTOR, reason);
		return;
	}
	g_cybZones.localCoursesPending = true;
	g_cybZones.createdCourses.push_back(KZ_NO_MAPAPI_COURSE_DESCRIPTOR);
	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] course_created map=%s descriptor=%s name=%s number=0\n", g_cybZones.mapName.c_str(),
				KZ_NO_MAPAPI_COURSE_DESCRIPTOR, KZ_NO_MAPAPI_COURSE_NAME);
}

// Это НЕ то же, что «курс платформы»: IsPlatformOwnedCourse на карте без Mapping API истинен для
// ЛЮБОГО дескриптора, потому что там единственный курс форковый.
static_function bool WeCreatedCourse(const char *descriptorName)
{
	if (!descriptorName || !descriptorName[0])
	{
		return false;
	}
	for (const std::string &created : g_cybZones.createdCourses)
	{
		if (KZ_STREQI(created.c_str(), descriptorName))
		{
			return true;
		}
	}
	return false;
}

// Полон ли набор нумерованных зон (stage/checkpoint) для курса: номера обязаны образовывать
// 1..N без дыр и повторов.
//
// Зачем предохранитель. Погашение родных зон идёт по паре (курс, тип), то есть сносит ВСЕ родные
// стейджи курса разом. Если наш набор при этом неполон — например, api прислал стейджи 1 и 3, или
// одна зона отвалилась на разборе, — валидация консекутивности не сойдётся, RecountCourseZones
// пропустит курс с warn'ом и оставит СТАРЫЙ stageCount, а TimerEnd после этого перестанет
// засчитывать финиш на всём курсе. Это единственное место, где одна кривая зона стоит курса
// целиком, поэтому проверяем сами, а не полагаемся на инвариант чужой стороны.
static_function bool HasCompleteNumberedSet(const char *descriptorName, KzCyberZoneType type)
{
	i32 count = 0;
	i32 seenXor = 0;
	for (const KzCyberZone &zone : g_cybZones.zones)
	{
		if (zone.type != type || !KZ_STREQI(zone.courseDescriptor, descriptorName))
		{
			continue;
		}
		if (zone.stageNumber <= 0)
		{
			return false;
		}
		// Тот же XOR-трюк, что у валидации Mapping API: 1..N в любом порядке даёт 0.
		seenXor ^= (++count) ^ zone.stageNumber;
	}
	return count > 0 && seenXor == 0;
}

static_function bool IsRejectedCourse(const std::vector<const char *> &rejected, const char *descriptorName)
{
	if (!descriptorName || !descriptorName[0])
	{
		return false;
	}
	for (const char *entry : rejected)
	{
		if (KZ_STREQI(entry, descriptorName))
		{
			return true;
		}
	}
	return false;
}

// Отказ от переопределения ключуется ПАРОЙ (курс, тип), а не одним курсом: неполнота проверяется
// именно по паре. Курс, где стейджи неполны, а чекпойнты полны, при ключе-курсе терял бы и
// чекпойнты — родные погасили бы, свои отбили, — и отказ в логе врал бы про тип.
struct KzOverrideRefusal
{
	const char *descriptor;
	KzCyberZoneType type;
};

static_function bool IsRefusedOverride(const std::vector<KzOverrideRefusal> &refused, const char *descriptorName, KzCyberZoneType type)
{
	if (!descriptorName || !descriptorName[0])
	{
		return false;
	}
	for (const KzOverrideRefusal &entry : refused)
	{
		if (entry.type == type && KZ_STREQI(entry.descriptor, descriptorName))
		{
			return true;
		}
	}
	return false;
}

// Совпадают ли записи ПОЛНОСТЬЮ. Сравнивать по одному id нельзя: дескриптор курса и номер
// стейджа запекаются в keyvalues на спавне, поэтому зона, у которой сменился курс или номер,
// обязана переспавниться — иначе она молча осталась бы в старом курсе.
static_function bool ZoneRecordEqual(const KzCyberZone &a, const KzCyberZone &b)
{
	return KZ_STREQ(a.id, b.id) && a.type == b.type && a.mins == b.mins && a.maxs == b.maxs && a.jumpFactor == b.jumpFactor
		   && a.stageNumber == b.stageNumber && KZ_STREQ(a.courseDescriptor, b.courseDescriptor);
}

static_function bool SlotListContains(const CUtlVector<CPlayerSlot> &list, CPlayerSlot slot)
{
	FOR_EACH_VEC(list, i)
	{
		if (list[i] == slot)
		{
			return true;
		}
	}
	return false;
}

// Кто из игроков сейчас касается этой энтити. Спрашиваем РЕАЛЬНЫЕ трекеры касания через
// публичный GetTriggerTracker, а не считаем по координатам: предупреждение, которое врёт, хуже
// отсутствующего.
//
// Списка два, потому что задевает по-разному и врать нельзя ни тем, ни другим:
//   outNormal — обычный игрок: StartTouch делает ResetCheckpoints, чекпоинты стираются;
//   outPrac   — игрок в prac: чекпоинты и таймер закрыты гардом, НО EndTouch старт-зоны уходит в
//               KZPracService::OnStartZoneEndTouch, а тот обнуляет и перезапускает часы попытки.
//               То есть репетиция сбрасывается — маленькая версия ровно той жалобы, ради которой
//               diff и делался.
static_function void CollectTouchingPlayers(CEntityHandle handle, CUtlVector<CPlayerSlot> &outNormal, CUtlVector<CPlayerSlot> &outPrac)
{
	CBaseTrigger *trigger = dynamic_cast<CBaseTrigger *>(GameEntitySystem() ? GameEntitySystem()->GetEntityInstance(handle) : nullptr);
	if (!trigger)
	{
		return;
	}
	// Граница цикла — см. развёрнутое обоснование в KZ::zones::ResetEditors.
	for (i32 i = 0; i < MAXPLAYERS; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(i));
		if (!player || !player->triggerService || !player->pracService)
		{
			continue;
		}
		if (!player->triggerService->GetTriggerTracker(trigger))
		{
			continue;
		}
		const CPlayerSlot slot = player->GetPlayerSlot();
		CUtlVector<CPlayerSlot> &out = player->pracService->IsInPrac() ? outPrac : outNormal;
		// Игрок может стоять сразу в двух снимаемых зонах — двух строк в чат он не заслужил.
		if (!SlotListContains(out, slot))
		{
			out.AddToTail(slot);
		}
	}
}

// Хвост применения набора. Зовётся, когда применение реально дошло до спавна.
static_function void SyncCoursesAfterApply()
{
	// Курс, заведённый нами, в локальной БД отсутствует: KZDatabaseService::SetupCourses отработал
	// на старте карты, до нашего создания. Без досетапа у курса остался бы localDatabaseID = 0 и
	// раны на нём легли бы в никуда. Ждём, пока карта заведена в БД; если она заведётся позже
	// нашего последнего применения, курс подхватит штатный OnMapSetup — он читает тот же
	// g_sortedCourses, где курс уже лежит.
	if (g_cybZones.localCoursesPending && KZDatabaseService::IsMapSetUp())
	{
		// Флаг гасим по РЕЗУЛЬТАТУ, а не по факту вызова: SetupLocalCourses асинхронна, и при
		// отказе транзакции (OnGenericTxnFailure) повтора бы не было — курс молча остался бы с
		// localDatabaseID = 0, то есть раны на нём легли бы в никуда. Проверяем, что id реально
		// приехал; пока нет — пробуем снова на следующем применении набора.
		// Спрашиваем по ДЕСКРИПТОРУ, а не по имени курса: лукап по имени отдаёт первое совпадение
		// в порядке сортировки по id, а наш id — самый большой. При любом сожительстве двух
		// «Main» мы прочитали бы ЧУЖОЙ id, погасили флаг и оставили свой курс с нулём — ровно тот
		// тихий отказ, который этот блок и лечит.
		// И проверяем КАЖДЫЙ заведённый нами курс, а не один захардкоженный дескриптор: иначе
		// курс с другим именем не гасил бы флаг никогда, а чужой "Default" с ненулевым id гасил
		// бы его за наш курс, оставляя тому localDatabaseID = 0.
		i32 registered = 0;
		for (const std::string &descriptor : g_cybZones.createdCourses)
		{
			u32 localId = 0;
			if (KZ::mapapi::GetCourseLocalDatabaseId(descriptor.c_str(), localId) && localId != 0)
			{
				registered++;
			}
		}
		if (!g_cybZones.createdCourses.empty() && registered == (i32)g_cybZones.createdCourses.size())
		{
			// Печатаем ровно один раз — сразу после того, как флаг погас.
			for (const std::string &descriptor : g_cybZones.createdCourses)
			{
				u32 localId = 0;
				KZ::mapapi::GetCourseLocalDatabaseId(descriptor.c_str(), localId);
				KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] course_local_setup map=%s descriptor=%s local_id=%u\n", g_cybZones.mapName.c_str(),
							descriptor.c_str(), localId);
			}
			g_cybZones.localCoursesPending = false;
		}
		else
		{
			KZ::course::SetupLocalCourses();
		}
	}

	// Счётчики зон курса гейтят финиш: KZTimerService::TimerEnd отбивает ран, если
	// currentStage != courseDesc->stageCount. Валидация Mapping API считает их один раз на
	// round_start и ДО того, как мы поставим свои зоны (порядок в hooks.cpp).
	// Теперь это не формальность: в наборе бывают stage и checkpoint, а погашение родных зон
	// меняет состав курса. Без пересчёта финиш отбивался бы как «missed stage» без единой строки
	// в логе — валидация Mapping API считает счётчики один раз на round_start и ДО того, как мы
	// тронем зоны.
	KZ::mapapi::RecountCourseZones();
}

static_function void DespawnAll()
{
	for (const KzSpawnedZone &spawned : g_cybZones.spawned)
	{
		CEntityInstance *inst = GameEntitySystem() ? GameEntitySystem()->GetEntityInstance(spawned.handle) : nullptr;
		if (inst && inst->m_pEntity && inst->m_pEntity->NameMatches(KZ_CYBER_ZONE_NAME))
		{
			g_pKZUtils->RemoveEntity(inst);
		}
	}
	g_cybZones.spawned.clear();
}

// focusZoneId/outFocusReason — только чтобы редактор мог сказать автору, почему ИМЕННО его зона
// не ожила. nullptr = не интересует.
static_function void ApplyLoadedZones(const char *reason, const char *focusZoneId = nullptr, const char **outFocusReason = nullptr);

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
	g_cybZones.courses.clear();
	g_cybZones.revision = revision ? (i32)revision->GetDouble(0.0) : 0;

	// Курсы разбираем ПЕРВЫМИ: зона ссылается на курс дескриптором, и к моменту применения набора
	// список курсов обязан быть полным. Поле аддитивное — его отсутствие означает старый api и
	// прежнее поведение (все зоны вне курсов), а не ошибку.
	KeyValues3 *coursesArr = kv.FindMember("courses");
	i32 coursesSkipped = 0;
	if (coursesArr && coursesArr->GetType() == KV3_TYPE_ARRAY)
	{
		const i32 courseCount = coursesArr->GetArrayElementCount();
		for (i32 i = 0; i < courseCount && (i32)g_cybZones.courses.size() < KZ_COURSES_PER_MAP_LIMIT; i++)
		{
			KeyValues3 *el = coursesArr->GetArrayElement(i);
			if (!el || el->GetType() != KV3_TYPE_TABLE)
			{
				coursesSkipped++;
				continue;
			}
			KeyValues3 *descMember = el->FindMember("descriptor");
			const char *desc = descMember ? descMember->GetString("") : "";
			if (!desc || !desc[0])
			{
				coursesSkipped++;
				continue;
			}
			KzCyberCourse course {};
			V_snprintf(course.descriptor, sizeof(course.descriptor), "%s", desc);
			KeyValues3 *kindMember = el->FindMember("kind");
			course.own = kindMember && KZ_STREQ(kindMember->GetString(""), "own");
			KeyValues3 *disabledMember = el->FindMember("disabled");
			course.disabled = disabledMember && disabledMember->GetBool(false);
			KeyValues3 *numberMember = el->FindMember("courseNumber");
			course.courseNumber = numberMember ? (i32)numberMember->GetDouble(0.0) : 0;
			g_cybZones.courses.push_back(course);
		}
	}

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
		// Курс зоны. Поле аддитивное: его отсутствие или null означает «зона вне курсов» —
		// прежнее поведение первой итерации, а не ошибка.
		KeyValues3 *courseMember = el->FindMember("courseDescriptor");
		const char *courseDesc = courseMember ? courseMember->GetString("") : "";
		if (courseDesc && courseDesc[0])
		{
			V_snprintf(zone.courseDescriptor, sizeof(zone.courseDescriptor), "%s", courseDesc);
		}
		if (zone.type == KZ_CYBER_ZONE_STAGE || zone.type == KZ_CYBER_ZONE_CHECKPOINT)
		{
			KeyValues3 *stageMember = el->FindMember("stageNumber");
			zone.stageNumber = stageMember ? (i32)stageMember->GetDouble(0.0) : 0;
			// Нумерация приходит от api готовой и консекутивной; плагин её не пересчитывает.
			// Но зона без номера или без курса уронила бы ВЕСЬ курс: валидация консекутивности
			// на round_start дропает курс целиком, а не одну зону. Поэтому такую зону не берём
			// вовсе — потерять одну зону несравнимо дешевле, чем курс.
			if (zone.stageNumber <= 0 || !zone.courseDescriptor[0])
			{
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_ingest_skipped id=%s type=%s stage=%d reason=%s\n", zone.id, ZoneTypeName(zone.type),
							zone.stageNumber, zone.courseDescriptor[0] ? "bad_stage_number" : "stage_without_course");
				skipped++;
				continue;
			}
		}
		g_cybZones.zones.push_back(zone);
	}

	g_cybZones.loaded = true;
	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zones_loaded map=%s count=%d skipped=%d courses=%d courses_skipped=%d revision=%d\n", mapName.c_str(),
				(i32)g_cybZones.zones.size(), skipped, (i32)g_cybZones.courses.size(), coursesSkipped, g_cybZones.revision);

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
static_function void ApplyLoadedZones(const char *reason, const char *focusZoneId, const char **outFocusReason)
{
	if (!g_cybZones.loaded)
	{
		DespawnAll();
		return; // набор ещё не получен — нечего применять и не о чем судить
	}
	// ПУСТОЙ НАБОР ЗОН — НЕ ПОВОД ВЫЙТИ. Состояние курсов живёт отдельно от зон: «админ отключил
	// курс, своих зон на карте нет» это ровно courses=[{disabled:true}] при zones=[]. Ранний выход
	// здесь делал бы отключение курса неработающим в самой естественной его конфигурации, а
	// удаление последней своей зоны оставляло бы родные зоны погашенными до конца раунда.
	// ШАГ 1. Возвращаем исходный тип всем родным зонам, которые гасили в прошлый раз. Набор мог
	// измениться (админ снял подмену), и без этого родная зона осталась бы погашенной до смены
	// карты. Так применение становится идемпотентным: «восстановить всё, затем погасить по
	// текущему набору», а не накоплением состояния между применениями.
	const i32 restored = KZ::mapapi::RestorePlatformDisabledZones();

	// ШАГИ 2-3 требуют, чтобы карта уже разобрала СВОИ дескрипторы курсов. Раньше это неявно
	// гарантировал round_start, но с выносом состояния курсов из-под worldReady осталась бы одна
	// лишь удача тайминга HTTP-колбэка. Цена ошибки высокая и громкая: до разбора
	// GetCourseCount() == 0 не значит «курсов нет», мы завели бы лишний главный курс на карте С
	// курсами, а занятый нами дескриптор заставил бы карту бить Mapi_Error — то есть спам в
	// ОБЩИЙ ЧАТ всем игрокам раз в минуту.
	const bool mapParsed = KZ::mapapi::IsMapParsed();

	// ШАГ 2. Заводим курсы платформы (kind=own), которых на карте ещё нет. Обязательно ДО спавна:
	// зона ссылается на курс дескриптором, и гейт в SpawnZone спрашивает уже готовый.
	// Дескрипторы курсов kind=own, которые оказались ЧУЖИМИ. Зоны на них не спавним и родные зоны
	// не гасим: иначе через kind=own вернулась бы ровно та дыра, которую закрыл прошлый круг
	// ревью, — курс платформы захватил бы мапперский курс и убил его старт с финишем.
	std::vector<const char *> rejectedCourses;
	for (const KzCyberCourse &course : g_cybZones.courses)
	{
		if (!course.own || !mapParsed)
		{
			continue;
		}
		if (KZ::mapapi::HasCourseDescriptor(course.descriptor))
		{
			// Дескриптор занят. Наш ли он? На карте без Mapping API форковый курс тоже «наш», и
			// это нормально; а вот мапперский курс с тем же именем — нет. Наш KZ_NO_MAPAPI_COURSE_
			// DESCRIPTOR это буквально "Default", и занять его карта вправе.
			if (!KZ::mapapi::IsPlatformOwnedCourse(course.descriptor))
			{
				KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] course_rejected map=%s descriptor=%s number=%d reason=own_course_is_native\n",
							 g_cybZones.mapName.c_str(), course.descriptor, course.courseNumber);
				rejectedCourses.push_back(course.descriptor);
			}
			continue;
		}
		// Имя курса выводим из его номера, а не выдумываем: GetCyberCourseNumber резолвит бонусы
		// именно по имени вида "Bonus N", и это же число — ключ лидерборда платформы. Разъехаться
		// тут значит писать рекорды не в тот курс.
		char courseName[KZ_MAX_COURSE_NAME_LENGTH];
		if (course.courseNumber <= 0)
		{
			V_snprintf(courseName, sizeof(courseName), "%s", KZ_NO_MAPAPI_COURSE_NAME);
		}
		else
		{
			V_snprintf(courseName, sizeof(courseName), "Bonus %d", course.courseNumber);
		}
		const char *createReason = KZ::mapapi::CreateExternalCourse(course.courseNumber, courseName, course.descriptor);
		if (createReason)
		{
			KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] course_create_failed map=%s descriptor=%s number=%d reason=%s\n", g_cybZones.mapName.c_str(),
						 course.descriptor, course.courseNumber, createReason);
			continue;
		}
		g_cybZones.localCoursesPending = true;
		g_cybZones.createdCourses.push_back(course.descriptor);
		KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] course_created map=%s descriptor=%s name=%s number=%d\n", g_cybZones.mapName.c_str(),
					course.descriptor, courseName, course.courseNumber);
	}

	// ШАГ 3. Дефолтный курс под зоны БЕЗ курса — прежнее поведение первой итерации. Только если
	// такие зоны есть: лишний курс в !courses на карте, где мы поставили один бустер, был бы
	// враньём. Зоны с явным курсом сюда не относятся.
	bool needsDefaultCourse = false;
	for (const KzCyberZone &zone : g_cybZones.zones)
	{
		if ((zone.type == KZ_CYBER_ZONE_START || zone.type == KZ_CYBER_ZONE_END) && !zone.courseDescriptor[0])
		{
			needsDefaultCourse = true;
			break;
		}
	}
	if (needsDefaultCourse && mapParsed)
	{
		EnsurePlatformCourse();
	}

	// ШАГ 4. Приводим флаг disabled курсов к тому, что говорит api. Декларативно и идемпотентно:
	// SetCourseDisabled сам ничего не делает, если курс уже в нужном состоянии.
	// Зоны отключённого курса api не отдаёт вовсе, поэтому сверять каждую зону со списком курсов
	// не нужно — этот класс тихого отказа убран на стороне контракта.
	i32 unknownCourses = 0;
	for (const KzCyberCourse &course : g_cybZones.courses)
	{
		// Курса из набора может не быть на карте (карта обновилась в Workshop). Это состояние
		// набора, а не отказ на каждом применении: считаем и печатаем числом в zones_applied,
		// иначе строка ошибки повторялась бы на каждый round_start и каждую постановку зоны.
		if (!KZ::mapapi::HasCourseDescriptor(course.descriptor))
		{
			unknownCourses++;
			continue;
		}
		KZ::mapapi::SetCourseDisabled(course.descriptor, course.disabled);
	}

	// Дальше — работа с миром: погашение родных зон и спавн своих. В неготовый мир соваться
	// бессмысленно (движковая очистка снесёт энтити), а таблица триггеров в этот момент ещё пуста.
	// Состояние курсов выше от готовности мира не зависит и уже применено.
	// Про шаг 1 в этом окне: погашенных записей здесь физически НЕТ, и это доказуемо, а не
	// «вероятно». worldReady сбрасывается в OnRoundPreStart сразу после triggers.RemoveAll(),
	// поэтому пока он false, таблица триггеров пуста и восстанавливать нечего.
	if (!g_cybZones.worldReady)
	{
		// Энтити в этом окне уже снесены движковой очисткой — чистим только свой учёт.
		DespawnAll();
		KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zones_deferred map=%s count=%d courses=%d reason=world_not_ready caller=%s\n",
					g_cybZones.mapName.c_str(), (i32)g_cybZones.zones.size(), (i32)g_cybZones.courses.size(), reason);
		return;
	}

	// ШАГ 5. Гасим родные зоны тех курсов и типов, которые переопределяем своими. Строго ДО
	// спавна: ключ (курс, тип) не различает наши зоны и родные, поэтому обратный порядок погасил
	// бы свои же. Модификаторы сюда попасть не могут — DisableCourseZones их отбивает, и это
	// намеренно (у них парная бухгалтерия Start/EndTouch, счётчики залипли бы навсегда).
	i32 nativeDisabled = 0;
	// Курсы, у которых набор нумерованных зон неполон: их stage/checkpoint-зоны не спавним и
	// родные не гасим. Отдельный список от rejectedCourses — причина отказа другая.
	std::vector<KzOverrideRefusal> incompleteOverrides;
	for (const KzCyberZone &zone : g_cybZones.zones)
	{
		if (!zone.courseDescriptor[0] || zone.type == KZ_CYBER_ZONE_MODIFIER)
		{
			continue;
		}
		if (IsRejectedCourse(rejectedCourses, zone.courseDescriptor))
		{
			continue; // курс отвергнут выше — не трогаем чужие зоны
		}
		// Неполный набор нумерованных зон — не переопределяем ВООБЩЕ: ни гасим родные, ни ставим
		// свои. Курс остаётся таким, каким его сделал маппер, и продолжает засчитывать финиш.
		// Половинчатое переопределение убило бы курс целиком, а не одну зону.
		if ((zone.type == KZ_CYBER_ZONE_STAGE || zone.type == KZ_CYBER_ZONE_CHECKPOINT) && !HasCompleteNumberedSet(zone.courseDescriptor, zone.type))
		{
			if (!IsRefusedOverride(incompleteOverrides, zone.courseDescriptor, zone.type))
			{
				KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] course_override_refused map=%s course=%s type=%s reason=incomplete_numbered_set\n",
							 g_cybZones.mapName.c_str(), zone.courseDescriptor, ZoneTypeName(zone.type));
				incompleteOverrides.push_back({zone.courseDescriptor, zone.type});
			}
			continue;
		}
		KzTriggerType nativeType = KZTRIGGER_ZONE_START;
		switch (zone.type)
		{
			case KZ_CYBER_ZONE_END:
				nativeType = KZTRIGGER_ZONE_END;
				break;
			case KZ_CYBER_ZONE_STAGE:
				nativeType = KZTRIGGER_ZONE_STAGE;
				break;
			case KZ_CYBER_ZONE_CHECKPOINT:
				nativeType = KZTRIGGER_ZONE_CHECKPOINT;
				break;
			default:
				break;
		}
		const i32 n = KZ::mapapi::DisableCourseZones(zone.courseDescriptor, nativeType);
		if (n > 0)
		{
			nativeDisabled += n;
		}
	}

	// ШАГ 6. Diff: оставляем на месте зоны, чья запись не изменилась и чья энтити жива. Снос и
	// переспавн неизменившейся зоны доигрывает EndTouch/StartTouch каждому, кто в ней стоит, —
	// у игрока стираются чекпоинты и дёргается таймер, а он ничего не делал.
	// Сопоставление строго один-к-одному (флаг matched), иначе одинаковые записи в наборе
	// «съели» бы друг друга и часть зон осталась бы неспавненной.
	std::vector<bool> matched(g_cybZones.zones.size(), false);
	std::vector<KzSpawnedZone> kept;
	CUtlVector<CPlayerSlot> disturbed;
	CUtlVector<CPlayerSlot> disturbedPrac;
	for (const KzSpawnedZone &spawned : g_cybZones.spawned)
	{
		CEntityInstance *inst = GameEntitySystem() ? GameEntitySystem()->GetEntityInstance(spawned.handle) : nullptr;
		const bool alive = inst && inst->m_pEntity && inst->m_pEntity->NameMatches(KZ_CYBER_ZONE_NAME);
		i32 match = -1;
		if (alive)
		{
			for (size_t i = 0; i < g_cybZones.zones.size(); i++)
			{
				if (!matched[i] && ZoneRecordEqual(spawned.zone, g_cybZones.zones[i]))
				{
					match = (i32)i;
					break;
				}
			}
		}
		if (match >= 0)
		{
			matched[match] = true;
			kept.push_back(spawned);
			continue;
		}
		if (alive)
		{
			// Живую старт-зону сносим — значит кому-то сейчас сотрёт чекпоинты. Собираем ИМЕННО
			// таких: на round_start предыдущие энтити уже уничтожены движковой очисткой, они не
			// alive, и это условие само исключает шумное предупреждение при смене раунда.
			if (spawned.zone.type == KZ_CYBER_ZONE_START)
			{
				CollectTouchingPlayers(spawned.handle, disturbed, disturbedPrac);
			}
			g_pKZUtils->RemoveEntity(inst);
		}
	}
	g_cybZones.spawned = kept;

	KZ::mapapi::BeginExternalTriggerSpawn();
	i32 ok = (i32)kept.size();
	i32 respawned = 0;
	for (size_t zoneIndex = 0; zoneIndex < g_cybZones.zones.size(); zoneIndex++)
	{
		const KzCyberZone &zone = g_cybZones.zones[zoneIndex];
		if (matched[zoneIndex])
		{
			// Уже стоит и не изменилась. Для редактора это успех: зона в мире и работает.
			if (focusZoneId && outFocusReason && KZ_STREQ(zone.id, focusZoneId))
			{
				*outFocusReason = nullptr;
			}
			continue;
		}
		const char *zoneReason = nullptr;
		const bool numbered = zone.type == KZ_CYBER_ZONE_STAGE || zone.type == KZ_CYBER_ZONE_CHECKPOINT;
		if (IsRejectedCourse(rejectedCourses, zone.courseDescriptor))
		{
			zoneReason = "own_course_is_native";
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_spawn_skipped id=%s type=%s course=%s reason=own_course_is_native\n", zone.id,
						ZoneTypeName(zone.type), zone.courseDescriptor);
		}
		else if (numbered && IsRefusedOverride(incompleteOverrides, zone.courseDescriptor, zone.type))
		{
			// Родные зоны этого курса мы не гасили — курс остаётся мапперским и рабочим.
			zoneReason = "incomplete_numbered_set";
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_spawn_skipped id=%s type=%s course=%s reason=incomplete_numbered_set\n", zone.id,
						ZoneTypeName(zone.type), zone.courseDescriptor);
		}
		else if (SpawnZone(zone, &zoneReason))
		{
			ok++;
			respawned++;
		}
		if (focusZoneId && outFocusReason && KZ_STREQ(zone.id, focusZoneId))
		{
			*outFocusReason = zoneReason;
		}
	}
	KZ::mapapi::EndExternalTriggerSpawn();
	// course= объясняет пропуск start/end, не заставляя лезть в соседние строки:
	//   own      — курс завели мы;
	//   fallback — дефолтный курс форка на карте без Mapping API;
	//   foreign  — дескриптор с таким именем есть, но он МАППЕРСКИЙ (карта заняла имя "Default");
	//   none     — дескриптора нет вовсе.
	// Различать foreign и none обязательно: в обоих случаях start/end отбиты, но причины разные,
	// и «карта заняла наше имя» иначе выглядела бы как загадка.
	const char *courseSource = WeCreatedCourse(KZ_NO_MAPAPI_COURSE_DESCRIPTOR)                     ? "own"
							   : KZ::mapapi::IsPlatformOwnedCourse(KZ_NO_MAPAPI_COURSE_DESCRIPTOR) ? "fallback"
							   : KZ::mapapi::HasCourseDescriptor(KZ_NO_MAPAPI_COURSE_DESCRIPTOR)   ? "foreign"
																								   : "none";
	KZ_LOG_INFO(LogChannel::MappingAPI,
				"[cyb] zones_applied map=%s count=%d of=%d kept=%d respawned=%d revision=%d course=%s courses=%d courses_unknown=%d "
				"native_disabled=%d restored=%d reason=%s\n",
				g_cybZones.mapName.c_str(), ok, (i32)g_cybZones.zones.size(), (i32)kept.size(), respawned, g_cybZones.revision, courseSource,
				(i32)g_cybZones.courses.size(), unknownCourses, nativeDisabled, restored, reason);

	// Предупреждаем ТЕХ, КОГО ЗАДЕЛО, и только их. Молчаливый сброс чекпоинтов выглядит как
	// «сервер съел прогресс» и диагностируется потом часами.
	FOR_EACH_VEC(disturbed, i)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(disturbed[i]);
		if (player && player->languageService)
		{
			player->languageService->PrintChat(true, false, "Zones Reapplied - Checkpoints Reset");
		}
	}
	FOR_EACH_VEC(disturbedPrac, i)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(disturbedPrac[i]);
		if (player && player->languageService)
		{
			player->languageService->PrintChat(true, false, "Zones Reapplied - Prac Attempt Reset");
		}
	}
	if (disturbed.Count() > 0 || disturbedPrac.Count() > 0)
	{
		KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zones_reapply_disturbed map=%s players=%d prac=%d reason=%s\n", g_cybZones.mapName.c_str(),
					disturbed.Count(), disturbedPrac.Count(), reason);
	}

	SyncCoursesAfterApply();
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
	g_cybZones.courses.clear();
	g_cybZones.revision = 0;
	g_cybZones.loaded = false;
	// Дескрипторы курсов живут ровно одну карту: Hook_StartupServer зовёт KZ::mapapi::Init(),
	// а тот обнуляет весь courseDescriptors. Значит и наша память о заведённом курсе обнуляется.
	g_cybZones.localCoursesPending = false;
	g_cybZones.createdCourses.clear();
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
	for (const KzSpawnedZone &spawned : g_cybZones.spawned)
	{
		g_cybZones.preRoundSpawned.AddToTail(spawned.handle);
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

const char *KZ::zones::AddAndSpawn(const KzCyberZone &zone, i32 apiRevision)
{
	if ((i32)g_cybZones.zones.size() >= KZ_ZONES_PER_MAP_LIMIT)
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_add_rejected map=%s reason=zone_limit\n", g_cybZones.mapName.c_str());
		return "zone_limit";
	}
	g_cybZones.zones.push_back(zone);
	// revision — версия набора у api; свой счётчик только фолбэк, иначе !zone list врёт.
	g_cybZones.revision = apiRevision > 0 ? apiRevision : g_cybZones.revision + 1;

	// Поставленная зона обязана ожить без рестарта раунда: рестарт срубил бы раны всем на сервере.
	// Окно регистрации открывает ApplyLoadedZones вокруг всего набора.
	// В неготовый мир не спавним: движковая очистка всё равно снесёт энтити, а игрок увидел бы
	// «зона поставлена» и не почувствовал её. Такую зону поставит round_start.
	// Только через общий путь. Отдельного быстрого пути больше нет: применение набора это пять
	// упорядоченных шагов (восстановить погашенное, завести свои курсы, дефолтный курс, привести
	// disabled, погасить родные — и лишь затем спавн), и второй путь неизбежно с ними разъедется.
	// Ровно так уже была получена зона, которая «поставлена», но не работает.
	// Переспавн всего набора здесь дёшев (потолок 64 зоны) и уже штатно происходит при удалении.
	// Причину неактивности определяем ДО применения по тем же ранним выходам, что есть у
	// ApplyLoadedZones: иначе на его ранней ветке reason остался бы nullptr, а редактор сказал бы
	// автору «поставлена и уже действует», не заспавнив ничего.
	const char *reason = nullptr;
	if (!g_cybZones.loaded)
	{
		reason = "set_not_loaded";
	}
	else if (!g_cybZones.worldReady)
	{
		reason = "world_not_ready";
	}
	ApplyLoadedZones("zone_added", g_cybZones.zones.back().id, reason ? nullptr : &reason);

	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zone_added steam_id=%llu map=%s id=%s type=%s spawned=%d reason=%s revision=%d\n",
				g_cybZones.zones.back().createdBy, g_cybZones.mapName.c_str(), g_cybZones.zones.back().id, ZoneTypeName(g_cybZones.zones.back().type),
				reason ? 0 : 1, reason ? reason : "ok", g_cybZones.revision);
	return reason;
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
