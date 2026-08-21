#include "kz_timer.h"
#include "kz/db/kz_db.h"
#include "kz/global/kz_global.h"
#include "kz/language/kz_language.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
#include "kz/noclip/kz_noclip.h"
#include "kz/option/kz_option.h"
#include "kz/prac/kz_prac.h"
#include "kz/language/kz_language.h"
#include "kz/profile/kz_profile.h"
#include "kz/trigger/kz_trigger.h"
#include "kz/spec/kz_spec.h"
#include "kz/recording/kz_recording.h"
#include "kz/savedrun/kz_savedrun.h"
#include "kz/replays/cyb_replay_common.h" // MapMode/IsValidMapName — общий с эмиттером маппинг режима/карты
#include "submission.h"

#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "utils/http.h"   // Steam async HTTP для догрузки платформенных PB/WR
#include "utils/ctimer.h" // периодическое обновление платформенных PB/WR в худе
#include "vendor/sql_mm/src/public/sql_mm.h"

#include <optional>
#include <string>

// clang-format off
constexpr const char *diffTextKeys[KZTimerService::CompareType::COMPARETYPE_COUNT] = {
	"",
	"Server PB Diff (Overall)",
	"Global PB Diff (Overall)",
	"SR Diff (Overall)",
	"WR Diff (Overall)"
};

constexpr const char *diffTextKeysPro[KZTimerService::CompareType::COMPARETYPE_COUNT] = {
	"",
	"Server PB Diff (Pro)",
	"Global PB Diff (Pro)",
	"SR Diff (Pro)",
	"WR Diff (Pro)"
};

constexpr const char *missedTimeKeys[KZTimerService::CompareType::COMPARETYPE_COUNT] = {
	"",
	"Missed Server PB (Overall)",
	"Missed Global PB (Overall)",
	"Missed SR (Overall)",
	"Missed WR (Overall)"
};

constexpr const char *missedTimeKeysPro[KZTimerService::CompareType::COMPARETYPE_COUNT] = {
	"",
	"Missed Server PB (Pro)",
	"Missed Global PB (Pro)",
	"Missed SR (Pro)",
	"Missed WR (Pro)"
};

constexpr const char *missedTimeKeysBoth[KZTimerService::CompareType::COMPARETYPE_COUNT] = {
	"",
	"Missed Server PB (Overall+Pro)",
	"Missed Global PB (Overall+Pro)",
	"Missed SR (Overall+Pro)",
	"Missed WR (Overall+Pro)"
};

// clang-format on

static_global class KZDatabaseServiceEventListener_Timer : public KZDatabaseServiceEventListener
{
public:
	virtual void OnMapSetup() override;
	virtual void OnClientSetup(Player *player, u64 steamID64, bool isBanned) override;
} databaseEventListener;

static_global class KZOptionServiceEventListener_Timer : public KZOptionServiceEventListener
{
	virtual void OnPlayerPreferencesLoaded(KZPlayer *player)
	{
		player->timerService->OnPlayerPreferencesLoaded();
	}
} optionEventListener;

std::unordered_map<PBDataKey, PBData> KZTimerService::srCache;
std::unordered_map<PBDataKey, PBData> KZTimerService::wrCache;
std::unordered_map<u64, f64> KZTimerService::platformWrCache;

namespace
{
	// api-mode-строка ("ckz"/"vnl"/"kzt") → индекс 0/1/2; -1 — режим не поддержан платформой.
	// Оба конца (запись ответа api и лукап из худа) идут через эту функцию → ключи сходятся.
	i32 ApiModeToIndex(const char *apiMode)
	{
		if (!apiMode || !apiMode[0])
		{
			return -1;
		}
		if (KZ_STREQI(apiMode, "ckz"))
		{
			return 0;
		}
		if (KZ_STREQI(apiMode, "vnl"))
		{
			return 1;
		}
		if (KZ_STREQI(apiMode, "kzt"))
		{
			return 2;
		}
		return -1;
	}

	// Ключ платформенного кэша: (api-mode-index, cyber-course-number).
	u64 ToPlatformKey(i32 modeIdx, i32 cyberCourse)
	{
		return (u32)modeIdx | ((u64)(u32)cyberCourse << 32);
	}

	// Число (ms) из члена KV3-объекта в f64. false — член отсутствует / null / не число.
	// GetDouble коэрсит INT/UINT/DOUBLE; null и прочие типы (JSON null у отсутствующего WR/PB)
	// отсекаются проверкой типа.
	bool KV3ReadNumber(KeyValues3 *obj, const char *key, f64 &out)
	{
		if (!obj)
		{
			return false;
		}
		KeyValues3 *m = obj->FindMember(key);
		if (!m)
		{
			return false;
		}
		KV3Type_t t = m->GetType();
		if (t != KV3_TYPE_INT && t != KV3_TYPE_UINT && t != KV3_TYPE_DOUBLE)
		{
			return false;
		}
		out = m->GetDouble(0.0);
		return true;
	}
} // namespace

static_global CUtlVector<KZTimerServiceEventListener *> eventListeners;

bool KZTimerService::RegisterEventListener(KZTimerServiceEventListener *eventListener)
{
	if (eventListeners.Find(eventListener) >= 0)
	{
		return false;
	}
	eventListeners.AddToTail(eventListener);
	return true;
}

bool KZTimerService::UnregisterEventListener(KZTimerServiceEventListener *eventListener)
{
	return eventListeners.FindAndRemove(eventListener);
}

void KZTimerService::ResetStartZoneGroundTouch()
{
	this->touchedGroundSinceTouchingStartZone = !!(this->player->GetPlayerPawn()->m_fFlags & FL_ONGROUND);
}

void KZTimerService::StartZoneStartTouch(const KZCourseDescriptor *course)
{
	this->ResetStartZoneGroundTouch();
	this->TimerStop(false, "start_zone");
}

void KZTimerService::StartZoneEndTouch(const KZCourseDescriptor *course)
{
	if (this->touchedGroundSinceTouchingStartZone)
	{
		this->TimerStart(course);
	}
}

void KZTimerService::SplitZoneStartTouch(const KZCourseDescriptor *course, i32 splitNumber)
{
	if (!this->timerRunning || course->guid != this->currentCourseGUID)
	{
		return;
	}

	assert(splitNumber > INVALID_SPLIT_NUMBER && splitNumber < KZ_MAX_SPLIT_ZONES);

	if (this->splitZoneTimes[splitNumber - 1] < 0)
	{
		this->PlayReachedSplitSound();
		this->splitZoneTimes[splitNumber - 1] = this->GetTime();
		this->ShowSplitText(splitNumber);
		this->lastSplit = splitNumber;
		CALL_FORWARD(eventListeners, OnSplitZoneTouchPost, this->player, splitNumber);
		if (this->player->optionService->GetPreferenceBool("mapOverlay"))
		{
			// clang-format off
			this->player->PrintConsole(false, false, "[CS2KZ] split|%d|%f", 
				splitNumber,
				this->GetTime()
			);
			// clang-format on
		}
	}
}

void KZTimerService::CheckpointZoneStartTouch(const KZCourseDescriptor *course, i32 cpNumber)
{
	if (!this->timerRunning || course->guid != this->currentCourseGUID)
	{
		return;
	}

	assert(cpNumber > INVALID_CHECKPOINT_NUMBER && cpNumber < KZ_MAX_CHECKPOINT_ZONES);

	if (this->cpZoneTimes[cpNumber - 1] < 0)
	{
		this->PlayReachedCheckpointSound();
		this->cpZoneTimes[cpNumber - 1] = this->GetTime();
		this->ShowCheckpointText(cpNumber);
		this->lastCheckpoint = cpNumber;
		this->reachedCheckpoints++;
		CALL_FORWARD(eventListeners, OnCheckpointZoneTouchPost, this->player, cpNumber);
		if (this->player->optionService->GetPreferenceBool("mapOverlay"))
		{
			// clang-format off
			this->player->PrintConsole(false, false, "[CS2KZ] checkpoint|%d|%f", 
				cpNumber,
				this->GetTime()
			);
			// clang-format on
		}
	}
}

void KZTimerService::StageZoneStartTouch(const KZCourseDescriptor *course, i32 stageNumber)
{
	if (!this->timerRunning || course->guid != this->currentCourseGUID)
	{
		return;
	}

	assert(stageNumber > INVALID_STAGE_NUMBER && stageNumber < KZ_MAX_STAGE_ZONES);

	if (stageNumber > this->currentStage + 1)
	{
		this->PlayMissedZoneSound();
		this->player->languageService->PrintChat(true, false, "Touched too high stage number (Missed stage)", this->currentStage + 1);
		return;
	}

	if (stageNumber == this->currentStage + 1)
	{
		this->stageZoneTimes[this->currentStage] = this->GetTime();
		this->PlayReachedStageSound();
		this->ShowStageText();
		this->currentStage++;
		CALL_FORWARD(eventListeners, OnStageZoneTouchPost, this->player, stageNumber);
		if (this->player->optionService->GetPreferenceBool("mapOverlay"))
		{
			// clang-format off
			this->player->PrintConsole(false, false, "[CS2KZ] stage|%d|%f", 
				stageNumber,
				this->GetTime()
			);
			// clang-format on
		}
	}
}

// Бескурсовая часть гарда TimerStart. Вынесена, чтобы prac-часы заводились ровно по тем же
// условиям (KZPracService::OnStartZoneEndTouch): один список на два вызывающих не разъедется.
bool KZTimerService::CanStartRunHere()
{
	// Пешки может не быть (спектатор): у TimerStart её проверяли вызывающие, а метод стал публичным.
	CCSPlayerPawn *pawn = this->player->GetPlayerPawn();
	if (!pawn)
	{
		return false;
	}
	// clang-format off
	return pawn->IsAlive()
		&& !this->JustStartedTimer()
		&& !this->player->JustTeleported()
		&& !this->player->inPerf
		&& !this->player->noclipService->JustNoclipped()
		&& this->HasValidMoveType()
		&& !this->JustLanded()
		&& ((pawn->m_fFlags & FL_ONGROUND) || this->GetValidJump());
	// clang-format on
}

