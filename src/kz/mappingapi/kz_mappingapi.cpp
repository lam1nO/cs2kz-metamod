/*
	Keeps track of course descriptors along with various types of triggers, applying effects to player when necessary.
*/

#include "kz/kz.h"
#include "kz/mode/kz_mode.h"
#include "kz/trigger/kz_trigger.h"
#include "kz/global/kz_global.h"
#include "movement/movement.h"
#include "kz_mappingapi.h"
#include "entity2/entitykeyvalues.h"
#include "entity2/entitysystem.h" // GameEntitySystem() — проверка живости триггера в пересчёте
#include "sdk/entity/cbasetrigger.h"
#include "utils/ctimer.h"
#include "kz/db/kz_db.h"
// Полный тип таймера: разыменовываем player->timerService. Приезжал транзитивно (kz_db.h,
// kz_prac.h) — явный инклюд, чтобы правка чужих инклюдов не ломала этот файл.
#include "kz/timer/kz_timer.h"
#include "kz/language/kz_language.h"
#include "kz/prac/kz_prac.h"
#include "utils/simplecmds.h"
#include "utils/tables.h"
#include "UtlSortVector.h"
// `!tier` ходит за тиром в наш api: HTTP-клиент, базовый URL из опций, общий маппинг
// режима/валидация имени карты и разбор JSON-ответа.
#include "utils/http.h"
#include "kz/option/kz_option.h"
#include "kz/replays/cyb_replay_common.h"
#include "tier1/keyvalues3.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

// isdigit/atoi в GetCyberCourseNumber и командах бонусов. Приезжали транзитивно — было до нас,
// но правило одно для всего файла.
#include <cctype>
#include <cstdlib>
// std::string/std::optional — тело и разбор ответа api в `!tier`.
#include <optional>
#include <string>

#include "tier0/memdbgon.h"

// Меню-движок cs2menus (определён в cs2kz.cpp); может быть nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

#define KEY_TRIGGER_TYPE         "timer_trigger_type"
#define KEY_IS_COURSE_DESCRIPTOR "timer_course_descriptor"

using namespace KZ::course;

class CourseLessFunc
{
public:
	bool Less(const KZCourseDescriptor *src1, const KZCourseDescriptor *src2, void *pCtx)
	{
		return src1->id < src2->id;
	}
};

enum
{
	MAPI_ERR_TOO_MANY_TRIGGERS = 1 << 0,
	MAPI_ERR_TOO_MANY_COURSES = 1 << 1,
};

static_global struct
{
	CUtlVectorFixed<KZCourseDescriptor, KZ_MAX_COURSE_COUNT> courseDescriptors;
	i32 mapApiVersion;
	bool apiVersionLoaded;
	bool fatalFailure;

	CUtlVectorFixed<KzTrigger, 2048> triggers;
	bool roundIsStarting;
	// Зоны платформы ставятся и вне окна раунда — см. KZ::mapapi::BeginExternalTriggerSpawn.
	bool externalSpawnAllowed;
	i32 errorFlags;
	i32 errorCount;
	char errors[32][256];

	bool hasJumpstatArea;
	Vector jumpstatAreaPos;
	QAngle jumpstatAreaAngles;
} g_mappingApi;

static_global CTimer<> *g_errorTimer;
static_global const char *g_errorPrefix = "{darkred} ERROR: ";
static_global const char *g_triggerNames[] = {"Disabled",   "Modifier",   "Reset Checkpoints", "Single Bhop Reset", "Antibhop",
											  "Start zone", "End zone",   "Split zone",        "Checkpoint zone",   "Stage zone",
											  "Teleport",   "Multi bhop", "Single bhop",       "Sequential bhop"};

static_function MappingInterface g_mappingInterface;

MappingInterface *g_pMappingApi = &g_mappingInterface;
static_global CUtlSortVector<KZCourseDescriptor *, CourseLessFunc> g_sortedCourses(KZ_MAX_COURSE_COUNT, KZ_MAX_COURSE_COUNT);

// TODO: add error check to make sure a course has at least 1 start zone and 1 end zone

static_function void Mapi_Error(const char *format, ...)
{
	i32 errorIndex = g_mappingApi.errorCount;
	if (errorIndex >= KZ_ARRAYSIZE(g_mappingApi.errors))
	{
		return;
	}
	else if (errorIndex == KZ_ARRAYSIZE(g_mappingApi.errors) - 1)
	{
		snprintf(g_mappingApi.errors[errorIndex], sizeof(g_mappingApi.errors[errorIndex]), "Too many errors to list!");
		return;
	}

	va_list args;
	va_start(args, format);
	vsnprintf(g_mappingApi.errors[errorIndex], sizeof(g_mappingApi.errors[errorIndex]), format, args);
	va_end(args);

	g_mappingApi.errorCount++;
}

static_function f64 Mapi_PrintErrors()
{
	if (g_mappingApi.errorFlags & MAPI_ERR_TOO_MANY_TRIGGERS)
	{
		utils::CPrintChatAll("%sToo many Mapping API triggers! Maximum is %i!", g_errorPrefix, g_mappingApi.triggers.Count());
	}
	if (g_mappingApi.errorFlags & MAPI_ERR_TOO_MANY_COURSES)
	{
		utils::CPrintChatAll("%sToo many Courses! Maximum is %i!", g_errorPrefix, g_mappingApi.courseDescriptors.Count());
	}
	for (i32 i = 0; i < g_mappingApi.errorCount; i++)
	{
		utils::CPrintChatAll("%s%s", g_errorPrefix, g_mappingApi.errors[i]);
	}

	return 60.0;
}

static_function bool Mapi_CreateCourse(i32 courseNumber = 1, const char *courseName = KZ_NO_MAPAPI_COURSE_NAME, i32 hammerId = -1,
									   const char *targetName = KZ_NO_MAPAPI_COURSE_DESCRIPTOR, bool disableCheckpoints = false)
{
	// Make sure we don't exceed this ridiculous value.
	// If we do, it is most likely that something went wrong, or it is caused by the mapper.
	if (g_mappingApi.courseDescriptors.Count() >= KZ_MAX_COURSE_COUNT)
	{
		assert(0);
		Mapi_Error("Failed to register course name '%s' (hammerId %i): Too many courses!", courseName, hammerId);
		return false;
	}

	auto &currentCourses = g_mappingApi.courseDescriptors;
	FOR_EACH_VEC(currentCourses, i)
	{
		if (currentCourses[i].hammerId == hammerId)
		{
			// This should only happen during start/end zone backwards compat where hammer IDs are KZ_NO_MAPAPI_VERSION, so this is not an error.
			return false;
		}
		if (KZ_STREQI(targetName, currentCourses[i].entityTargetname))
		{
			Mapi_Error("Course descriptor '%s' already existed! (registered by Hammer ID %i)", targetName, currentCourses[i].hammerId);
			return false;
		}
	}

	i32 index = g_mappingApi.courseDescriptors.AddToTail(
		{hammerId, targetName, disableCheckpoints, (u32)g_mappingApi.courseDescriptors.Count() + 1, courseNumber, courseName});
	g_sortedCourses.Insert(&g_mappingApi.courseDescriptors[index]);
	return true;
}

