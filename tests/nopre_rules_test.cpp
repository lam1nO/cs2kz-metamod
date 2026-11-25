#include "kz/style/nopre_rules.h"
#include "kz/style/style_key.h"

#include <cassert>
#include <limits>

int main()
{
	using namespace KZ::style;
	assert(!ShouldRemoveCrouchJumpBind(true, true, false, true));
	assert(ShouldRemoveCrouchJumpBind(false, true, false, true));
	assert(!ShouldRemoveCrouchJumpBind(false, true, true, true));
	assert(!ShouldRemoveCrouchJumpBind(false, false, false, true));
	assert(!ShouldRemoveCrouchJumpBind(false, true, false, false));
	assert(NoprePerfSpeed(500.0f, 287.5f) == 287.5f);
	assert(NoprePerfSpeed(260.0f, 287.5f) == 260.0f);
	assert(NoprePerfSpeed(260.0f, -1.0f) == 260.0f);
	assert(NoprePerfSpeed(500.0f, std::numeric_limits<float>::quiet_NaN()) == 287.5f);
	assert(NoprePerfVerticalSpeed(298.875f, 800.0f, 302.0f, true) == 302.0f);
	assert(NoprePerfVerticalSpeed(302.0f, 800.0f, 302.0f, true) == 302.0f);
	assert(NoprePerfVerticalSpeed(298.875f, 800.0f, 302.0f, false) == 298.875f);
	assert(NoprePerfVerticalSpeed(298.875f, 400.0f, 302.0f, true) == 298.875f);
	assert(NoprePerfVerticalSpeed(298.875f, 800.0f, 250.0f, true) == 298.875f);
	assert(NoprePerfVerticalSpeed(327.0f, 800.0f, 302.0f, true) == 327.0f);
	for (const bool longJump : {false, true})
	{
		const float expected[] = {longJump ? 255.0f : 265.0f, longJump ? 261.0f : 270.0f, longJump ? 264.0f : 280.0f,
			longJump ? 267.0f : 290.0f, longJump ? 270.0f : 295.0f};
		for (int i = 0; i < 5; ++i)
		{
			assert(NopreDistanceTierOffset(longJump ? NopreJumpGroup::LongJump : NopreJumpGroup::Hop, expected[i] - 0.01f) == i);
			assert(NopreDistanceTierOffset(longJump ? NopreJumpGroup::LongJump : NopreJumpGroup::Hop, expected[i]) == i + 1);
		}
	}
	assert(NopreDistanceTierOffset(NopreJumpGroup::LongJump, 274.99f) == 5);
	assert(NopreDistanceTierOffset(NopreJumpGroup::LongJump, 275.0f) == 6);
	assert(NopreDistanceTierOffset(NopreJumpGroup::Hop, 297.99f) == 5);
	assert(NopreDistanceTierOffset(NopreJumpGroup::Hop, 298.0f) == 6);
	assert(NopreDistanceTierOffset(NopreJumpGroup::WeirdJump, 292.99f) == 4);
	assert(NopreDistanceTierOffset(NopreJumpGroup::WeirdJump, 293.0f) == 5);
	assert(NopreDistanceTierOffset(NopreJumpGroup::WeirdJump, 295.99f) == 5);
	assert(NopreDistanceTierOffset(NopreJumpGroup::WeirdJump, 296.0f) == 6);
	assert(NopreDistanceTierOffset(NopreJumpGroup::Jumpbug, 294.99f) == 4);
	assert(NopreDistanceTierOffset(NopreJumpGroup::Jumpbug, 500.0f) == 5);
	assert(StyleKey({"LGJ", "ABH", "LGJ"}) == "ABH+LGJ");
	assert(EncodeStyleKey(StyleKey({"NOPRE", "ABH"})) == "ABH%2BNOPRE");
	assert(StyleKey({}).empty());
}
