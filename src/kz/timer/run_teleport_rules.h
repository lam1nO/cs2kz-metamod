#pragma once
#include <cstdint>

// Правила, которые держат PRO/NUB рана честным (T-19, 20.10). Чистые функции — их гоняет
// host-тест tests/run_teleport_rules_test.cpp.
//
// Баг: выход из стартовой зоны безусловно звал ResetCheckpoints() — чекпоинты и счётчик ТП в ноль.
// В апстриме это безопасно: в зону нельзя войти, не остановив таймер (StartTouch -> TimerStop), так
// что на выходе рана нет. Восстановления рана (SavedRuns-бэкап, выход из !prac) поднимают таймер,
// когда игрок уже стоит в стартовой зоне (спаун там / prac разрешает в неё влетать), и телепортируют
// его наружу: EndTouch приходит при ИДУЩЕМ ране, ТП обнуляются, TimerStart отказывает (тот же курс /
// только что телепортирован) — ран со старым временем доезжал до финиша как PRO.
namespace KZ::timer::rules
{
	enum class StartZoneExit
	{
		// prac: только prac-часы, чекпоинты замороженного рана живые.
		PracClock,
		// Ран идёт и это его зона или он восстановлен — попасть в зону он мог только
		// телепортом/восстановлением, а не входом (вход остановил бы таймер). Выход из зоны такой
		// ран не трогает: ни сброса ТП, ни нового старта.
		KeepRun,
		// Обычный выход: сброс чекпоинтов/ТП и попытка старта. Сюда же — выход из стартовой зоны
		// ДРУГОГО курса при обычном ране (перекрытые зоны A и B: вышел из A, потом из B — ран B,
		// как в апстриме).
		ResetAndStart,
	};

	// sameCourse — зона принадлежит курсу идущего рана; restored — у рана стоит пол ТП
	// (восстановлен из бэкапа/prac, его пол всегда ≥ 1).
	inline StartZoneExit OnStartZoneExit(bool inPrac, bool timerRunning, bool sameCourse, bool restored)
	{
		if (inPrac)
		{
			return StartZoneExit::PracClock;
		}
		if (timerRunning && (sameCourse || restored))
		{
			return StartZoneExit::KeepRun;
		}
		return StartZoneExit::ResetAndStart;
	}

	// Счётчик ТП, который видит финиш: живой счётчик, но не ниже «пола» рана. Пол ставит
	// восстановление (SavedRuns/prac) и снимает только новый старт/стоп таймера — так любой
	// будущий путь, обнуливший живой счётчик посреди восстановленного рана, не сделает его PRO.
	inline uint32_t EffectiveTeleports(uint32_t live, uint32_t floor)
	{
		return live > floor ? live : floor;
	}

	// Восстановленный ран никогда не PRO (решение пользователя 15.08): минимум 1 ТП.
	inline uint32_t RestoredTeleports(uint32_t saved)
	{
		return saved > 0 ? saved : 1;
	}
} // namespace KZ::timer::rules