// Example keyvalues:
/*
	timer_anti_bhop_time: 0.2
	timer_teleport_relative: true
	timer_teleport_reorient_player: false
	timer_teleport_reset_speed: false
	timer_teleport_use_dest_angles: false
	timer_teleport_delay: 0
	timer_teleport_destination: landmark_teleport
	timer_zone_stage_number: 1
	timer_modifier_enable_slide: false
	timer_modifier_disable_jumpstats: false
	timer_modifier_disable_teleports: false
	timer_modifier_disable_checkpoints: false
	timer_modifier_disable_pause: false
	timer_trigger_type: 10
	wait: 1
	spawnflags: 4097
	StartDisabled: false
	useLocalOffset: false
	classname: trigger_multiple
	origin: 1792.000000 768.000000 -416.000000
	angles: 0.000000 0.000000 0.000000
	scales: 1.000000 1.000000 1.000000
	hammerUniqueId: 48
	model: maps\kz_mapping_api\entities\unnamed_48.vmdl
*/
static_function void Mapi_OnTriggerMultipleSpawn(const EntitySpawnInfo_t *info)
{
	const CEntityKeyValues *ekv = info->m_pKeyValues;
	i32 hammerId = ekv->GetInt("hammerUniqueId", -1);
	Vector origin = ekv->GetVector("origin");

	KzTriggerType type = (KzTriggerType)ekv->GetInt(KEY_TRIGGER_TYPE, KZTRIGGER_DISABLED);

	if (!g_mappingApi.roundIsStarting && !g_mappingApi.externalSpawnAllowed)
	{
		// Only allow triggers and zones that were spawned during the round start phase.
		return;
	}

	if (type < KZTRIGGER_DISABLED || type >= KZTRIGGER_COUNT)
	{
		assert(0);
		Mapi_Error("Trigger type %i is invalid and out of range (%i-%i) for trigger with Hammer ID %i, origin (%.0f %.0f %.0f)!", type,
				   KZTRIGGER_DISABLED, KZTRIGGER_COUNT - 1, hammerId, origin.x, origin.y, origin.z);
		return;
	}

	KzTrigger trigger = {};
	trigger.type = type;
	trigger.hammerId = hammerId;
	trigger.entity = info->m_pEntity->GetRefEHandle();
	// Единственный момент, когда наши зоны отличимы от родных: окно externalSpawnAllowed
	// открывается ровно вокруг нашего DispatchSpawn. Позже этот признак не восстановить.
	trigger.platformSpawned = g_mappingApi.externalSpawnAllowed;

	switch (type)
	{
		case KZTRIGGER_MODIFIER:
		{
			trigger.modifier.disablePausing = ekv->GetBool("timer_modifier_disable_pause");
			trigger.modifier.disableCheckpoints = ekv->GetBool("timer_modifier_disable_checkpoints");
			trigger.modifier.disableTeleports = ekv->GetBool("timer_modifier_disable_teleports");
			trigger.modifier.disableJumpstats = ekv->GetBool("timer_modifier_disable_jumpstats");
			trigger.modifier.enableSlide = ekv->GetBool("timer_modifier_enable_slide");
			trigger.modifier.gravity = ekv->GetFloat("timer_modifier_gravity", 1);
			trigger.modifier.jumpFactor = ekv->GetFloat("timer_modifier_jump_impulse", 1.0f);
			trigger.modifier.forceDuck = ekv->GetBool("timer_modifier_force_duck");
			trigger.modifier.forceUnduck = ekv->GetBool("timer_modifier_force_unduck");
		}
		break;

		// NOTE: Nothing to do here
		case KZTRIGGER_RESET_CHECKPOINTS:
		case KZTRIGGER_SINGLE_BHOP_RESET:
			break;

		case KZTRIGGER_ANTI_BHOP:
		{
			trigger.antibhop.time = ekv->GetFloat("timer_anti_bhop_time");
			trigger.antibhop.time = MAX(trigger.antibhop.time, 0);
		}
		break;

		case KZTRIGGER_ZONE_START:
		case KZTRIGGER_ZONE_END:
		case KZTRIGGER_ZONE_SPLIT:
		case KZTRIGGER_ZONE_CHECKPOINT:
		case KZTRIGGER_ZONE_STAGE:
		{
			const char *courseDescriptor = ekv->GetString("timer_zone_course_descriptor");

			if (!courseDescriptor || !courseDescriptor[0])
			{
				Mapi_Error("Course descriptor targetname of %s trigger is empty! Hammer ID %i, origin (%.0f %.0f %.0f)", g_triggerNames[type],
						   hammerId, origin.x, origin.y, origin.z);
				assert(0);
				return;
			}

			snprintf(trigger.zone.courseDescriptor, sizeof(trigger.zone.courseDescriptor), "%s", courseDescriptor);
			// TODO: code is a little repetitive...
			if (type == KZTRIGGER_ZONE_SPLIT)
			{
				trigger.zone.number = ekv->GetInt("timer_zone_split_number", INVALID_SPLIT_NUMBER);
				if (trigger.zone.number <= INVALID_SPLIT_NUMBER)
				{
					Mapi_Error("Split zone number \"%i\" is invalid! Hammer ID %i, origin (%.0f %.0f %.0f)", trigger.zone.number, hammerId, origin.x,
							   origin.y, origin.z);
					assert(0);
					return;
				}
			}
			else if (type == KZTRIGGER_ZONE_CHECKPOINT)
			{
				trigger.zone.number = ekv->GetInt("timer_zone_checkpoint_number", INVALID_CHECKPOINT_NUMBER);

				if (trigger.zone.number <= INVALID_CHECKPOINT_NUMBER)
				{
					Mapi_Error("Checkpoint zone number \"%i\" is invalid! Hammer ID %i, origin (%.0f %.0f %.0f)", trigger.zone.number, hammerId,
							   origin.x, origin.y, origin.z);
					assert(0);
					return;
				}
			}
			else if (type == KZTRIGGER_ZONE_STAGE)
			{
				trigger.zone.number = ekv->GetInt("timer_zone_stage_number", INVALID_STAGE_NUMBER);

				if (trigger.zone.number <= INVALID_STAGE_NUMBER)
				{
					Mapi_Error("Stage zone number \"%i\" is invalid! Hammer ID %i, origin (%.0f %.0f %.0f)", trigger.zone.number, hammerId, origin.x,
							   origin.y, origin.z);
					assert(0);
					return;
				}
			}
			else // Start/End zones
			{
				// Note: Triggers shouldn't be rotated most of the time anyway. If that ever happens for timer triggers, it's probably unintentional.
				QAngle angles = ekv->GetQAngle("angles");

				if (angles != vec3_angle)
				{
					Mapi_Error(
						"Warning: Unexpected rotation for timer trigger, some functionalities might not work properly! Hammer ID %i, origin (%.0f "
						"%.0f %.0f)",
						hammerId, origin.x, origin.y, origin.z);
				}
			}
		}
		break;

		case KZTRIGGER_TELEPORT:
		case KZTRIGGER_MULTI_BHOP:
		case KZTRIGGER_SINGLE_BHOP:
		case KZTRIGGER_SEQUENTIAL_BHOP:
		{
			const char *destination = ekv->GetString("timer_teleport_destination");
			V_snprintf(trigger.teleport.destination, sizeof(trigger.teleport.destination), "%s", destination);
			trigger.teleport.delay = ekv->GetFloat("timer_teleport_delay", 0);
			trigger.teleport.delay = MAX(trigger.teleport.delay, 0);
			if (KZ::mapapi::IsBhopTrigger(type))
			{
				trigger.teleport.delay = MAX(trigger.teleport.delay, 0.1);
			}
			trigger.teleport.useDestinationAngles = ekv->GetBool("timer_teleport_use_dest_angles");
			trigger.teleport.resetSpeed = ekv->GetBool("timer_teleport_reset_speed");
			trigger.teleport.reorientPlayer = ekv->GetBool("timer_teleport_reorient_player");
			trigger.teleport.relative = ekv->GetBool("timer_teleport_relative");
		}
		break;
		case KZTRIGGER_PUSH:
		{
			Vector impulse = ekv->GetVector("timer_push_amount");
			trigger.push.impulse[0] = impulse.x;
			trigger.push.impulse[1] = impulse.y;
			trigger.push.impulse[2] = impulse.z;
			trigger.push.setSpeed[0] = ekv->GetBool("timer_push_abs_speed_x");
			trigger.push.setSpeed[1] = ekv->GetBool("timer_push_abs_speed_y");
			trigger.push.setSpeed[2] = ekv->GetBool("timer_push_abs_speed_z");
			trigger.push.cancelOnTeleport = ekv->GetBool("timer_push_cancel_on_teleport");
			trigger.push.cooldown = ekv->GetFloat("timer_push_cooldown", 0.1f);
			trigger.push.delay = ekv->GetFloat("timer_push_delay", 0.0f);

			trigger.push.pushConditions |= ekv->GetBool("timer_push_condition_start_touch") ? KzMapPush::KZ_PUSH_START_TOUCH : 0;
			trigger.push.pushConditions |= ekv->GetBool("timer_push_condition_touch") ? KzMapPush::KZ_PUSH_TOUCH : 0;
			trigger.push.pushConditions |= ekv->GetBool("timer_push_condition_end_touch") ? KzMapPush::KZ_PUSH_END_TOUCH : 0;
			trigger.push.pushConditions |= ekv->GetBool("timer_push_condition_jump_event") ? KzMapPush::KZ_PUSH_JUMP_EVENT : 0;
			trigger.push.pushConditions |= ekv->GetBool("timer_push_condition_jump_button") ? KzMapPush::KZ_PUSH_JUMP_BUTTON : 0;
			trigger.push.pushConditions |= ekv->GetBool("timer_push_condition_attack") ? KzMapPush::KZ_PUSH_ATTACK : 0;
			trigger.push.pushConditions |= ekv->GetBool("timer_push_condition_attack2") ? KzMapPush::KZ_PUSH_ATTACK2 : 0;
			trigger.push.pushConditions |= ekv->GetBool("timer_push_condition_use") ? KzMapPush::KZ_PUSH_USE : 0;
		}
		break;
		case KZTRIGGER_DISABLED:
		{
			// Check for pre-mapping api triggers for backwards compatibility.
			if (g_mappingApi.mapApiVersion == KZ_NO_MAPAPI_VERSION)
			{
				if (info->m_pEntity->NameMatches("timer_startzone") || info->m_pEntity->NameMatches("timer_endzone"))
				{
					snprintf(trigger.zone.courseDescriptor, sizeof(trigger.zone.courseDescriptor), KZ_NO_MAPAPI_COURSE_DESCRIPTOR);
					trigger.type = info->m_pEntity->NameMatches("timer_startzone") ? KZTRIGGER_ZONE_START : KZTRIGGER_ZONE_END;
				}
			}
			break;
			// Otherwise these are just regular trigger_multiple.
		}
		default:
		{
			// technically impossible to happen, leave an assert here anyway for debug builds.
			assert(0);
			return;
		}
		break;
	}

	g_mappingApi.triggers.AddToTail(trigger);
}

static_function void Mapi_OnInfoTargetSpawn(const CEntityKeyValues *ekv)
{
	if (!ekv->GetBool(KEY_IS_COURSE_DESCRIPTOR))
	{
		return;
	}

	i32 hammerId = ekv->GetInt("hammerUniqueId", -1);
	Vector origin = ekv->GetVector("origin");

	i32 courseNumber = ekv->GetInt("timer_course_number", INVALID_COURSE_NUMBER);
	const char *courseName = ekv->GetString("timer_course_name");
	const char *targetName = ekv->GetString("targetname");
	constexpr static_persist const char *targetNamePrefix = "[PR#]";
	if (KZ_STREQLEN(targetName, targetNamePrefix, strlen(targetNamePrefix)))
	{
		targetName = targetName + strlen(targetNamePrefix);
	}

	if (courseNumber <= INVALID_COURSE_NUMBER)
	{
		Mapi_Error("Course number must be bigger than %i! Course descriptor Hammer ID %i, origin (%.0f %.0f %.0f)", INVALID_COURSE_NUMBER, hammerId,
				   origin.x, origin.y, origin.z);
		return;
	}

	if (!courseName[0])
	{
		Mapi_Error("Course name is empty! Course number %i. Course descriptor Hammer ID %i, origin (%.0f %.0f %.0f)", courseNumber, hammerId,
				   origin.x, origin.y, origin.z);
		return;
	}

	if (!targetName[0])
	{
		Mapi_Error("Course targetname is empty! Course name \"%s\". Course number %i. Course descriptor Hammer ID %i, origin (%.0f %.0f %.0f)",
				   courseName, courseNumber, hammerId, origin.x, origin.y, origin.z);
		return;
	}

	Mapi_CreateCourse(courseNumber, courseName, hammerId, targetName, ekv->GetBool("timer_course_disable_checkpoint"));
}

// Перестроить g_sortedCourses по актуальному courseDescriptors.
//
// Зачем: g_sortedCourses хранит УКАЗАТЕЛИ внутрь courseDescriptors, а валидация на round_start
// удаляет курс через FastRemove. courseDescriptors — CUtlVectorFixed, память инлайновая и
// никуда не переезжает, поэтому «висячего указателя» в смысле обращения к освобождённой памяти
// тут нет. Ломается другое: FastRemove (utlvector.h) делает memcpy последнего элемента в дырку
// и уменьшает счётчик, ничего не освобождая, так что
//   - указатель на освободившийся слот начинает читать данные бывшего ПОСЛЕДНЕГО курса (дубль;
//     при удалении САМОГО последнего memcpy пропускается, и тогда дубля нет — только фантом),
//   - указатель на бывший последний слот читает его же копию уже за Count() (фантом),
//   - и рушится инвариант сортировки CUtlSortVector, по которому Insert ищет позицию бинарным
//     поиском (FindLessOrEqual), — следующая вставка садится не туда.
// Наружу это вылезло бы дублем курса в !courses/лидерборде и лукапами KZ::course::GetCourse*,
// возвращающими фантом.
static_function void Mapi_RebuildSortedCourses()
{
	g_sortedCourses.RemoveAll();
	FOR_EACH_VEC(g_mappingApi.courseDescriptors, i)
	{
		// Отключённый платформой курс в витрину не попадает — этим он и «удаляется» для игрока:
		// весь остальной код (!courses, !main, !b*, HUD, SetupLocalCourses, лукапы KZ::course::*)
		// читает именно g_sortedCourses.
		if (g_mappingApi.courseDescriptors[i].disabled)
		{
			continue;
		}
		g_sortedCourses.Insert(&g_mappingApi.courseDescriptors[i]);
	}
}

