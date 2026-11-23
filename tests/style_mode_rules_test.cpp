#include "kz/mode/kzt_perf.h"
#include "kz/style/style_key.h"

#include <cassert>

int main()
{
	using KZ::mode::ResolveKztPerf;
	using KZ::style::CanUseStyleInMode;

	assert(CanUseStyleInMode("LGJ", "VNL"));
	assert(!CanUseStyleInMode("LGJ", "CKZ"));
	assert(!CanUseStyleInMode("LGJ", "KZT"));
	assert(!CanUseStyleInMode("LGJ", ""));
	assert(CanUseStyleInMode("NOPRE", "CKZ"));
	assert(!CanUseStyleInMode("NOPRE", "VNL"));
	assert(!CanUseStyleInMode("NOPRE", "KZT"));
	assert(CanUseStyleInMode("ABH", "KZT"));
	assert(CanUseStyleInMode("AUD", "CKZ"));

	constexpr float tick = 0.015625f;
	constexpr float land = 10.0f;
	KZ::mode::KztAutoBhopContact contact {};
	contact.landingTime = land;
	contact.takeoffFrameEnd = land + tick;
	contact.jumpPressTime = 9.0f;
	contact.latestLanding = true;
	contact.jumpHeld = true;
	contact.positiveImpulse = true;
	// Repeated hops where the held press precedes the landing (and may be absent).
	assert(ResolveKztPerf(false, true, false, true, contact, tick));
	contact.jumpPressTime = -1.0f;
	assert(ResolveKztPerf(false, true, false, true, contact, tick));

	// Ordinary KZT uses the manual classification when the compatibility cvar is off.
	assert(!ResolveKztPerf(false, true, false, false, contact, tick));
	assert(ResolveKztPerf(true, true, false, false, contact, tick));
	// Preserve the old global structural override.
	assert(ResolveKztPerf(false, true, true, false, contact, tick));
	assert(!ResolveKztPerf(true, false, true, false, contact, tick));

	// Non-jump takeoffs, ladders, and a WalkMove in the previous segment all lack structural eligibility.
	assert(!ResolveKztPerf(false, false, false, true, contact, tick));
	contact.latestLanding = false;
	assert(!ResolveKztPerf(false, true, false, true, contact, tick));
	contact.latestLanding = true;
	contact.jumpHeld = false;
	assert(!ResolveKztPerf(false, true, false, true, contact, tick));
	contact.jumpHeld = true;
	contact.walkMoved = true;
	assert(!ResolveKztPerf(false, true, false, true, contact, tick));
	contact.walkMoved = false;
	contact.positiveImpulse = false;
	assert(!ResolveKztPerf(false, true, false, true, contact, tick));
	contact.positiveImpulse = true;

	// A new press after contact still uses the manual window.
	contact.jumpPressTime = land + 0.005f;
	assert(!ResolveKztPerf(false, true, false, true, contact, tick));
	assert(ResolveKztPerf(true, true, false, true, contact, tick));
	contact.jumpPressTime = -1.0f;
	contact.takeoffFrameEnd = land;
	assert(ResolveKztPerf(false, true, false, true, contact, tick));
	contact.takeoffFrameEnd = land - tick;
	assert(!ResolveKztPerf(false, true, false, true, contact, tick));
	contact.takeoffFrameEnd = land + 2 * tick;
	assert(!ResolveKztPerf(false, true, false, true, contact, tick));
}
