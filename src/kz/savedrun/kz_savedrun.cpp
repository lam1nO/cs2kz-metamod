#include "../kz.h"
#include "kz_savedrun.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "kz/db/kz_db.h"
#include "kz/language/kz_language.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
#include "kz/timer/kz_timer.h"
#include "kz/trigger/kz_trigger.h"
#include "utils/json.h"
#include "utils/utils.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

namespace
{
	// Один чекпоинт в снапшоте (см. t3-task-2-brief.md, формат v=1):
	// {"o": [x,y,z], "a": [p,y,r], "ln": [x,y,z], "l": bool}.
	// groundEnt намеренно НЕ сериализуем — CHandle невалиден между сессиями/сменой карты.
	struct CpSnapshotJson
	{
		std::vector<f64> o;
		std::vector<f64> a;
		std::vector<f64> ln;
		bool l {};

		bool ToJson(Json &json) const
		{
			return json.Set("o", o) && json.Set("a", a) && json.Set("ln", ln) && json.Set("l", l);
		}

		bool FromJson(const Json &json)
		{
			if (!json.Get("o", o) || !json.Get("a", a) || !json.Get("ln", ln) || !json.Get("l", l))
			{
				return false;
			}
			// origin/angles/ladderNormal — всегда триплеты, иначе снапшот повреждён.
			return o.size() == 3 && a.size() == 3 && ln.size() == 3;
		}
	};

	// Результат парсинга + валидации снапшота (Task 2). Хранится только локально в ApplySnapshot:
	// применение к timerService/checkpointService игрока делает Task 4.
	struct ParsedSnapshot
	{
		f64 time {};
		bool valid {};
		i32 cpIndex {};
		u32 lastCheckpoint {};
		i32 reachedCheckpoints {};
		std::vector<f64> splits;
		std::vector<f64> cpTimes;
		std::vector<f64> stageTimes;
		std::vector<CpSnapshotJson> checkpoints;
	};
} // namespace

CUtlString KZSavedRunService::BuildStylesString(KZPlayer *player)
{
	// Ключ SavedRuns хранится в колонке Styles VARCHAR(64) на MySQL (см. queries/savedruns.h) —
	// переполнение роняет upsert целиком под strict-MySQL (сейв рана теряется молча), а не
	// обрезается движком. Капаем здесь же, ДО похода в БД, по границе последнего ПОЛНОГО имени
	// стиля — частично обрезанное имя не совпало бы byte-в-byte между upsert (save_savedrun.cpp)
	// и fetch (TryRestoreOnSpawn)/delete (InvalidateCurrent), которые все идут через этот метод.
	constexpr i32 kMaxStylesLength = 64;

	CUtlString styles;
	FOR_EACH_VEC(player->styleServices, i)
	{
		const char *shortName = player->styleServices[i]->GetStyleShortName();

		CUtlString candidate = styles;
		if (i > 0)
		{
			candidate.Append(",");
		}
		candidate.Append(shortName);

		if (candidate.Length() > kMaxStylesLength)
		{
			KZ_LOG_WARN(LogChannel::DB,
						"[SavedRuns] Styles string for %s would exceed %d chars, truncating before style '%s' (kept: '%s').\n",
						player->GetName(), kMaxStylesLength, shortName, styles.Get());
			break;
		}
		styles = candidate;
	}
	return styles;
}

