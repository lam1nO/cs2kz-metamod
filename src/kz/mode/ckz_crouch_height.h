#pragma once

#include <cmath>
#include <cstdint>

namespace KZ::ckz
{
	class GroundedDuckHistory
	{
		std::int64_t tick {-2};
		bool heldOnTick {};
		bool heldOnPreviousTick {};

	public:
		void Clear()
		{
			*this = {};
		}

		void FinishTick(std::int64_t moveTick, bool onGround, bool duckHeld)
		{
			if (moveTick != tick)
			{
				heldOnPreviousTick = moveTick == tick + 1 && heldOnTick;
				tick = moveTick;
			}
			heldOnTick = onGround && duckHeld;
		}

		bool HeldPreviousTick(std::int64_t moveTick) const
		{
			return moveTick == tick ? heldOnPreviousTick : (moveTick == tick + 1 && heldOnTick);
		}
	};

	inline float StandardJumpVerticalSpeed(float speed, float gravity, float impulse, bool groundedDuck, bool duckInput)
	{
		// Standard CKZ legacy takeoff is 302 u/s when crouched and 298.875 u/s standing.
		// Leave other impulses and gravity alone. This changes no duck state or hull.
		if (duckInput && !groundedDuck && gravity == 800.0f && impulse == 302.0f && std::abs(speed - impulse) < 0.001f)
		{
			return 298.875f;
		}
		return speed;
	}
}
