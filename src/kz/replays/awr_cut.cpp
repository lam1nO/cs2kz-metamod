#include "awr_cut.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace KZ::replaysystem::awr
{
	// Квадрат расстояния: сравнения идут по нему (корень в скане назад — лишние такты на
	// каждый кадр), сам корень нужен только строке detail.
	static float DistSq(const float *a, const float *b)
	{
		const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
		return dx * dx + dy * dy + dz * dz;
	}

	static float Dist(const float *a, const float *b)
	{
		return std::sqrt(DistSq(a, b));
	}

	static constexpr float AWR_DEST_TOLERANCE_SQ = AWR_DEST_TOLERANCE * AWR_DEST_TOLERANCE;

	// Кадр постановки чекпоинта №k: первый кадр, где cpCount вырос ДО k+1 (позднейшая постановка
	// побеждает — после сброса счётчика индексы переиспользуются). Только внутри окна рана:
	// чекпоинты предзаписи к рану не относятся, а их позиции сшили бы ран с предстартовым куском.
	static std::vector<int64_t> BuildCpSetIndex(const Frame *frames, uint32_t runStart, uint32_t runEnd)
	{
		std::vector<int64_t> cpSet;
		for (uint32_t i = runStart + 1; i <= runEnd; i++)
		{
			int32_t prev = frames[i - 1].cpCount, cur = frames[i].cpCount;
			for (int32_t k = prev; k < cur; k++)
			{
				if (k < 0)
				{
					continue;
				}
				if ((size_t)k >= cpSet.size())
				{
					cpSet.resize(k + 1, -1);
				}
				cpSet[k] = i;
			}
		}
		return cpSet;
	}

	// Подходит ли кадр j как место, ОТКУДА игрок телепортировался в позицию a. Возвращает
	// индекс последнего ЖИВОГО кадра (то самое D) или -1.
	//
	// Позиция чекпоинта/undo снята в СЕРЕДИНЕ тика (см. AWR_DEST_TOLERANCE), поэтому она
	// сравнивается с обоими концами кадра:
	//  - совпала с post (конец тика j) → игрок был там к концу j, последний живой кадр = j;
	//  - совпала с pre (начало тика j) → он был там ещё до симуляции j, значит последним
	//    живым надо считать j-1, иначе в живой путь попал бы кадр, уже уводящий его прочь.
	// На нижней границе окна j-1 упирается в runStart: срезать сам старт рана нельзя.
	static int64_t MatchDest(const Frame *frames, uint32_t j, const float *a, uint32_t runStart)
	{
		if (DistSq(a, frames[j].origin) <= AWR_DEST_TOLERANCE_SQ)
		{
			return (int64_t)j;
		}
		if (DistSq(a, frames[j].preOrigin) <= AWR_DEST_TOLERANCE_SQ)
		{
			return j > runStart ? (int64_t)j - 1 : (int64_t)runStart;
		}
		return -1;
	}

	// Разбор неудачного поиска назначения — только для строки detail в логе.
	struct DestProbe
	{
		int64_t cpFrame = -1;    // S: кадр постановки чекпоинта по индексу, -1 если нет
		float cpPostDist = -1.0f;
		float cpPreDist = -1.0f;
		int64_t bestFrame = -1;  // ближайший кадр скана
		float bestDistSq = -1.0f;
	};

	// Кадр назначения телепорта, прибывшего на кадре t; -1 — не нашли.
	static int64_t DestFrame(const Frame *frames, uint32_t t, const std::vector<int64_t> &cpSet, uint32_t runStart, DestProbe *probe)
	{
		const float *arrival = frames[t].origin;

		// (а) Прямая ссылка: телепорт на чекпоинт №k, кадр его постановки известен.
		int32_t k = frames[t].cpIndex;
		if (k >= 0 && (size_t)k < cpSet.size() && cpSet[k] >= (int64_t)runStart && (uint32_t)cpSet[k] < t)
		{
			const uint32_t sFrame = (uint32_t)cpSet[k];
			if (probe)
			{
				probe->cpFrame = (int64_t)sFrame;
				probe->cpPostDist = Dist(arrival, frames[sFrame].origin);
				probe->cpPreDist = Dist(arrival, frames[sFrame].preOrigin);
			}
			int64_t d = MatchDest(frames, sFrame, arrival, runStart);
			if (d >= 0)
			{
				return d;
			}
		}

		// (б) !undo, переполнение списка чекпоинтов, сдвиг индексов: САМЫЙ ПОЗДНИЙ кадр окна
		// в радиусе допуска. Поздний, а не ранний, потому что undo возвращает на позицию
		// ПЕРЕД последним телепортом — это кадр его прибытия, ближайший к t из подходящих.
		for (int64_t j = (int64_t)t - 1; j >= (int64_t)runStart; j--)
		{
			if (probe)
			{
				const float dSq = std::min(DistSq(arrival, frames[j].origin), DistSq(arrival, frames[j].preOrigin));
				if (probe->bestFrame < 0 || dSq < probe->bestDistSq)
				{
					probe->bestFrame = j;
					probe->bestDistSq = dSq;
				}
			}
			int64_t d = MatchDest(frames, (uint32_t)j, arrival, runStart);
			if (d >= 0)
			{
				return d;
			}
		}
		return -1;
	}

	CutResult ComputeAwrCut(const Frame *frames, uint32_t count, const Interval *pauses, uint32_t pauseCount, uint64_t timeMs, double tickInterval,
							uint32_t runStart, uint32_t runEnd)
	{
		CutResult r;
		if (!frames || count == 0)
		{
			r.reason = "empty";
			return r;
		}
		if (runEnd >= count || runStart >= runEnd)
		{
			// Окна нет или оно вырождено — резать нечего и не по чему.
			r.reason = "no_run_window";
			return r;
		}
		std::vector<int64_t> cpSet = BuildCpSetIndex(frames, runStart, runEnd);

		// Прибытия ТП: кадры внутри окна, где вырос teleportCount.
		std::vector<char> arrival(count, 0);
		for (uint32_t i = runStart + 1; i <= runEnd; i++)
		{
			if (frames[i].tpCount > frames[i - 1].tpCount)
			{
				arrival[i] = 1;
				r.teleports++;
			}
		}

		// Инвариант спеки §4.5: число прибытий по кадрам == приросту teleportCount за окно.
		// Разойтись они могут, если счётчик за один кадр прыгнул больше чем на 1 (два телепорта
		// в одном тике или пропущенные кадры записи) — восстановить такую петлю нечем, а
		// молча выдавать неверную сшивку хуже, чем отказать.
		const int64_t expectedTeleports = (int64_t)frames[runEnd].tpCount - (int64_t)frames[runStart].tpCount;
		if ((int64_t)r.teleports != expectedTeleports)
		{
			r.reason = "counter_mismatch";
			std::snprintf(r.detail, sizeof(r.detail), "arrivals=%u expected=%lld window=%u..%u tp_start=%d tp_end=%d", r.teleports,
						  (long long)expectedTeleports, runStart, runEnd, frames[runStart].tpCount, frames[runEnd].tpCount);
			return r;
		}

		// Обход от финиша назад по ссылкам телепортов (спека §4), в пределах окна рана.
		int64_t cursor = (int64_t)runEnd;
		while (cursor > (int64_t)runStart)
		{
			if (!arrival[cursor])
			{
				cursor--;
				continue;
			}
			int64_t d = DestFrame(frames, (uint32_t)cursor, cpSet, runStart, nullptr);
			if (d < 0)
			{
				r.reason = "dest_not_found";
				r.dead.clear();
				// Второй проход — только ради строки detail. Считать дистанции на КАЖДОМ кадре
				// удачного скана незачем: успешный путь обычно обрывается на первых кадрах, а
				// неудачный и так уже прошёл всё окно — лишний проход платится один раз за файл.
				DestProbe probe;
				DestFrame(frames, (uint32_t)cursor, cpSet, runStart, &probe);
				const Frame &a = frames[cursor];
				const float bestDist = probe.bestDistSq >= 0.0f ? std::sqrt(probe.bestDistSq) : -1.0f;
				std::snprintf(
					r.detail, sizeof(r.detail),
					"t=%lld tick=%u arrival=%.1f/%.1f/%.1f cp_index=%d cp_frame=%lld cp_post_d=%.1f cp_pre_d=%.1f best_frame=%lld best_d=%.1f",
					(long long)cursor, a.serverTick, a.origin[0], a.origin[1], a.origin[2], a.cpIndex, (long long)probe.cpFrame, probe.cpPostDist,
					probe.cpPreDist, (long long)probe.bestFrame, bestDist);
				return r;
			}
			r.dead.push_back({(uint32_t)d + 1, (uint32_t)cursor});
			cursor = d;
		}
		std::reverse(r.dead.begin(), r.dead.end());

		// Мёртвое время по serverTick (кадры могут быть с пропусками), минус пересечение с паузами:
		// пауза в time_ms уже не входит, вычесть её дважды нельзя.
		auto ticksBetween = [&](uint32_t from, uint32_t to) -> uint64_t
		{
			uint32_t a = frames[from > 0 ? from - 1 : 0].serverTick, b = frames[to].serverTick;
			return b > a ? b - a : 0;
		};
		uint64_t deadTicks = 0;
		for (const Interval &d : r.dead)
		{
			deadTicks += ticksBetween(d.from, d.to);
			for (uint32_t p = 0; p < pauseCount; p++)
			{
				uint32_t f = std::max(d.from, pauses[p].from), t = std::min(d.to, pauses[p].to);
				if (f <= t)
				{
					deadTicks -= std::min(deadTicks, ticksBetween(f, t));
				}
			}
		}
		uint64_t deadMs = (uint64_t)((double)deadTicks * tickInterval * 1000.0 + 0.5);
		r.awrMs = timeMs > deadMs ? timeMs - deadMs : 0;
		r.ok = true;
		r.reason = "";
		return r;
	}

	std::vector<Interval> LiveIntervals(const std::vector<Interval> &dead, uint32_t count)
	{
		std::vector<Interval> live;
		if (count == 0)
		{
			return live;
		}
		uint32_t start = 0;
		for (const Interval &d : dead)
		{
			if (d.from > start)
			{
				live.push_back({start, d.from - 1});
			}
			start = d.to + 1;
		}
		if (start <= count - 1)
		{
			live.push_back({start, count - 1});
		}
		return live;
	}
}