std::string KZSavedRunService::SerializeSnapshot()
{
	KZTimerService *timerService = this->player->timerService;
	KZCheckpointService *checkpointService = this->player->checkpointService;

	KZTimerService::TimerSaveSnapshot timerSnapshot = timerService->SnapshotForSave();

	Json json;
	// v: версия формата снапшота (see t3-task-2-brief.md). Меняется при несовместимой правке формата.
	// Кап: не более 200 чекпоинтов в снапшоте (спам !cp не должен раздувать
	// запрос на дисконнекте). Окно якорится на ТЕКУЩЕМ чекпоинте: если игрок
	// стоит на чекпоинте старше хвостового окна — сдвигаем окно к нему
	// (теряется часть новейших, но текущий cp никогда не выпадает).
	const CUtlVector<KZCheckpointService::Checkpoint> &savedCheckpoints = checkpointService->GetCheckpointsForSave();
	const i32 cpCap = 200;
	const i32 rawCpIndex = MAX(0, checkpointService->GetRawCpIndex());
	const i32 cpOffset = MIN(MAX(0, savedCheckpoints.Count() - cpCap), rawCpIndex);
	const i32 cpEnd = MIN(savedCheckpoints.Count(), cpOffset + cpCap);

	json.Set("v", (u32)1);
	json.Set("time", timerSnapshot.time);
	json.Set("valid", timerSnapshot.valid);
	// currentCpIndex всегда >= 0 (см. kz_checkpoint.cpp: ResetCheckpoints/Tp*), безопасно кастить в u32.
	// Json не умеет Set() для знаковых int-типов (нет FromJson у примитива, см. utils/json.h).
	json.Set("cpIndex", (u32)(rawCpIndex - cpOffset));
	json.Set("lastCheckpoint", timerSnapshot.lastCheckpoint);
	json.Set("reachedCheckpoints", (u32)timerSnapshot.reachedCheckpoints);
	json.Set("splits", timerSnapshot.splits);
	json.Set("cpTimes", timerSnapshot.cpTimes);
	json.Set("stageTimes", timerSnapshot.stageTimes);

	std::vector<CpSnapshotJson> checkpoints;
	for (i32 i = cpOffset; i < cpEnd; i++)
	{
		const KZCheckpointService::Checkpoint &cp = savedCheckpoints[i];
		CpSnapshotJson entry;
		entry.o = {cp.origin.x, cp.origin.y, cp.origin.z};
		entry.a = {cp.angles.x, cp.angles.y, cp.angles.z};
		entry.ln = {cp.ladderNormal.x, cp.ladderNormal.y, cp.ladderNormal.z};
		entry.l = cp.onLadder;
		checkpoints.push_back(entry);
	}
	json.Set("checkpoints", checkpoints);

	return json.ToString();
}

