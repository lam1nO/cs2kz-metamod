#include "../kz.h"
#include "kz_savedrun.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "kz/timer/kz_timer.h"
#include "utils/json.h"

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

std::string KZSavedRunService::SerializeSnapshot()
{
	KZTimerService *timerService = this->player->timerService;
	KZCheckpointService *checkpointService = this->player->checkpointService;

	KZTimerService::TimerSaveSnapshot timerSnapshot = timerService->SnapshotForSave();

	Json json;
	// v: версия формата снапшота (see t3-task-2-brief.md). Меняется при несовместимой правке формата.
	json.Set("v", (u32)1);
	json.Set("time", timerSnapshot.time);
	json.Set("valid", timerSnapshot.valid);
	// currentCpIndex всегда >= 0 (см. kz_checkpoint.cpp: ResetCheckpoints/Tp*), безопасно кастить в u32.
	// Json не умеет Set() для знаковых int-типов (нет FromJson у примитива, см. utils/json.h).
	json.Set("cpIndex", (u32)checkpointService->GetRawCpIndex());
	json.Set("lastCheckpoint", timerSnapshot.lastCheckpoint);
	json.Set("reachedCheckpoints", (u32)timerSnapshot.reachedCheckpoints);
	json.Set("splits", timerSnapshot.splits);
	json.Set("cpTimes", timerSnapshot.cpTimes);
	json.Set("stageTimes", timerSnapshot.stageTimes);

	std::vector<CpSnapshotJson> checkpoints;
	const CUtlVector<KZCheckpointService::Checkpoint> &savedCheckpoints = checkpointService->GetCheckpointsForSave();
	FOR_EACH_VEC(savedCheckpoints, i)
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

bool KZSavedRunService::ApplySnapshot(const std::string &snapshot)
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

	// TODO(Task 4): применить parsed.* к состоянию игрока:
	//  - timerService: SetTime(parsed.time), пометить validTime = parsed.valid, восстановить
	//    lastCheckpoint/reachedCheckpoints и cpZoneTimes/splitZoneTimes/stageZoneTimes (нужен новый
	//    RestoreFromSnapshot(...) в KZTimerService, т.к. эти поля приватны — см. SnapshotForSave выше).
	//  - checkpointService: воссоздать checkpoints из parsed.checkpoints (origin/angles/ladderNormal/onLadder),
	//    затем currentCpIndex = parsed.cpIndex и TpToCheckpoint(), если игрок ожил на нужной карте/курсе.
	// Здесь (Task 2) только парсинг + валидация — состояние игрока не трогаем.
	return true;
}

void KZSavedRunService::SaveOnDisconnect()
{
	// Task 3
}

void KZSavedRunService::TryRestoreOnSpawn()
{
	// Task 4
}

void KZSavedRunService::InvalidateCurrent(const char *reason)
{
	// Task 5
}

void KZSavedRunService::PurgeExpired()
{
	// Task 5
}