bool KZTimerService::TimerStart(const KZCourseDescriptor *courseDesc, bool playSound)
{
	// Курсовое условие осталось здесь: prac-часы курса не ведут (см. CanStartRunHere).
	if (!this->CanStartRunHere() || (this->GetTimerRunning() && courseDesc->guid == this->currentCourseGUID))
	{
		return false;
	}
	if (V_strlen(this->player->modeService->GetModeName()) > KZ_MAX_MODE_NAME_LENGTH)
	{
		Warning("[KZ] Timer start failed: Mode name is too long!");
		return false;
	}

	bool allowStart = true;
	FOR_EACH_VEC(eventListeners, i)
	{
		allowStart &= eventListeners[i]->OnTimerStart(this->player, courseDesc->guid);
	}
	if (!allowStart)
	{
		return false;
	}

	// In CKZ you can touch trigger in half tick intervals, but here we are incrementing by full tick intervals only.
	// Since the player was still in the trigger for half a tick, we need to offset by half a tick if we started in a half tick.
	// So the current time should be subtracted by the difference between server curtime and client curtime at the moment of starting the timer,
	// That way when we increment by full tick intervals in OnPhysicsSimulatePost, the time will be correct.
	this->currentTime = g_pKZUtils->GetGlobals()->curtime - g_pKZUtils->GetServerGlobals()->curtime;
	assert(this->currentTime <= 0 && this->currentTime > -ENGINE_FIXED_TICK_INTERVAL);
	this->timerRunning = true;
	this->currentStage = 0;
	this->reachedCheckpoints = 0;
	this->lastCheckpoint = 0;
	this->lastSplit = 0;
	// Новый ран — незакрытые подтверждения сброса от прошлого не переносим (см. CheckSafeguard).
	V_memset(this->resetConfirmTime, 0, sizeof(this->resetConfirmTime));

	f64 invalidTime = -1;
	this->splitZoneTimes.SetSize(courseDesc->splitCount);
	this->cpZoneTimes.SetSize(courseDesc->checkpointCount);
	this->stageZoneTimes.SetSize(courseDesc->stageCount);

	this->splitZoneTimes.FillWithValue(invalidTime);
	this->cpZoneTimes.FillWithValue(invalidTime);
	this->stageZoneTimes.FillWithValue(invalidTime);

	// Print course change message if needed
	if (this->currentCourseGUID != courseDesc->guid)
	{
		this->player->languageService->PrintChat(true, false, "Started Run on Course", courseDesc->name);
		// First run of a course on any map. We can use this to print out a one-time message if the map is not global.
		if (KZGlobalService::IsAvailable() && this->currentCourseGUID == 0)
		{
			KZGlobalService::WithCurrentMap(
				[&](const std::optional<KZ::api::Map> &map)
				{
					if (!map || map->state != KZ::api::Map::State::Approved)
					{
						this->player->languageService->PrintChat(true, false, "Non Global Map Warning");
					}
				});
		}
		else if (KZGlobalService::IsAvailable() && this->player->styleServices.Count() > 0)
		{
			bool ranked = false;
			KZGlobalService::WithCurrentMap(
				[&](const std::optional<KZ::api::Map> &map)
				{
					if (map && map->state == KZ::api::Map::State::Approved)
					{
						for (const auto &apiCourse : map->courses)
						{
							if (apiCourse.id == courseDesc->globalDatabaseID)
							{
								ranked = (KZ_STREQI(this->player->modeService->GetModeName(), "Classic")
										  && apiCourse.filters.classic.state == KZ::api::Map::Course::Filter::State::Ranked)
										 || (KZ_STREQI(this->player->modeService->GetModeName(), "Vanilla")
											 && apiCourse.filters.vanilla.state == KZ::api::Map::Course::Filter::State::Ranked);
								break;
							}
						}
						if (!ranked)
						{
							this->player->languageService->PrintChat(true, false, "Started Run on Non Ranked Course", courseDesc->name);
						}
					}
				});
		}
		SetCourse(courseDesc->guid);
	}

	this->validTime = true;
	this->shouldAnnounceMissedTime = true;
	this->shouldAnnounceMissedProTime = true;

	// Начало рана: без него run_stop/run_finish не с чем сопоставить по времени.
	KZ_LOG_INFO(LogChannel::Timer, "[cyb] run_start steam_id=%llu map=%s course=%s mode=%s\n", this->player->GetSteamId64(false),
				g_pKZUtils->GetCurrentMapName().Get(), courseDesc->name, this->player->modeService->GetModeName());

	this->UpdateCurrentCompareType(ToPBDataKey(KZ::mode::GetModeInfo(this->player->modeService).id, courseDesc->guid));

	if (playSound)
	{
		for (KZPlayer *spec = player->specService->GetNextSpectator(NULL); spec != NULL; spec = player->specService->GetNextSpectator(spec))
		{
			player->timerService->PlayTimerStartSound();
		}
		this->PlayTimerStartSound();
	}

	if (!this->player->IsAuthenticated())
	{
		this->player->languageService->PrintChat(true, false, "No Steam Authentication Warning");
	}
	if (KZGlobalService::IsAvailable() && !this->player->hasPrime)
	{
		this->player->languageService->PrintChat(true, false, "No Prime Warning");
	}

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnTimerStartPost(this->player, courseDesc->guid);
	}
	if (this->player->optionService->GetPreferenceBool("mapOverlay"))
	{
		// clang-format off
		this->player->PrintConsole(false, false, "[CS2KZ] timer_start|%s|%d|%d|%d|%s", 
			courseDesc->name,
			courseDesc->splitCount,
			courseDesc->checkpointCount,
			courseDesc->stageCount,
			this->player->modeService->GetModeShortName()
		);
		// clang-format on
	}
	return true;
}

bool KZTimerService::TimerEnd(const KZCourseDescriptor *courseDesc)
{
	// Все четыре пути отказа ниже сейчас сообщают игроку в чат и молчат в логах —
	// именно поэтому жалобу «мой ран не засчитался» нечем было разбирать.
	if (!this->player->IsAlive())
	{
		KZ_LOG_INFO(LogChannel::Timer, "[cyb] run_reject steam_id=%llu course=%s reason=not_alive\n", this->player->GetSteamId64(false),
					courseDesc->name);
		return false;
	}

	if (!this->timerRunning || courseDesc->guid != this->currentCourseGUID)
	{
		for (KZPlayer *spec = player->specService->GetNextSpectator(NULL); spec != NULL; spec = player->specService->GetNextSpectator(spec))
		{
			player->timerService->PlayTimerFalseEndSound();
		}
		this->PlayTimerFalseEndSound();
		this->lastFalseEndTime = g_pKZUtils->GetServerGlobals()->curtime;
		// Только wrong_course: подслучай «таймер не бежал» сыплется на каждый вход в
		// финишную зону (на bhop-картах игроки на ней стоят) и ничего не диагностирует.
		if (this->timerRunning)
		{
			KZ_LOG_INFO(LogChannel::Timer, "[cyb] run_reject steam_id=%llu course=%s reason=wrong_course\n", this->player->GetSteamId64(false),
						courseDesc->name);
		}
		return false;
	}

	if (this->currentStage != courseDesc->stageCount)
	{
		this->PlayMissedZoneSound();
		KZ_LOG_INFO(LogChannel::Timer, "[cyb] run_reject steam_id=%llu course=%s reason=missed_stage stage=%d/%d\n",
					this->player->GetSteamId64(false), courseDesc->name, this->currentStage + 1, courseDesc->stageCount);
		this->player->languageService->PrintChat(true, false, "Can't Finish Run (Missed Stage)", this->currentStage + 1);
		return false;
	}

	if (this->reachedCheckpoints != courseDesc->checkpointCount)
	{
		this->PlayMissedZoneSound();
		i32 missCount = courseDesc->checkpointCount - this->reachedCheckpoints;
		KZ_LOG_INFO(LogChannel::Timer, "[cyb] run_reject steam_id=%llu course=%s reason=missed_checkpoints missed=%d of=%d\n",
					this->player->GetSteamId64(false), courseDesc->name, missCount, courseDesc->checkpointCount);
		if (missCount == 1)
		{
			this->player->languageService->PrintChat(true, false, "Can't Finish Run (Missed a Checkpoint Zone)");
		}
		else
		{
			this->player->languageService->PrintChat(true, false, "Can't Finish Run (Missed Checkpoint Zones)", missCount);
		}
		return false;
	}

	f32 time = this->GetTime() + g_pKZUtils->GetServerGlobals()->frametime;
	u32 teleportsUsed = this->player->checkpointService->GetTeleportCount();

	bool allowEnd = true;
	FOR_EACH_VEC(eventListeners, i)
	{
		allowEnd &= eventListeners[i]->OnTimerEnd(this->player, this->currentCourseGUID, time, teleportsUsed);
	}
	if (!allowEnd)
	{
		KZ_LOG_INFO(LogChannel::Timer, "[cyb] run_reject steam_id=%llu course=%s reason=listener_denied\n", this->player->GetSteamId64(false),
					courseDesc->name);
		return false;
	}
	// Update current time for one last time.
	this->currentTime = time;

	this->timerRunning = false;
	this->lastEndTime = g_pKZUtils->GetServerGlobals()->curtime;

	// Финиш рана: ровно то, чего не хватало при жалобе «мой ран не засчитался».
	KZ_LOG_INFO(LogChannel::Timer, "[cyb] run_finish steam_id=%llu map=%s course=%s mode=%s style=%s time=%.3f tps=%u\n",
				this->player->GetSteamId64(false), g_pKZUtils->GetCurrentMapName().Get(), courseDesc->name, this->player->modeService->GetModeName(),
				this->player->styleServices.Count() > 0 ? this->player->styleServices[0]->GetStyleShortName() : "normal", time, teleportsUsed);

	for (KZPlayer *spec = player->specService->GetNextSpectator(NULL); spec != NULL; spec = player->specService->GetNextSpectator(spec))
	{
		player->timerService->PlayTimerEndSound();
	}
	this->PlayTimerEndSound();

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnTimerEndPost(this->player, this->currentCourseGUID, time, teleportsUsed);
	}
	// This must be called after OnTimerEndPost so that the run UUID is set correctly.
	if (this->player->optionService->GetPreferenceBool("mapOverlay"))
	{
		this->player->PrintConsole(false, false, "[CS2KZ] timer_end");
	}
	if (!this->player->GetPlayerPawn()->IsBot())
	{
		RunSubmission::Create(this->player);
		// Ран ушёл в инджест платформы (CybEmitter) — с малым лагом подтянуть NUB-очки/звание.
		this->player->profileService->OnRunFinished();
	}

	// Успешный финиш - ран завершён, сейв незавершённого рана по этому ключу больше не актуален.
	this->player->savedRunService->InvalidateCurrent("finish");

	return true;
}

bool KZTimerService::TimerStop(bool playSound, const char *reason)
{
	if (!this->timerRunning)
	{
		return false;
	}
	this->timerRunning = false;
	// Главный класс багрепортов — «таймер сбросился сам». Одна строка с причиной
	// закрывает разбор без воспроизведения. Событие редкое (раз на ран), не на тик.
	KZ_LOG_INFO(LogChannel::Timer, "[cyb] run_stop steam_id=%llu map=%s mode=%s time=%.3f reason=%s\n", this->player->GetSteamId64(false),
				g_pKZUtils->GetCurrentMapName().Get(), this->player->modeService->GetModeName(), this->GetTime(), reason);
	if (playSound)
	{
		for (KZPlayer *spec = player->specService->GetNextSpectator(NULL); spec != NULL; spec = player->specService->GetNextSpectator(spec))
		{
			spec->timerService->PlayTimerStopSound();
		}
		this->PlayTimerStopSound();
	}

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnTimerStopped(this->player, this->currentCourseGUID);
	}
	if (this->player->optionService->GetPreferenceBool("mapOverlay"))
	{
		this->player->PrintConsole(false, false, "[CS2KZ] timer_stop");
	}
	return true;
}

void KZTimerService::TimerStopAll(bool playSound, const char *reason)
{
	for (int i = 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (!player || !player->timerService)
		{
			continue;
		}
		player->timerService->TimerStop(playSound, reason);
	}
}

void KZTimerService::InvalidateJump()
{
	this->validJump = false;
	this->lastInvalidateTime = g_pKZUtils->GetServerGlobals()->curtime;
}

void KZTimerService::PlayTimerStartSound()
{
	if (g_pKZUtils->GetServerGlobals()->curtime - this->lastStartSoundTime > KZ_TIMER_SOUND_COOLDOWN)
	{
		utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_START);
		this->lastStartSoundTime = g_pKZUtils->GetServerGlobals()->curtime;
	}
}

void KZTimerService::InvalidateRun()
{
	if (!this->validTime)
	{
		return;
	}
	this->validTime = false;

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnTimerInvalidated(this->player);
	}
}

bool KZTimerService::HasValidMoveType()
{
	return KZTimerService::IsValidMoveType(this->player->GetMoveType());
}

bool KZTimerService::JustEndedTimer()
{
	return g_pKZUtils->GetServerGlobals()->curtime - this->lastEndTime > 1.0f;
}

void KZTimerService::PlayTimerEndSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_END);
}

void KZTimerService::PlayTimerFalseEndSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_FALSE_END);
}

void KZTimerService::PlayMissedZoneSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_MISSED_ZONE);
}

void KZTimerService::PlayReachedSplitSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_REACH_SPLIT);
}

void KZTimerService::PlayReachedCheckpointSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_REACH_CHECKPOINT);
}

void KZTimerService::PlayReachedStageSound()
{
	utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_REACH_STAGE);
}

void KZTimerService::PlayTimerStopSound()
{
	if (this->shouldPlayTimerStopSound)
	{
		utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_STOP);
	}
}

void KZTimerService::PlayMissedTimeSound()
{
	if (g_pKZUtils->GetServerGlobals()->curtime - this->lastMissedTimeSoundTime > KZ_TIMER_SOUND_COOLDOWN)
	{
		utils::PlaySoundToClient(this->player->GetPlayerSlot(), KZ_TIMER_SND_MISSED_TIME);
		this->lastMissedTimeSoundTime = g_pKZUtils->GetServerGlobals()->curtime;
	}
}

static_function std::string GetTeleportCountText(int tpCount, const char *language)
{
	return tpCount == 1 ? KZLanguageService::PrepareMessageWithLang(language, "1 Teleport Text")
						: KZLanguageService::PrepareMessageWithLang(language, "2+ Teleports Text", tpCount);
}

