// T-19: PRO-ран из бэкапа/prac после курса с телепортами. Модель повторяет порядок вызовов
// сервера (callbacks.cpp ZONE_START, KZTimerService::TimerStart/TimerStop, ApplySnapshot,
// KZPracService::EnterPrac/ExitPrac, KZCheckpointService) на правилах run_teleport_rules.h —
// по одному сценарию на каждый путь, где счётчик ТП мог потеряться.
#include "../src/kz/timer/run_teleport_rules.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
using namespace KZ::timer::rules;

struct Sim
{
	// checkpoint service
	uint32_t tpCount = 0, tpFloor = 0;
	int checkpoints = 0;
	// timer
	bool running = false;
	double time = 0;
	// prac
	bool inPrac = false;
	struct
	{
		bool active = false;
		double time = 0;
		uint32_t tp = 0;
		int cps = 0;
	} frozen;
	bool justTeleported = false;
	bool inStartZone = false;
	int zoneCourse = 0; // курс стартовой зоны, где стоит игрок
	int runCourse = -1; // курс идущего рана

	uint32_t Tps() const
	{
		return EffectiveTeleports(tpCount, tpFloor);
	}

	void ResetCheckpoints()
	{
		checkpoints = 0;
		tpCount = 0;
	}

	void TimerStop()
	{
		if (!running) return;
		running = false;
		tpFloor = 0;
	}

	bool TimerStart()
	{
		if (justTeleported || (running && runCourse == zoneCourse)) return false; // CanStartRunHere / тот же курс
		running = true;
		time = 0;
		tpFloor = 0;
		runCourse = zoneCourse;
		return true;
	}

	void StartZoneEnter()
	{
		inStartZone = true;
		if (inPrac) return;
		ResetCheckpoints();
		TimerStop();
	}

	void StartZoneExit()
	{
		inStartZone = false;
		switch (OnStartZoneExit(inPrac, running, runCourse == zoneCourse, tpFloor > 0))
		{
			case StartZoneExit::PracClock: break;
			case StartZoneExit::KeepRun: break;
			case StartZoneExit::ResetAndStart:
				ResetCheckpoints();
				TimerStart();
				break;
		}
	}

	void Cp()
	{
		checkpoints++;
	}

	void Tp()
	{
		tpCount++;
		justTeleported = true;
	}

	void Tick(double dt)
	{
		justTeleported = false;
		if (running) time += dt;
	}

	// KZSavedRunService::SaveOnDisconnect -> строка SavedRuns
	struct Row
	{
		double time;
		uint32_t tp;
		int cps;
	};

	Row Save() const
	{
		if (frozen.active) return {frozen.time, RestoredTeleports(frozen.tp), frozen.cps};
		return {time, RestoredTeleports(Tps()), checkpoints};
	}

	// ApplySnapshot: RestoreFromSnapshot (cp + timer), телепорт на чекпоинт/pos. Телепорт из
	// стартовой зоны (игрок на спауне) даёт EndTouch на следующем тике.
	void Restore(const Row &r)
	{
		checkpoints = r.cps;
		tpCount = tpFloor = RestoredTeleports(r.tp);
		running = true;
		runCourse = 0; // курс бэкапа
		time = r.time;
		justTeleported = true;
		if (inStartZone) StartZoneExit();
	}

	void EnterPrac()
	{
		frozen = {true, time, Tps() + 1, checkpoints};
		inPrac = true;
		TimerStop();
	}

	// ExitPrac: inPrac=false, RestoreFromSnapshot, EndTouchAll (зона, где игрок стоит), телепорт.
	void ExitPrac()
	{
		inPrac = false;
		running = true;
		time = frozen.time;
		checkpoints = frozen.cps;
		tpCount = tpFloor = frozen.tp;
		frozen = {};
		if (inStartZone) StartZoneExit();
		justTeleported = true;
	}

	bool FinishIsPro() const
	{
		assert(running);
		return Tps() == 0;
	}
};

static Sim NubRunWithTps(int tps)
{
	Sim s;
	s.inStartZone = true;
	s.Tick(1);
	s.StartZoneExit();
	assert(s.running && s.Tps() == 0);
	for (int i = 0; i < tps; i++)
	{
		s.Tick(10);
		s.Cp();
		s.Tick(10);
		s.Tp();
	}
	s.Tick(100);
	return s;
}

