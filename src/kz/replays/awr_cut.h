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
		int32_t cpIndex;   // TickData::checkpoint.index
		int32_t cpCount;   // TickData::checkpoint.checkpointCount
		int32_t tpCount;   // TickData::checkpoint.teleportCount
		float origin[3];   // TickData::post.origin
	};

	// Интервал ИНДЕКСОВ кадров, оба конца включительно.
	struct Interval
	{
		uint32_t from;
		uint32_t to;
	};

	struct CutResult
	{
		bool ok = false;
		const char *reason = "";        // dest_not_found | counter_mismatch | empty
		std::vector<Interval> dead;      // мёртвые интервалы, по возрастанию, без пересечений
		uint32_t teleports = 0;          // число прибытий ТП по кадрам
		uint64_t awrMs = 0;              // timeMs - мёртвое время (за вычетом пересечения с паузами)
	};

	// pauses — записанные паузы в индексах кадров (включительно), могут быть пустыми.
	// tickInterval — секунд на серверный тик (ENGINE_FIXED_TICK_INTERVAL = 1/64).
	CutResult ComputeAwrCut(const Frame *frames, uint32_t count, const Interval *pauses, uint32_t pauseCount, uint64_t timeMs,
							double tickInterval);

	// Живые интервалы (дополнение dead на [0, count-1]) — нужны !lead.
	std::vector<Interval> LiveIntervals(const std::vector<Interval> &dead, uint32_t count);
}
