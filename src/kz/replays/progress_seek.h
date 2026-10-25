#ifndef KZ_REPLAY_PROGRESS_SEEK_H
#define KZ_REPLAY_PROGRESS_SEEK_H
#include <cstdint>
#include <cmath>
#include <optional>

namespace KZ::replaysystem::progress
{
	// Сканируем только окно рана; distance возвращает квадрат расстояния до видимого
	// кадра или отрицательное значение для пропущенного кадра. При равенстве — первый.
	template<typename Distance>
	std::optional<uint32_t> FindTick(uint32_t count, uint32_t begin, uint32_t end, Distance distance, float radius = 128.0f)
	{
		if (!count || begin > end || end >= count || !std::isfinite(radius) || radius <= 0.0f)
		{
			return {};
		}
		float best = radius * radius;
		std::optional<uint32_t> result;
		for (uint32_t tick = begin;; ++tick)
		{
			const float candidate = distance(tick);
			if (std::isfinite(candidate) && candidate >= 0.0f && candidate < best)
			{
				best = candidate;
				result = tick;
			}
			if (tick == end)
			{
				break;
			}
		}
		return result;
	}
} // namespace KZ::replaysystem::progress
#endif
