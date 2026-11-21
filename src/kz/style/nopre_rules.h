#pragma once

#include <algorithm>
#include <cmath>

namespace KZ::style
{
	inline constexpr char NopreName[] = "NOPRE";
	inline constexpr float NopreDefaultCap = 287.5f;

	inline float NoprePerfSpeed(float speed, float cap)
	{
		return std::min(speed, std::isfinite(cap) && cap >= 250.0f ? cap : NopreDefaultCap);
	}
}