bool KZSavedRunService::ApplySnapshot(i32 course, u32 tpCount, const std::string &snapshot)
{
	if (snapshot.empty())
	{
		return false;
	}

	Json json(snapshot);
	if (!json.IsValid())
	{
		KZ_LOG_WARN(LogChannel::Timer, "[SavedRuns] Failed to parse snapshot JSON for %s (invalid JSON).\n", this->player->GetName());
		return false;
	}

	ParsedSnapshot parsed;
	u32 version {};
	u32 cpIndexRaw {};
	u32 reachedCheckpointsRaw {};

	// clang-format off
	bool ok = json.Get("v", version)
		&& json.Get("time", parsed.time)
		&& json.Get("valid", parsed.valid)
		&& json.Get("cpIndex", cpIndexRaw)
		&& json.Get("lastCheckpoint", parsed.lastCheckpoint)
		&& json.Get("reachedCheckpoints", reachedCheckpointsRaw)
		&& json.Get("splits", parsed.splits)
		&& json.Get("cpTimes", parsed.cpTimes)
		&& json.Get("stageTimes", parsed.stageTimes)
		&& json.Get("checkpoints", parsed.checkpoints);
	// clang-format on

	if (!ok)
	{
		KZ_LOG_WARN(LogChannel::Timer, "[SavedRuns] Snapshot for %s failed field validation (missing/wrong-typed field).\n",
					this->player->GetName());
		return false;
	}

	if (version != 1)
	{
		KZ_LOG_WARN(LogChannel::Timer, "[SavedRuns] Snapshot for %s has unsupported version %u, discarding.\n", this->player->GetName(), version);
		return false;
	}

	parsed.cpIndex = (i32)cpIndexRaw;
	parsed.reachedCheckpoints = (i32)reachedCheckpointsRaw;

	// Базовая санность значений (структурная валидность массивов уже проверена в CpSnapshotJson::FromJson).
	if (parsed.time < 0.0)
	{
		KZ_LOG_WARN(LogChannel::Timer, "[SavedRuns] Snapshot for %s has negative time (%f), discarding.\n", this->player->GetName(), parsed.time);
		return false;
	}

	// Резолв курса по cyber-номеру — обратная операция KZ::course::GetCyberCourseNumber
	// (см. save_savedrun.cpp). Карта могла обновиться и потерять этот курс между сессиями —
	// отказ без побочных эффектов и без чат-сообщения (не наша вина, не должны спамить игроку).
	const KZCourseDescriptor *courseDescriptor = KZ::course::GetCourseByCyberNumber(course);
	if (!courseDescriptor)
	{
		KZ_LOG_WARN(LogChannel::Timer, "[SavedRuns] Snapshot for %s references course %d not found on current map, discarding.\n",
					this->player->GetName(), course);
		return false;
	}

	// Пустой снапшот чекпоинтов — pro-ран (0 телепортов), который ни разу не оставил !cp/сплит.
	// Честный отказ вместо декоративного восстановления: раньше этот путь телепортировал игрока
	// на старт курса и ставил ForcePause, но выход из старт-зоны сразу дёргает
	// StartZoneEndTouch -> TimerStart, который безусловно сбрасывает currentTime/currentStage
	// (см. TimerStart) — восстановленное время исчезало на первом же движении, при этом чат уже
	// сказал "Run Restored". Сейв НЕ удаляем (это не порча данных, а пустой pro-ран) — он живёт до
	// TTL (см. PurgeExpired, Task 5).
	if (parsed.checkpoints.empty())
	{
		KZ_LOG_INFO(LogChannel::Timer, "[SavedRuns] skip restore: no checkpoints (pro-run) for %s on course %s.\n", this->player->GetName(),
					courseDescriptor->name);
		return false;
	}

	// Карта запрещает ТП на чекпоинты: DoTeleport ниже молча откажет, и состояние применилось
	// бы «наполовину» (таймер/пауза без позиции). Отказ целиком ДО любых мутаций.
	if (!this->player->triggerService->CanTeleportToCheckpoints())
	{
		KZ_LOG_WARN(LogChannel::Timer, "[SavedRuns] Snapshot for %s has checkpoints but map forbids checkpoint teleports, discarding.\n",
					this->player->GetName());
		return false;
	}

	// --- Валидация выше не мутирует состояние; дальше только применение. ---

	KZTimerService *timerService = this->player->timerService;
	KZCheckpointService *checkpointService = this->player->checkpointService;

	CUtlVector<KZCheckpointService::Checkpoint> restoredCheckpoints;
	for (const CpSnapshotJson &cpJson : parsed.checkpoints)
	{
		KZCheckpointService::Checkpoint cp {};
		cp.origin = Vector((f32)cpJson.o[0], (f32)cpJson.o[1], (f32)cpJson.o[2]);
		cp.angles = QAngle((f32)cpJson.a[0], (f32)cpJson.a[1], (f32)cpJson.a[2]);
		cp.ladderNormal = Vector((f32)cpJson.ln[0], (f32)cpJson.ln[1], (f32)cpJson.ln[2]);
		cp.onLadder = cpJson.l;
		// cp.groundEnt намеренно не трогаем — CHandle по умолчанию невалиден, ground entity
		// не переживает сессию/смену карты (см. CpSnapshotJson выше).
		restoredCheckpoints.AddToTail(cp);
	}
	checkpointService->RestoreFromSnapshot(restoredCheckpoints, parsed.cpIndex, tpCount);

	KZTimerService::TimerSaveSnapshot restoreSnap;
	restoreSnap.time = parsed.time;
	restoreSnap.valid = parsed.valid;
	restoreSnap.lastCheckpoint = parsed.lastCheckpoint;
	restoreSnap.reachedCheckpoints = parsed.reachedCheckpoints;
	restoreSnap.splits = parsed.splits;
	restoreSnap.cpTimes = parsed.cpTimes;
	restoreSnap.stageTimes = parsed.stageTimes;
	timerService->RestoreFromSnapshot(courseDescriptor->guid, restoreSnap);

	// Телепорт ДО паузы: DoTeleport гардит "в паузе телепорт запрещён" (cyb.19-инвариант), а
	// игрок сейчас ещё не paused. restoredCheckpoints гарантированно непуст здесь — пустой
	// снапшот чекпоинтов отбивается выше (pro-ран, честный skip без ТП/паузы).
	//
	// Намеренно НЕ используем checkpointService->TpToCheckpoint(): она идёт через
	// DoTeleport(i32 index), который гардит racingService->CanTeleport() и
	// timerService->CheckSafeguardPro() — это политики для игрок-инициированного !tp/!cp
	// (лимит ТП в гонке, сейфгард "первый TP ломает pro-ран"), нерелевантные для внутреннего
	// восстановления. Вместо этого — "сырой" DoTeleport(Checkpoint), который их не проверяет
	// (но всё ещё уважает паузный гард). Он, как и любой физический телепорт, инкрементит
	// tpCount как побочный эффект — пин-бэчим только счётчик, не трогая teleportTime
	// (окно TpHoldPlayerStill свежего телепорта должно жить).
	checkpointService->DoTeleport(restoredCheckpoints[checkpointService->GetRawCpIndex()]);
	checkpointService->SetTeleportCountForRestore(tpCount);

	// Форс-пауза, а не Pause(): CanPause почти всегда откажет по JustLanded — landingTime
	// выставляется на первом тике после спауна, а колбэк локальной БД приходит через 1-2 тика
	// (окно KZ_TIMER_MIN_GROUND_TIME = 0.05s). Состояние при этом валидно: игрок только что
	// телепортирован нами, velocity 0 — форсим паузу без гарда.
	timerService->ForcePause();

	char timeText[32];
	utils::FormatTime(parsed.time, timeText, sizeof(timeText));
	this->player->languageService->PrintChat(true, false, "Run Restored", timeText);

	return true;
}

