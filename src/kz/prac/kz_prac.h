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
		// Режим и стили на момент заморозки. RunSubmission читает ТЕКУЩИЕ modeService/
		// styleServices, поэтому ран, переживший !mode/!style внутри prac, уехал бы в чужой
		// лидерборд со временем, набранным на другой физике — сверяем на выходе (ExitPrac).
		char modeName[KZ_MAX_MODE_NAME_LENGTH + 1] {};
		// Канонический список стилей — тот же вид, что ключ SavedRuns (BuildStylesString, 64 симв.).
		char styles[65] {};
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
	// Летел ли игрок в момент ухода в спектатор — возврат восстанавливает ровно это состояние
	// (вход в prac ноуклип не включает, поэтому включать его на возврате безусловно нельзя).
	bool noclipBeforeSpec {};
	FrozenRun frozen;
	CUtlVector<PracPoint> points;
	i32 currentIndex {};

public:
	static void Init();
	// Сброс замороженных ранов у всех игроков — по образцу KZTimerService::TimerStopAll.
	// Нужен там, где ран умирает у всех сразу (старт/рестарт раунда).
	static void DropFrozenRunAll(const char *reason);
	virtual void Reset() override;

	bool IsInPrac()
	{
		return this->inPrac;
	}

	bool HasFrozenRun()
	{
		return this->frozen.active;
	}

	// Общее условие "есть замороженный ран, в который можно вернуться" — используется в
	// SavedRuns (kz_savedrun.cpp, save_savedrun.cpp) вместо дублирования IsInPrac() && HasFrozenRun().
	bool HasActiveFrozenRun()
	{
		return this->inPrac && this->frozen.active;
	}

	// Снапшот для SavedRuns при дисконнекте в prac (Task 7). Не мутирует состояние.
	const FrozenRun &GetFrozenRun()
	{
		return this->frozen;
	}

	// Тоггл: вход в prac (заморозка рана, если он есть) или возврат в замороженный ран.
	void TogglePrac();
	// Потеря prac без возврата в ран (смерть, !r, смена карты, смена режима/стиля).
	// reason — в лог как run_lost, значения берём из словаря TimerStop (kz_timer.h), а не
	// изобретаем (nullptr = не логировать, смена карты); phrase — ключ чат-фразы
	// (nullptr = молча, когда вызывающий уже напечатал свою причину).
	void DropFrozenRun(const char *reason, const char *phrase = "Prac - Run Lost");

	void SetPoint();
	void TpToPoint();
	void TpToPrevPoint();
	void TpToNextPoint();
	void ResetPoints();

	// Уход в спектатор: prac и замороженный ран НЕ теряются, гасим только ноуклип
	// и стек prac-точек (pawn у обсервера всё равно исчезает).
	void OnJoinSpectator();
	// Возврат из спектатора: если игрок был в prac — снова включить ноуклип.
	void OnPlayerSpawn();

private:
	void EnterPrac();
	void ExitPrac();
	void ClearPoints();
	void DoTpToPoint(const PracPoint &pt);
	// Общий гард для всех prac-команд: печатает отказ и возвращает false вне prac.
	bool RequirePrac();
	// Гард всех prac-телепортов: вне prac или при пустом стеке печатает отказ и возвращает false.
	bool RequirePracPoint();
	// ЕДИНЫЙ гард всех четырёх prac-путей, которые трогают пешку (EnterPrac, ExitPrac,
	// DoTpToPoint, OnPlayerSpawn): HandleNoclip, ForcePause и Teleport разыменовывают пешку и
	// move services без проверок (kz_noclip.cpp, KZTimerService::ForcePause), а у спектатора
	// пешки нет вовсе. Звать ДО любых мутаций состояния. showError=false — тихий путь
	// (колбэк спауна), true — игрок позвал команду сам.
	bool RequireLivePawn(bool showError);
};