// Курс с наименьшим mapper-id по ПОЛНОМУ списку, включая отключённые платформой. Именно он
// считается «главным» (cyber-номер 0), и это обязано не зависеть от того, что кто-то отключил
// курс: cyber-номер — ключ лидерборда, кэшей PB/WR и сейва рана.
static_function const KZCourseDescriptor *Mapi_FirstCourseIncludingDisabled()
{
	const KZCourseDescriptor *first = nullptr;
	FOR_EACH_VEC(g_mappingApi.courseDescriptors, i)
	{
		const KZCourseDescriptor *course = &g_mappingApi.courseDescriptors[i];
		if (!first || course->id < first->id)
		{
			first = course;
		}
	}
	return first;
}

static_function KzTrigger *Mapi_FindKzTrigger(CBaseTrigger *trigger)
{
	if (!trigger->m_pEntity)
	{
		return nullptr;
	}

	CEntityHandle triggerHandle = trigger->GetRefEHandle();
	if (!trigger || !triggerHandle.IsValid() || trigger->m_pEntity->m_flags & EF_IS_INVALID_EHANDLE)
	{
		return nullptr;
	}

	FOR_EACH_VEC(g_mappingApi.triggers, i)
	{
		if (triggerHandle == g_mappingApi.triggers[i].entity)
		{
			return &g_mappingApi.triggers[i];
		}
	}

	return nullptr;
}

static_function KZCourseDescriptor *Mapi_FindCourse(const char *targetname)
{
	KZCourseDescriptor *result = nullptr;
	if (!targetname)
	{
		return result;
	}

	FOR_EACH_VEC(g_mappingApi.courseDescriptors, i)
	{
		if (KZ_STREQI(g_mappingApi.courseDescriptors[i].entityTargetname, targetname))
		{
			result = &g_mappingApi.courseDescriptors[i];
			break;
		}
	}

	return result;
}

static_function bool Mapi_SetStartPosition(const char *descriptorName, Vector origin, QAngle angles)
{
	KZCourseDescriptor *desc = Mapi_FindCourse(descriptorName);

	if (!desc)
	{
		return false;
	}
	desc->SetStartPosition(origin, angles);
	return true;
}

static_function void Mapi_OnInfoTeleportDestinationSpawn(const EntitySpawnInfo_t *info)
{
	const CEntityKeyValues *ekv = info->m_pKeyValues;
	const char *targetname = ekv->GetString("targetname");
	if (KZ_STREQI(targetname, "timer_start"))
	{
		if (g_mappingApi.mapApiVersion == KZ_NO_MAPAPI_VERSION)
		{
			Mapi_SetStartPosition(KZ_NO_MAPAPI_COURSE_DESCRIPTOR, ekv->GetVector("origin"), ekv->GetQAngle("angles"));
		}
		else if (g_mappingApi.mapApiVersion == KZ_MAPAPI_VERSION) // TODO do better check
		{
			const char *courseDescriptor = ekv->GetString("timer_zone_course_descriptor");
			i32 hammerId = ekv->GetInt("hammerUniqueId", -1);
			Vector origin = ekv->GetVector("origin");
			if (!courseDescriptor || !courseDescriptor[0])
			{
				Mapi_Error("Course descriptor targetname of timer_start info_teleport_destination is empty! Hammer ID %i, origin (%.0f %.0f %.0f)",
						   hammerId, origin.x, origin.y, origin.z);
				assert(0);
				return;
			}
			Mapi_SetStartPosition(courseDescriptor, origin, ekv->GetQAngle("angles"));
		}
	}
	else if (KZ_STREQI(targetname, "timer_jumpstat_area"))
	{
		g_mappingApi.hasJumpstatArea = true;
		g_mappingApi.jumpstatAreaPos = ekv->GetVector("origin");
		g_mappingApi.jumpstatAreaAngles = ekv->GetQAngle("angles");
	}
};

void KZ::mapapi::Init()
{
	g_mappingApi = {};

	g_errorTimer = g_errorTimer ? g_errorTimer : StartTimer(Mapi_PrintErrors, true);
}

void KZ::mapapi::OnCreateLoadingSpawnGroupHook(const CUtlVector<const CEntityKeyValues *> *pKeyValues)
{
	if (!pKeyValues)
	{
		return;
	}

	if (g_mappingApi.apiVersionLoaded)
	{
		return;
	}

	for (i32 i = 0; i < pKeyValues->Count(); i++)
	{
		auto ekv = (*pKeyValues)[i];

		if (!ekv)
		{
			continue;
		}
		const char *classname = ekv->GetString("classname");
		if (KZ_STREQI(classname, "worldspawn"))
		{
			// We only care about the first spawn group's worldspawn because the rest might use prefabs compiled outside of mapping API.
			g_mappingApi.apiVersionLoaded = true;
			g_mappingApi.mapApiVersion = ekv->GetInt("timer_mapping_api_version", KZ_NO_MAPAPI_VERSION);
			// NOTE(GameChaos): When a new mapping api version comes out, this will change
			//  for backwards compatibility.
			if (g_mappingApi.mapApiVersion == KZ_NO_MAPAPI_VERSION)
			{
				KZ_LOG_INFO(LogChannel::MappingAPI, "Warning: Map is not compiled with Mapping API. Reverting to default behavior.\n");

				// Manually create a KZ_NO_MAPAPI_COURSE_NAME course here because there shouldn't be any info_target_server_only around.
				Mapi_CreateCourse();
				break;
			}
			if (g_mappingApi.mapApiVersion != KZ_MAPAPI_VERSION)
			{
				Mapi_Error("FATAL. Mapping API version %i is invalid!", g_mappingApi.mapApiVersion);
				g_mappingApi.fatalFailure = true;
				return;
			}
			break;
		}
	}
	// Do a second pass for course descriptors.
	if (g_mappingApi.mapApiVersion != KZ_NO_MAPAPI_VERSION)
	{
		for (i32 i = 0; i < pKeyValues->Count(); i++)
		{
			auto ekv = (*pKeyValues)[i];

			if (!ekv)
			{
				continue;
			}
			const char *classname = ekv->GetString("classname");
			if (KZ_STREQI(classname, "info_target_server_only"))
			{
				Mapi_OnInfoTargetSpawn(ekv);
			}
		}
	}
}

void KZ::mapapi::OnSpawn(int count, const EntitySpawnInfo_t *info)
{
	if (!info || g_mappingApi.fatalFailure)
	{
		return;
	}

	for (i32 i = 0; i < count; i++)
	{
		auto ekv = info[i].m_pKeyValues;
#if 0
		// Debug print for all keyvalues
		FOR_EACH_ENTITYKEY(ekv, iter)
		{
			auto kv = ekv->GetKeyValue(iter);
			if (!kv)
			{
				continue;
			}
			CBufferStringGrowable<128> bufferStr;
			const char *key = ekv->GetEntityKeyId(iter).GetString();
			const char *value = kv->ToString(bufferStr);
			Msg("\t%s: %s\n", key, value);
		}
#endif

		if (!info[i].m_pEntity || !ekv || !info[i].m_pEntity->GetClassname())
		{
			continue;
		}
		const char *classname = info[i].m_pEntity->GetClassname();
		if (KZ_STREQI(classname, "trigger_multiple"))
		{
			Mapi_OnTriggerMultipleSpawn(&info[i]);
		}
	}

	// We need to pass the second time for the spawn points of courses.

	for (i32 i = 0; i < count; i++)
	{
		auto ekv = info[i].m_pKeyValues;

		if (!info[i].m_pEntity || !ekv || !info[i].m_pEntity->GetClassname())
		{
			continue;
		}
		const char *classname = info[i].m_pEntity->GetClassname();
		if (KZ_STREQI(classname, "info_teleport_destination"))
		{
			Mapi_OnInfoTeleportDestinationSpawn(&info[i]);
		}
	}

	if (g_mappingApi.fatalFailure)
	{
		g_mappingApi.triggers.RemoveAll();
		g_mappingApi.courseDescriptors.RemoveAll();
		// Тот же инвариант, что и после FastRemove в валидации: g_sortedCourses держит указатели
		// внутрь courseDescriptors. Без этой строки на всю карту оставались бы фантомы —
		// GetCourseCount()/!courses/GetCourse* показывали бы курсы, которых уже нет, а
		// Mapi_FindCourse их уже не находит. Сбрасывалось только следующим Hook_StartupServer.
		Mapi_RebuildSortedCourses();
	}
}

void KZ::mapapi::BeginExternalTriggerSpawn()
{
	g_mappingApi.externalSpawnAllowed = true;
}

void KZ::mapapi::EndExternalTriggerSpawn()
{
	g_mappingApi.externalSpawnAllowed = false;
}

i32 KZ::mapapi::RegisteredTriggerCount()
{
	return g_mappingApi.triggers.Count();
}

i32 KZ::mapapi::MaxRegisteredTriggers()
{
	return g_mappingApi.triggers.NumAllocated();
}

void KZ::mapapi::OnRoundPreStart()
{
	g_mappingApi.triggers.RemoveAll();
	g_mappingApi.roundIsStarting = true;
}

// Счётчики зон одного курса по ТЕКУЩЕЙ таблице триггеров.
// Find the number of split/checkpoint/stage zones that a course has
//  and make sure that they all start from 1 and are consecutive by
//  XORing the values with a consecutive 1...n sequence.
//  https://florian.github.io/xor-trick/
struct MapiCourseZoneCounts
{
	i32 splitCount;
	i32 cpCount;
	i32 stageCount;
	bool splitConsecutive;
	bool cpConsecutive;
	bool stageConsecutive;
};