void KZTimerService::Pause()
{
	if (!this->CanPause(true))
	{
		return;
	}
	this->ForcePause();
}

void KZTimerService::ForcePause()
{
	bool allowPause = true;
	FOR_EACH_VEC(eventListeners, i)
	{
		allowPause &= eventListeners[i]->OnPause(this->player);
	}
	if (!allowPause)
	{
		this->player->languageService->PrintChat(true, false, "Can't Pause (Generic)");
		this->player->PlayErrorSound();
		return;
	}

	this->paused = true;
	this->pausedOnLadder = this->player->GetMoveType() == MOVETYPE_LADDER;
	this->lastDuckValue = this->player->GetMoveServices()->m_flDuckAmount;
	this->lastStaminaValue = this->player->GetMoveServices()->m_flStamina;
	this->player->SetVelocity(vec3_origin);
	this->player->SetMoveType(MOVETYPE_NONE);
	this->player->GetPlayerPawn()->SetGravityScale(0);

	if (this->GetTimerRunning())
	{
		this->hasPausedInThisRun = true;
		this->lastPauseTime = g_pKZUtils->GetServerGlobals()->curtime;
	}

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnPausePost(this->player);
	}
}

bool KZTimerService::CanPause(bool showError)
{
	if (this->paused)
	{
		return false;
	}
	if (this->JustLanded())
	{
		if (showError)
		{
			this->player->languageService->PrintChat(true, false, "Can't Pause (Just Landed)");
			this->player->PlayErrorSound();
		}
		return false;
	}
	if (this->player->triggerService->InAntiPauseArea())
	{
		if (showError)
		{
			this->player->languageService->PrintChat(true, false, "Can't Pause (Anti Pause Area)");
			this->player->PlayErrorSound();
		}
		return false;
	}

	Vector velocity;
	this->player->GetVelocity(&velocity);

	if (this->GetTimerRunning())
	{
		if (this->hasResumedInThisRun && g_pKZUtils->GetServerGlobals()->curtime - this->lastResumeTime < KZ_PAUSE_COOLDOWN)
		{
			if (showError)
			{
				this->player->languageService->PrintChat(true, false, "Can't Pause (Just Resumed)");
				this->player->PlayErrorSound();
			}
			return false;
		}
		else if (!(this->player->GetPlayerPawn()->m_fFlags & FL_ONGROUND) && !(velocity.Length2D() == 0.0f && velocity.z == 0.0f))
		{
			if (showError)
			{
				this->player->languageService->PrintChat(true, false, "Can't Pause (Midair)");
				this->player->PlayErrorSound();
			}
			return false;
		}
	}
	return true;
}

void KZTimerService::Resume(bool force)
{
	if (!this->paused)
	{
		return;
	}
	if (!force && !this->CanResume(true))
	{
		return;
	}

	bool allowResume = true;
	FOR_EACH_VEC(eventListeners, i)
	{
		allowResume &= eventListeners[i]->OnResume(this->player);
	}
	if (!allowResume)
	{
		this->player->languageService->PrintChat(true, false, "Can't Resume (Generic)");
		this->player->PlayErrorSound();
		return;
	}

	if (this->pausedOnLadder)
	{
		this->player->SetMoveType(MOVETYPE_LADDER);
	}
	else
	{
		this->player->SetMoveType(MOVETYPE_WALK);
	}

	// GOKZ: prevent noclip exploit
	this->player->GetPlayerPawn()->m_Collision().m_CollisionGroup() = KZ_COLLISION_GROUP_STANDARD;
	this->player->GetPlayerPawn()->CollisionRulesChanged();

	this->paused = false;
	if (this->GetTimerRunning())
	{
		this->hasResumedInThisRun = true;
		this->lastResumeTime = g_pKZUtils->GetServerGlobals()->curtime;
	}
	this->player->GetMoveServices()->m_flDuckAmount = this->lastDuckValue;
	this->player->GetMoveServices()->m_flStamina = this->lastStaminaValue;

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnResumePost(this->player);
	}
}

bool KZTimerService::CanResume(bool showError)
{
	if (this->GetTimerRunning() && this->hasPausedInThisRun && g_pKZUtils->GetServerGlobals()->curtime - this->lastPauseTime < KZ_PAUSE_COOLDOWN)
	{
		if (showError)
		{
			this->player->languageService->PrintChat(true, false, "Can't Resume (Just Paused)");
			this->player->PlayErrorSound();
		}
		return false;
	}
	return true;
}

void KZTimerService::TogglePause()
{
	if (!this->player->IsAlive())
	{
		KZ::misc::JoinTeam(player, CS_TEAM_CT);
		return;
	}
	if (this->paused)
	{
		this->Resume();
		return;
	}
	// Пауза без запущенного таймера бессмысленна (игрок просто замирает) — запрещаем.
	// Гард именно здесь, а не в CanPause(): на CanPause завязан spectate-флоу (kz_misc.cpp).
	if (!this->GetTimerRunning())
	{
		this->player->languageService->PrintChat(true, false, "Can't Pause (Timer Not Running)");
		this->player->PlayErrorSound();
		return;
	}
	this->Pause();
}

SCMD(kz_timerstopsound, SCFL_TIMER | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->timerService->ToggleTimerStopSound();
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_tss, kz_timerstopsound);

void KZTimerService::ToggleTimerStopSound()
{
	this->shouldPlayTimerStopSound = !this->shouldPlayTimerStopSound;
	this->player->optionService->SetPreferenceBool("timerStopSound", this->shouldPlayTimerStopSound);
	this->player->languageService->PrintChat(true, false, this->shouldPlayTimerStopSound ? "Timer Stop Sound Enabled" : "Timer Stop Sound Disabled");
}

// Safeguard — два независимых предохранителя.
// «Блок телепортов» (PRO, преф "sgTeleport", команда !pro) — гейтит только чекпоинт-ТП, ран
// остаётся PRO; рестарт/noclip при этом разрешены. «Блок сброса таймера» (преф "sgReset",
// команда !sg) — гейтит noclip/рестарт/стоп/!end/!lj/уход в спектатор. Раньше это был один
// int-преф "safeguard" (DISABLED/NUB/PRO), где PRO блокировал И ТП, И сброс, а NUB — только
// сброс. Теперь расцеплено. Старый преф читается как фолбэк (см. GetSafeguard*): пока игрок не
// трогал новую команду, поведение выводится из старого значения — настройки не теряются.

bool KZTimerService::GetSafeguardTeleport()
{
	i64 v = this->player->optionService->GetPreferenceInt("sgTeleport", -1);
	if (v >= 0)
	{
		return v != 0;
	}
	// Фолбэк на старый единый преф: телепорты блокировал только PRO.
	return this->player->optionService->GetPreferenceInt("safeguard", SAFEGUARD_DISABLED) == SAFEGUARD_PRO;
}

bool KZTimerService::GetSafeguardReset()
{
	i64 v = this->player->optionService->GetPreferenceInt("sgReset", -1);
	if (v >= 0)
	{
		return v != 0;
	}
	// Фолбэк: сброс таймера блокировали И NUB, И PRO.
	i64 legacy = this->player->optionService->GetPreferenceInt("safeguard", SAFEGUARD_DISABLED);
	return legacy == SAFEGUARD_NUB || legacy == SAFEGUARD_PRO;
}

void KZTimerService::ToggleSafeguard()
{
	// !sg — независимый флаг «блок сброса таймера» (noclip/рестарт/стоп/инвалидаторы).
	bool enabled = !this->GetSafeguardReset();
	this->player->optionService->SetPreferenceInt("sgReset", enabled ? 1 : 0);
	this->player->languageService->PrintChat(true, false, enabled ? "Safeguard - Enable" : "Safeguard - Disable");
}

void KZTimerService::ToggleProSafeguard()
{
	// !pro — независимый флаг «блок телепортов» (ран остаётся PRO); рестарт/noclip не трогает.
	bool enabled = !this->GetSafeguardTeleport();
	this->player->optionService->SetPreferenceInt("sgTeleport", enabled ? 1 : 0);
	this->player->languageService->PrintChat(true, false, enabled ? "Safeguard - Enable (PRO)" : "Safeguard PRO - Disable");
}

bool KZTimerService::CheckSafeguard(ResetConfirmAction action, bool showError, bool wantConfirm)
{
	// Индекс приходит от вызывающего — за границу массива уходить нельзя.
	Assert(action < RESET_CONFIRM_COUNT);
	if (action >= RESET_CONFIRM_COUNT)
	{
		return true;
	}
	// Защищать нечего: рана нет или он уже невалиден.
	if (!this->GetTimerRunning() || !this->GetValidTimer())
	{
		return true;
	}
	// Гейт «сброса таймера»: только флаг sgReset (!sg). Телепорты сюда НЕ относятся.
	if (this->GetSafeguardReset())
	{
		// Единственное исключение — окно свободного рестарта в самом начале рана.
		if (action == RESET_CONFIRM_RESTART && this->GetTime() < KZ_SAFEGUARD_RESTART_FREE_WINDOW)
		{
			// Незакрытое подтверждение не должно пережить окно и «засчитаться» первым
			// нажатием после него.
			this->resetConfirmTime[action] = 0.0;
			return true;
		}
		if (showError)
		{
			this->player->languageService->PrintChat(true, false, "Safeguard - Blocked");
			this->player->PlayErrorSound();
		}
		return false;
	}

	// !sg ВЫКЛЮЧЕН — сброс разрешён, но длинный ран не должен умирать от одного случайного
	// нажатия (noclip-бинд, смена команды, !end).
	// Порог по ИГРОВОМУ времени рана (GetTime), а не realtime: пауза длину забега не растит.
	if (!wantConfirm || this->GetTime() < KZ_RESET_CONFIRM_MIN_RUNTIME)
	{
		return true;
	}

	const f64 now = g_pKZUtils->GetServerGlobals()->realtime;
	const f64 pending = this->resetConfirmTime[action];
	const f64 elapsed = now - pending;
	// elapsed >= 0 обязателен: без него откат часов назад (смена карты обнуляет отсчёт, а
	// отметка живёт до дисконнекта) делал бы условие истинным и пропускал сброс молча, без
	// предупреждения — fail-open ровно в том сценарии, от которого защита и нужна.
	if (pending > 0.0 && elapsed >= 0.0 && elapsed <= KZ_RESET_CONFIRM_WINDOW)
	{
		// Повтор того же действия в окне — подтверждено. Защиту сразу взводим заново, чтобы
		// следующий сброс в этом же ране опять спросил.
		this->resetConfirmTime[action] = 0.0;
		return true;
	}
	// Либо первое нажатие, либо окно истекло — предупреждаем и (пере)взводим защиту.
	// Отметка ставится даже при showError=false: иначе тихий вызывающий сжёг бы ран, не
	// показав игроку ничего.
	this->resetConfirmTime[action] = now;
	if (showError)
	{
		this->player->languageService->PrintChat(true, false, "Reset Confirm - Warning");
		this->player->PlayErrorSound();
	}
	return false;
}

bool KZTimerService::CheckSafeguardPro(bool showError)
{
	// Гейт телепорта: только флаг sgTeleport (!pro). После первого ТП ран уже не PRO — не гейтим.
	if (!this->GetSafeguardTeleport() || !this->GetTimerRunning() || !this->GetValidTimer()
		|| this->player->checkpointService->GetTeleportCount() > 0)
	{
		return true;
	}
	if (showError)
	{
		this->player->languageService->PrintChat(true, false, "Safeguard - Blocked (PRO)");
		this->player->PlayErrorSound();
	}
	return false;
}

SCMD(kz_safeguard, SCFL_TIMER | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->timerService->ToggleSafeguard();
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_safe, kz_safeguard);
SCMD_LINK(kz_sg, kz_safeguard, SCFL_HELP);

SCMD(kz_pro, SCFL_TIMER | SCFL_PREFERENCE | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->timerService->ToggleProSafeguard();
	return MRES_SUPERCEDE;
}

