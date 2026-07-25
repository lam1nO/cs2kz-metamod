#pragma once
#include "../kz.h"
#include "kz/timer/kz_timer.h"
#include "kz/checkpoint/kz_checkpoint.h"

// Режим отработки элементов: заморозка рана + ноуклип + prac-чекпоинты со скоростью.
// Спека: docs/superpowers/specs/2026-07-25-kz-prac-mode-design.md (монорепо).
// Порог «игрок стоял» для возврата из prac: ниже него скорость считаем нулевой и
// возвращаем на паузе. Единицы движка (u/s).
#define KZ_PRAC_STILL_SPEED 5.0f

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
		// Скорость на момент входа: с ревизии 2 в prac можно войти в воздухе, и возврат должен
		// вернуть игрока в тот же полёт, а не уронить с нулевой скоростью.
		Vector velocity {};
		// Игрок реально СТОЯЛ на входе — только тогда возврат ставит ForcePause: она обнуляет
		// скорость, поэтому вошедшему на бегу или в полёте вернула бы не то состояние. Критерий —
		// скорость, а не FL_ONGROUND: бег по земле должен сохраняться (решение пользователя).
		bool enteredStill {};
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
		// Показание prac-часов в момент снятия точки: !practp откручивает часы к нему.
		// Признак «часы шли» тоже часть точки — точка, снятая без действующей попытки
		// (свободный prac до старта, после обнуления ноуклипом), не должна на возврате
		// рождать время из ничего.
		f64 pracTime {};
		bool pracTimeRunning {};
	};

private:
	bool inPrac {};
	// Летел ли игрок в момент ухода в спектатор — возврат восстанавливает ровно это состояние
	// (вход в prac ноуклип не включает, поэтому включать его на возврате безусловно нельзя).
	bool noclipBeforeSpec {};
	FrozenRun frozen;
	CUtlVector<PracPoint> points;
	i32 currentIndex {};
	// prac-часы («репетиция»): СВОЙ счётчик, а не таймер рана. Настоящий таймер в prac обязан
	// оставаться остановленным — на timerRunning висит вся машинерия финиша/сабмита/реплеев/
	// SavedRuns (см. четыре Critical в спеке). Эти часы никуда не уезжают: только худ и строка
	// в чат на финише.
	f64 pracTime {};
	bool pracTimeRunning {};

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

	// prac-часы для худа: значение и «идут ли». Часы стоят = действующей попытки нет.
	f64 GetPracTime()
	{
		return this->pracTime;
	}

	bool IsPracTimeRunning()
	{
		return this->pracTimeRunning;
	}

	// Общее условие "есть замороженный ран, в который можно вернуться" — используется в
	// SavedRuns (kz_savedrun.cpp, save_savedrun.cpp) вместо дублирования inPrac && frozen.active.
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

	// Тик prac-часов. Зовётся из KZPlayer::OnPhysicsSimulatePost рядом с таймерным хуком и
	// тем же шагом ENGINE_FIXED_TICK_INTERVAL — иначе prac-время нельзя было бы сравнивать
	// с настоящим.
	void OnPhysicsSimulatePost();
	// Вето настоящего таймера на выходе из стартовой зоны (см. events.cpp): для prac это
	// «свежая попытка» — часы в 0 и пуск.
	void OnTimerStartBlocked();
	// Включение ноуклипа в prac: попытка недействительна, часы в 0 и стоп. Висит на переходе
	// в MOVETYPE_NOCLIP внутри KZNoclipService::HandleNoclip, а не на команде !nc — иначе
	// ноуклип из меню/бинда правило бы обошёл.
	void OnNoclipEnabled();
	// Касание финишной зоны курса. true = обработали сами (настоящий TimerEnd звать НЕЛЬЗЯ:
	// в prac рана не существует, а весь сабмит висит на нём).
	bool OnEndZoneTouch();
	// Уход в спектатор: prac и замороженный ран НЕ теряются, гасим только ноуклип
	// и стек prac-точек (pawn у обсервера всё равно исчезает).
	void OnJoinSpectator();
	// Возврат из спектатора: если игрок был в prac — снова включить ноуклип.
	void OnPlayerSpawn();

private:
	void EnterPrac();
	void ExitPrac();
	void ClearPoints();
	// Часы в 0 и стоп: попытки нет.
	void ResetPracTime();
	// Захват точки без гардов и сообщений — общий путь для !praccp и для точки №1 на входе.
	void CapturePoint();
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