// liveOnly=true считает только те триггеры, чья энтити ещё жива. Нужно пересчёту после
// применения набора зон платформы: DespawnAll снимает энтити, но записи в g_mappingApi.triggers
// живут до round_prestart, и повторное применение иначе удвоило бы счётчики курса. Валидация на
// round_start зовёт с false — там все триггеры только что зарегистрированы, и поведение
// апстрима остаётся байт-в-байт прежним.
static_function MapiCourseZoneCounts Mapi_CountCourseZones(const KZCourseDescriptor *courseDescriptor, bool liveOnly)
{
	i32 splitXor = 0;
	i32 cpXor = 0;
	i32 stageXor = 0;
	MapiCourseZoneCounts counts {};
	FOR_EACH_VEC(g_mappingApi.triggers, i)
	{
		KzTrigger *trigger = &g_mappingApi.triggers[i];
		if (!KZ::mapapi::IsTimerTrigger(trigger->type))
		{
			continue;
		}

		if (liveOnly && (!GameEntitySystem() || !GameEntitySystem()->GetEntityInstance(trigger->entity)))
		{
			continue;
		}

		if (!KZ_STREQ(trigger->zone.courseDescriptor, courseDescriptor->entityTargetname))
		{
			continue;
		}

		switch (trigger->type)
		{
			case KZTRIGGER_ZONE_SPLIT:
				splitXor ^= (++counts.splitCount) ^ trigger->zone.number;
				break;
			case KZTRIGGER_ZONE_CHECKPOINT:
				cpXor ^= (++counts.cpCount) ^ trigger->zone.number;
				break;
			case KZTRIGGER_ZONE_STAGE:
				stageXor ^= (++counts.stageCount) ^ trigger->zone.number;
				break;
		}
	}
	counts.splitConsecutive = splitXor == 0;
	counts.cpConsecutive = cpXor == 0;
	counts.stageConsecutive = stageXor == 0;
	return counts;
}

void KZ::mapapi::OnRoundStart()
{
	g_mappingApi.roundIsStarting = false;
	bool coursesRemoved = false;
	FOR_EACH_VEC(g_mappingApi.courseDescriptors, courseInd)
	{
		KZCourseDescriptor *courseDescriptor = &g_mappingApi.courseDescriptors[courseInd];
		const MapiCourseZoneCounts counts = Mapi_CountCourseZones(courseDescriptor, false);
		const i32 splitCount = counts.splitCount;
		const i32 cpCount = counts.cpCount;
		const i32 stageCount = counts.stageCount;

		bool invalid = false;
		if (!counts.splitConsecutive)
		{
			Mapi_Error("Course \"%s\" Split zones aren't consecutive or don't start at 1!", courseDescriptor->name);
			invalid = true;
		}

		if (!counts.cpConsecutive)
		{
			Mapi_Error("Course \"%s\" Checkpoint zones aren't consecutive or don't start at 1!", courseDescriptor->name);
			invalid = true;
		}

		if (!counts.stageConsecutive)
		{
			Mapi_Error("Course \"%s\" Stage zones aren't consecutive or don't start at 1!", courseDescriptor->name);
			invalid = true;
		}

		if (splitCount > KZ_MAX_SPLIT_ZONES)
		{
			Mapi_Error("Course \"%s\" Too many split zones! Maximum is %i.", courseDescriptor->name, KZ_MAX_SPLIT_ZONES);
			invalid = true;
		}

		if (cpCount > KZ_MAX_CHECKPOINT_ZONES)
		{
			Mapi_Error("Course \"%s\" Too many checkpoint zones! Maximum is %i.", courseDescriptor->name, KZ_MAX_CHECKPOINT_ZONES);
			invalid = true;
		}

		if (stageCount > KZ_MAX_STAGE_ZONES)
		{
			Mapi_Error("Course \"%s\" Too many stage zones! Maximum is %i.", courseDescriptor->name, KZ_MAX_STAGE_ZONES);
			invalid = true;
		}

		if (invalid)
		{
			// continue, а не break: невалидный курс дропается, остальные обязаны быть
			// провалидированы и получить свои счётчики. courseInd-- возвращает индекс на слот,
			// куда FastRemove переставил бывший последний курс, — его ещё предстоит проверить.
			g_mappingApi.courseDescriptors.FastRemove(courseInd);
			coursesRemoved = true;
			courseInd--;
			continue;
		}
		courseDescriptor->splitCount = splitCount;
		courseDescriptor->checkpointCount = cpCount;
		courseDescriptor->stageCount = stageCount;
	}

	// Обязательно ПОСЛЕ цикла (внутри него g_sortedCourses никто не читает) и обязательно ЗДЕСЬ,
	// а не позже: следом в том же событии идёт KZ::zones::OnRoundStart (hooks.cpp), а он через
	// CreateExternalCourse делает Insert в этот же вектор — бинарным поиском по сломанному
	// порядку. Подробности, что именно ломает FastRemove, — у Mapi_RebuildSortedCourses.
	if (coursesRemoved)
	{
		Mapi_RebuildSortedCourses();
	}
}

void KZ::mapapi::RecountCourseZones()
{
	// Пустая таблица триггеров означает не «у курсов нет зон», а «мир ещё не отдал их нам».
	// Таких окон ДВА, и оба реальны:
	//   1) от загрузки карты до первого round_prestart — дескрипторы курсов уже созданы хуком
	//      spawn-группы, а Mapi_OnTriggerMultipleSpawn ещё выходит по !roundIsStarting;
	//   2) между round_prestart (там triggers.RemoveAll()) и round_start.
	// Пересчёт в любом из них обнулил бы счётчики ВСЕХ курсов карты, включая родные, то есть
	// сорвал бы финиш каждого забега (TimerEnd сверяет currentStage со stageCount). Сегодня сюда
	// так не приходят — единственный вызывающий спавнит зоны только при готовом мире, — но цена
	// ошибки будущего вызывающего слишком велика, чтобы полагаться на порядок вызовов.
	// Обратная сторона гарда узкая: «на карте правда ноль триггеров» означает нулевые счётчики у
	// всех курсов, то есть пересчёт и так был бы пустой операцией.
	if (g_mappingApi.triggers.Count() == 0)
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] course_recount_skipped map=%s reason=trigger_table_empty\n",
					g_pKZUtils->GetCurrentMapName().Get());
		return;
	}

	FOR_EACH_VEC(g_mappingApi.courseDescriptors, courseInd)
	{
		KZCourseDescriptor *course = &g_mappingApi.courseDescriptors[courseInd];
		const MapiCourseZoneCounts counts = Mapi_CountCourseZones(course, true);

		// Молча НЕ трогаем счётчики курса, чью нумерацию сломали: значение, выставленное
		// валидацией на round_start, детерминировано, а половинчатое — нет. Дропать курс здесь
		// нельзя тем более: это путь FastRemove, и вызывают нас вне окна раунда.
		const char *reason = nullptr;
		if (!counts.splitConsecutive)
		{
			reason = "split_numbers_not_consecutive";
		}
		else if (!counts.cpConsecutive)
		{
			reason = "checkpoint_numbers_not_consecutive";
		}
		else if (!counts.stageConsecutive)
		{
			reason = "stage_numbers_not_consecutive";
		}
		else if (counts.splitCount > KZ_MAX_SPLIT_ZONES || counts.cpCount > KZ_MAX_CHECKPOINT_ZONES || counts.stageCount > KZ_MAX_STAGE_ZONES)
		{
			reason = "too_many_zones";
		}
		if (reason)
		{
			// В лог сервера, а НЕ через Mapi_Error: тот копит строки и раз в минуту высыпает их
			// в общий чат всем игрокам, а нас зовут после каждого применения набора зон.
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] course_recount_skipped course=%s reason=%s split=%i cp=%i stage=%i\n", course->name, reason,
						counts.splitCount, counts.cpCount, counts.stageCount);
			continue;
		}

		if (course->splitCount == counts.splitCount && course->checkpointCount == counts.cpCount && course->stageCount == counts.stageCount)
		{
			continue;
		}
		KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] course_recounted course=%s split=%i->%i cp=%i->%i stage=%i->%i\n", course->name,
					course->splitCount, counts.splitCount, course->checkpointCount, counts.cpCount, course->stageCount, counts.stageCount);
		course->splitCount = counts.splitCount;
		course->checkpointCount = counts.cpCount;
		course->stageCount = counts.stageCount;
	}
}

bool KZ::mapapi::IsMapParsed()
{
	return g_mappingApi.apiVersionLoaded;
}

bool KZ::mapapi::HasCourseDescriptor(const char *targetname)
{
	return Mapi_FindCourse(targetname) != nullptr;
}

bool KZ::mapapi::IsPlatformOwnedCourse(const char *descriptorName)
{
	const KZCourseDescriptor *desc = Mapi_FindCourse(descriptorName);
	if (!desc)
	{
		return false;
	}
	// Курс, заведённый нами через CreateExternalCourse. Требуем ОБА признака сразу: по одному
	// только id критерий остался бы эвристикой — маппер вправе дать своему курсу номер 1000 и
	// назвать дескриптор "Default", и мы снова сочли бы чужой курс своим. Совпасть же ещё и
	// hammerId'ом из отрицательного диапазона компилятор карт не может.
	const bool platformId = desc->id >= KZ_PLATFORM_COURSE_ID_BASE && desc->id <= KZ_PLATFORM_COURSE_ID_BASE + KZ_PLATFORM_COURSE_MAX_NUMBER;
	const bool platformHammerId =
		desc->hammerId <= KZ_PLATFORM_COURSE_HAMMER_ID_BASE && desc->hammerId >= KZ_PLATFORM_COURSE_HAMMER_ID_BASE - KZ_PLATFORM_COURSE_MAX_NUMBER;
	if (platformId && platformHammerId)
	{
		return true;
	}
	// Дефолтный курс карты без Mapping API: его заводит сам форк в OnCreateLoadingSpawnGroupHook,
	// и других курсов на такой карте быть не может — второй проход по info_target_server_only
	// там не выполняется вовсе. Значит любой дескриптор на такой карте — форковый.
	return g_mappingApi.mapApiVersion == KZ_NO_MAPAPI_VERSION;
}

const char *KZ::mapapi::GetCanonicalCourseDescriptor(const char *descriptorName)
{
	const KZCourseDescriptor *desc = Mapi_FindCourse(descriptorName);
	return desc ? desc->entityTargetname : nullptr;
}

