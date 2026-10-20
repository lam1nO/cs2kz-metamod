// Направление прыжка (JumpDirection): Forward / Sideways / Backwards — как отдельные «стили»
// джамптопа (решение владельца 20.10). Чистая логика без SDK — для host-теста
// tests/jump_direction_test.cpp.
//
// Считается по всему полёту, а не по моменту отрыва (как у графика стрейфов): иначе можно
// оттолкнуться боком и сразу развернуться вперёд. Каждый вызов AirAccelerate даёт угол между
// взглядом и горизонтальной скоростью и свою длительность; прыжок «боком», если не меньше
// minShare времени угол 45..135°, «спиной» — если больше 135°, иначе «вперёд». Смешанный
// прыжок уходит во «вперёд»: это основной топ, и ему смешанный не даёт преимущества.
#pragma once

#include <cmath>
#include <cstdint>

namespace KZ::jumpstats::direction
{
	enum Direction : int32_t
	{
		Forward = 0,
		Sideways = 1,
		Backwards = 2,
		Count = 3,
	};

	constexpr float minShare = 0.8f;
	// Медленнее этого направление скорости ненадёжно (как у графика стрейфов) — вызов не считаем.
	constexpr float minSpeed = 50.0f;

	// Имена для отчёта, Details и api — английские, как написал владелец.
	inline const char *Name(int32_t dir)
	{
		switch (dir)
		{
			case Sideways:
				return "Sideways";
			case Backwards:
				return "Backwards";
			default:
				return "Forward";
		}
	}

	// Угол взгляда относительно скорости, градусы в (-180, 180].
	inline float YawDelta(float viewYaw, float velX, float velY)
	{
		float velYaw = std::atan2(velY, velX) * 57.29577951308232f;
		float d = std::fmod(viewYaw - velYaw, 360.0f);
		if (d > 180.0f)
		{
			d -= 360.0f;
		}
		else if (d <= -180.0f)
		{
			d += 360.0f;
		}
		return d;
	}

	struct Accumulator
	{
		float time[Count] {};

		void Add(float viewYaw, float velX, float velY, float duration)
		{
			if (!(duration > 0.0f) || !std::isfinite(viewYaw) || std::sqrt(velX * velX + velY * velY) < minSpeed)
			{
				return;
			}
			float a = std::fabs(YawDelta(viewYaw, velX, velY));
			time[a <= 45.0f ? Forward : (a <= 135.0f ? Sideways : Backwards)] += duration;
		}

		int32_t Result() const
		{
			float total = time[Forward] + time[Sideways] + time[Backwards];
			if (!(total > 0.0f))
			{
				return Forward;
			}
			if (time[Sideways] >= minShare * total)
			{
				return Sideways;
			}
			if (time[Backwards] >= minShare * total)
			{
				return Backwards;
			}
			return Forward;
		}
	};
} // namespace KZ::jumpstats::direction
