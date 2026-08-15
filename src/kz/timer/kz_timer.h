#pragma once

#include "../kz.h"
#include "../checkpoint/kz_checkpoint.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "utils/uuid.h"

#define KZ_MAX_MODE_NAME_LENGTH 128

#define KZ_TIMER_MIN_GROUND_TIME 0.05f

#define KZ_TIMER_SOUND_COOLDOWN       0.15f
#define KZ_TIMER_SND_START            "Buttons.snd9"
#define KZ_TIMER_SND_END              "tr.ScoreRegular"
#define KZ_TIMER_SND_FALSE_END        "UIPanorama.buymenu_failure"
#define KZ_TIMER_SND_MISSED_ZONE      "UIPanorama.buymenu_failure"
#define KZ_TIMER_SND_REACH_SPLIT      "tr.Popup"
#define KZ_TIMER_SND_REACH_CHECKPOINT "tr.Popup"
#define KZ_TIMER_SND_REACH_STAGE      "UIPanorama.round_report_odds_up"
#define KZ_TIMER_SND_STOP             "tr.PuckFail"
#define KZ_TIMER_SND_MISSED_TIME      "UI.RankDown"

#define KZ_PAUSE_COOLDOWN 1.0f

// Период обновления платформенных PB/WR в худе (сек realtime). Рекорд, поставленный на другом
// сервере сети, до этого появлялся только после реконнекта (см. StartPlatformRecordsRefresh).
#define KZ_PLATFORM_RECORDS_REFRESH_INTERVAL 3.0

#define KZ_SAFEGUARD_RESTART_MIN_DELAY 0.6f
#define KZ_SAFEGUARD_RESTART_MAX_DELAY 5.0f
// Окно «свободного рестарта» в начале рана (сек ИГРОВОГО времени рана, не realtime): пока
// таймер не набрал столько, !r при включённом !sg проходит без двойного тапа. Терять тут
// нечего — забег ещё не начался по существу, а сейфгард в этот момент только мешает
// перезаходить на старт. Дальше — обычный двойной тап (см. CheckSafeguardRestart).
#define KZ_SAFEGUARD_RESTART_FREE_WINDOW 15.0f
// Порог «ран уже жалко» для подтверждения сброса при ВЫКЛЮЧЕННОМ !sg (сек игрового времени
// рана): после него первый сброс таймера не проходит молча, а просит подтверждения.
// См. KZTimerService::CheckSafeguard и resetConfirmWarned.
#define KZ_RESET_CONFIRM_MIN_RUNTIME 300.0f

// Legacy: старый единый int-преф "safeguard". Оставлен ТОЛЬКО для миграции —
// расцеплённые флаги теперь живут в отдельных префах "sgTeleport"/"sgReset"
// (см. KZTimerService::GetSafeguardTeleport/GetSafeguardReset). Старое значение
// читается лишь как фолбэк: PRO → блок ТП + блок сброса, NUB → только блок сброса.
enum SafeguardOption : u8
{
	SAFEGUARD_DISABLED = 0,
	SAFEGUARD_NUB,
	SAFEGUARD_PRO
};

struct PBData
{
	PBData()
	{
		Reset();
	}

	void Reset()
	{
		overall.pbTime = {};
		overall.pbSplitZoneTimes.SetCount(KZ_MAX_SPLIT_ZONES);
		overall.pbSplitZoneTimes.FillWithValue(-1.0);
		overall.pbCpZoneTimes.SetCount(KZ_MAX_CHECKPOINT_ZONES);
		overall.pbCpZoneTimes.FillWithValue(-1.0);
		overall.pbStageZoneTimes.SetCount(KZ_MAX_STAGE_ZONES);
		overall.pbStageZoneTimes.FillWithValue(-1.0);
		pro.pbTime = {};
		pro.pbSplitZoneTimes.SetCount(KZ_MAX_SPLIT_ZONES);
		pro.pbSplitZoneTimes.FillWithValue(-1.0);
		pro.pbCpZoneTimes.SetCount(KZ_MAX_CHECKPOINT_ZONES);
		pro.pbCpZoneTimes.FillWithValue(-1.0);
		pro.pbStageZoneTimes.SetCount(KZ_MAX_STAGE_ZONES);
		pro.pbStageZoneTimes.FillWithValue(-1.0);
	}