i32 KZ::mapapi::DisableCourseZones(const char *descriptorName, KzTriggerType type)
{
	// ЖЁСТКИЙ ЗАПРЕТ, не оптимизация: отключать разрешено ТОЛЬКО зоны таймера.
	// Причина — трекер касания (kz/trigger/kz_trigger.h, TriggerTouchTracker::kzTrigger) держит
	// живой указатель на KzTrigger, а OnMappingApiTriggerStartTouchPost и
	// OnMappingApiTriggerEndTouchPost оба ветвятся по ТЕКУЩЕМУ type. Смена типа между входом и
	// выходом рвёт пару: у модификатора StartTouch уже увеличил счётчики (disableJumpstatsCount,
	// enableSlideCount и соседи), а EndTouch по новому типу их не уменьшит — они залипнут
	// НАВСЕГДА, до смены карты. Неотключённый чужой бустер несравнимо дешевле.
	// У зон таймера цена ограничена и приемлема: игрок, стоящий внутри в момент отключения, не
	// получит StartZoneEndTouch, то есть таймер не стартует — чего мы и добиваемся.
	if (!KZ::mapapi::IsTimerTrigger(type))
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] course_zones_disable_refused descriptor=%s type=%i reason=not_a_timer_zone\n",
					 descriptorName ? descriptorName : "(null)", (i32)type);
		return -1;
	}
	if (!descriptorName || !descriptorName[0])
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] course_zones_disable_refused type=%i reason=empty_descriptor\n", (i32)type);
		return -1;
	}
	// Несуществующий дескриптор (опечатка в наборе api) обязан отличаться от «зон такого типа
	// нет»: иначе оба случая дают 0 и молчание.
	if (!Mapi_FindCourse(descriptorName))
	{
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] course_zones_disable_refused descriptor=%s type=%i reason=unknown_course\n", descriptorName,
					 (i32)type);
		return -1;
	}

	i32 disabled = 0;
	FOR_EACH_VEC(g_mappingApi.triggers, i)
	{
		KzTrigger *trigger = &g_mappingApi.triggers[i];
		if (trigger->type != type)
		{
			continue;
		}
		// НАШИ зоны не гасим никогда. Фильтр идёт по паре (курс, тип), а у зоны платформы,
		// переопределяющей родную, курс и тип совпадают с родными НАМЕРЕННО — иначе
		// переопределение не работало бы вовсе. Без этой проверки второе применение набора в
		// пределах раунда гасило бы собственные живые зоны: они остались бы стоять в мире с
		// типом DISABLED, то есть таймер по ним не стартовал бы и не финишировал, а
		// zones_applied рапортовал бы здоровое kept=N.
		if (trigger->platformSpawned)
		{
			continue;
		}
		// Регистр — как в Mapi_FindCourse (KZ_STREQI): двух курсов, различающихся только
		// регистром, на карте быть не может, Mapi_CreateCourse отбивает такие по тому же
		// сравнению. Строгое KZ_STREQ здесь пропустило бы часть зон курса.
		if (!KZ_STREQI(trigger->zone.courseDescriptor, descriptorName))
		{
			continue;
		}
		// Считаем только ЖИВЫЕ зоны: записи переживают DespawnAll и живут до round_prestart, а
		// возвращаемое число уходит в лог как «сколько родных зон погашено». Красим при этом и
		// мёртвые — они безвредны, но врать счётчиком нельзя.
		const bool alive = GameEntitySystem() && GameEntitySystem()->GetEntityInstance(trigger->entity);
		trigger->platformDisabledFrom = trigger->type;
		trigger->platformDisabled = true;
		trigger->type = KZTRIGGER_DISABLED;
		disabled += alive ? 1 : 0;
	}
	return disabled;
}

i32 KZ::mapapi::RestorePlatformDisabledZones()
{
	i32 restored = 0;
	FOR_EACH_VEC(g_mappingApi.triggers, i)
	{
		KzTrigger *trigger = &g_mappingApi.triggers[i];
		if (!trigger->platformDisabled)
		{
			continue;
		}
		trigger->type = trigger->platformDisabledFrom;
		trigger->platformDisabled = false;
		restored++;
	}
	return restored;
}

bool KZ::mapapi::SetCourseDisabled(const char *descriptorName, bool disabled)
{
	KZCourseDescriptor *desc = Mapi_FindCourse(descriptorName);
	if (!desc)
	{
		// Отличается от «уже в этом состоянии»: это ошибка конфигурации платформы (курс из набора
		// api не существует на карте), и молчать о ней нельзя.
		KZ_LOG_ERROR(LogChannel::MappingAPI, "[cyb] course_toggle_refused descriptor=%s disabled=%d reason=unknown_course\n",
					 descriptorName ? descriptorName : "(null)", disabled ? 1 : 0);
		return false;
	}
	if (desc->disabled == disabled)
	{
		return false; // штатный no-op, набор применяется повторно каждый раунд
	}

	// Таймеры останавливаем ДО перестроения витрины и строго в этом порядке: GetCourse() ищет
	// курс по guid в g_sortedCourses, а после перестроения отключённого курса там уже нет —
	// сравнить стало бы не с чем, и ран продолжал бы идти в никуда.
	if (disabled)
	{
		// Граница цикла — как в KZ::zones::ResetEditors: перегрузка ToPlayer(CPlayerSlot) внутри
		// делает index = slot.Get() + 1 по массиву players[MAXPLAYERS + 1], поэтому строго
		// `i < MAXPLAYERS`. Индексная перегрузка ToPlayer(u32) требует другой границы — не
		// переносить сюда цикл из соседнего файла, не посмотрев, какая из них там вызывается.
		for (i32 i = 0; i < MAXPLAYERS; i++)
		{
			KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(i));
			if (!player)
			{
				continue;
			}

			// Живой таймер на этом курсе.
			const bool runningHere = player->timerService && player->timerService->GetTimerRunning() && player->timerService->GetCourse() == desc;

			// ЗАМОРОЖЕННЫЙ ран в prac — отдельный случай, и пропустить его нельзя. В prac
			// timerRunning == false, поэтому проверка выше такого игрока не увидит, а на выходе
			// из prac ExitPrac поднимет таймер снапшотом на курсе, которого в витрине уже нет:
			// финиш через родную энд-зону отдаст course == nullptr, TimerEnd не позовётся вовсе,
			// и ран пойдёт вечно, умерев молча. Ровно тот класс отказа, ради которого здесь
			// вообще стоит явная остановка.
			const bool frozenHere =
				player->pracService && player->pracService->HasActiveFrozenRun() && player->pracService->GetFrozenRun().courseGUID == desc->guid;

			if (!runningHere && !frozenHere)
			{
				continue;
			}

			KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] timer_stopped_course_disabled steam_id=%llu course=%s frozen=%d\n",
						player->GetSteamId64(false), desc->name, frozenHere ? 1 : 0);
			if (frozenHere)
			{
				// nullptr вместо ключа фразы: свою причину печатаем ниже одной строкой, иначе
				// игрок получил бы два разных объяснения одного события.
				player->pracService->DropFrozenRun("course_disabled", nullptr);
			}
			if (runningHere)
			{
				player->timerService->TimerStop(true, "course_disabled");
			}
			// Молчаливый срыв рана неотличим от съеденного времени — говорим причину.
			player->languageService->PrintChat(true, false, "Course Disabled - Run Stopped", desc->name);
		}
	}

	desc->disabled = disabled;
	Mapi_RebuildSortedCourses();
	KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] course_%s descriptor=%s name=%s\n", disabled ? "disabled" : "enabled", desc->entityTargetname,
				desc->name);
	return true;
}

bool KZ::mapapi::GetCourseLocalDatabaseId(const char *descriptorName, u32 &out)
{
	// Пишем out ВСЕГДА, включая ветку отказа: контракт «на false out не трогаем» держался бы на
	// том, что вызывающий сам инициализировал переменную. Он инициализирует, но именно на таких
	// неявных договорённостях мы сегодня уже спотыкались трижды.
	out = 0;
	const KZCourseDescriptor *desc = Mapi_FindCourse(descriptorName);
	if (!desc)
	{
		return false;
	}
	out = desc->localDatabaseID;
	return true;
}

const char *KZ::mapapi::CreateExternalCourse(i32 platformNumber, const char *courseName, const char *descriptorName)
{
	if (g_mappingApi.fatalFailure)
	{
		return "map_api_fatal";
	}
	if (platformNumber < 0 || platformNumber > KZ_PLATFORM_COURSE_MAX_NUMBER)
	{
		return "course_number_out_of_range";
	}
	if (!courseName || !courseName[0])
	{
		return "course_name_empty";
	}
	if (V_strlen(courseName) >= KZ_MAX_COURSE_NAME_LENGTH)
	{
		return "course_name_too_long";
	}
	if (!descriptorName || !descriptorName[0])
	{
		return "descriptor_empty";
	}
	if (g_mappingApi.courseDescriptors.Count() >= KZ_MAX_COURSE_COUNT)
	{
		return "too_many_courses";
	}

	const i32 courseId = KZ_PLATFORM_COURSE_ID_BASE + platformNumber;
	const i32 hammerId = KZ_PLATFORM_COURSE_HAMMER_ID_BASE - platformNumber;
	FOR_EACH_VEC(g_mappingApi.courseDescriptors, i)
	{
		const KZCourseDescriptor &existing = g_mappingApi.courseDescriptors[i];
		// Самое опасное столкновение во всей затее. Локальная таблица MapCourses уникальна по
		// (MapID, StageID), где StageID — именно этот id, а вставка идёт
		// `ON CONFLICT(MapID, StageID) DO UPDATE SET Name` (kz/db/queries/courses.h). Совпадение
		// id с родным курсом ПЕРЕИМЕНОВАЛО БЫ родной курс в БД и пришило к нему наши рекорды.
		// Одного «мы берём с 1000» мало: id родного курса задаёт маппер, ничто не мешает ему
		// поставить 1000.
		if (existing.id == courseId)
		{
			return "course_id_taken";
		}
		// Mapi_CreateCourse дедуплицирует по hammerId и на совпадении молча возвращает false
		// (это его штатный путь для backwards-compat зон). Молчание нам не годится — свой reason.
		if (existing.hammerId == hammerId)
		{
			return "hammer_id_taken";
		}
		// UQ_MapCourses_MapIDName: совпадение имени уронило бы транзакцию сетапа курсов целиком.
		if (KZ_STREQI(existing.name, courseName))
		{
			return "course_name_taken";
		}
		if (KZ_STREQI(existing.entityTargetname, descriptorName))
		{
			return "descriptor_taken";
		}
	}

	if (!Mapi_CreateCourse(courseId, courseName, hammerId, descriptorName, false))
	{
		return "create_failed";
	}

	// guid нового курса. Mapi_CreateCourse выводит его из Count() + 1, а Count() проседает, когда
	// валидация дропает курс, — значит новый курс МОЖЕТ получить guid живого. Цена: KZTimerService
	// опознаёт курс забега именно по guid (currentCourseGUID), то есть таймер приписал бы ран
	// чужому курсу.
	//
	// Чиним по ФАКТУ, а не предсказанием формулы апстрима: предсказание разошлось бы молча, если
	// формулу однажды поменяют. Присвоение безопасно именно здесь и только здесь — курс создан
	// мгновение назад, его не держит ни один забег и не кэширует ни один рекорд, а порядок
	// g_sortedCourses ключуется по id, не по guid, поэтому вектор не разъедется.
	KZCourseDescriptor *created = Mapi_FindCourse(descriptorName);
	if (created)
	{
		bool guidTaken = false;
		u32 maxGuid = 0;
		FOR_EACH_VEC(g_mappingApi.courseDescriptors, i)
		{
			const KZCourseDescriptor &other = g_mappingApi.courseDescriptors[i];
			maxGuid = MAX(maxGuid, other.guid);
			if (&other != created && other.guid == created->guid)
			{
				guidTaken = true;
			}
		}
		if (guidTaken)
		{
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] course_guid_reassigned descriptor=%s from=%u to=%u reason=guid_taken\n", descriptorName,
						created->guid, maxGuid + 1);
			created->guid = maxGuid + 1;
		}
	}
	return nullptr;
}