int main()
{
	// 0. Честный PRO: без ТП финиш PRO, с ТП — нет.
	{
		Sim s = NubRunWithTps(0);
		assert(s.FinishIsPro());
		Sim n = NubRunWithTps(3);
		assert(!n.FinishIsPro() && n.Tps() == 3);
	}

	// 1. SavedRuns: бэкап NUB-рана, вход (на этом или ДРУГОМ сервере) — спаун в стартовой зоне,
	//    восстановление телепортирует на чекпоинт -> EndTouch при идущем ране. Был PRO.
	{
		Sim a = NubRunWithTps(5);
		Sim::Row row = a.Save();
		Sim b;
		b.inStartZone = true;
		b.StartZoneEnter(); // спаун в старте
		b.Tick(0.1);
		b.Restore(row);
		b.Tick(1);
		assert(b.running);
		assert(b.time > row.time);
		assert(b.checkpoints == 5);
		assert(b.Tps() == 5);
		assert(!b.FinishIsPro());
	}

	// 2. SavedRuns: ран без единого !tp (только !cp) — восстановленный ран всё равно не PRO.
	{
		Sim a = NubRunWithTps(0);
		a.Cp();
		Sim::Row row = a.Save();
		assert(row.tp == 1);
		Sim b;
		b.inStartZone = true;
		b.StartZoneEnter();
		b.Restore(row);
		assert(!b.FinishIsPro());
	}

	// 3. !prac, влетел в стартовую зону (prac это разрешает), !prac — возврат к рану. Был PRO.
	{
		Sim s = NubRunWithTps(2);
		s.EnterPrac();
		s.StartZoneEnter(); // в prac — без эффекта
		assert(s.frozen.active);
		s.ExitPrac();
		s.Tick(1);
		assert(s.running && s.checkpoints == 2);
		assert(s.Tps() == 3); // 2 + штраф prac
		assert(!s.FinishIsPro());
	}

	// 4. PRO-ран -> !prac в стартовой зоне -> !prac: штраф +1 не теряется.
	{
		Sim s = NubRunWithTps(0);
		s.EnterPrac();
		s.StartZoneEnter();
		s.ExitPrac();
		assert(s.Tps() == 1 && !s.FinishIsPro());
	}

	// 5. Дисконнект в prac (pos-снапшот) -> восстановление на спауне в старте.
	{
		Sim s = NubRunWithTps(4);
		s.EnterPrac();
		Sim::Row row = s.Save();
		assert(row.tp == 5);
		Sim b;
		b.inStartZone = true;
		b.StartZoneEnter();
		b.Restore(row);
		assert(b.Tps() == 5 && !b.FinishIsPro());
	}

	// 6. Пол защищает от любого будущего пути, обнулившего живой счётчик посреди
	//    восстановленного рана (вызов ResetCheckpoints мимо зоны).
	{
		Sim a = NubRunWithTps(7);
		Sim b;
		b.Restore(a.Save());
		b.ResetCheckpoints();
		assert(b.running && b.Tps() == 7 && !b.FinishIsPro());
	}

	// 7. Штатные пути не сломаны: вход в старт останавливает ран, выход — новый PRO с нуля
	//    (пол восстановленного рана снят стопом/стартом).
	{
		Sim a = NubRunWithTps(3);
		Sim b;
		b.Restore(a.Save());
		b.Tick(1);
		b.StartZoneEnter();
		assert(!b.running);
		b.Tick(1);
		b.StartZoneExit();
		assert(b.running && b.time == 0 && b.Tps() == 0 && b.FinishIsPro());
	}

	// 8. Повторный бэкап восстановленного рана (вышел снова, зашёл на третий сервер) — ТП растут,
	//    не теряются.
	{
		Sim a = NubRunWithTps(2);
		Sim b;
		b.inStartZone = true;
		b.StartZoneEnter();
		b.Restore(a.Save());
		b.Tick(5);
		b.Tp();
		Sim c;
		c.inStartZone = true;
		c.StartZoneEnter();
		c.Restore(b.Save());
		assert(c.Tps() == 3 && !c.FinishIsPro());
	}

	// 9. Правило выхода из зоны.
	for (int m = 0; m < 4; m++)
	{
		assert(OnStartZoneExit(true, true, m & 1, m & 2) == StartZoneExit::PracClock);
		assert(OnStartZoneExit(true, false, m & 1, m & 2) == StartZoneExit::PracClock);
		assert(OnStartZoneExit(false, false, m & 1, m & 2) == StartZoneExit::ResetAndStart);
	}
	assert(OnStartZoneExit(false, true, true, false) == StartZoneExit::KeepRun);
	assert(OnStartZoneExit(false, true, false, true) == StartZoneExit::KeepRun);
	assert(OnStartZoneExit(false, true, true, true) == StartZoneExit::KeepRun);
	assert(OnStartZoneExit(false, true, false, false) == StartZoneExit::ResetAndStart);

	// 10. Перекрытые стартовые зоны курсов A(0) и B(1), обычный ран: вышел из A (ран A), потом
	//     из B — стартует ран B с нуля, как в апстриме.
	{
		Sim s;
		s.inStartZone = true;
		s.zoneCourse = 0;
		s.Tick(1);
		s.StartZoneExit();
		assert(s.running && s.runCourse == 0);
		s.Tick(1);
		s.zoneCourse = 1;
		s.inStartZone = true; // ещё стоит в B
		s.StartZoneExit();
		assert(s.running && s.runCourse == 1 && s.time == 0 && s.Tps() == 0);
	}

	// 11. Бэкап курса 0, спаун в стартовой зоне курса 1 (бонус у спауна) — ран не теряет ТП.
	{
		Sim a = NubRunWithTps(4);
		Sim b;
		b.inStartZone = true;
		b.zoneCourse = 1;
		b.StartZoneEnter();
		b.Restore(a.Save());
		assert(b.running && b.runCourse == 0 && b.Tps() == 4 && !b.FinishIsPro());
	}
	assert(EffectiveTeleports(0, 4) == 4 && EffectiveTeleports(9, 4) == 9 && EffectiveTeleports(0, 0) == 0);
	assert(RestoredTeleports(0) == 1 && RestoredTeleports(6) == 6);

	printf("run_teleport_rules_test: OK\n");
	return 0;
}