void KZTimerService::Reset()
{
	this->timerRunning = {};
	this->currentTime = {};
	this->currentCourseGUID = 0;
	this->lastEndTime = {};
	this->lastFalseEndTime = {};
	this->lastStartSoundTime = {};
	this->lastMissedTimeSoundTime = {};
	this->validTime = {};
	this->paused = {};
	this->pausedOnLadder = {};
	this->lastPauseTime = {};
	this->hasPausedInThisRun = {};
	this->lastResumeTime = {};
	this->hasResumedInThisRun = {};
	this->lastDuckValue = {};
	this->lastStaminaValue = {};
	this->validJump = {};
	this->lastInvalidateTime = {};
	this->touchedGroundSinceTouchingStartZone = {};
	this->shouldPlayTimerStopSound = true;
	V_memset(this->resetConfirmTime, 0, sizeof(this->resetConfirmTime));
	// Гигиена: залипший в true флаг тихо отключил бы DropFrozenRun("death") в OnPlayerDeath.
	this->changingTeam = {};
}

void KZTimerService::OnPhysicsSimulatePost()
{
	if (this->player->IsAlive() && this->GetTimerRunning() && !this->GetPaused())
	{
		this->currentTime += ENGINE_FIXED_TICK_INTERVAL;
		this->CheckMissedTime();
	}
}

void KZTimerService::OnStartTouchGround()
{
	this->touchedGroundSinceTouchingStartZone = true;
}

void KZTimerService::OnStopTouchGround()
{
	if (this->HasValidMoveType() && this->lastInvalidateTime != g_pKZUtils->GetServerGlobals()->curtime)
	{
		this->validJump = true;
	}
	else
	{
		this->InvalidateJump();
	}
}

void KZTimerService::OnChangeMoveType(MoveType_t oldMoveType)
{
	if (oldMoveType == MOVETYPE_LADDER && this->player->GetMoveType() == MOVETYPE_WALK
		&& this->lastInvalidateTime != g_pKZUtils->GetServerGlobals()->curtime)
	{
		this->validJump = true;
	}
	else
	{
		this->InvalidateJump();
	}
	// Check if player has escaped MOVETYPE_NONE
	if (!this->paused || this->player->GetMoveType() == MOVETYPE_NONE)
	{
		return;
	}

	this->paused = false;
	if (this->GetTimerRunning())
	{
		this->hasResumedInThisRun = true;
		this->lastResumeTime = g_pKZUtils->GetServerGlobals()->curtime;
	}

	FOR_EACH_VEC(eventListeners, i)
	{
		eventListeners[i]->OnResumePost(this->player);
	}
}

void KZTimerService::OnTeleportToStart()
{
	// Инвалидация ДО TimerStop - курс ещё известен. Гейт на бегущий таймер:
	// повторный !r после финиша/стопа не должен слать пустой DELETE.
	if (this->GetTimerRunning())
	{
		this->player->savedRunService->InvalidateCurrent("teleport_to_start");
	}
	this->TimerStop(true, "teleport_to_start");
}

void KZTimerService::OnClientDisconnect()
{
	// Персист незавершённого рана (транш 3) — до TimerStop, пока состояние живо.
	this->player->savedRunService->SaveOnDisconnect();
	this->TimerStop(true, "disconnect");
}

void KZTimerService::OnPlayerSpawn()
{
	if (this->player->GetPlayerPawn() && this->paused)
	{
		// Player has left paused state by spawning in, so resume
		this->paused = false;
		if (this->GetTimerRunning())
		{
			this->hasResumedInThisRun = true;
			this->lastResumeTime = g_pKZUtils->GetServerGlobals()->curtime;
		}
		this->player->GetMoveServices()->m_flDuckAmount = this->lastDuckValue;
		this->player->GetMoveServices()->m_flStamina = this->lastStaminaValue;

		FOR_EACH_VEC(eventListeners, i)
		{
			eventListeners[i]->OnResumePost(this->player);
		}
	}

	// Возврат из спектатора: если игрок был в prac — вернуть ноуклип.
	this->player->pracService->OnPlayerSpawn();

	// Восстановление персист-рана (транш 3): один раз за сессию на карте. Вызываем ПОСЛЕ
	// авто-снятия паузы выше — не конфликтует, т.к. restoreAttempted защищает от повторного
	// восстановления на следующих спаунах, а само применение снапшота ставит паузу заново.
	// Пока игрок в prac, замороженный ран держит KZPracService — восстановление из БД
	// его затрёт.
	if (!this->player->pracService->IsInPrac())
	{
		this->player->savedRunService->TryRestoreOnSpawn();
	}
}

void KZTimerService::OnPlayerJoinTeam(i32 team)
{
	if (team == CS_TEAM_SPECTATOR)
	{
		this->paused = true;
		if (this->GetTimerRunning())
		{
			this->hasPausedInThisRun = true;
			this->lastPauseTime = g_pKZUtils->GetServerGlobals()->curtime;
		}

		FOR_EACH_VEC(eventListeners, i)
		{
			eventListeners[i]->OnPausePost(this->player);
		}
	}
}

void KZTimerService::OnPlayerDeath()
{
	// Только реальная смерть. changingTeam гейтит весь интервал JoinTeam (ChangeTeam/
	// CommitSuicide/SwitchTeam) - ревью нашло, что CommitSuicide в ветке CT<->T стреляет ровно
	// этот player_death ДО смены команды, так что одна лишь проверка живой команды не спасает.
	if (!this->changingTeam)
	{
		i32 team = this->player->GetController() ? this->player->GetController()->GetTeam() : CS_TEAM_NONE;
		if (team == CS_TEAM_CT || team == CS_TEAM_T)
		{
			this->player->pracService->DropFrozenRun("death");
		}
	}
	// Тот же флаг уточняет причину в логе: CommitSuicide из JoinTeam приходит сюда ДО
	// TimerStop("team_change") в самой JoinTeam, а лог забирает первый стоп (повторный —
	// no-op на !timerRunning), иначе смена команды писалась бы как reason=death.
	this->TimerStop(true, this->changingTeam ? "team_change" : "death");
}

void KZTimerService::OnRoundStart()
{
	// Рестарт раунда сбрасывает мир, поэтому по конвенции проекта убивает все активные раны
	// (TimerStopAll ниже). Замороженный prac-ран — тот же ран, только его состояние держит
	// KZPracService, и живой таймер у него уже остановлен — значит TimerStopAll до него не
	// достанет. Убираем симметрично, иначе игрок вернулся бы в ран, чей мир уже сброшен.
	KZPracService::DropFrozenRunAll("round_start");
	KZTimerService::TimerStopAll(true, "round_start");
}

void KZTimerService::OnTeleport(const Vector *newPosition, const QAngle *newAngles, const Vector *newVelocity)
{
	if (newPosition || newVelocity)
	{
		this->InvalidateJump();
	}
}

SCMD(kz_stop, SCFL_TIMER | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (player->timerService->GetTimerRunning())
	{
		if (!player->timerService->CheckSafeguard(RESET_CONFIRM_OTHER))
		{
			return MRES_SUPERCEDE;
		}
		player->savedRunService->InvalidateCurrent("stop");
		player->timerService->TimerStop(true, "stop");
	}
	return MRES_SUPERCEDE;
}

SCMD(kz_pause, SCFL_TIMER | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->timerService->TogglePause();
	return MRES_SUPERCEDE;
}

SCMD(kz_comparelevel, SCFL_RECORD | SCFL_TIMER | SCFL_PREFERENCE | SCFL_GLOBAL)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->timerService->SetCompareTarget(args->Arg(1));
	return MRES_SUPERCEDE;
}

static_function KZTimerService::CompareType GetCompareTypeFromString(const char *typeString)
{
	if (V_stricmp("off", typeString) == 0 || V_stricmp("none", typeString) == 0)
	{
		return KZTimerService::CompareType::COMPARE_NONE;
	}
	if (V_stricmp("spb", typeString) == 0)
	{
		return KZTimerService::CompareType::COMPARE_SPB;
	}
	if (V_stricmp("gpb", typeString) == 0 || V_stricmp("pb", typeString) == 0)
	{
		return KZTimerService::CompareType::COMPARE_GPB;
	}
	if (V_stricmp("sr", typeString) == 0)
	{
		return KZTimerService::CompareType::COMPARE_SR;
	}
	if (V_stricmp("wr", typeString) == 0)
	{
		return KZTimerService::CompareType::COMPARE_WR;
	}
	return KZTimerService::CompareType::COMPARETYPE_COUNT;
}

void KZTimerService::SetCompareTarget(const char *typeString)
{
	if (!typeString || !V_stricmp("", typeString))
	{
		this->player->languageService->PrintChat(true, false, "Compare Command Usage");
		return;
	}

	CompareType type = GetCompareTypeFromString(typeString);
	if (type == COMPARETYPE_COUNT)
	{
		this->player->languageService->PrintChat(true, false, "Compare Command Usage");
		return;
	}

	assert(type < COMPARETYPE_COUNT && type >= COMPARE_NONE);
	switch (type)
	{
		case COMPARE_NONE:
		{
			this->player->languageService->PrintChat(true, false, "Compare Disabled");
			break;
		}
		case COMPARE_SPB:
		{
			this->player->languageService->PrintChat(true, false, "Compare Server PB");
			break;
		}
		case COMPARE_GPB:
		{
			this->player->languageService->PrintChat(true, false, "Compare Global PB");
			break;
		}
		case COMPARE_SR:
		{
			this->player->languageService->PrintChat(true, false, "Compare Server Record");
			break;
		}
		case COMPARE_WR:
		{
			this->player->languageService->PrintChat(true, false, "Compare World Record");
			break;
		}
	}
	this->preferredCompareType = type;
	this->player->optionService->SetPreferenceInt("preferredCompareType", this->preferredCompareType);
	if (this->GetCourse())
	{
		this->UpdateCurrentCompareType(ToPBDataKey(KZ::mode::GetModeInfo(this->player->modeService).id, this->GetCourse()->guid));
	}
}

void KZTimerService::UpdateCurrentCompareType(PBDataKey key)
{
	for (u8 type = this->preferredCompareType; type > COMPARE_NONE; type--)
	{
		if (this->GetCompareTargetForType((CompareType)type, key))
		{
			this->currentCompareType = (CompareType)type;
			return;
		}
	}
	this->currentCompareType = COMPARE_NONE;
}

const PBData *KZTimerService::GetCompareTargetForType(CompareType type, PBDataKey key)
{
	switch (type)
	{
		case COMPARE_WR:
		{
			if (KZTimerService::wrCache.find(key) != KZTimerService::wrCache.end())
			{
				return &KZTimerService::wrCache[key];
			}
			break;
		}
		case COMPARE_SR:
		{
			if (KZTimerService::srCache.find(key) != KZTimerService::srCache.end())
			{
				return &KZTimerService::srCache[key];
			}
			break;
		}
		case COMPARE_GPB:
		{
			if (KZTimerService::globalPBCache.find(key) != KZTimerService::globalPBCache.end())
			{
				return &this->globalPBCache[key];
			}
			break;
		}
		case COMPARE_SPB:
		{
			if (KZTimerService::localPBCache.find(key) != KZTimerService::localPBCache.end())
			{
				return &this->localPBCache[key];
			}
			break;
		}
	}
	return nullptr;
}

const PBData *KZTimerService::GetCompareTarget(PBDataKey key)
{
	switch (this->currentCompareType)
	{
		case COMPARE_WR:
		{
			if (KZTimerService::wrCache.find(key) != KZTimerService::wrCache.end())
			{
				return &KZTimerService::wrCache[key];
			}
			break;
		}
		case COMPARE_SR:
		{
			if (KZTimerService::srCache.find(key) != KZTimerService::srCache.end())
			{
				return &KZTimerService::srCache[key];
			}
			break;
		}
		case COMPARE_GPB:
		{
			if (KZTimerService::globalPBCache.find(key) != KZTimerService::globalPBCache.end())
			{
				return &this->globalPBCache[key];
			}
			break;
		}
		case COMPARE_SPB:
		{
			if (KZTimerService::localPBCache.find(key) != KZTimerService::localPBCache.end())
			{
				return &this->localPBCache[key];
			}
			break;
		}
	}
	return nullptr;
}