bool KZ::mapapi::SetCourseStartPositionFromTrigger(const char *descriptorName, CBaseTrigger *trigger, bool overwrite)
{
	KZCourseDescriptor *desc = Mapi_FindCourse(descriptorName);
	if (!desc || !trigger)
	{
		return false;
	}
	if (desc->hasStartPosition && !overwrite)
	{
		return true; // позиция уже есть и перебивать её не просили — это успех, а не отказ
	}
	Vector origin;
	QAngle angles;
	if (!utils::FindValidPositionForTrigger(trigger, origin, angles))
	{
		return false;
	}
	desc->SetStartPosition(origin, angles);
	return true;
}

void KZ::mapapi::CheckEndTimerTrigger(CBaseTrigger *trigger)
{
	KzTrigger *kzTrigger = Mapi_FindKzTrigger(trigger);
	if (kzTrigger && kzTrigger->type == KZTRIGGER_ZONE_END)
	{
		KZCourseDescriptor *desc = Mapi_FindCourse(kzTrigger->zone.courseDescriptor);
		if (!desc)
		{
			return;
		}
		// АСИММЕТРИЯ СО СТАРТОВОЙ ПОЗИЦИЕЙ, и она осознанная. Стартовую SetCourseStartPositionFromTrigger
		// перебивает только у СВОЕГО курса (параметр overwrite): она задаёт точку рестарта всей
		// карты, и смещать её на чужом курсе мы не вправе. Конечная позиция такой цены не имеет —
		// это апстримный путь, он пересчитывается от любой энд-зоны, включая нашу, и означает лишь
		// «где закончился курс». Проверки владения здесь нет намеренно.
		desc->hasEndPosition = utils::FindValidPositionForTrigger(trigger, desc->endPosition, desc->endAngles);
	}
}

const KzTrigger *KZ::mapapi::GetKzTrigger(CBaseTrigger *trigger)
{
	return Mapi_FindKzTrigger(trigger);
}

const KZCourseDescriptor *KZ::mapapi::GetCourseDescriptorFromTrigger(CBaseTrigger *trigger)
{
	KzTrigger *kzTrigger = Mapi_FindKzTrigger(trigger);
	if (!kzTrigger)
	{
		return nullptr;
	}
	return KZ::mapapi::GetCourseDescriptorFromTrigger(kzTrigger);
}

const KZCourseDescriptor *KZ::mapapi::GetCourseDescriptorFromTrigger(const KzTrigger *trigger)
{
	const KZCourseDescriptor *course = nullptr;
	switch (trigger->type)
	{
		case KZTRIGGER_ZONE_START:
		case KZTRIGGER_ZONE_END:
		case KZTRIGGER_ZONE_SPLIT:
		case KZTRIGGER_ZONE_CHECKPOINT:
		case KZTRIGGER_ZONE_STAGE:
		{
			course = Mapi_FindCourse(trigger->zone.courseDescriptor);
			if (!course)
			{
				Mapi_Error("%s: Couldn't find course descriptor from name \"%s\"! Trigger's Hammer Id: %i", g_errorPrefix,
						   trigger->zone.courseDescriptor, trigger->hammerId);
			}
			else if (course->disabled)
			{
				// Курс отключён платформой: ведём себя как «курса нет», но МОЛЧА. Mapi_Error здесь
				// был бы катастрофой — он копит строки и раз в минуту высыпает их в общий чат
				// всем игрокам, а касание отключённой зоны это штатное событие, а не ошибка.
				// Вызывающие (kz/trigger/callbacks.cpp, StartTouch и EndTouch) уже умеют выходить
				// по !course для зон таймера, поэтому таймер на отключённом курсе не стартует.
				course = nullptr;
			}
		}
		break;
	}
	return course;
}

bool MappingInterface::IsTriggerATimerZone(CBaseTrigger *trigger)
{
	KzTrigger *kzTrigger = Mapi_FindKzTrigger(trigger);
	if (!kzTrigger)
	{
		return false;
	}
	return KZ::mapapi::IsTimerTrigger(kzTrigger->type);
}

bool MappingInterface::GetJumpstatArea(Vector &pos, QAngle &angles)
{
	if (g_mappingApi.hasJumpstatArea)
	{
		pos = g_mappingApi.jumpstatAreaPos;
		angles = g_mappingApi.jumpstatAreaAngles;
	}

	return g_mappingApi.hasJumpstatArea;
}

void KZ::course::ClearCourses()
{
	g_sortedCourses.RemoveAll();
	KZTimerService::ClearRecordCache();
}

u32 KZ::course::GetCourseCount()
{
	return g_sortedCourses.Count();
}

const KZCourseDescriptor *KZ::course::GetCourseByCourseID(i32 id)
{
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		if (g_sortedCourses[i]->id == id)
		{
			return g_sortedCourses[i];
		}
	}
	return nullptr;
}

const KZCourseDescriptor *KZ::course::GetCourseByLocalCourseID(u32 id)
{
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		if (g_sortedCourses[i]->localDatabaseID == id)
		{
			return g_sortedCourses[i];
		}
	}
	return nullptr;
}

const KZCourseDescriptor *KZ::course::GetCourseByGlobalCourseID(u32 id)
{
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		if (g_sortedCourses[i]->globalDatabaseID == id)
		{
			return g_sortedCourses[i];
		}
	}
	return nullptr;
}

const KZCourseDescriptor *KZ::course::GetCourse(const char *courseName, bool caseSensitive, bool matchPartial)
{
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		const char *name = g_sortedCourses[i]->name;
		if (caseSensitive ? KZ_STREQ(name, courseName) : KZ_STREQI(name, courseName))
		{
			return g_sortedCourses[i];
		}
	}
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		const char *name = g_sortedCourses[i]->name;
		if (matchPartial)
		{
			if (caseSensitive ? V_strstr(name, courseName) : V_stristr(name, courseName))
			{
				return g_sortedCourses[i];
			}
		}
	}
	return nullptr;
}

const KZCourseDescriptor *KZ::course::GetCourse(u32 guid)
{
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		if (g_sortedCourses[i]->guid == guid)
		{
			return g_sortedCourses[i];
		}
	}
	return nullptr;
}

const KZCourseDescriptor *KZ::course::GetFirstCourse()
{
	if (g_sortedCourses.Count() >= 1)
	{
		return g_sortedCourses[0];
	}
	return nullptr;
}

void KZ::course::SetupLocalCourses()
{
	if (KZDatabaseService::IsMapSetUp())
	{
		KZDatabaseService::SetupCourses(g_sortedCourses);
	}
}

bool KZ::course::UpdateCourseLocalID(const char *courseName, u32 databaseID)
{
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		if (g_sortedCourses[i]->GetName() == courseName)
		{
			g_sortedCourses[i]->localDatabaseID = databaseID;
			return true;
		}
	}
	return false;
}

bool KZ::course::UpdateCourseGlobalID(const char *courseName, u32 globalID)
{
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		if (g_sortedCourses[i]->GetName() == courseName)
		{
			g_sortedCourses[i]->globalDatabaseID = globalID;
			return true;
		}
	}
	return false;
}

i32 KZ::course::GetCyberCourseNumber(const KZCourseDescriptor *course)
{
	if (!course)
	{
		return 0;
	}
	// "Bonus 3" / "bonus3" → 3; вменяемый диапазон 1..99, иначе считаем именем не-бонуса
	if (V_strnicmp(course->name, "bonus", 5) == 0)
	{
		const char *p = course->name + 5;
		while (*p && !isdigit((unsigned char)*p))
		{
			p++;
		}
		if (isdigit((unsigned char)*p))
		{
			i32 n = atoi(p);
			if (n >= 1 && n <= 99)
			{
				return n;
			}
		}
	}
	// «Главный» курс определяем по ПОЛНОМУ списку курсов карты, включая отключённые платформой.
	// GetFirstCourse() смотрит в g_sortedCourses, откуда отключённый курс убран, — и отключение
	// первого курса молча передало бы cyber-номер 0 следующему. А это ключ лидерборда платформы
	// (submission.cpp, course.number), кэшей PB/WR и сейва рана: рекорды другого курса начали бы
	// писаться и читаться вместе с рекордами настоящего main. Бонусы уцелели бы (их номер из
	// имени), а карты вида Main/Second — нет.
	const KZCourseDescriptor *first = Mapi_FirstCourseIncludingDisabled();
	if (first && first->guid == course->guid)
	{
		return 0;
	}
	// Не-бонусный не-главный курс: уводим в диапазон 100+, чтобы номер не
	// столкнулся с "Bonus N" на той же карте (лидерборд ключуется по course).
	return 100 + course->id;
}

const KZCourseDescriptor *KZ::course::GetCourseByCyberNumber(i32 n)
{
	// Зеркало FindBonusCourse ниже, но без ограничения n >= 1 (Task 4: main-курс тоже
	// нужно резолвить обратно, cyber-номер 0).
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		if (KZ::course::GetCyberCourseNumber(g_sortedCourses[i]) == n)
		{
			return g_sortedCourses[i];
		}
	}
	return nullptr;
}

static void ListCourses(KZPlayer *player)
{
	if (player->timerService->GetCourse())
	{
		player->languageService->PrintChat(true, false, "Current Course", player->timerService->GetCourse()->name);
	}
	else
	{
		player->languageService->PrintChat(true, false, "No Current Course");
	}
	KZ::course::PrintCourses(player);
}