	struct
	{
		f64 pbTime {};
		f64 points {};
		CUtlVectorFixed<f64, KZ_MAX_SPLIT_ZONES> pbSplitZoneTimes;
		CUtlVectorFixed<f64, KZ_MAX_CHECKPOINT_ZONES> pbCpZoneTimes;
		CUtlVectorFixed<f64, KZ_MAX_STAGE_ZONES> pbStageZoneTimes;
	} overall, pro;
};

// Convert mode and course ID to one single value.
typedef u64 PBDataKey;

inline PBDataKey ToPBDataKey(u32 modeID, u32 courseID)
{
	return modeID | ((u64)courseID << 32);
}

inline void ConvertFromPBDataKey(PBDataKey key, uint32_t *modeID, uint32_t *courseID)
{
	if (modeID)
	{
		*modeID = (uint32_t)key;
	}
	if (courseID)
	{
		*courseID = (uint32_t)(key >> 32);
	}
}

class KZTimerServiceEventListener
{
public:
	virtual bool OnTimerStart(KZPlayer *player, u32 courseGUID)
	{
		return true;
	}

	virtual void OnTimerStartPost(KZPlayer *player, u32 courseGUID) {}

	virtual bool OnTimerEnd(KZPlayer *player, u32 courseGUID, f32 time, u32 teleportsUsed)
	{
		return true;
	}

	virtual void OnTimerEndPost(KZPlayer *player, u32 courseGUID, f32 time, u32 teleportsUsed) {}

	virtual void OnTimerStopped(KZPlayer *player, u32 courseGUID) {}

	virtual void OnTimerInvalidated(KZPlayer *player) {}

	virtual bool OnPause(KZPlayer *player)
	{
		return true;
	}

	virtual void OnPausePost(KZPlayer *player) {}

	virtual bool OnResume(KZPlayer *player)
	{
		return true;
	}

	virtual void OnResumePost(KZPlayer *player) {}

	virtual void OnSplitZoneTouchPost(KZPlayer *player, u32 splitZone) {}

	virtual void OnCheckpointZoneTouchPost(KZPlayer *player, u32 checkpointZone) {}

	virtual void OnStageZoneTouchPost(KZPlayer *player, u32 stageZone) {}
};

class KZTimerService : public KZBaseService
{
	using KZBaseService::KZBaseService;

private:
	bool timerRunning {};
	f64 currentTime {};
	u32 currentCourseGUID {};
	f64 lastEndTime {};
	f64 lastFalseEndTime {};
	f64 lastStartSoundTime {};
	f64 lastMissedTimeSoundTime {};
	bool validTime {};

	u32 lastSplit {};
	CUtlVectorFixed<f64, KZ_MAX_SPLIT_ZONES> splitZoneTimes {};

	u32 lastCheckpoint {};
	i32 reachedCheckpoints {};
	CUtlVectorFixed<f64, KZ_MAX_CHECKPOINT_ZONES> cpZoneTimes {};

	i32 currentStage {};
	CUtlVectorFixed<f64, KZ_MAX_STAGE_ZONES> stageZoneTimes {};

	// PB cache per mode and per course.
	std::unordered_map<PBDataKey, PBData> localPBCache;
	std::unordered_map<PBDataKey, PBData> globalPBCache;

	// SR cache should be loaded upon map start, every time !wr is queried and every time a run beats the server record.
	static std::unordered_map<PBDataKey, PBData> srCache;

	static std::unordered_map<PBDataKey, PBData> wrCache;

	// Платформенные кэши PB/WR из cyber-api (те же данные, что лидерборд сайта). Ключуются по
	// (api-mode-index, cyber-course-number), а НЕ по внутренним mode/course id — чтобы сойтись с
	// ответом api (см. FetchPlatform*/IngestPlatformRecords). Времена в секундах (api отдаёт ms).
	// WR — общий (static, по карте); PB — по игроку. Наполняются async; промах → худ падает на
	// локальный фолбэк (wrCache/srCache, globalPBCache/localPBCache).
	static std::unordered_map<u64, f64> platformWrCache;
	std::unordered_map<u64, f64> platformPbCache;

