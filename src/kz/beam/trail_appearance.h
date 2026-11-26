#pragma once

#include <algorithm>
#include <cstdint>

namespace trailappearance
{
	inline constexpr char BeamColor[] = "beamTrailColor";
	inline constexpr char BeamWidth[] = "beamTrailWidth";
	inline constexpr char LeadColor[] = "leadTrailColor";
	inline constexpr char LeadWidth[] = "leadTrailWidth";

	inline bool HasPersonalColor(std::int64_t packed)
	{
		return packed >= 0 && packed <= 0xffffffffLL && (packed & 0xff) == 0xff;
	}

	inline float WidthScale(double percent)
	{
		return percent >= 50 && percent <= 200 ? static_cast<float>(percent / 100) : 1.f;
	}

	inline float ScaledRadiusOffset(float base, float offset, float scale, float minRadius, float maxRadius)
	{
		const float radius = std::clamp(base + offset, minRadius, maxRadius);
		return std::clamp(radius * scale, minRadius, maxRadius) - base;
	}
}
