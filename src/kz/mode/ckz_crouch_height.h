#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
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

	class DuckCommandInput
	{
		bool heldAtStart {};
		bool sawTransition {};
		float firstPress {std::numeric_limits<float>::infinity()};

	public:
		explicit DuckCommandInput(bool held = false) : heldAtStart(held) {}

		void AddTransition(float fraction, bool pressed)
		{
			// The command's held bit can reflect its end state. Its first transition tells us its start state.
			if (!sawTransition)
			{
				heldAtStart = !pressed;
				sawTransition = true;
			}
			if (pressed)
			{
				firstPress = std::min(firstPress, fraction);
			}
		}

		bool OccurredBy(float fraction) const
		{
			return heldAtStart || firstPress <= fraction;
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