bool KZTimerService::GetHudPBTime(f64 &outTime, const KZCourseDescriptor *course)
{
	if (!course)
	{
		course = this->GetCourse();
	}
	if (!course)
	{
		return false;
	}
	auto modeInfo = KZ::mode::GetModeInfo(this->player->modeService->GetModeName());
	// ЛУЧШЕЕ из двух источников, а НЕ «платформенный, иначе локальный» (правка 18.08 по
	// багрепорту «в худе PB/WR замерли с загрузки карты, а !pb/!wr правы»).
	// Почему так: платформенный кэш наполняется ОДИН раз, на загрузке карты (периодический
	// таймер не работает — отдельная задача), а локальный обновляется из БД на КАЖДОМ финише
	// (RunSubmission::UpdateLocalCache). Прежний порядок «платформенный первым» на этом и ломался:
	// найдя непустое значение с загрузки карты, худ до свежего локального просто не доходил, и
	// рекорд, поставленный при живом игроке, в худ не попадал до смены карты. Минимум снимает
	// зависимость и от таймера, и от сети: кто из источников знает время лучше, тот и прав.
	// Известный компромисс: удалённый рекорд, о котором платформа ещё не знает, останется
	// видимым до смены карты — ровно как и до этой правки (кэш платформы не умеет забывать).
	f64 best = 0.0;
	i32 modeIdx = ApiModeToIndex(CybReplayCommon::MapMode(std::string(modeInfo.shortModeName.Get(), modeInfo.shortModeName.Length())));
	if (modeIdx >= 0)
	{
		const i32 cyberCourse = KZ::course::GetCyberCourseNumber(course);
		auto it = this->platformPbCache.find(ToPlatformKey(modeIdx, cyberCourse));
		if (it != this->platformPbCache.end() && it->second > 0)
		{
			best = it->second;
		}
		else
		{
			KZTimerService::NoteHudPlatformMiss(false, modeInfo.shortModeName.Get(), modeIdx, cyberCourse, this->platformPbCache.size());
		}
	}
	else
	{
		KZTimerService::NoteHudPlatformMiss(false, modeInfo.shortModeName.Get(), modeIdx, -1, this->platformPbCache.size());
	}
	// В МИНИМУМ идёт только НАШ источник — localPBCache (COMPARE_SPB), он наполняется из общей
	// MySQL нашей сети. globalPBCache (COMPARE_GPB) — PB из ЧУЖОЙ сети cs2kz (api.cs2kz.org,
	// KZGlobalService), и пускать его в минимум нельзя: он почти всегда меньше нашего, и худ
	// показывал бы число, которого нет ни в !pb, ни в лидерборде сайта. Сейчас он пуст только
	// потому, что apiUrl в gameops пустой — то есть инвариант держался бы строкой конфига в
	// другом репозитории. Оставлен КРАЙНИМ фолбэком (как и до правки 18.08): если своего нет
	// вовсе, показать чужое лучше, чем прочерк.
	// platformHit фиксируем ДО сравнения: «наше побило платформенное» и «платформа по этому
	// ключу не знала ничего» — разные события, и смешивать их в одном счётчике нельзя. Ценна
	// именно первая: она и означает, что платформенный кэш устарел.
	const bool platformHitPb = best > 0;
	PBDataKey key = ToPBDataKey(modeInfo.id, course->guid);
	const PBData *ourPb = this->GetCompareTargetForType(COMPARE_SPB, key);
	if (ourPb && ourPb->overall.pbTime > 0 && (best <= 0 || ourPb->overall.pbTime < best))
	{
		best = ourPb->overall.pbTime;
		if (platformHitPb)
		{
			KZTimerService::NoteHudLocalWin(false);
		}
	}
	if (best <= 0)
	{
		const PBData *globalPb = this->GetCompareTargetForType(COMPARE_GPB, key);
		if (globalPb && globalPb->overall.pbTime > 0)
		{
			best = globalPb->overall.pbTime;
		}
	}
	if (best <= 0)
	{
		return false;
	}
	outTime = best;
	return true;
}

bool KZTimerService::GetHudWorldRecordTime(f64 &outTime, const KZCourseDescriptor *course)
{
	if (!course)
	{
		course = this->GetCourse();
	}
	if (!course)
	{
		return false;
	}
	auto modeInfo = KZ::mode::GetModeInfo(this->player->modeService->GetModeName());
	// ЛУЧШЕЕ из двух источников — обоснование целиком в GetHudPBTime выше (та же правка 18.08).
	// Коротко: платформенный кэш наполняется один раз на загрузке карты, локальный (srCache/
	// wrCache через UpdateLocalRecordCache) — на каждом финише из БД; «платформенный первым»
	// прятал свежий рекорд до смены карты.
	f64 best = 0.0;
	i32 modeIdx = ApiModeToIndex(CybReplayCommon::MapMode(std::string(modeInfo.shortModeName.Get(), modeInfo.shortModeName.Length())));
	if (modeIdx >= 0)
	{
		const i32 cyberCourse = KZ::course::GetCyberCourseNumber(course);
		auto it = KZTimerService::platformWrCache.find(ToPlatformKey(modeIdx, cyberCourse));
		if (it != KZTimerService::platformWrCache.end() && it->second > 0)
		{
			best = it->second;
		}
		else
		{
			KZTimerService::NoteHudPlatformMiss(true, modeInfo.shortModeName.Get(), modeIdx, cyberCourse, KZTimerService::platformWrCache.size());
		}
	}
	else
	{
		KZTimerService::NoteHudPlatformMiss(true, modeInfo.shortModeName.Get(), modeIdx, -1, KZTimerService::platformWrCache.size());
	}
	// В МИНИМУМ идёт только НАШ источник — srCache (COMPARE_SR), рекорд нашей сети из общей
	// MySQL: семантически то же, что платформенный kz_records. wrCache (COMPARE_WR) — рекорд
	// ЧУЖОЙ сети cs2kz, в минимуме он побеждал бы почти всегда, и худ расходился бы с !wr,
	// !maptop и сайтом. Оставлен крайним фолбэком, как и до правки 18.08. Подробнее — в
	// GetHudPBTime выше.
	const bool platformHitWr = best > 0; // см. GetHudPBTime: считаем только реальную победу
	PBDataKey key = ToPBDataKey(modeInfo.id, course->guid);
	const PBData *ourWr = this->GetCompareTargetForType(COMPARE_SR, key);
	if (ourWr && ourWr->overall.pbTime > 0 && (best <= 0 || ourWr->overall.pbTime < best))
	{
		best = ourWr->overall.pbTime;
		if (platformHitWr)
		{
			KZTimerService::NoteHudLocalWin(true);
		}
	}
	if (best <= 0)
	{
		const PBData *globalWr = this->GetCompareTargetForType(COMPARE_WR, key);
		if (globalWr && globalWr->overall.pbTime > 0)
		{
			best = globalWr->overall.pbTime;
		}
	}
	if (best <= 0)
	{
		return false;
	}
	outTime = best;
	return true;
}

void KZTimerService::ClearRecordCache()
{
	KZTimerService::srCache.clear();
	KZTimerService::wrCache.clear();
	KZTimerService::platformWrCache.clear();
	for (i32 i = 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (player && player->timerService)
		{
			player->timerService->ClearPBCache();
		}
	}
}

// Наблюдаемость тракта платформенных PB/WR. Заведена 18.08 по багрепорту «в худе PB/WR замерли
// на момент загрузки карты, лечит только перезаход или рестарт», при том что !wr/!pb/!maptop
// показывают верное. Замерами исключено: таймер живёт, api отвечает 200 и отдаёт то же, что
// локальная БД, код на флоте совпадает с текущим, худ читает геттеры каждый тик.
//
// Приборы стоят на ВСЕХ молчащих ветках тракта, а не на одной: «ответ 200» и «данные легли в
// кэш» — разные события, и первое достижимо при пустом records (api отдаёт 200 + [] для карты,
// которой нет в каталоге platform). Поэтому считаются ФАКТИЧЕСКИЕ записи в кэш (wr_set/pb_set),
// отдельно — причины пропуска, отдельно — промахи ЧТЕНИЯ в худе (там симптом объясняется так же:
// при промахе ключа худ уходит на локальный кэш, а тот грузится лишь на OnMapSetup).
struct PlatformIngestStats
{
	u32 fetchWr, fetchPb;             // выдано запросов
	u32 noBody, staleMap, noArray;    // ответ отброшен
	u32 slotReused;                   // PB-колбэк: слот занял другой игрок
	u32 recordsSeen;                  // элементов в массиве records
	u32 skipNoCourse, skipBadMode;    // пропуск записи
	u32 wrSet, pbSet;                 // ФАКТИЧЕСКИ записано в кэш
	u32 hudMissWr, hudMissPb;         // промах платформенного кэша при чтении в худе
	u32 hudLocalWinWr, hudLocalWinPb; // наш источник оказался ЛУЧШЕ НЕПУСТОГО платформенного
	bool warnedStale, warnedHudMiss, snapshotDone;
	// Имя карты, к которой относятся числа. Запоминается при ПЕРВОМ выданном запросе, а не
	// читается на печати: сводка prev_map печатается из OnMapSetup, когда GetCurrentMapName()
	// уже отдаёт НОВУЮ карту, и числа приписались бы не той карте.
	char map[64];
};

static_global PlatformIngestStats g_pis {};

// wrCacheSize параметром: сама функция файловая, а platformWrCache приватный —
// читают его вызывающие (они члены класса).
static_function void PrintPlatformIngestSummary(const char *tag, size_t wrCacheSize)
{
	// key=value, как остальные строки форка (bin/logs.sh и алерты собираются по полям).
	KZ_LOG_INFO(LogChannel::Timer,
				"[cyb_records] platform_ingest_%s map=%s fetch_wr=%u fetch_pb=%u wr_set=%u pb_set=%u records=%u "
				"stale_map=%u no_array=%u no_body=%u slot_reused=%u skip_no_course=%u skip_bad_mode=%u "
				"hud_miss_wr=%u hud_miss_pb=%u local_win_wr=%u local_win_pb=%u wr_cache=%zu\n",
				tag, g_pis.map, g_pis.fetchWr, g_pis.fetchPb, g_pis.wrSet, g_pis.pbSet, g_pis.recordsSeen, g_pis.staleMap, g_pis.noArray,
				g_pis.noBody, g_pis.slotReused, g_pis.skipNoCourse, g_pis.skipBadMode, g_pis.hudMissWr, g_pis.hudMissPb, g_pis.hudLocalWinWr,
				g_pis.hudLocalWinPb, wrCacheSize);
}

void KZTimerService::ResetPlatformIngestStats()
{
	// Итог ПРОШЛОЙ карты печатаем до обнуления: снимок в момент, когда всё уже случилось.
	if (g_pis.fetchWr || g_pis.fetchPb)
	{
		PrintPlatformIngestSummary("prev_map", KZTimerService::platformWrCache.size());
	}
	g_pis = PlatformIngestStats {};
}

// Снимок в пределах ОДНОЙ карты: без него прогон, после которого сервер рестартуют «чтобы
// полечить», не оставляет лога вовсе — а это как раз тот прогон, который нужен.
void KZTimerService::MaybeSnapshotPlatformIngest()
{
	if (g_pis.snapshotDone || (g_pis.fetchWr + g_pis.fetchPb) < 30)
	{
		return;
	}
	g_pis.snapshotDone = true;
	PrintPlatformIngestSummary("snapshot", KZTimerService::platformWrCache.size());
}

void KZTimerService::NotePlatformFetchIssued(bool isWr)
{
	if (g_pis.map[0] == '\0')
	{
		V_strncpy(g_pis.map, g_pKZUtils->GetCurrentMapName().Get(), sizeof(g_pis.map));
	}
	if (isWr)
	{
		g_pis.fetchWr++;
	}
	else
	{
		g_pis.fetchPb++;
	}
}

// Наш локальный источник оказался лучше платформенного — то есть минимум сработал и худ
// показал свежее значение. Это и метрика полезности правки 18.08, и косвенный признак того,
// что платформенный кэш устарел (периодическое обновление не работает — отдельная задача).
void KZTimerService::NoteHudLocalWin(bool isWr)
{
	if (isWr)
	{
		g_pis.hudLocalWinWr++;
	}
	else
	{
		g_pis.hudLocalWinPb++;
	}
}

void KZTimerService::NotePlatformRespNoBody()
{
	g_pis.noBody++;
}

void KZTimerService::NotePlatformSlotReused()
{
	g_pis.slotReused++;
}

// expected — имя карты на момент ОТПРАВКИ (захвачено лямбдой), current — на момент ответа.
// Обе половины обязательны: по одной нельзя отличить патологию (имена разные ВСЮ карту) от
// штатного отбоя на смене карты (expected = прошлая карта, ответ доехал уже после OnMapSetup).
void KZTimerService::NotePlatformIngestStaleMap(const char *expected, const char *current)
{
	if (!g_pis.warnedStale)
	{
		g_pis.warnedStale = true;
		KZ_LOG_WARN(LogChannel::Timer, "[cyb_records] resp_rejected reason=stale_map expected=%s current=%s\n", expected ? expected : "",
					current ? current : "");
	}
	g_pis.staleMap++;
}

