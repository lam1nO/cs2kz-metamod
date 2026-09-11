#include "ctimer.h"

CUtlVector<CTimerBase *> g_NonPersistentTimers;
CUtlVector<CTimerBase *> g_PersistentTimers;

static_function void ProcessTimerList(CUtlVector<CTimerBase *> &timers)
{
	for (int i = timers.Count() - 1; i >= 0; i--)
	{
		auto timer = timers[i];
		f64 currentTime = timer->useRealTime ? g_pKZUtils->GetGlobals()->realtime : g_pKZUtils->GetGlobals()->curtime;
		if (timer->lastExecute == -1)
		{
			timer->lastExecute = currentTime;
		}

		// Часы движка пошли НАЗАД. На changelevel обнуляются обе шкалы — и curtime, и то, что
		// SDK форка зовёт realtime (доказано 08.09 на srv-5: уборщик оружия
		// kz_weapon_ground_cleanup после смены карты молчал ровно длительность прошлой карты,
		// 15,6 мин). lastExecute персистентного таймера смену карты переживает, поэтому
		// разница lastExecute+interval - currentTime остаётся положительной, пока новые часы
		// не догонят старое значение, и таймер спит всё это время.
		// Чинит КЛАСС, а не один уборщик: то же самое было у PrintTips (подсказки пропадают на
		// N минут после смены карты), CheckRestart (пустая карта не рестартится),
		// Mapi_PrintErrors, таймеров outbox/AWR-бэкфилла и сторожей зон/невидимки —
		// RemoveNonPersistentTimers() в форке ниоткуда не зовётся, так что персистентны
		// де-факто ВСЕ таймеры.
		// Приводим lastExecute к новым часам, а не выполняем немедленно: на первом кадре
		// карты иначе разом выстрелили бы все таймеры форка.
		if (timer->lastExecute > currentTime)
		{
			timer->lastExecute = currentTime;
		}

		if (timer->lastExecute + timer->interval <= currentTime)
		{
			if (!timer->Execute())
			{
				delete timer;
				timers.Remove(i);
			}
			else
			{
				timer->lastExecute = currentTime;
			}
		}
	}
}

void ProcessTimers()
{
	ProcessTimerList(g_PersistentTimers);
	ProcessTimerList(g_NonPersistentTimers);
}

void RemoveNonPersistentTimers()
{
	g_NonPersistentTimers.PurgeAndDeleteElements();
}
