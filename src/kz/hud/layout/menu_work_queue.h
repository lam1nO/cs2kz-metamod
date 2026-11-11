#pragma once

#include <array>
#include <deque>
#include <string>
#include <utility>

// Общий бюджет всех меню, а не отдельный лимит на каждого игрока.
// Clock возвращает монотонные микросекунды; неделимый шаг может пересечь deadline.
template<int Slots>
class MenuWorkQueue
{
public:
	static constexpr int MaxUnits = 128;
	static constexpr long long MaxMicros = 1000;

	void Push(int slot, std::string op)
	{
		queues[slot].push_back(std::move(op));
	}

	void Clear(int slot)
	{
		queues[slot].clear();
	}

	template<typename Apply, typename Fill, typename Clock>
	int Run(Apply apply, Fill fill, Clock clock)
	{
		const auto start = clock();
		int units = 0, idle = 0;
		while (units < MaxUnits && idle < Slots && clock() - start < MaxMicros)
		{
			const int slot = next;
			next = (next + 1) % Slots;
			bool worked = false;
			if (!queues[slot].empty())
			{
				// Apply вправе закрыть/recycle сущность и очистить очередь.
				auto op = std::move(queues[slot].front());
				queues[slot].pop_front();
				apply(slot, op);
				worked = true;
			}
			else
			{
				worked = fill(slot);
			}
			if (worked)
			{
				++units;
				idle = 0;
			}
			else
			{
				++idle;
			}
		}
		return units;
	}

private:
	std::array<std::deque<std::string>, Slots> queues;
	int next = 0;
};