	// Разбор тела ответа GET /ingest/v1/kz/records в платформенные кэши. pbPlayer != nullptr →
	// его pbTimeMs пишутся в platformPbCache; wrTimeMs всегда актуализирует общий platformWrCache.
	static void IngestPlatformRecords(const char *body, KZPlayer *pbPlayer);

public:
	enum CompareType : u8
	{
		COMPARE_NONE = 0,
		COMPARE_SPB, // Local PB
		COMPARE_GPB, // Global PB
		COMPARE_SR,  // Server Record
		COMPARE_WR,  // Global Record
		COMPARETYPE_COUNT
	};

private:
	// The maximum level that we should compare our current time with.
	// For example, if the value is set to COMPARE_GPB, the player will not attempt to compare their splits with SR/WR,
	// but only global PB, and local PB if global data is not available.
	CompareType preferredCompareType = COMPARE_GPB;

	// What we are currently comparing our run against in this current run.
	// This stays the same from the start of the run (unless preferredCompareType changes) to have a consistent comparison across the run.
	CompareType currentCompareType = COMPARE_GPB;

	void UpdateCurrentCompareType(PBDataKey key);
	const PBData *GetCompareTargetForType(CompareType type, PBDataKey key);
	const PBData *GetCompareTarget(PBDataKey key);

	bool shouldAnnounceMissedTime = true;
	bool shouldAnnounceMissedProTime = true;

public:
	static void ClearRecordCache();
	static void UpdateLocalRecordCache();
	// Платформенная догрузка PB/WR из cyber-api (тот же источник, что лидерборд сайта). WR — раз
	// на карту (OnMapSetup); PB игрока — на его заходе (OnClientSetup). Async, fail-soft: сбой /
	// timeout / выключенный cybEmitUrl оставляет платформенные кэши как есть, худ падает на локаль.
	// resetCache=true (заход/смена карты) чистит кэш перед запросом; периодическое обновление
	// (StartPlatformRecordsRefresh) зовёт с false, чтобы худ не мигал на время round-trip.
	static void FetchPlatformWorldRecords(bool resetCache = true);
	static void FetchPlatformPB(KZPlayer *player, bool resetCache = true);
	// Заводит persistent-таймер обновления платформенных PB/WR (см. RefreshPlatformRecords).
	// Идемпотентна: повторные вызовы — no-op.
	static void StartPlatformRecordsRefresh();
	static void InsertRecordToCache(f64 time, const KZCourseDescriptor *courseName, PluginId modeID, bool hasTeleports, bool global,
									CUtlString metadata = "");

	void ClearPBCache();
	const PBData *GetGlobalCachedPB(const KZCourseDescriptor *course, PluginId modeID);
	void UpdateLocalPBCache();
	void InsertPBToCache(f64 time, const KZCourseDescriptor *courseName, PluginId modeID, bool overall, bool global, CUtlString metadata = "",
						 f64 points = 0);
	void SetCompareTarget(const char *typeString);

	void CheckMissedTime();

	void ShowSplitText(u32 currentSplit);
	void ShowCheckpointText(u32 currentCheckpoint);
	void ShowStageText();

	CUtlString GetCurrentRunMetadata();

private:
	bool validJump {};
	f64 lastInvalidateTime {};

public:
	static void Init();
	static bool RegisterEventListener(KZTimerServiceEventListener *eventListener);
	static bool UnregisterEventListener(KZTimerServiceEventListener *eventListener);

	bool GetTimerRunning()
	{
		return timerRunning;
	}

	bool GetValidTimer()
	{
		return validTime;
	}

	f64 GetTime()
	{
		return currentTime;
	}

	// Task 2 (SavedRuns): срез приватных полей таймера для сериализации снапшота незавершённого рана.
	// Не мутирует состояние. Восстановление (Task 4) — отдельный RestoreFromSnapshot.
	struct TimerSaveSnapshot
	{
		f64 time {};
		bool valid {};
		u32 lastCheckpoint {};
		i32 reachedCheckpoints {};
		std::vector<f64> splits;
		std::vector<f64> cpTimes;
		std::vector<f64> stageTimes;
	};

