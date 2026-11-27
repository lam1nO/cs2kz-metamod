#include "../src/kz/mode/ckz_crouch_height.h"

#include <cassert>

int main()
{
	using namespace KZ::ckz;
	GroundedDuckHistory history;
	assert(!history.HeldPreviousTick(100));
	history.FinishTick(100, false, true);
	assert(!history.HeldPreviousTick(101)); // Duck held in the air.
	history.FinishTick(101, true, false);
	assert(!history.HeldPreviousTick(102)); // Landed and released before takeoff.
	history.FinishTick(102, true, true);
	assert(!history.HeldPreviousTick(102)); // Two subticks within one tick.
	assert(history.HeldPreviousTick(103));
	history.FinishTick(103, true, false);
	assert(history.HeldPreviousTick(103)); // Same tick, second movement step.
	assert(!history.HeldPreviousTick(104)); // The last step recorded the release.
	history.FinishTick(105, true, true);
	assert(!history.HeldPreviousTick(107)); // Stale tick.
	history.Clear();
	assert(!history.HeldPreviousTick(103)); // New ground contact, teleport or mode reset.

	DuckCommandInput laterPress(true); // The held bit reflects the end of the command.
	laterPress.AddTransition(0.5f, true);
	assert(!laterPress.OccurredBy(0.0f));
	assert(laterPress.OccurredBy(0.5f)); // A press on the same subtick qualifies as input.
	DuckCommandInput pulse;
	pulse.AddTransition(0.0f, true);
	pulse.AddTransition(0.0f, false);
	assert(pulse.OccurredBy(0.0f));
	DuckCommandInput released;
	released.AddTransition(0.5f, false);
	assert(released.OccurredBy(0.0f)); // Duck was held from before this command.
	assert(!DuckCommandInput().OccurredBy(0.5f));
	assert(DuckCommandInput(true).OccurredBy(0.0f));

	constexpr float gravity = 800.0f;
	constexpr float impulse = 302.0f;
	constexpr float standing = 298.875f;
	assert(InvalidCrouchTakeoff(impulse, gravity, impulse, false, true));
	assert(!InvalidCrouchTakeoff(impulse, gravity, impulse, true, true));
	assert(!InvalidCrouchTakeoff(impulse, gravity, impulse, false, false));
	assert(!InvalidCrouchTakeoff(standing, gravity, impulse, false, true));
	assert(!InvalidCrouchTakeoff(350.0f, gravity, impulse, false, true));
	assert(!InvalidCrouchTakeoff(impulse, 400.0f, impulse, false, true));
	assert(!InvalidCrouchTakeoff(impulse, gravity, 300.0f, false, true));
	assert(!InvalidCrouchTakeoff(0.0f, gravity, impulse, false, true));
	assert(!InvalidCrouchTakeoff(impulse, gravity, impulse, false, laterPress.OccurredBy(0.0f)));
	assert(InvalidCrouchTakeoff(impulse, gravity, impulse, false, laterPress.OccurredBy(0.5f)));
}