// Промах ЧТЕНИЯ в худе: ключ не найден в платформенном кэше, значение берётся из локального.
// Один раз на карту — геттеры зовутся каждый тик.
// cacheSize — размер ИМЕННО того кэша, в котором промахнулись (WR общий, PB на игрока):
// иначе главный вопрос «данные не доехали или доехали, а ключ не совпал» строкой не решается.
void KZTimerService::NoteHudPlatformMiss(bool isWr, const char *modeShort, i32 modeIdx, i32 course, size_t cacheSize)
{
	if (isWr)
	{
		g_pis.hudMissWr++;
	}
	else
	{
		g_pis.hudMissPb++;
	}
	// warn — ТОЛЬКО когда кэш непуст, то есть данные доехали, а ключ не совпал: это патология.
	// Пустой кэш — штатная ситуация (новичок без PB на карте, курс/режим без рекорда), и warn
	// на ней занял бы единственный слот на карте ложной тревогой. Само «кэш пуст» видно в
	// сводке по wr_set/pb_set.
	if (g_pis.warnedHudMiss || cacheSize == 0)
	{
		return;
	}
	g_pis.warnedHudMiss = true;
	KZ_LOG_WARN(LogChannel::Timer, "[cyb_records] hud_lookup_miss kind=%s mode=%s mode_idx=%d course=%d cache=%zu\n", isWr ? "wr" : "pb",
				modeShort ? modeShort : "", modeIdx, course, cacheSize);
}

void KZTimerService::IngestPlatformRecords(const char *body, KZPlayer *pbPlayer)
{
	KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
	CUtlString error = "";
	LoadKV3FromJSON(&kv, &error, body, "");
	if (!error.IsEmpty())
	{
		KZ_LOG_WARN(LogChannel::Timer, "[cyb_records] failed to parse api response: %s\n", error.Get());
		return;
	}

	KeyValues3 *records = kv.FindMember("records");
	if (!records || records->GetType() != KV3_TYPE_ARRAY)
	{
		g_pis.noArray++;
		return; // нет массива records — трактуем как "данных нет", худ на фолбэке
	}

	int count = records->GetArrayElementCount();
	g_pis.recordsSeen += (u32)(count > 0 ? count : 0);
	for (int i = 0; i < count; i++)
	{
		KeyValues3 *rec = records->GetArrayElement(i);
		if (!rec)
		{
			continue;
		}
		// course (cyber-номер) — обязателен.
		f64 courseF = 0;
		if (!KV3ReadNumber(rec, "course", courseF))
		{
			g_pis.skipNoCourse++;
			continue;
		}
		// mode (api-строка) — обязателен и должен маппиться в поддерживаемый индекс.
		KeyValues3 *modeMember = rec->FindMember("mode");
		if (!modeMember || modeMember->GetType() != KV3_TYPE_STRING)
		{
			g_pis.skipBadMode++;
			continue;
		}
		i32 modeIdx = ApiModeToIndex(modeMember->GetString(""));
		if (modeIdx < 0)
		{
			g_pis.skipBadMode++;
			continue;
		}
		u64 key = ToPlatformKey(modeIdx, (i32)courseF);

		// WR — актуализируем всегда, когда присутствует (ms → секунды).
		f64 wrMs = 0;
		if (KV3ReadNumber(rec, "wrTimeMs", wrMs) && wrMs > 0)
		{
			KZTimerService::platformWrCache[key] = wrMs / 1000.0;
			g_pis.wrSet++;
		}
		// PB — только для player-level ответа (пришёл steamId64).
		if (pbPlayer)
		{
			f64 pbMs = 0;
			if (KV3ReadNumber(rec, "pbTimeMs", pbMs) && pbMs > 0)
			{
				pbPlayer->timerService->platformPbCache[key] = pbMs / 1000.0;
				g_pis.pbSet++;
			}
		}
	}
}

void KZTimerService::FetchPlatformWorldRecords(bool resetCache)
{
	const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
	if (!url || url[0] == '\0')
	{
		return; // платформенный источник выключен
	}
	const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");

	std::string mapName = g_pKZUtils->GetCurrentMapName().Get();
	if (!CybReplayCommon::IsValidMapName(mapName))
	{
		return; // api валидирует map — заведомо мимо, сеть не дёргаем
	}

	// Свежая карта — прежний WR-кэш недействителен (наполнится из ответа).
	// На ПЕРИОДИЧЕСКОМ обновлении (resetCache=false) чистить нельзя: между clear() и приходом
	// ответа кэш пуст, и худ каждые несколько секунд ронял бы строку WR на фолбэк и обратно —
	// заметное мигание. IngestPlatformRecords пишет только положительные значения поверх, так
	// что перезапись без очистки безопасна; исчезнувший рекорд подчистит смена карты.
	if (resetCache)
	{
		KZTimerService::platformWrCache.clear();
	}

	std::string fullUrl = url;
	if (!fullUrl.empty() && fullUrl.back() == '/')
	{
		fullUrl.pop_back();
	}
	fullUrl += "/ingest/v1/kz/records";

	HTTP::Request req(HTTP::Method::GET, fullUrl);
	req.SetQuery("map", mapName);
	if (token && token[0] != '\0')
	{
		req.SetHeader("Authorization", std::string("Bearer ") + token);
	}

	KZTimerService::NotePlatformFetchIssued(true);

	// clang-format off
	req.Send(
		[mapName](HTTP::Response resp)
		{
			if (resp.status < 200 || resp.status >= 300)
			{
				KZ_LOG_INFO(LogChannel::Timer, "[cyb_records] WR fetch HTTP %u\n", (unsigned)resp.status);
				return;
			}
			// Карта могла смениться, пока запрос летел — не засоряем кэш новой карты старыми данными.
			if (!KZ_STREQ(mapName.c_str(), g_pKZUtils->GetCurrentMapName().Get()))
			{
				KZTimerService::NotePlatformIngestStaleMap(mapName.c_str(), g_pKZUtils->GetCurrentMapName().Get());
				return;
			}
			std::optional<std::string> respBody = resp.Body();
			if (!respBody.has_value())
			{
				KZTimerService::NotePlatformRespNoBody();
				return;
			}
			KZTimerService::IngestPlatformRecords(respBody->c_str(), nullptr);
		},
		[]()
		{
			KZ_LOG_INFO(LogChannel::Timer, "[cyb_records] WR fetch network error\n");
		});
	// clang-format on
}

void KZTimerService::FetchPlatformPB(KZPlayer *player, bool resetCache)
{
	if (!player)
	{
		return;
	}
	// Свежий заход игрока (в т.ч. на переиспользованный слот) — платформенный PB начинаем с чистого
	// листа, чтобы не показать чужой PB прежнего владельца слота; наполнится из ответа на его steamId.
	// На периодическом обновлении (resetCache=false) — не чистим, см. FetchPlatformWorldRecords.
	if (resetCache)
	{
		player->timerService->platformPbCache.clear();
	}

	const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
	if (!url || url[0] == '\0')
	{
		return;
	}
	const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");

	std::string mapName = g_pKZUtils->GetCurrentMapName().Get();
	if (!CybReplayCommon::IsValidMapName(mapName))
	{
		return;
	}

	u64 steamID64 = player->GetSteamId64();
	if (steamID64 == 0 || !player->GetClient())
	{
		return; // не аутентифицирован / нет клиента — PB спросить не по кому
	}
	CPlayerUserId userID = player->GetClient()->GetUserID();
	KZTimerService::NotePlatformFetchIssued(false);

	std::string fullUrl = url;
	if (!fullUrl.empty() && fullUrl.back() == '/')
	{
		fullUrl.pop_back();
	}
	fullUrl += "/ingest/v1/kz/records";

	HTTP::Request req(HTTP::Method::GET, fullUrl);
	req.SetQuery("map", mapName);
	req.SetQuery("steamId64", std::to_string(steamID64));
	if (token && token[0] != '\0')
	{
		req.SetHeader("Authorization", std::string("Bearer ") + token);
	}

	// clang-format off
	req.Send(
		[mapName, userID, steamID64](HTTP::Response resp)
		{
			if (resp.status < 200 || resp.status >= 300)
			{
				KZ_LOG_INFO(LogChannel::Timer, "[cyb_records] PB fetch HTTP %u\n", (unsigned)resp.status);
				return;
			}
			if (!KZ_STREQ(mapName.c_str(), g_pKZUtils->GetCurrentMapName().Get()))
			{
				KZTimerService::NotePlatformIngestStaleMap(mapName.c_str(), g_pKZUtils->GetCurrentMapName().Get());
				return; // карта сменилась — PB относится к другой карте
			}
			KZPlayer *pl = g_pKZPlayerManager->ToPlayer(userID);
			// Гард переиспользования userID: слот мог освободиться и занять другой игрок.
			if (!pl || pl->GetSteamId64() != steamID64)
			{
				KZTimerService::NotePlatformSlotReused();
				return;
			}
			std::optional<std::string> respBody = resp.Body();
			if (!respBody.has_value())
			{
				KZTimerService::NotePlatformRespNoBody();
				return;
			}
			KZTimerService::IngestPlatformRecords(respBody->c_str(), pl);
		},
		[]()
		{
			KZ_LOG_INFO(LogChannel::Timer, "[cyb_records] PB fetch network error\n");
		});
	// clang-format on
}

// Периодическое обновление платформенных PB/WR (п.5 пакета 15.08). До этого WR тянулись один
// раз на карту (OnMapSetup), а PB — один раз на заход игрока (OnClientSetup), поэтому рекорд,
// поставленный кем-то на другом сервере сети, появлялся в худе только после реконнекта.
//
// Стоимость держим независимой от онлайна: PB — по игроку, но НЕ веером, а по одному игроку
// за тик (round-robin по индексам); WR — общий на сервер, одним запросом и только если в этом
// же обходе нашёлся живой не-бот (на пустом сервере не ходим в api вовсе). Иначе на полном
// сервере это давало бы до 2*N запросов каждые 3 секунды. Свой PB на этом сервере и так
// обновляется локально на финише — polling здесь закрывает только «поставил на соседнем».
static_global CTimer<> *g_platformRecordsTimer = nullptr;
// ИНДЕКС (не слот!), с которого продолжаем обход по кругу. Именно индексная перегрузка
// ToPlayer(u32) — она же players[index] — выбирается для i32 без явного каста: i32 -> u32 это
// стандартное преобразование, а i32 -> CPlayerSlot пользовательское. Отсюда и граница
// MAXPLAYERS + 1, как в ClearRecordCache. Про эти грабли «граница цикла ↔ перегрузка» в репо
// уже спотыкались (kz_zones.cpp, kz_mappingapi.cpp) — трогая цикл, сперва посмотри перегрузку.
static_global i32 g_platformPbRefreshIndex = 0;

static_function f64 RefreshPlatformRecords()
{
	// Один живой аутентифицированный игрок за тик, начиная со следующего после прошлого.
	// Заодно это ответ на вопрос «есть ли кому показывать»: пустой сервер не должен
	// круглосуточно долбить api ни PB-, ни WR-запросом.
	bool anyHuman = false;
	for (i32 step = 0; step < MAXPLAYERS + 1; step++)
	{
		i32 index = (g_platformPbRefreshIndex + step) % (MAXPLAYERS + 1);
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(index);
		// Те же предусловия, что внутри FetchPlatformPB (клиент есть, steamID резолвится) —
		// проверяем ЗДЕСЬ, иначе тик обхода тратился бы на пустой слот или бота, и на сервере
		// с ботами живые игроки обновлялись бы кратно реже. Пустой слот безопасен: GetClient()
		// там nullptr, а GetSteamId64() у неаутентифицированного отдаёт 0 (см. Player::GetSteamId).
		if (player && player->timerService && player->GetClient() && !player->IsFakeClient() && player->GetSteamId64() != 0)
		{
			anyHuman = true;
			KZTimerService::FetchPlatformPB(player, false);
			g_platformPbRefreshIndex = (index + 1) % (MAXPLAYERS + 1);
			break;
		}
	}

	// WR общий на сервер, но живым он нужен только когда есть кому смотреть в худ.
	if (anyHuman)
	{
		KZTimerService::FetchPlatformWorldRecords(false);
	}
	// Снимок в пределах карты (см. MaybeSnapshotPlatformIngest): иначе прогон, после которого
	// сервер рестартуют, не оставит лога — а нужен именно он.
	KZTimerService::MaybeSnapshotPlatformIngest();

	return KZ_PLATFORM_RECORDS_REFRESH_INTERVAL;
}