void KZSavedRunService::SaveOnDisconnect()
{
	// Guard здесь — только "таймер активен" (paused неважно, спека п.4). БД-готовность
	// и наличие активного курса проверяет сам KZDatabaseService::SaveRun — не дублируем.
	KZTimerService *timerService = this->player->timerService;
	if (!timerService->GetTimerRunning())
	{
		return;
	}
	// Ботов не персистим (pawn-независимая проверка — pawn на дисконнекте невалиден).
	if (this->player->IsFakeClient())
	{
		return;
	}
	// Без полной Steam-аутентификации GetSteamId64() отдаёт 0 (см. Player::GetSteamId) — upsert
	// ушёл бы со SteamID64=0 и разные неавторизованные коннекты схлопывались бы в одну строку
	// ключа. Хук идёт ДО фактического дисконнекта (см. Hook_ClientDisconnect), так что для
	// нормально доигравшего игрока IsAuthenticated() здесь ещё true.
	if (!this->player->IsAuthenticated())
	{
		return;
	}

	// Сериализация читает только сервисные поля (timerService/checkpointService), pawn не трогает —
	// у вышедшего игрока pawn уже может быть невалиден/уничтожен.
	std::string snapshot = this->SerializeSnapshot();
	f64 runTime = timerService->GetTime();
	u32 tpCount = this->player->checkpointService->GetTeleportCount();

	KZDatabaseService::SaveRun(this->player, runTime, tpCount, snapshot);
}

