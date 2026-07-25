#pragma once
#include "../kz.h"
#include "kz/timer/kz_timer.h"
#include "kz/checkpoint/kz_checkpoint.h"

// Режим отработки элементов: заморозка рана + ноуклип + prac-чекпоинты со скоростью.
// Спека: docs/superpowers/specs/2026-07-25-kz-prac-mode-design.md (монорепо).
class KZPracService : public KZBaseService
{
	using KZBaseService::KZBaseService;

public:
	// Замороженный ран. active=false — в prac вошли без запущенного таймера
	// (свободная тренировка), возвращаться некуда.
	struct FrozenRun
	{
		bool active {};
		u32 courseGUID {};
		KZTimerService::TimerSaveSnapshot timer;
		CUtlVector<KZCheckpointService::Checkpoint> checkpoints;
		i32 cpIndex {};
		u32 tpCount {};
		Vector origin {};
		QAngle angles {};
	};

	// Точка отработки внутри prac. В отличие от обычного чекпоинта хранит ВЕКТОР
	// СКОРОСТИ — в этом весь смысл: элемент отрабатывается с того же разгона.
	struct PracPoint
	{
		Vector origin {};
		Vector velocity {};
		QAngle angles {};
		f32 duckAmount {};
		f32 stamina {};
		bool onGround {};
		bool onLadder {};
		Vector ladderNormal {};
	};

private:
	bool inPrac {};
	FrozenRun frozen;
	CUtlVector<PracPoint> points;
	i32 currentIndex {};

public:
	static void Init();
	virtual void Reset() override;

	bool IsInPrac()
	{
		return this->inPrac;
	}

	bool HasFrozenRun()
	{
		return this->frozen.active;
	}

	// Снапшот для SavedRuns при дисконнекте в prac (Task 7). Не мутирует состояние.
	const FrozenRun &GetFrozenRun()
	{
		return this->frozen;
	}

	// Тоггл: вход в prac (заморозка рана, если он есть) или возврат в замороженный ран.
	void TogglePrac();
	// Потеря prac без возврата в ран (смерть, !r, смена карты). reason — для чат-сообщения
	// и лога; nullptr = молча (смена карты).
	void DropFrozenRun(const char *reason);

	void SetPoint();
	void TpToPoint();
	void TpToPrevPoint();
	void TpToNextPoint();
	void ResetPoints();

private:
	void EnterPrac();
	void ExitPrac();
	void ClearPoints();
	void DoTpToPoint(const PracPoint &pt);
	// Общий гард для всех prac-команд: печатает отказ и возвращает false вне prac.
	bool RequirePrac();
};
