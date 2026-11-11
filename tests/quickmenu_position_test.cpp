#include "kz/hud/layout/quickmenu_position.h"

#include <cassert>

int main()
{
	using namespace KZ::quickmenu;
	assert(RightOffsetLimit("0.5") == 50);
	assert(RightOffsetLimit("0.6") == 33);
	assert(RightOffsetLimit("0.9") == 5);
	assert(RightOffsetLimit("1") == 0);
	assert(RightOffsetLimit("0.4") == 50);
	assert(RightOffsetLimit("nan") == 0);
	assert(RightOffsetLimit("0.6oops") == 0);
	assert(RightOffsetLimit(nullptr) == 0);
	const auto right = ClampTabPosition(55, -51, 33);
	assert(right.x == 33 && right.y == -45);
	const auto left = ClampTabPosition(-100, 50, 0);
	assert(left.x == -85 && left.y == 45);
	const auto safe = ClampTabPosition(20, -5, 0);
	assert(safe.x == 0 && safe.y == -5);
	const auto auto05 = ResolveTabPosition(TabAutoX, 0, RightOffsetLimit("0.5"));
	assert(auto05.x == 50 && auto05.y == 0);
	const auto auto06 = ResolveTabPosition(TabAutoX, -5, RightOffsetLimit("0.6"));
	assert(auto06.x == 33 && auto06.y == -5);
	const auto autoUnknown = ResolveTabPosition(TabAutoX, 0, RightOffsetLimit(nullptr));
	assert(autoUnknown.x == 0 && autoUnknown.y == 0);
	const auto manual = ResolveTabPosition(0, 0, RightOffsetLimit("0.6"));
	assert(manual.x == 0 && manual.y == 0);
	const auto defaults = ClampTabPosition(0, 0, 33);
	assert(defaults.x == 0 && defaults.y == 0);
	const auto drawerRight = ResolveDrawerPlacement({33, 0});
	assert(drawerRight.x == 33 && drawerRight.upperSpace == 50 && !drawerRight.opensRight);
	const auto drawerLeft = ResolveDrawerPlacement({-85, -45});
	assert(drawerLeft.x == 15 && drawerLeft.upperSpace == 5 && drawerLeft.opensRight);
	const auto lower = ResolveDrawerPlacement({-50, 45});
	assert(lower.x == -50 && lower.upperSpace == 95 && !lower.opensRight);
}