void KZSavedRunService::TryRestoreOnSpawn()
{
	if (this->restoreAttempted || this->fetchStarted)
	{
		return;
	}
	// Ботов не персистим (см. SaveOnDisconnect) - восстанавливать для них нечего.
	if (this->player->IsFakeClient())
	{
		this->restoreAttempted = true;
		return;
	}
	// До полной Steam-аутентификации GetSteamId64() возвращает 0 (validated-путь, см.
	// Player::GetSteamId), а prefs/mode ещё не загружены (SetupClient идёт после auth) —
	// fetch со SteamID64=0 гарантированно пуст и навсегда сжёг бы restoreAttempted.
	// Выходим БЕЗ выставления флагов: попытка повторится на следующем спауне.
	if (!this->player->IsAuthenticated())
	{
		return;
	}

	bool mapNameOk = false;
	CUtlString mapName = g_pKZUtils->GetCurrentMapName(&mapNameOk);
	if (!mapNameOk || mapName.IsEmpty())
	{
		this->restoreAttempted = true;
		return;
	}

	// Порядок выбран по brief (упрощённый вариант): fetch стартует на первом живом спауне,
	// применение снапшота - прямо в колбэке этого fetch'а, а не на "следующем" спауне (следующего
	// спауна может не быть до конца карты). fetchStarted ставим здесь, а не раньше отказов выше -
	// те отказы должны сразу выставлять restoreAttempted, а не блокироваться в fetchStarted навечно.
	this->fetchStarted = true;

	KZModeManager::ModePluginInfo modeInfo = KZ::mode::GetModeInfo(this->player->modeService);
	CUtlString styles = BuildStylesString(this->player);
	u64 steamID64 = this->player->GetSteamId64();
	// Наблюдаемость: без этого лога отказ рестора не диагностируется по логам сервера.
	KZ_LOG_INFO(LogChannel::Timer, "[SavedRuns] fetch: steam=%llu map=%s mode=%s styles='%s' (player %s)\n", (unsigned long long)steamID64,
				mapName.Get(), modeInfo.shortModeName.Get(), styles.Get(), this->player->GetName());
	CPlayerUserId userID = this->player->GetClient()->GetUserID();

	// Захват map/mode/styles на момент старта fetch'а: колбэк обязан сверить их с текущими
	// перед применением (гард "mode/styles не изменились с момента fetch", см. brief).
	CUtlString capturedMapName = mapName;
	CUtlString capturedMode = modeInfo.shortModeName;
	CUtlString capturedStyles = styles;

	auto onSuccess = [userID, capturedMapName, capturedMode, capturedStyles](std::vector<ISQLQuery *> queries)
	{
		KZPlayer *pl = g_pKZPlayerManager->ToPlayer(userID);
		if (!pl)
		{
			// Игрок отключился за время round-trip - восстанавливать уже некому.
			return;
		}
		KZSavedRunService *savedRunService = pl->savedRunService;
		if (savedRunService->restoreAttempted)
		{
			// Уже решено иначе (защита от повторного входа, не должно случаться в норме).
			return;
		}

		ISQLResult *result = queries.size() > 0 ? queries[0]->GetResultSet() : nullptr;
		if (!result || !result->FetchRow())
		{
			// Сейва нет - штатный случай (первый визит на карту/курс).
			KZ_LOG_INFO(LogChannel::Timer, "[SavedRuns] fetch empty for %s.\n", pl->GetName());
			savedRunService->restoreAttempted = true;
			return;
		}

		i32 course = result->GetInt(0);
		u32 tpCount = (u32)result->GetInt(2);
		const char *snapshotRaw = result->GetString(3);
		std::string snapshot = snapshotRaw ? snapshotRaw : "";

		// Условия ДО применения (brief Step 2): жив, таймер ещё не запущен (игрок не начал новый
		// ран, пока фетч летел), карта/mode/styles не изменились с момента старта fetch'а.
		bool mapStillOk = false;
		CUtlString currentMap = g_pKZUtils->GetCurrentMapName(&mapStillOk);
		KZModeManager::ModePluginInfo currentModeInfo = KZ::mode::GetModeInfo(pl->modeService);
		CUtlString currentStyles = KZSavedRunService::BuildStylesString(pl);

		bool environmentChanged = !mapStillOk || currentMap != capturedMapName || currentModeInfo.shortModeName != capturedMode
								   || currentStyles != capturedStyles;

		if (environmentChanged)
		{
			// Mode/styles (или имя карты) доехали асинхронно между стартом fetch'а и этим
			// колбэком (SetupClient/prefs-загрузка гонится с нашим fetch'ем) - снапшот был
			// запрошен под уже неактуальным ключом. НЕ жжём restoreAttempted: даём ретрай на
			// следующем спауне с актуальными mode/styles. fetchStarted сбрасываем, иначе
			// TryRestoreOnSpawn молча no-op'нет навечно (см. её ранний return по fetchStarted).
			savedRunService->fetchStarted = false;
			return;
		}

		if (!pl->IsAlive())
		{
			// Сетап-триггер мог выстрелить до фактического спауна (медленная загрузка) -
			// не жжём попытку, спаун-триггер в OnPlayerSpawn перезапустит fetch.
			savedRunService->fetchStarted = false;
			return;
		}
		if (pl->timerService->GetTimerRunning())
		{
			savedRunService->restoreAttempted = true;
			return;
		}

		savedRunService->ApplySnapshot(course, tpCount, snapshot);
		savedRunService->restoreAttempted = true;
	};

	auto onFailure = [userID](std::string error, int failIndex)
	{
		KZPlayer *pl = g_pKZPlayerManager->ToPlayer(userID);
		if (pl)
		{
			pl->savedRunService->restoreAttempted = true;
		}
		KZ_LOG_WARN(LogChannel::DB, "[SavedRuns] Fetch for restore failed: %s\n", error.c_str());
	};

	KZDatabaseService::FetchSavedRun(steamID64, mapName, modeInfo.shortModeName, styles, onSuccess, onFailure);
}