	TimerSaveSnapshot SnapshotForSave()
	{
		TimerSaveSnapshot snap;
		snap.time = this->currentTime;
		snap.valid = this->validTime;
		snap.lastCheckpoint = this->lastCheckpoint;
		snap.reachedCheckpoints = this->reachedCheckpoints;

		FOR_EACH_VEC(this->splitZoneTimes, i)
		{
			snap.splits.push_back(this->splitZoneTimes[i]);
		}
		FOR_EACH_VEC(this->cpZoneTimes, i)
		{
			snap.cpTimes.push_back(this->cpZoneTimes[i]);
		}
		FOR_EACH_VEC(this->stageZoneTimes, i)
		{
			snap.stageTimes.push_back(this->stageZoneTimes[i]);
		}

		return snap;
	}

	// Task 4 (SavedRuns): восстанавливает состояние таймера из распарсенного снапшота
	// (обратное SnapshotForSave). timerRunning выставляется в true безусловно — снапшот
	// сохраняется только для активного незавершённого рана. Целевые размеры
	// splits/cpTimes/stageTimes берутся из ТЕКУЩЕГО courseDesc (карта могла обновиться между
	// сессиями и поменять число зон курса), а не из длины снапшота — иначе индексация по
	// currentStage/StageZoneStartTouch/SplitZoneStartTouch разъедется с текущим курсом (тот же
	// паттерн SetSize+FillWithValue(-1), что в TimerStart). currentStage и lastSplit не
	// сериализуются отдельно (см. TimerSaveSnapshot) — восстанавливаются как длина непрерывного
	// префикса пройденных зон с начала (первый -1 останавливает счёт): для стейджей это точно
	// соответствует порядку прохождения (StageZoneStartTouch пишет stageZoneTimes[currentStage]
	// строго последовательно, зона выше currentStage+1 отбивается как "missed stage"), без этого
	// TimerEnd (currentStage == courseDesc->stageCount) недостижим на курсах со stage-зонами после
	// рестора. Не трогает паузу/телепорт — порядок применения держит KZSavedRunService.
	void RestoreFromSnapshot(u32 courseGUID, const TimerSaveSnapshot &snap)
	{
		this->currentCourseGUID = courseGUID;
		this->currentTime = snap.time;
		this->timerRunning = true;
		this->validTime = snap.valid;
		this->lastCheckpoint = snap.lastCheckpoint;
		this->reachedCheckpoints = snap.reachedCheckpoints;

		// Вызывающая сторона (KZSavedRunService::ApplySnapshot) уже резолвит курс по
		// cyber-номеру до вызова, так что courseDesc здесь ожидаемо не-null; на случай future
		// caller'а без этой гарантии деградируем на длину снапшота, капнутую фиксированным буфером
		// (старое поведение), а не падаем в null deref.
		const KZCourseDescriptor *courseDesc = KZ::course::GetCourse(courseGUID);
		f64 invalidTime = -1;

		i32 splitTarget = courseDesc ? courseDesc->splitCount : (i32)snap.splits.size();
		splitTarget = MIN(splitTarget, (i32)KZ_MAX_SPLIT_ZONES);
		this->splitZoneTimes.SetSize(splitTarget);
		this->splitZoneTimes.FillWithValue(invalidTime);
		i32 splitCopyCount = MIN(splitTarget, (i32)snap.splits.size());
		for (i32 i = 0; i < splitCopyCount; i++)
		{
			this->splitZoneTimes[i] = snap.splits[i];
		}

		i32 cpTarget = courseDesc ? courseDesc->checkpointCount : (i32)snap.cpTimes.size();
		cpTarget = MIN(cpTarget, (i32)KZ_MAX_CHECKPOINT_ZONES);
		this->cpZoneTimes.SetSize(cpTarget);
		this->cpZoneTimes.FillWithValue(invalidTime);
		i32 cpCopyCount = MIN(cpTarget, (i32)snap.cpTimes.size());
		for (i32 i = 0; i < cpCopyCount; i++)
		{
			this->cpZoneTimes[i] = snap.cpTimes[i];
		}

		i32 stageTarget = courseDesc ? courseDesc->stageCount : (i32)snap.stageTimes.size();
		stageTarget = MIN(stageTarget, (i32)KZ_MAX_STAGE_ZONES);
		this->stageZoneTimes.SetSize(stageTarget);
		this->stageZoneTimes.FillWithValue(invalidTime);
		i32 stageCopyCount = MIN(stageTarget, (i32)snap.stageTimes.size());
		for (i32 i = 0; i < stageCopyCount; i++)
		{
			this->stageZoneTimes[i] = snap.stageTimes[i];
		}

		// currentStage = длина непрерывного префикса пройденных стейджей (см. комментарий выше
		// метода). Обязателен для курсов со stage-зонами: без него TimerEnd недостижим.
		this->currentStage = 0;
		FOR_EACH_VEC(this->stageZoneTimes, i)
		{
			if (this->stageZoneTimes[i] < 0)
			{
				break;
			}
			this->currentStage++;
		}

		// lastSplit не сериализуется отдельно (нет соответствующего поля в TimerSaveSnapshot/v=1) —
		// тот же префиксный вывод по splitZoneTimes. Влияет только на "diff since last split" в
		// ShowSplitText, не на завершаемость рана.
		this->lastSplit = 0;
		FOR_EACH_VEC(this->splitZoneTimes, i)
		{
			if (this->splitZoneTimes[i] < 0)
			{
				break;
			}
			this->lastSplit = i + 1;
		}
	}