// Колбэк меню !courses: info-тег — map-defined id курса строкой.
// Ревалидация обязательна: карта могла смениться, пока меню висело.
static_function void OnCoursesMenuSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!p)
	{
		return;
	}
	const char *info = g_pMenus->GetItemInfo(menu, item);
	if (!info || !info[0])
	{
		return;
	}
	// Отказ prac — ДО проверки стартпозиции: иначе на курсе без неё игрок получил бы
	// «No Start Position For Course» вместо настоящей причины, а в логах не было бы отказа.
	// Сама воронка TeleportToCourse всё равно закрыта — это только про верное сообщение.
	if (p->pracService->RejectMapTeleport("teleport_to_start"))
	{
		return;
	}
	const KZCourseDescriptor *course = KZ::course::GetCourseByCourseID(V_StringToInt32(info, -1));
	if (!course)
	{
		return;
	}
	if (!course->hasStartPosition)
	{
		p->languageService->PrintChat(true, false, "No Start Position For Course", course->name);
		return;
	}
	KZ::misc::TeleportToCourse(p, course);
}

// Меню !courses: main первым, затем бонусы по номерам, затем прочие курсы.
static_function void OpenCoursesMenu(KZPlayer *player)
{
	// Без движка меню — прежнее поведение (список в чат/консоль).
	if (g_pMenus == nullptr)
	{
		ListCourses(player);
		return;
	}
	int slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return;
	}

	// Один хэндл на слот — пересоздаём при повторном вызове (паттерн kz_option_menu).
	static MenuHandle s_coursesMenu[MAXPLAYERS + 1] = {};
	if (s_coursesMenu[slot] != kInvalidMenuHandle)
	{
		g_pMenus->DestroyMenu(s_coursesMenu[slot]);
		s_coursesMenu[slot] = kInvalidMenuHandle;
	}

	const char *lang = player->languageService->GetLanguage();
	std::string title = KZLanguageService::PrepareMessageWithLang(lang, "Courses Menu - Title");
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), &OnCoursesMenuSelect);
	if (m == kInvalidMenuHandle)
	{
		ListCourses(player);
		return;
	}

	// Сортировка вставками по cyber-номеру (0 = main, 1..99 = бонусы, 100+ = прочие);
	// дубликаты номеров сохраняют порядок g_sortedCourses. Опираться на него как на «исходный»
	// нельзя: ключ сортировки (id) не уникален — Mapi_CreateCourse дедуплицирует по hammerId и
	// targetname, но не по id, — а перестроение вектора после дропа курса валидацией не обязано
	// воспроизвести прежний относительный порядок равных ключей. Порядок детерминирован в
	// пределах одного состояния вектора, и только на это тут и рассчитываем.
	const KZCourseDescriptor *ordered[KZ_MAX_COURSE_COUNT];
	i32 count = 0;
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		const KZCourseDescriptor *course = g_sortedCourses[i];
		i32 num = KZ::course::GetCyberCourseNumber(course);
		i32 pos = count;
		while (pos > 0 && KZ::course::GetCyberCourseNumber(ordered[pos - 1]) > num)
		{
			ordered[pos] = ordered[pos - 1];
			pos--;
		}
		ordered[pos] = course;
		count++;
	}

	for (i32 i = 0; i < count; i++)
	{
		char info[16];
		V_snprintf(info, sizeof(info), "%d", ordered[i]->id);
		// Курс без стартовой позиции — неактивный (серый) пункт.
		g_pMenus->AddItem(m, ordered[i]->name, info, !ordered[i]->hasStartPosition);
	}

	// Одноразовый выбор — меню закрывается по клику (в отличие от !options).
	g_pMenus->SetCloseOnSelect(m, true);
	s_coursesMenu[slot] = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

SCMD(kz_courses, SCFL_MAP | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	OpenCoursesMenu(player);
	return MRES_SUPERCEDE;
}

SCMD(kz_course, SCFL_MAP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (args->ArgC() < 2)
	{
		ListCourses(player);
	}
	else
	{
		KZ::misc::HandleTeleportToCourse(player, args);
	}
	return MRES_SUPERCEDE;
}

static_function const KZCourseDescriptor *FindBonusCourse(i32 n)
{
	// Зеркало конвенции эмиттера: бонус N = курс, чей GetCyberCourseNumber == n.
	// Так !b3 находит и "bonus_3"/"BONUS STAGE 3", а не только "Bonus 3".
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		if (KZ::course::GetCyberCourseNumber(g_sortedCourses[i]) == n)
		{
			return g_sortedCourses[i];
		}
	}
	return nullptr;
}

static_function META_RES GotoBonus(CCSPlayerController *controller, i32 n)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	// Как и в меню !courses: причина отказа должна быть prac, а не «у бонуса нет стартпозиции».
	if (player->pracService->RejectMapTeleport("teleport_to_start"))
	{
		return MRES_SUPERCEDE;
	}
	const KZCourseDescriptor *course = (n >= 1) ? FindBonusCourse(n) : nullptr;
	if (!course)
	{
		char num[16];
		V_snprintf(num, sizeof(num), "%d", n);
		player->languageService->PrintChat(true, false, "Bonus Not Found", num);
		return MRES_SUPERCEDE;
	}
	if (!course->hasStartPosition)
	{
		// Бонус есть, но маппер не задал стартовую позицию — честное сообщение.
		player->languageService->PrintChat(true, false, "No Start Position For Course", course->name);
		return MRES_SUPERCEDE;
	}
	KZ::misc::TeleportToCourse(player, course);
	return MRES_SUPERCEDE;
}

SCMD(kz_main, SCFL_MAP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	// Как и в !b*/меню !courses: сначала prac, потом уже причина «нет стартпозиции».
	if (player->pracService->RejectMapTeleport("teleport_to_start"))
	{
		return MRES_SUPERCEDE;
	}
	const KZCourseDescriptor *course = KZ::course::GetFirstCourse();
	if (!course || !course->hasStartPosition)
	{
		player->languageService->PrintChat(true, false, "No Start Position For Course", "Main");
		return MRES_SUPERCEDE;
	}
	KZ::misc::TeleportToCourse(player, course);
	return MRES_SUPERCEDE;
}

SCMD(kz_b, SCFL_MAP)
{
	i32 n = (args->ArgC() < 2) ? 1 : atoi(args->Arg(1));
	return GotoBonus(controller, n);
}

SCMD_LINK(kz_bonus, kz_b);

