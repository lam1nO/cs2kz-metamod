#include "awr_cut.h"
#include <algorithm>

namespace KZ::replaysystem::awr
{
	static bool SameOrigin(const Frame &a, const Frame &b)
	{
		// Чекпоинт и undo-точка копируют origin игрока побитно — сравниваем точно.
		return a.origin[0] == b.origin[0] && a.origin[1] == b.origin[1] && a.origin[2] == b.origin[2];
	}

	// Кадр постановки чекпоинта №k: первый кадр, где cpCount вырос ДО k+1 (позднейшая постановка
	// побеждает — после сброса счётчика индексы переиспользуются).
	static std::vector<int64_t> BuildCpSetIndex(const Frame *frames, uint32_t count)
	{
		std::vector<int64_t> cpSet;
		for (uint32_t i = 1; i < count; i++)
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

	// Кадр назначения телепорта, прибывшего на кадре t; -1 — не нашли.
	static int64_t DestFrame(const Frame *frames, uint32_t t, const std::vector<int64_t> &cpSet)
	{
		int32_t k = frames[t].cpIndex;
		if (k >= 0 && (size_t)k < cpSet.size() && cpSet[k] >= 0 && (uint32_t)cpSet[k] < t && SameOrigin(frames[cpSet[k]], frames[t]))
		{
			return cpSet[k];
		}
		// !undo, переполнение списка чекпоинтов: последний прежний кадр с той же позицией.
		for (int64_t j = (int64_t)t - 1; j >= 0; j--)
		{
			if (SameOrigin(frames[j], frames[t]))
			{
				return j;
			}
		}
		return -1;
	}

	CutResult ComputeAwrCut(const Frame *frames, uint32_t count, const Interval *pauses, uint32_t pauseCount, uint64_t timeMs,
							double tickInterval)
	{
		CutResult r;
		if (!frames || count == 0)
		{
			r.reason = "empty";
			return r;
		}
		std::vector<int64_t> cpSet = BuildCpSetIndex(frames, count);

		// Прибытия ТП: кадры, где вырос teleportCount.
		std::vector<char> arrival(count, 0);
		for (uint32_t i = 1; i < count; i++)
		{
			if (frames[i].tpCount > frames[i - 1].tpCount)
			{
				arrival[i] = 1;
				r.teleports++;
			}
		}

		// Обход от финиша назад по ссылкам телепортов (спека §4).
		int64_t cursor = (int64_t)count - 1;
		while (cursor > 0)
		{
			if (!arrival[cursor])
			{
				cursor--;
				continue;
			}
			int64_t d = DestFrame(frames, (uint32_t)cursor, cpSet);
			if (d < 0)
			{
				r.reason = "dest_not_found";
				r.dead.clear();
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