void KZSavedRunService::InvalidateCurrent(const char *reason)
{
	// Ботов не персистим (см. SaveOnDisconnect) - нечего инвалидировать.
	if (this->player->IsFakeClient())
	{
		return;
	}

	// Курс неизвестен -> нечего удалять. Вызывающие точки (kz_stop, noclip) уже гейтят это через
	// GetTimerRunning() перед вызовом, но держим no-op и здесь на случай будущих вызовов без гарда.
	const KZCourseDescriptor *course = this->player->timerService->GetCourse();
	if (!course)
	{
		return;
	}

	bool mapNameOk = false;
	CUtlString mapName = g_pKZUtils->GetCurrentMapName(&mapNameOk);
	if (!mapNameOk || mapName.IsEmpty())
	{
		return;
	}

	i32 courseNumber = KZ::course::GetCyberCourseNumber(course);
	KZModeManager::ModePluginInfo modeInfo = KZ::mode::GetModeInfo(this->player->modeService);
	// Единая точка построения styles-подписи ключа (см. BuildStylesString) - обязана совпадать
	// байт-в-байт с той, что писал upsert, иначе удалим не тот сейв (или ни один).
	CUtlString styles = KZSavedRunService::BuildStylesString(this->player);

	KZ_LOG_DEBUG(LogChannel::DB, "[SavedRuns] Invalidating saved run for %s (reason: %s).\n", this->player->GetName(), reason);

	KZDatabaseService::DeleteSavedRun(this->player->GetSteamId64(), mapName, courseNumber, modeInfo.shortModeName, styles);
}

void KZSavedRunService::PurgeExpired()
{
	// Раз на загрузку карты, fire-and-forget (см. вызов рядом с SetupLocalCourses в kz_timer.cpp).
	KZDatabaseService::PurgeExpiredSavedRuns();
}