	static void FormatDiffTime(f64 time, char *output, u32 length, bool precise = true)
	{
		char temp[32];
		if (time > 0)
		{
			utils::FormatTime(time, temp, sizeof(temp), precise);
			V_snprintf(output, length, "+%s", temp);
		}
		else
		{
			utils::FormatTime(-time, temp, sizeof(temp), precise);
			V_snprintf(output, length, "-%s", temp);
		}
	}

	static CUtlString FormatDiffTime(f64 time, bool precise = true)
	{
		char temp[32];
		FormatDiffTime(time, temp, sizeof(temp), precise);
		return CUtlString(temp);
	}

	void SetTime(f64 time)
	{
		currentTime = time;
		timerRunning = time > 0.0f;
	}

	const KZCourseDescriptor *GetCourse()
	{
		return KZ::course::GetCourse(currentCourseGUID);
	}

	// Текущий достигнутый стейдж в забеге (для HUD-строки "STAGE n/total").
	i32 GetCurrentStage()
	{
		return currentStage;
	}

	// --- HUD (кибершоковский стандартный худ, строка PB/WR) ---
	// Лучшее доступное персональное время для режима игрока и курса; overall = зачёт с телепортами.
	// Приоритет — платформенный кэш (platformPbCache, совпадает с сайтом), при промахе — фолбэк на
	// локальные PB-кэши (глоб. PB важнее локального). Только чтение кэшей, без сети. course == nullptr
	// → активный курс игрока (this->GetCourse()); передан явно (напр. главный курс) — лукап по нему,
	// чтобы PB показывался и вне старт-зоны. false, если курса нет или PB не наполнен нигде.
	bool GetHudPBTime(f64 &outTime, const KZCourseDescriptor *course = nullptr);
	// WR-время (overall) для режима+курса. Приоритет — платформенный кэш (platformWrCache, рекорд
	// сети с сайта); при промахе — глобальный wrCache (глобальные карты), затем srCache (рекорд наших
	// серверов). course == nullptr → активный курс; иначе лукап по переданному. false только когда
	// нет ни одного источника.
	bool GetHudWorldRecordTime(f64 &outTime, const KZCourseDescriptor *course = nullptr);

	void SetCourse(u32 courseGUID)
	{
		currentCourseGUID = courseGUID;
	}

	enum TimeType_t
	{
		TimeType_Standard,
		TimeType_Pro
	};

	TimeType_t GetCurrentTimeType()
	{
		return this->player->checkpointService->GetTeleportCount() > 0 ? TimeType_Standard : TimeType_Pro;
	}

