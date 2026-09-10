// awr_cut — вырезка телепорт-петель из записанного рана (AWR).
// БЕЗ common.h и SDK: модуль обязан компилироваться голым clang++ (host-тесты
// tests/awr_cut_test.cpp), поэтому здесь uint32_t/int32_t, а не u32/i32 форка.
#pragma once
#include <cstdint>
#include <vector>

namespace KZ::replaysystem::awr
{
	struct Frame
	{
		uint32_t serverTick;
		int32_t cpIndex;     // TickData::checkpoint.index
		int32_t cpCount;     // TickData::checkpoint.checkpointCount
		int32_t tpCount;     // TickData::checkpoint.teleportCount
		float origin[3];     // TickData::post.origin — конец тика
		float preOrigin[3];  // TickData::pre.origin — начало тика
	};

	// Допуск сопоставления позиции прибытия телепорта с кадром, юниты.
	//
	// Побитового совпадения НЕТ и быть не может: KZCheckpointService::SetCheckpoint копирует
	// origin игрока В МОМЕНТ команды (середина тика, игрок обычно на бегу), а в кадре записаны
	// только pre.origin (начало тика) и post.origin (конец). То же у !undo: undoTeleportData
	// хранит позицию на момент команды. Первый живой прогон бэкфилла это и показал —
	// 50 файлов из 50 упали в dest_not_found.
	//
	// 16 юнитов: за один тик при 400 u/s (быстрее в KZ бегают редко) игрок проходит ~6 u, то
	// есть позиция чекпоинта заведомо внутри отрезка pre→post своего кадра, и 16 покрывает его
	// с запасом (+ погрешность позиции после самого телепорта: движок ставит игрока на землю).
	// При этом 16 u — четверть ширины игрока, заведомо меньше расстояния между РАЗНЫМИ местами
	// стояния на маршруте, поэтому чужой чекпоинт под допуск не попадает.
	inline constexpr float AWR_DEST_TOLERANCE = 16.0f;

	// Интервал ИНДЕКСОВ кадров, оба конца включительно.
	struct Interval
	{
		uint32_t from;
		uint32_t to;
	};

	struct CutResult
	{
		bool ok = false;
		// dest_not_found | counter_mismatch | no_run_window | empty. Вызывающий может выставить
		// сюда и свою причину отказа до вызова (not_a_run, course_mismatch — commands.cpp).
		const char *reason = "";
		std::vector<Interval> dead;      // мёртвые интервалы, по возрастанию, без пересечений
		uint32_t teleports = 0;          // число прибытий ТП по кадрам
		uint64_t awrMs = 0;              // timeMs - мёртвое время (за вычетом пересечения с паузами)
		// Разбор отказа для лога (пусто при ok): что именно не сошлось и насколько. Без него
		// живой прогон бэкфилла отвечает только «dest_not_found», и следующий шаг требует
		// ещё одного цикла сборка→канарейка. Фиксированный буфер: структура уходит между
		// потоками воркера бэкфилла.
		char detail[192] = {};
	};

	// pauses — записанные паузы в индексах кадров (включительно), могут быть пустыми.
	// tickInterval — секунд на серверный тик (ENGINE_FIXED_TICK_INTERVAL = 1/64).
	// runStart/runEnd — окно САМОГО рана в индексах кадров, включительно (кадры TIMER_START и
	// TIMER_END). Окно обязательно: в run-реплее есть ~5 с предзаписи до старта и ~4 с хвоста
	// после финиша (RunRecorder), и телепорт в хвосте («!r» сразу после финиша) без окна
	// объявил бы мёртвым весь ран — awrMs≈0, и такая строка выиграла бы минимум у api.
	// Прибытия вне окна игнорируются, назначение телепорта ищется только в [runStart, T)
	// и с допуском AWR_DEST_TOLERANCE (см. выше).
	CutResult ComputeAwrCut(const Frame *frames, uint32_t count, const Interval *pauses, uint32_t pauseCount, uint64_t timeMs, double tickInterval,
							uint32_t runStart, uint32_t runEnd);

	// Живые интервалы (дополнение dead на [0, count-1]) — нужны !lead.
	std::vector<Interval> LiveIntervals(const std::vector<Interval> &dead, uint32_t count);
}
