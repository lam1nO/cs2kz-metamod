#pragma once

#include <algorithm>
#include <cmath>

namespace KZ::style
{
	inline constexpr char NopreName[] = "NOPRE";
	inline constexpr float NopreDefaultCap = 287.5f;

	inline bool ShouldRemoveCrouchJumpBind(bool nopre, bool onGround, bool wasDucking, bool justJumped)
	{
		return !nopre && onGround && !wasDucking && justJumped;
	}

	inline int NopreDistanceTierOffset(bool longJump, float distance)
	{
		constexpr float longJumpThresholds[] = {255.0f, 261.0f, 264.0f, 267.0f, 270.0f};
		constexpr float hopThresholds[] = {265.0f, 270.0f, 280.0f, 290.0f, 295.0f};
		const auto &thresholds = longJump ? longJumpThresholds : hopThresholds;
		int tier = 0;
		for (float threshold : thresholds)
		{
			if (distance < threshold)
			{
				break;
			}
			++tier;
		}
		return tier;
	}

	inline float NoprePerfVerticalSpeed(float speed, float gravity, float jumpImpulse, bool standardTakeoff)
	{
		// The standard standing legacy jump leaves the ground at 298.875 u/s.
		// Restore the 302 u/s impulse for a 57-unit apex, as in the crouched takeoff.
		if (standardTakeoff && gravity == 800.0f && jumpImpulse == 302.0f && std::abs(speed - 298.875f) < 0.001f)
		{
			return jumpImpulse;
		}
		return speed;
	}

	inline float NoprePerfSpeed(float speed, float cap)
	{
		return std::min(speed, std::isfinite(cap) && cap >= 250.0f ? cap : NopreDefaultCap);
	}
}
