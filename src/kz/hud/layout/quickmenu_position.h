#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace KZ::quickmenu
{
	inline constexpr char TabXKey[] = "quickMenuTabX";
	inline constexpr char TabYKey[] = "quickMenuTabY";
	// A stored value just beyond the manual X range means follow the safe area.
	inline constexpr int TabAutoX = 51;

	struct TabPosition
	{
		int x = 0;
		int y = 0;
	};

	// The right-hand overscan is half of the width excluded by safezonex.
	// Express it as a percentage of the safe area, the unit used by Panorama.
	inline int RightOffsetLimit(const char *value)
	{
		if (!value)
		{
			return 0;
		}
		char *end = nullptr;
		const double safe = std::strtod(value, &end);
		if (end == value || *end || !std::isfinite(safe) || safe <= 0 || safe >= 1)
		{
			return 0;
		}
		return static_cast<int>(50 * (1 / (std::max)(0.5, safe) - 1));
	}

	inline TabPosition ClampTabPosition(double x, double y, int rightLimit)
	{
		return {
			std::isfinite(x) ? static_cast<int>(std::clamp(x, -85.0, static_cast<double>(rightLimit))) : 0,
			std::isfinite(y) ? static_cast<int>(std::clamp(y, -45.0, 45.0)) : 0,
		};
	}

	inline TabPosition ResolveTabPosition(double x, double y, int rightLimit)
	{
		return ClampTabPosition(x == TabAutoX ? rightLimit : x, y, rightLimit);
	}

	struct DrawerPlacement
	{
		int x;
		int upperSpace;
		bool opensRight;
	};

	inline DrawerPlacement ResolveDrawerPlacement(TabPosition tab)
	{
		const bool opensRight = tab.x < -50;
		// Choose the edge of a full-width anchor that meets the tab.
		return {tab.x + (opensRight ? 100 : 0), 50 + tab.y, opensRight};
	}
}
