#pragma once

#include "trail_appearance.h"
#include "kz/option/kz_option.h"

struct PlayerTrailAppearance
{
	i64 color;
	f32 scale;

	Color ColorOr(Color fallback) const
	{
		if (!trailappearance::HasPersonalColor(color))
		{
			return fallback;
		}
		return Color((color >> 24) & 0xff, (color >> 16) & 0xff, (color >> 8) & 0xff, 255);
	}
};

inline PlayerTrailAppearance ReadPlayerTrailAppearance(KZOptionService &options, const char *colorKey, const char *widthKey)
{
	return {options.GetPreferenceInt(colorKey), trailappearance::WidthScale(options.GetPreferenceFloat(widthKey, 100))};
}