// !b1..!b9 / !bonus1..!bonus9 — фиксированные алиасы (чат-триггер матчит имя целиком).
#define KZ_BONUS_NUM_CMD(n) \
	SCMD(kz_b##n, SCFL_MAP) \
	{ \
		return GotoBonus(controller, n); \
	} \
	SCMD_LINK(kz_bonus##n, kz_b##n);

KZ_BONUS_NUM_CMD(1)
KZ_BONUS_NUM_CMD(2)
KZ_BONUS_NUM_CMD(3)
KZ_BONUS_NUM_CMD(4)
KZ_BONUS_NUM_CMD(5)
KZ_BONUS_NUM_CMD(6)
KZ_BONUS_NUM_CMD(7)
KZ_BONUS_NUM_CMD(8)
KZ_BONUS_NUM_CMD(9)

// TODO: Does this *really* belong here?
// clang-format off
#define COURSE_TABLE_NAME "Courses - Table Name"
static_global const char *columnKeysWithAPI[] = {
	"#.",
	"Course Info Header - Name",
	"Course Info Header - Mappers",
	"Course Info Header - State (Classic)",
	"Course Info Header - Tier (Classic)",
	"Course Info Header - State (Vanilla)",
	"Course Info Header - Tier (Vanilla)"
};
static_global const char *columnKeysWithoutAPI[] = {
	"#.",
	"Course Info Header - Name",
};
static_global const char *courseStateKeys[] = {
	"Course Info - Unranked",
	"Course Info - Pending",
	"Course Info - Ranked"
};
// clang-format on

static_function void PrintCoursesWithMap(KZPlayer *player, const std::vector<KZ::api::Map::Course> &courses)
{
	CUtlString headers[KZ_ARRAYSIZE(columnKeysWithAPI)];
	for (u32 col = 0; col < KZ_ARRAYSIZE(columnKeysWithAPI); col++)
	{
		headers[col] = player->languageService->PrepareMessage(columnKeysWithAPI[col]).c_str();
	}
	utils::Table<KZ_ARRAYSIZE(columnKeysWithAPI)> table(player->languageService->PrepareMessage(COURSE_TABLE_NAME).c_str(), headers);
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		KZCourseDescriptor *course = g_sortedCourses[i];
		const KZ::api::Map::Course *apiCourse = nullptr;
		for (auto &c : courses)
		{
			if (c.id == course->globalDatabaseID)
			{
				apiCourse = &c;
				break;
			}
		}
		std::string tierClassic {}, tierVanilla {}, stateClassic {}, stateVanilla {}, mappers {};
		if (apiCourse)
		{
			// Mappers
			for (u32 m = 0; m < apiCourse->mappers.size(); m++)
			{
				mappers += apiCourse->mappers[m].name;
				if (m < apiCourse->mappers.size() - 1)
				{
					mappers += ", ";
				}
			}
			// Tiers
			tierClassic = std::to_string((u8)apiCourse->filters.classic.nubTier) + "/" + std::to_string((u8)apiCourse->filters.classic.proTier);
			tierVanilla = std::to_string((u8)apiCourse->filters.vanilla.nubTier) + "/" + std::to_string((u8)apiCourse->filters.vanilla.proTier);
			// States
			stateClassic = player->languageService->PrepareMessage(courseStateKeys[(i8)apiCourse->filters.classic.state + 1]);
			stateVanilla = player->languageService->PrepareMessage(courseStateKeys[(i8)apiCourse->filters.vanilla.state + 1]);
		}
		else
		{
			KZ_LOG_WARN(LogChannel::MappingAPI, "Warning: Course ID %i not found in API map data!\n", course->globalDatabaseID);
		}
		// clang-format off
		table.SetRow(
			i,
			std::to_string(course->id).c_str(),
			course->name,
			mappers.c_str(),
			stateClassic.c_str(),
			tierClassic.c_str(),
			stateVanilla.c_str(),
			tierVanilla.c_str()
		);
		// clang-format on
	}
	player->PrintConsole(false, false, table.GetSeparator("="));
	player->PrintConsole(false, false, table.GetTitle());
	player->PrintConsole(false, false, table.GetHeader());
	for (u32 i = 0; i < table.GetNumEntries(); i++)
	{
		player->PrintConsole(false, false, table.GetLine(i));
	}
	player->PrintConsole(false, false, table.GetSeparator("="));
}

static_function void PrintCoursesWithoutMap(KZPlayer *player)
{
	CUtlString headers[KZ_ARRAYSIZE(columnKeysWithoutAPI)];
	for (u32 col = 0; col < KZ_ARRAYSIZE(columnKeysWithoutAPI); col++)
	{
		headers[col] = player->languageService->PrepareMessage(columnKeysWithoutAPI[col]).c_str();
	}
	utils::Table<KZ_ARRAYSIZE(columnKeysWithoutAPI)> table(player->languageService->PrepareMessage(COURSE_TABLE_NAME).c_str(), headers);
	FOR_EACH_VEC(g_sortedCourses, i)
	{
		KZCourseDescriptor *course = g_sortedCourses[i];
		// clang-format off
		table.SetRow(
			i,
			std::to_string(course->id).c_str(),
			course->name
		);
		// clang-format on
	}
	player->PrintConsole(false, false, table.GetSeparator("="));
	player->PrintConsole(false, false, table.GetTitle());
	player->PrintConsole(false, false, table.GetHeader());
	for (u32 i = 0; i < table.GetNumEntries(); i++)
	{
		player->PrintConsole(false, false, table.GetLine(i));
	}
	player->PrintConsole(false, false, table.GetSeparator("="));
}

void KZ::course::PrintCourses(KZPlayer *player)
{
	// If we have API map data, that means we have information about individual mappers for each course, along with tiers for both modes.
	// Otherwise, the only course information we have is the course ID and the course name.
	bool hasMap = false;
	std::vector<KZ::api::Map::Course> courses;
	hasMap = KZGlobalService::WithCurrentMap(
		[&](const std::optional<KZ::api::Map> &currentMap)
		{
			if (currentMap)
			{
				courses = currentMap->courses;
				return true;
			}
			return false;
		});
	if (hasMap)
	{
		PrintCoursesWithMap(player, courses);
	}
	else
	{
		PrintCoursesWithoutMap(player);
	}
}

static_function void PrintCurrentMapCoursesInfo(KZPlayer *player)
{
	if (!KZGlobalService::IsAvailable())
	{
		player->languageService->PrintChat(true, false, "Map Info - No API Connection");
	}

	bool hasMap = false;
	std::vector<KZ::api::Map::Course> courses;
	std::vector<KZ::api::PlayerInfo> mappers;
	std::string mapApprovedAt;
	KZ::api::Map::State mapState;
	std::string mapDescription;
	hasMap = KZGlobalService::WithCurrentMap(
		[&](const std::optional<KZ::api::Map> &currentMap)
		{
			if (currentMap)
			{
				courses = currentMap->courses;
				mappers = currentMap->mappers;
				mapState = currentMap->state;
				mapDescription = currentMap->description.value_or("");
				mapApprovedAt = currentMap->approvedAt;
				return true;
			}
			return false;
		});

	if (!hasMap)
	{
		player->languageService->PrintChat(true, false, "Map Info - No API Map Data");
	}
	player->languageService->PrintChat(true, false, "Map Info - Check Console");
	CUtlString mapName = g_pKZUtils->GetCurrentMapName();

	// Map name
	if (g_pKZUtils->GetCurrentMapWorkshopID())
	{
		char workshopID[16];
		V_snprintf(workshopID, sizeof(workshopID), " (%llu)", g_pKZUtils->GetCurrentMapWorkshopID());
		mapName += workshopID;
	}
	player->languageService->PrintConsole(false, false, "Map Info - Map Info Header", mapName.Get());

	if (hasMap)
	{
		// Mappers
		if (mappers.size() > 0)
		{
			std::string mappersStr;
			for (u32 i = 0; i < mappers.size(); i++)
			{
				mappersStr += mappers[i].name;
				if (i < mappers.size() - 1)
				{
					mappersStr += ", ";
				}
			}
			player->languageService->PrintConsole(false, false, "Map Info - Mappers", mappersStr.c_str());
		}
		// Map state
		std::string stateStr;
		switch (mapState)
		{
			case KZ::api::Map::State::Approved:
				stateStr = player->languageService->PrepareMessage("Map Info - Approved", mapApprovedAt.c_str());
				break;
			case KZ::api::Map::State::InTesting:
				stateStr = player->languageService->PrepareMessage("Map Info - In Testing");
				break;
			default:
				stateStr = player->languageService->PrepareMessage("Map Info - Invalid");
				break;
		}
		player->languageService->PrintConsole(false, false, "Map Info - State", stateStr.c_str());
		// Map description
		if (mapDescription.length() > 0)
		{
			player->languageService->PrintConsole(false, false, "Map Info - Description", mapDescription.c_str());
		}
	}
	// Courses
	KZ::course::PrintCourses(player);
}

SCMD(kz_mapinfo, SCFL_MAP | SCFL_GLOBAL | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	PrintCurrentMapCoursesInfo(player);
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_mi, kz_mapinfo)

// `!tier` берёт тир с НАШЕЙ платформы (cyber-api), а не из глобального cs2kz-api.
// Почему: глобальный api для этой сети недоступен (запрос падал с ошибкой подключения) и
// принимает только vanilla/classic со сверкой md5 бинарника режима — на kzt и прочих
// кастомных режимах `!tier` не работал в принципе. Истина по тирам у нас и так своя:
// coalesce(map_tier_overrides(map, mode), maps.tier), порежимно (см. apps/api).
//
// Плата за переезд, осознанная: платформенный тир — ОДИН на (карта, режим). Прежняя выдача
// была богаче (курс, пара nub/pro, state, описание), но её источник для нас мёртв, а этих
// полей платформа не хранит. Поэтому аргумент курса больше не участвует в резолве: команда
// отвечает про текущую карту в текущем режиме. Если понадобится порежимный тир по курсам —
// это расширение и схемы api, и таблицы оверрайдов, отдельной задачей.
static_function void PrintCourseTier(KZPlayer *player, const CCommand *args)
{
	// Аргумент (курс) осознанно игнорируем — см. комментарий выше. Молча: ругаться на
	// привычный `!tier <курс>` значило бы наказывать игрока за нашу смену источника.
	(void)args;

	const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
	if (!url || url[0] == '\0')
	{
		player->languageService->PrintChat(true, false, "Tier Info - Platform Unavailable");
		return;
	}

	bool mapNameOk = false;
	CUtlString mapNameStr = g_pKZUtils->GetCurrentMapName(&mapNameOk);
	std::string mapName = mapNameOk ? mapNameStr.Get() : "";
	// Тот же вайтлист имени карты, что у реплеев/PB-WR: api валидирует map тем же
	// [a-z0-9_-]{1,128}, заведомо мимо — не ходим в сеть и не показываем игроку «нет связи».
	if (!CybReplayCommon::IsValidMapName(mapName))
	{
		player->languageService->PrintChat(true, false, "Tier Info - Platform Unknown", mapName.c_str());
		return;
	}

	// Режим — в api-нотацию (ckz/vnl/kzt) общим маппингом. Пустая строка = режим, которого
	// центральное хранилище не знает: гейт остался, но он ПРО ПЛАТФОРМУ, а не про
	// Classic/Vanilla, как было раньше, — kzt теперь проходит.
	const char *mode = CybReplayCommon::MapMode(player->modeService->GetModeShortName());
	if (!mode || mode[0] == '\0')
	{
		player->languageService->PrintChat(true, false, "Tier Info - Platform Mode Unsupported");
		return;
	}

	std::string fullUrl = url;
	if (!fullUrl.empty() && fullUrl.back() == '/')
	{
		fullUrl.pop_back();
	}
	fullUrl += "/v1/kz/maps/" + mapName + "/tier";

	HTTP::Request req(HTTP::Method::GET, fullUrl);
	req.SetQuery("mode", mode);

	// Ответ адресуем по userID, а не по указателю: игрок мог выйти за время round-trip,
	// а слот — успеть достаться другому (тот же гард, что у PB-фетча в kz_timer.cpp).
	CPlayerUserId userID = player->GetClient()->GetUserID();
	u64 requesterSteamId64 = player->GetSteamId64();
	std::string requestedMap = mapName;

	// clang-format off
	req.Send(
		[userID, requesterSteamId64, requestedMap](HTTP::Response resp)
		{
			KZPlayer *pl = g_pKZPlayerManager->ToPlayer(userID);
			if (!pl)
			{
				return;
			}
			// Гард переиспользования userID, как у поиска реплеев и PB-фетча: слот мог достаться
			// другому игроку, пока летел запрос. Здесь цена ошибки меньше (чужая строка в чат,
			// а не запуск реплея), но guard тот же — расхождение в таких местах и рождает баги.
			if (pl->GetSteamId64() != requesterSteamId64)
			{
				return;
			}
			if (resp.status < 200 || resp.status >= 300)
			{
				KZ_LOG_INFO(LogChannel::General, "[cyb_tier] HTTP %u for map=%s\n", (unsigned)resp.status, requestedMap.c_str());
				pl->languageService->PrintChat(true, false, "Tier Info - Platform Unavailable");
				return;
			}
			std::optional<std::string> body = resp.Body();
			if (!body.has_value())
			{
				pl->languageService->PrintChat(true, false, "Tier Info - Platform Unavailable");
				return;
			}

			KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
			CUtlString error = "";
			LoadKV3FromJSON(&kv, &error, body->c_str(), "");
			if (!error.IsEmpty())
			{
				KZ_LOG_WARN(LogChannel::General, "[cyb_tier] failed to parse api response: %s\n", error.Get());
				pl->languageService->PrintChat(true, false, "Tier Info - Platform Unavailable");
				return;
			}

			// tier: number|null по контракту (kzMapTierResponseSchema). null — штатный ответ
			// «карты не знаем ИЛИ тир не задан», а не ошибка: ручка на этот случай отдаёт 200.
			// Отсутствующий/не-числовой член трактуем так же — нам нечего показать.
			KeyValues3 *tierMember = kv.FindMember("tier");
			KV3Type_t tierType = tierMember ? tierMember->GetType() : KV3_TYPE_NULL;
			if (!tierMember || (tierType != KV3_TYPE_INT && tierType != KV3_TYPE_UINT && tierType != KV3_TYPE_DOUBLE))
			{
				pl->languageService->PrintChat(true, false, "Tier Info - Platform Unknown", requestedMap.c_str());
				return;
			}
			i32 tier = (i32)tierMember->GetDouble(0.0);
			pl->languageService->PrintChat(true, false, "Tier Info - Platform", requestedMap.c_str(), tier);
		},
		[userID]()
		{
			KZPlayer *pl = g_pKZPlayerManager->ToPlayer(userID);
			if (pl)
			{
				pl->languageService->PrintChat(true, false, "Tier Info - Platform Unavailable");
			}
			KZ_LOG_INFO(LogChannel::General, "[cyb_tier] network error\n");
		});
	// clang-format on
}

SCMD(kz_tier, SCFL_MAP | SCFL_GLOBAL | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	PrintCourseTier(player, args);
	return MRES_SUPERCEDE;
}