	// Пересчёт «касался ли земли с момента влёта в стартовую зону» по текущему FL_ONGROUND.
	// Это защита от старта рана в воздухе: StartZoneEndTouch пускает таймер только по true.
	// Вынесено из StartZoneStartTouch отдельным методом, потому что в prac единственный эффект
	// стартовой зоны — запуск prac-часов (trigger/callbacks.cpp), а ЭТА бухгалтерия обязана
	// вестись всегда — иначе флаг остаётся с догоночного значения и игрок, вышедший из prac
	// внутри зоны, стартует ран в воздухе с разгоном, набранным снаружи.
	void ResetStartZoneGroundTouch();

	// Чтение того же флага: prac-часы стартуют по выходу из стартовой зоны с теми же условиями,
	// что и настоящий таймер (KZPracService::OnStartZoneEndTouch) — иначе prac-время было бы
	// несравнимо с временем рана, ради сравнения оно и существует.
	bool GetTouchedGroundInStartZone()
	{
		return this->touchedGroundSinceTouchingStartZone;
	}

	// Предусловия старта рана, НЕ зависящие от курса — ровно первый гард TimerStart, вынесенный
	// отдельно, чтобы prac-часы могли стартовать по тому же условию, а не по своему списку
	// (иначе «репетиция старта» показывала бы время там, где настоящий ран не завёлся бы:
	// телепорт, ноуклип, перф/бхоп навылет, недостаточное время на земле). Курсовые условия
	// TimerStart оставляет себе. Звать из того же места вызова, что и TimerStart, — часть
	// условий читает состояние текущего тика (inPerf, landingTime). Пешки нет (спектатор) — false.
	bool CanStartRunHere();

	void StartZoneStartTouch(const KZCourseDescriptor *course);
	void StartZoneEndTouch(const KZCourseDescriptor *course);
	void SplitZoneStartTouch(const KZCourseDescriptor *course, i32 splitNumber);
	void CheckpointZoneStartTouch(const KZCourseDescriptor *course, i32 cpNumber);
	void StageZoneStartTouch(const KZCourseDescriptor *course, i32 stageNumber);
	bool TimerStart(const KZCourseDescriptor *course, bool playSound = true);
	bool TimerEnd(const KZCourseDescriptor *course);
	// reason — машинно-читаемая причина остановки, уходит в лог (инвариант
	// «у отказа всегда причина»): по ней разбирается жалоба «таймер сбросился сам».
	// Значения: start_zone, teleport_to_start, teleport_to_end, disconnect, death,
	// round_start, noclip, cheat_cvar, mode_change, style_change, jumpstat_area,
	// pause_denied, team_change, stop, race, race_event, prac, prac_discard,
	// stop_all, unknown. Тот же словарь у run_lost (KZPracService::DropFrozenRun) —
	// одно действие игрока должно давать одно значение в обеих строках.
	bool TimerStop(bool playSound = true, const char *reason = "unknown");
	static void TimerStopAll(bool playSound = true, const char *reason = "stop_all");

	bool GetValidJump()
	{
		return validJump;
	}

	void InvalidateJump();
	void PlayTimerStartSound();

	// To be used for saveloc.
	void InvalidateRun();

private:
	bool HasValidMoveType();

	static bool IsValidMoveType(MoveType_t moveType)
	{
		return moveType == MOVETYPE_WALK || moveType == MOVETYPE_LADDER || moveType == MOVETYPE_NONE || moveType == MOVETYPE_OBSERVER;
	}

	bool JustLanded()
	{
		return g_pKZUtils->GetGlobals()->curtime - this->player->landingTime < KZ_TIMER_MIN_GROUND_TIME;
	}

	bool JustStartedTimer()
	{
		return timerRunning && this->GetTime() < EPSILON;
	}

	bool JustEndedTimer();

public:
	void PlayTimerEndSound();
	void PlayTimerFalseEndSound();
	void PlayMissedZoneSound();
	void PlayReachedSplitSound();
	void PlayReachedCheckpointSound();
	void PlayReachedStageSound();
	void PlayTimerStopSound();
	void PlayMissedTimeSound();

	/*
	 * Pause stuff also goes here.
	 */

private:
	bool paused {};
	bool pausedOnLadder {};
	f32 lastPauseTime {};
	bool hasPausedInThisRun {};
	f32 lastResumeTime {};
	bool hasResumedInThisRun {};
	f32 lastDuckValue {};
	f32 lastStaminaValue {};
	bool touchedGroundSinceTouchingStartZone {};
	bool shouldPlayTimerStopSound = true;

