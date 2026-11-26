#include "../src/kz/beam/trail_appearance.h"

#include <cassert>
#include <limits>

int main()
{
	using namespace trailappearance;

	assert(!HasPersonalColor(0));
	assert(HasPersonalColor(0x000000ff));
	assert(HasPersonalColor(0xb02ad7ff));
	assert(!HasPersonalColor(0x11223380));
	assert(!HasPersonalColor(-1));
	assert(!HasPersonalColor(0x1000000ffLL));

	assert(WidthScale(50) == .5f);
	assert(WidthScale(175) == 1.75f);
	assert(WidthScale(200) == 2.f);
	assert(WidthScale(49) == 1.f);
	assert(WidthScale(201) == 1.f);
	assert(WidthScale(std::numeric_limits<double>::quiet_NaN()) == 1.f);

	assert(ScaledRadiusOffset(1.f, -.5f, 1.f, .3f, 9.f) == -.5f);
	assert(ScaledRadiusOffset(1.f, -.5f, 2.f, .3f, 9.f) == 0.f);
	assert(ScaledRadiusOffset(.5f, 0.f, .5f, .15f, 8.5f) == -.25f);
	assert(ScaledRadiusOffset(1.f, 8.f, 2.f, .3f, 9.f) == 8.f);
}