void KZTimerService::StartPlatformRecordsRefresh()
{
	if (g_platformRecordsTimer)
	{
		return; // таймер persistent (переживает смену карты) — заводим ровно один раз
	}
	// preserveMapChange=true: источник и интервал от карты не зависят, а пересоздание таймера на
	// каждой карте плодило бы дубликаты. useRealTime=true: обновление худа не должно замирать
	// вместе с игровым временем (пауза/смена карты).
	g_platformRecordsTimer = StartTimer(RefreshPlatformRecords, KZ_PLATFORM_RECORDS_REFRESH_INTERVAL, true, true);
}

void KZTimerService::UpdateLocalRecordCache()
{
	auto onQuerySuccess = [](std::vector<ISQLQuery *> queries)
	{
		ISQLResult *result = queries[0]->GetResultSet();
		if (result && result->GetRowCount() > 0)
		{
			while (result->FetchRow())
			{
				auto modeInfo = KZ::mode::GetModeInfoFromDatabaseID(result->GetInt(2));
				if (modeInfo.databaseID < 0)
				{
					continue;
				}
				const KZCourseDescriptor *course = KZ::course::GetCourseByLocalCourseID(result->GetInt(1));
				if (!course)
				{
					continue;
				}
				KZTimerService::InsertRecordToCache(result->GetFloat(0), course, modeInfo.id, true, false, result->GetString(3));
			}
		}
		result = queries[1]->GetResultSet();
		if (result && result->GetRowCount() > 0)
		{
			while (result->FetchRow())
			{
				auto modeInfo = KZ::mode::GetModeInfoFromDatabaseID(result->GetInt(2));
				if (modeInfo.databaseID < 0)
				{
					continue;
				}
				const KZCourseDescriptor *course = KZ::course::GetCourseByLocalCourseID(result->GetInt(1));
				if (!course)
				{
					continue;
				}
				KZTimerService::InsertRecordToCache(result->GetFloat(0), course, modeInfo.id, false, false, result->GetString(3));
			}
		}
	};
	KZDatabaseService::QueryAllRecords(g_pKZUtils->GetCurrentMapName(), onQuerySuccess, KZDatabaseService::OnGenericTxnFailure);
}

void KZTimerService::InsertRecordToCache(f64 time, const KZCourseDescriptor *course, PluginId modeID, bool overall, bool global, CUtlString metadata)
{
	PBData &pb = global ? KZTimerService::wrCache[ToPBDataKey(modeID, course->guid)] : KZTimerService::srCache[ToPBDataKey(modeID, course->guid)];

	overall ? pb.overall.pbTime = time : pb.pro.pbTime = time;
	KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
	CUtlString error = "";
	if (metadata.IsEmpty())
	{
		return;
	}
	LoadKV3FromJSON(&kv, &error, metadata.Get(), "");
	if (!error.IsEmpty())
	{
		KZ_LOG_WARN(LogChannel::Timer, "Failed to insert PB to cache due to metadata error: %s\n", error.Get());
		return;
	}

	KeyValues3 *data = kv.FindMember("splitZoneTimes");
	if (data && data->GetType() == KV3_TYPE_ARRAY)
	{
		for (i32 i = 0; i < course->splitCount; i++)
		{
			f64 time = -1.0f;
			KeyValues3 *element = data->GetArrayElement(i);
			if (element)
			{
				time = element->GetDouble(-1.0);
			}
			overall ? pb.overall.pbSplitZoneTimes[i] = time : pb.pro.pbSplitZoneTimes[i] = time;
		}
	}

	data = kv.FindMember("cpZoneTimes");
	if (data && data->GetType() == KV3_TYPE_ARRAY)
	{
		for (i32 i = 0; i < course->checkpointCount; i++)
		{
			f64 time = -1.0f;
			KeyValues3 *element = data->GetArrayElement(i);
			if (element)
			{
				time = element->GetDouble(-1.0);
			}
			overall ? pb.overall.pbCpZoneTimes[i] = time : pb.pro.pbCpZoneTimes[i] = time;
		}
	}

	data = kv.FindMember("stageZoneTimes");
	if (data && data->GetType() == KV3_TYPE_ARRAY)
	{
		for (i32 i = 0; i < course->stageCount; i++)
		{
			f64 time = -1.0f;
			KeyValues3 *element = data->GetArrayElement(i);
			if (element)
			{
				time = element->GetDouble(-1.0);
			}
			overall ? pb.overall.pbStageZoneTimes[i] = time : pb.pro.pbStageZoneTimes[i] = time;
		}
	}
}

void KZTimerService::ClearPBCache()
{
	this->localPBCache.clear();
	this->globalPBCache.clear();
	this->platformPbCache.clear();
}

const PBData *KZTimerService::GetGlobalCachedPB(const KZCourseDescriptor *course, PluginId modeID)
{
	PBDataKey key = ToPBDataKey(modeID, course->guid);

	if (this->globalPBCache.find(key) == this->globalPBCache.end())
	{
		return nullptr;
	}

	return &this->globalPBCache[key];
}

void KZTimerService::InsertPBToCache(f64 time, const KZCourseDescriptor *course, PluginId modeID, bool overall, bool global, CUtlString metadata,
									 f64 points)
{
	PBData &pb = global ? this->globalPBCache[ToPBDataKey(modeID, course->guid)] : this->localPBCache[ToPBDataKey(modeID, course->guid)];

	overall ? pb.overall.points = points : pb.pro.points = points;
	overall ? pb.overall.pbTime = time : pb.pro.pbTime = time;
	KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
	CUtlString error = "";
	if (metadata.IsEmpty())
	{
		return;
	}
	LoadKV3FromJSON(&kv, &error, metadata.Get(), "");
	if (!error.IsEmpty())
	{
		KZ_LOG_WARN(LogChannel::Timer, "Failed to insert server record to cache due to metadata error: %s\n", error.Get());
		return;
	}

	KeyValues3 *data = kv.FindMember("splitZoneTimes");
	if (data && data->GetType() == KV3_TYPE_ARRAY)
	{
		for (i32 i = 0; i < course->splitCount; i++)
		{
			f64 time = -1.0f;
			KeyValues3 *element = data->GetArrayElement(i);
			if (element)
			{
				time = element->GetDouble(-1.0);
			}
			overall ? pb.overall.pbSplitZoneTimes[i] = time : pb.pro.pbSplitZoneTimes[i] = time;
		}
	}

	data = kv.FindMember("cpZoneTimes");
	if (data && data->GetType() == KV3_TYPE_ARRAY)
	{
		for (i32 i = 0; i < course->checkpointCount; i++)
		{
			f64 time = -1.0f;
			KeyValues3 *element = data->GetArrayElement(i);
			if (element)
			{
				time = element->GetDouble(-1.0);
			}
			overall ? pb.overall.pbCpZoneTimes[i] = time : pb.pro.pbCpZoneTimes[i] = time;
		}
	}

	data = kv.FindMember("stageZoneTimes");
	if (data && data->GetType() == KV3_TYPE_ARRAY)
	{
		for (i32 i = 0; i < course->stageCount; i++)
		{
			f64 time = -1.0f;
			KeyValues3 *element = data->GetArrayElement(i);
			if (element)
			{
				time = element->GetDouble(-1.0);
			}
			overall ? pb.overall.pbStageZoneTimes[i] = time : pb.pro.pbStageZoneTimes[i] = time;
		}
	}
}

void KZTimerService::CheckMissedTime()
{
	const KZCourseDescriptor *course = this->GetCourse();
	// No active course, the timer is not running or if we already announce late PBs.
	if (!course || !this->GetTimerRunning() || (!this->shouldAnnounceMissedTime && !this->shouldAnnounceMissedProTime))
	{
		return;
	}
	// Игрок мог выключить оповещение о потере рекорда (!options → Сообщения): гасим и чат-строку,
	// и звук. Флаги при этом ГАСИМ, а не просто выходим: иначе они залипали в true и при
	// включении настройки посреди рана выстреливало отложенное сообщение (в т.ч. pro-вариант
	// у игрока с чекпоинтами — его апстрим намеренно глушит ниже).
	if (!this->player->optionService->GetPreferenceBool("missedTimeAnnounce", true))
	{
		this->shouldAnnounceMissedTime = false;
		this->shouldAnnounceMissedProTime = false;
		return;
	}
	// No comparison available for styled runs.
	if (this->player->styleServices.Count() > 0)
	{
		return;
	}
	if (this->player->checkpointService->GetCheckpointCount() > 0)
	{
		this->shouldAnnounceMissedProTime = false;
	}
	auto modeInfo = KZ::mode::GetModeInfo(this->player->modeService->GetModeName());

	PBDataKey key = ToPBDataKey(modeInfo.id, course->guid);

	// Check if there is personal best data for this mode and course.
	auto pb = this->GetCompareTarget(key);
	if (!pb)
	{
		return;
	}
	if (this->shouldAnnounceMissedProTime && pb->pro.pbTime > 0 && this->GetTime() > pb->pro.pbTime)
	{
		// Check if they share the same time.
		if (this->shouldAnnounceMissedTime && pb->overall.pbTime == pb->pro.pbTime)
		{
			CUtlString timeText = utils::FormatTime(pb->overall.pbTime);
			this->player->languageService->PrintChat(true, false, missedTimeKeysBoth[this->currentCompareType], timeText.Get());
			this->shouldAnnounceMissedTime = false;
		}
		else
		{
			CUtlString timeText = utils::FormatTime(pb->pro.pbTime);
			this->player->languageService->PrintChat(true, false, missedTimeKeysPro[this->currentCompareType], timeText.Get());
		}
		this->shouldAnnounceMissedProTime = false;
		this->PlayMissedTimeSound();
	}

	if (this->shouldAnnounceMissedTime && pb->overall.pbTime > 0 && this->GetTime() > pb->overall.pbTime)
	{
		CUtlString timeText = utils::FormatTime(pb->overall.pbTime);
		this->player->languageService->PrintChat(true, false, missedTimeKeys[this->currentCompareType], timeText.Get());
		this->shouldAnnounceMissedTime = false;
		this->PlayMissedTimeSound();
	}
}

void KZTimerService::ShowSplitText(u32 currentSplit)
{
	const KZCourseDescriptor *course = this->GetCourse();
	// No active course so we can't compare anything.
	if (!course)
	{
		return;
	}
	// No comparison available for styled runs.
	if (this->player->styleServices.Count() > 0)
	{
		return;
	}

	CUtlString time;
	std::string pbDiff, pbDiffPro = "";

	time = utils::FormatTime(this->splitZoneTimes[currentSplit - 1]);
	if (this->lastSplit != 0)
	{
		f64 diff = this->splitZoneTimes[currentSplit - 1] - this->splitZoneTimes[this->lastSplit - 1];
		CUtlString splitTime = KZTimerService::FormatDiffTime(diff);
		splitTime.Format(" {grey}({default}%s{grey})", splitTime.Get());
		time.Append(splitTime.Get());
	}

	auto modeInfo = KZ::mode::GetModeInfo(this->player->modeService->GetModeName());
	PBDataKey key = ToPBDataKey(modeInfo.id, course->guid);

	// Check if there is personal best data for this mode and course.
	const PBData *pb = this->GetCompareTarget(key);
	if (pb)
	{
		if (pb->overall.pbSplitZoneTimes[currentSplit - 1] > 0)
		{
			KZ_LOG_DEBUG(LogChannel::Timer, "pb->overall.pbSplitZoneTimes[currentSplit - 1] = %lf\n", pb->overall.pbSplitZoneTimes[currentSplit - 1]);
			f64 diff = this->splitZoneTimes[currentSplit - 1] - pb->overall.pbSplitZoneTimes[currentSplit - 1];
			CUtlString diffText = KZTimerService::FormatDiffTime(diff);
			diffText.Format("{grey}%s%s{grey}", diff < 0 ? "{green}" : "{lightred}", diffText.Get());
			pbDiff = this->player->languageService->PrepareMessage(diffTextKeys[this->currentCompareType], diffText.Get());
		}
		if (this->player->checkpointService->GetTeleportCount() == 0 && pb->pro.pbTime > 0 && pb->pro.pbSplitZoneTimes[currentSplit - 1] > 0)
		{
			f64 diff = this->splitZoneTimes[currentSplit - 1] - pb->pro.pbSplitZoneTimes[currentSplit - 1];
			CUtlString diffText = KZTimerService::FormatDiffTime(diff);
			diffText.Format("{grey}%s%s{grey}", diff < 0 ? "{green}" : "{lightred}", diffText.Get());
			pbDiffPro = this->player->languageService->PrepareMessage(diffTextKeysPro[this->currentCompareType], diffText.Get());
		}
	}

	this->player->languageService->PrintChat(true, false, "Course Split Reached", currentSplit, time.Get(), pbDiff.c_str(), pbDiffPro.c_str());
}

