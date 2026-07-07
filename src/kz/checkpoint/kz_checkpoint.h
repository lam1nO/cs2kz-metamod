#pragma once
#include "../kz.h"

class KZCheckpointService : public KZBaseService
{
public:
	KZCheckpointService(KZPlayer *player) : KZBaseService(player)
	{
		this->checkpoints = CUtlVector<Checkpoint>(1, 0);
	}

	static void Init();
	virtual void Reset() override;

	// Checkpoint stuff
	struct Checkpoint
	{
		Vector origin;
		QAngle angles;
		Vector ladderNormal;
		bool onLadder {};
		CHandle<CBaseEntity> groundEnt;
	};

	// UndoTeleport stuff
	struct UndoTeleportData : public Checkpoint
	{
		bool teleportOnGround {};
		bool teleportInBhopTrigger {};
		bool teleportInAntiCpTrigger {};
	};

	struct CustomStartPosition
	{
		Vector origin;
		QAngle angles;
		Vector ladderNormal;
		bool onLadder {};
		bool onGround {};
	};

private:
	i32 currentCpIndex {};
	u32 tpCount {};
	bool holdingStill {};
	f32 teleportTime {};
	CUtlVector<Checkpoint> checkpoints;
	UndoTeleportData undoTeleportData;

	bool hasCustomStartPosition {};
	CustomStartPosition customStartPosition;
	Checkpoint lastTeleportedCheckpoint {};
	bool lastTeleportForcedOnGround {};

public:
	void OnPlayerPreferencesLoaded();
	void ResetCheckpoints(bool playSound = false, bool resetTeleports = true);
	void SetCheckpoint();

	void UndoTeleport();
	void DoTeleport(const Checkpoint cp);
	void DoTeleport(const Checkpoint cp, bool stayOnGround);
	void DoTeleport(i32 index);
	void TpHoldPlayerStill();
	void TpToCheckpoint();
	void TpToPrevCp();
	void TpToNextCp();

	u32 GetTeleportCount()
	{
		return this->tpCount;
	}

	i32 GetCurrentCpIndex()
	{
		if (this->checkpoints.Count() > 0)
		{
			return this->currentCpIndex + 1;
		}
		else
		{
			return this->currentCpIndex;
		}
	}

	i32 GetCheckpointCount()
	{
		return this->checkpoints.Count();
	}

	// Task 2 (SavedRuns): срез состояния для сериализации снапшота незавершённого рана.
	// Возвращает "сырой" (0-based) индекс, как ожидает DoTeleport(i32 index); не мутирует состояние.
	i32 GetRawCpIndex()
	{
		return this->currentCpIndex;
	}

	const CUtlVector<Checkpoint> &GetCheckpointsForSave()
	{
		return this->checkpoints;
	}

	// Task 4 (SavedRuns): восстановление чекпоинтов/индекса/счётчика ТП из снапшота БД.
	// Каждый Checkpoint должен приходить с невалидным groundEnt (CHandle не переживает
	// сессию/смену карты — вызывающая сторона его не сериализовала). cpIndex клэмпится
	// в границы; при пустом savedCheckpoints оседает на 0. Не телепортирует и не трогает
	// паузу/таймер — порядок применения держит KZSavedRunService::ApplySnapshot.
	void RestoreFromSnapshot(const CUtlVector<Checkpoint> &savedCheckpoints, i32 cpIndex, u32 tpCountValue);

	// Task 4 (SavedRuns): пин-бэк счётчика ТП после внутреннего DoTeleport восстановления
	// (тот инкрементит tpCount как побочный эффект любого физического телепорта). Трогает
	// ТОЛЬКО tpCount — teleportTime/undo-буфер свежего телепорта должны жить (на teleportTime
	// завязано окно TpHoldPlayerStill).
	void SetTeleportCountForRestore(u32 tpCountValue)
	{
		this->tpCount = tpCountValue;
	}

	void SetStartPosition();
	void ClearStartPosition();

	bool HasCustomStartPosition()
	{
		return this->hasCustomStartPosition;
	}

	void TpToStartPosition();

	void PlayCheckpointErrorSound();
	void PlayTeleportErrorSound();
	void PlayCheckpointSound();
	void PlayTeleportSound();
	void PlayCheckpointResetSound();
};