	f64 lastRestartAttemptTime {};
	// «Предупреждение о сбросе уже потрачено в этом ране» (см. CheckSafeguard, п.4 пакета
	// 15.08). Ровно одно на ран: второй случайный сброс той же попытки проходит молча —
	// иначе гейт превращается в постоянный двойной тап на всё подряд. Сбрасывается вместе
	// с остальным состоянием рана в KZTimerService::Reset() и на TimerStart.
	bool resetConfirmWarned {};

public:
	bool GetPaused()
	{
		return paused;
	}

	void SetPausedOnLadder(bool ladder)
	{
		pausedOnLadder = ladder;
	}

	void Pause();
	// Пауза БЕЗ CanPause-гарда (JustLanded/анти-пауза зона/midair/кулдаун), но с
	// OnPause/OnPausePost-листенерами. Для внутренних вызовов, где вызывающая сторона сама
	// гарантирует валидность состояния (SavedRuns-рестор: игрок только что телепортирован,
	// velocity 0). Игрокские команды должны идти через Pause().
	void ForcePause();
	bool CanPause(bool showError = false);
	void Resume(bool force = false);
	bool CanResume(bool showError = false);

	void TogglePause();

	void ToggleTimerStopSound();

	// Safeguard — два НЕЗАВИСИМЫХ предохранителя (расцеплены из старого единого префа):
	//  - «блок телепортов» (PRO)  — преф "sgTeleport", команда !pro; гейтит ТОЛЬКО чекпоинт-ТП,
	//    чтобы ран остался PRO. Рестарт и noclip при этом РАЗРЕШЕНЫ.
	//  - «блок сброса таймера»     — преф "sgReset",   команда !sg;  гейтит noclip/рестарт/стоп/
	//    !end/!lj/уход в спектатор — всё, что останавливает/инвалидирует таймер. ТП сюда НЕ входит.
	// Каждая команда ставит СВОЙ флаг независимо. CheckSafeguardPro смотрит только на "sgTeleport";
	// CheckSafeguard/CheckSafeguardRestart — только на "sgReset".
	void ToggleSafeguard();
	void ToggleProSafeguard();
	bool CheckSafeguard(bool showError = true);
	bool CheckSafeguardPro(bool showError = true);
	bool CheckSafeguardRestart(bool showError = true);
	// Эффективные значения флагов: новый преф в приоритете, при его отсутствии — вывод из
	// старого "safeguard" по прежней семантике (миграция без потери настроек игрока).
	bool GetSafeguardTeleport();
	bool GetSafeguardReset();

private:
	// JoinTeam (kz_misc.cpp) держит этот флаг поднятым вокруг ChangeTeam/CommitSuicide/
	// SwitchTeam: эти вызовы могут поднять player_death как побочный эффект смены команды,
	// а не как реальную смерть игрока (см. OnPlayerDeath).
	bool changingTeam {};

public:
	// Ставится/снимается ТОЛЬКО KZ::misc::JoinTeam на каждом выходе из неё.
	void SetChangingTeam(bool value)
	{
		this->changingTeam = value;
	}

	// Идёт ли намеренная смена команды. Невидимке это ВТОРАЯ линия против «команду сменил
	// движок» (mp_force_pick_time); первая и основная — явный OnObserveEnd() в обёртках
	// захода в игру, потому что !goto меняет команду мимо JoinTeam и этот флаг не поднимает.
	bool IsChangingTeam() const
	{
		return this->changingTeam;
	}

	virtual void Reset() override;
	void OnPhysicsSimulatePost();
	void OnStartTouchGround();
	void OnStopTouchGround();
	void OnChangeMoveType(MoveType_t oldMoveType);
	void OnTeleportToStart();
	void OnClientDisconnect();
	void OnPlayerSpawn();
	void OnPlayerJoinTeam(i32 team);
	void OnPlayerDeath();
	static void OnRoundStart();
	void OnTeleport(const Vector *newPosition, const QAngle *newAngles, const Vector *newVelocity);

	void OnPlayerPreferencesLoaded();
};