void KZTimerService::ShowCheckpointText(u32 currentCheckpoint)
{
	const KZCourseDescriptor *course = this->GetCourse();
	// No active course so we can't compare anything.
	if (!course)
	{
		return;
	}
	// No comparison available for styled runs.
	if (this->player->styleServices.Count() > 0)
	{
		return;
	}

	CUtlString time;
	std::string pbDiff, pbDiffPro = "";

	time = utils::FormatTime(this->cpZoneTimes[currentCheckpoint - 1]);
	if (this->lastCheckpoint != 0)
	{
		f64 diff = this->cpZoneTimes[currentCheckpoint - 1] - this->cpZoneTimes[this->lastCheckpoint - 1];
		CUtlString splitTime = KZTimerService::FormatDiffTime(diff);
		splitTime.Format(" {grey}({default}%s{grey})", splitTime.Get());
		time.Append(splitTime.Get());
	}

	auto modeInfo = KZ::mode::GetModeInfo(this->player->modeService->GetModeName());
	PBDataKey key = ToPBDataKey(modeInfo.id, course->guid);

	// Check if there is personal best data for this mode and course.
	const PBData *pb = this->GetCompareTarget(key);
	if (pb)
	{
		if (pb->overall.pbCpZoneTimes[currentCheckpoint - 1] > 0)
		{
			f64 diff = this->cpZoneTimes[currentCheckpoint - 1] - pb->overall.pbCpZoneTimes[currentCheckpoint - 1];
			CUtlString diffText = KZTimerService::FormatDiffTime(diff);
			diffText.Format("{grey}%s%s{grey}", diff < 0 ? "{green}" : "{lightred}", diffText.Get());
			pbDiff = this->player->languageService->PrepareMessage(diffTextKeys[this->currentCompareType], diffText.Get());
		}
		if (this->player->checkpointService->GetTeleportCount() == 0 && pb->pro.pbTime > 0 && pb->pro.pbCpZoneTimes[currentCheckpoint - 1] > 0)
		{
			f64 diff = this->cpZoneTimes[currentCheckpoint - 1] - pb->pro.pbCpZoneTimes[currentCheckpoint - 1];
			CUtlString diffText = KZTimerService::FormatDiffTime(diff);
			diffText.Format("{grey}%s%s{grey}", diff < 0 ? "{green}" : "{lightred}", diffText.Get());
			pbDiffPro = this->player->languageService->PrepareMessage(diffTextKeysPro[this->currentCompareType], diffText.Get());
		}
	}

	this->player->languageService->PrintChat(true, false, "Course Checkpoint Reached", currentCheckpoint, time.Get(), pbDiff.c_str(),
											 pbDiffPro.c_str());
}

void KZTimerService::ShowStageText()
{
	const KZCourseDescriptor *course = this->GetCourse();
	// No active course so we can't compare anything.
	if (!course)
	{
		return;
	}
	// No comparison available for styled runs.
	if (this->player->styleServices.Count() > 0)
	{
		return;
	}

	CUtlString time;
	std::string pbDiff, pbDiffPro = "";

	time = utils::FormatTime(this->stageZoneTimes[this->currentStage]);
	if (this->currentStage > 0)
	{
		f64 diff = this->stageZoneTimes[this->currentStage] - this->stageZoneTimes[this->currentStage - 1];
		CUtlString splitTime = KZTimerService::FormatDiffTime(diff);
		splitTime.Format(" {grey}({default}%s{grey})", splitTime.Get());
		time.Append(splitTime.Get());
	}

	auto modeInfo = KZ::mode::GetModeInfo(this->player->modeService->GetModeName());
	PBDataKey key = ToPBDataKey(modeInfo.id, course->guid);

	// Check if there is personal best data for this mode and course.
	const PBData *pb = this->GetCompareTarget(key);
	if (pb)
	{
		if (pb->overall.pbStageZoneTimes[this->currentStage] > 0)
		{
			f64 diff = this->stageZoneTimes[this->currentStage] - pb->overall.pbStageZoneTimes[this->currentStage];
			CUtlString diffText = KZTimerService::FormatDiffTime(diff);
			diffText.Format("{grey}%s%s{grey}", diff < 0 ? "{green}" : "{lightred}", diffText.Get());
			pbDiff = this->player->languageService->PrepareMessage(diffTextKeys[this->currentCompareType], diffText.Get());
		}
		if (this->player->checkpointService->GetTeleportCount() == 0 && pb->pro.pbTime > 0 && pb->pro.pbStageZoneTimes[this->currentStage] > 0)
		{
			f64 diff = this->stageZoneTimes[this->currentStage] - pb->pro.pbStageZoneTimes[this->currentStage];
			CUtlString diffText = KZTimerService::FormatDiffTime(diff);
			diffText.Format("{grey}%s%s{grey}", diff < 0 ? "{green}" : "{lightred}", diffText.Get());
			pbDiffPro = this->player->languageService->PrepareMessage(diffTextKeysPro[this->currentCompareType], diffText.Get());
		}
	}

	this->player->languageService->PrintChat(true, false, "Course Stage Reached", this->currentStage + 1, time.Get(), pbDiff.c_str(),
											 pbDiffPro.c_str());
}

CUtlString KZTimerService::GetCurrentRunMetadata()
{
	KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);

	KeyValues3 *splitZoneTimesKV = kv.FindOrCreateMember("splitZoneTimes");

	splitZoneTimesKV->SetToEmptyArray();
	FOR_EACH_VEC(this->splitZoneTimes, i)
	{
		KeyValues3 *time = splitZoneTimesKV->ArrayAddElementToTail();
		time->SetDouble(this->splitZoneTimes[i]);
	}

	KeyValues3 *cpZoneTimesKV = kv.FindOrCreateMember("cpZoneTimes");
	cpZoneTimesKV->SetToEmptyArray();
	FOR_EACH_VEC(this->cpZoneTimes, i)
	{
		KeyValues3 *time = cpZoneTimesKV->ArrayAddElementToTail();
		time->SetDouble(this->cpZoneTimes[i]);
	}

	splitZoneTimesKV->SetToEmptyArray();

	KeyValues3 *stageZoneTimesKV = kv.FindOrCreateMember("stageZoneTimes");
	FOR_EACH_VEC(this->stageZoneTimes, i)
	{
		KeyValues3 *time = stageZoneTimesKV->ArrayAddElementToTail();
		time->SetDouble(this->stageZoneTimes[i]);
	}

	CUtlString result, error;
	if (SaveKV3AsJSON(&kv, &error, &result))
	{
		return result;
	}
	KZ_LOG_WARN(LogChannel::Timer, "Failed to obtain current run's metadata! (%s)\n", error.Get());
	return "";
}

void KZTimerService::UpdateLocalPBCache()
{
	CPlayerUserId uid = player->GetClient()->GetUserID();

	auto onQuerySuccess = [uid](std::vector<ISQLQuery *> queries)
	{
		KZPlayer *pl = g_pKZPlayerManager->ToPlayer(uid);
		if (!pl)
		{
			return;
		}
		ISQLResult *result = queries[0]->GetResultSet();
		if (result && result->GetRowCount() > 0)
		{
			while (result->FetchRow())
			{
				auto modeInfo = KZ::mode::GetModeInfoFromDatabaseID(result->GetInt(2));
				if (modeInfo.databaseID < 0)
				{
					continue;
				}
				const KZCourseDescriptor *course = KZ::course::GetCourseByLocalCourseID(result->GetInt(1));
				if (!course)
				{
					continue;
				}
				pl->timerService->InsertPBToCache(result->GetFloat(0), course, modeInfo.id, true, false, result->GetString(3));
			}
		}
		result = queries[1]->GetResultSet();
		if (result && result->GetRowCount() > 0)
		{
			while (result->FetchRow())
			{
				auto modeInfo = KZ::mode::GetModeInfoFromDatabaseID(result->GetInt(2));
				if (modeInfo.databaseID < 0)
				{
					continue;
				}
				const KZCourseDescriptor *course = KZ::course::GetCourseByLocalCourseID(result->GetInt(1));
				if (!course)
				{
					continue;
				}
				pl->timerService->InsertPBToCache(result->GetFloat(0), course, modeInfo.id, false, false, result->GetString(3));
			}
		}
	};
	KZDatabaseService::QueryAllPBs(player->GetSteamId64(), g_pKZUtils->GetCurrentMapName(), onQuerySuccess, KZDatabaseService::OnGenericTxnFailure);
}

void KZTimerService::Init()
{
	KZDatabaseService::RegisterEventListener(&databaseEventListener);
	KZOptionService::RegisterEventListener(&optionEventListener);
}

void KZTimerService::OnPlayerPreferencesLoaded()
{
	if (this->player->optionService->GetPreferenceInt("preferredCompareType", COMPARE_GPB) > COMPARETYPE_COUNT)
	{
		this->preferredCompareType = COMPARE_GPB;
		return;
	}
	this->preferredCompareType = (CompareType)this->player->optionService->GetPreferenceInt("preferredCompareType", COMPARE_GPB);
	this->shouldPlayTimerStopSound = this->player->optionService->GetPreferenceBool("timerStopSound", true);
}

void KZDatabaseServiceEventListener_Timer::OnMapSetup()
{
	KZ::course::SetupLocalCourses();
	KZTimerService::UpdateLocalRecordCache();
	// Платформенные WR (тот же источник, что лидерборд сайта) — одна async-догрузка на карту,
	// параллельно локальному кэшу; худ покажет их даже вне активного курса (главный курс).
	KZTimerService::ResetPlatformIngestStats();
	KZTimerService::FetchPlatformWorldRecords();
	// Дальше те же WR (и PB игроков) обновляются периодически, чтобы рекорд, поставленный на
	// другом сервере сети, доезжал в худ без реконнекта. Идемпотентно: таймер persistent.
	KZTimerService::StartPlatformRecordsRefresh();
	// Раз на загрузку карты (OnMapSetup стреляет один раз после успешного SetupMap()) -
	// TTL-чистка SavedRuns, fire-and-forget (Task 5).
	KZSavedRunService::PurgeExpired();
}

void KZDatabaseServiceEventListener_Timer::OnClientSetup(Player *player, u64 steamID64, bool isBanned)
{
	KZPlayer *kzPlayer = g_pKZPlayerManager->ToKZPlayer(player);
	kzPlayer->timerService->UpdateLocalPBCache();
	// Платформенный PB игрока (тот же, что на сайте) — на его заходе, async.
	KZTimerService::FetchPlatformPB(kzPlayer);
}

SCMD(kz_recordvolume, SCFL_TIMER | SCFL_GLOBAL | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	f32 volume = Clamp((f32)utils::StringToFloat(args->Arg(1)), 0.0f, 2.0f);
	player->optionService->SetPreferenceFloat("recordVolume", volume);
	player->languageService->PrintChat(true, false, "Record Volume Set", volume);
	return MRES_SUPERCEDE;
}

SCMD(kz_mapoverlay, SCFL_TIMER | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	bool hasOverlay = player->optionService->GetPreferenceBool("mapOverlay", false);
	player->optionService->SetPreferenceBool("mapOverlay", !hasOverlay);
	// clang-format off
	player->languageService->PrintChat(true, false, player->optionService->GetPreferenceBool("mapOverlay") ? "Map Overlay Enabled" : "Map Overlay Disabled");
	// clang-format on
	return MRES_SUPERCEDE;
}
