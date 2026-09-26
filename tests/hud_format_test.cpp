// tests/hud_format_test.cpp
#include "../src/kz/hud/hud_format.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace KZ::hudfmt;
int main()
{
	char b[64];
	FormatDelta(0.312, true, b, sizeof(b));  assert(!strcmp(b, "+0.312"));
	FormatDelta(-0.184, true, b, sizeof(b)); assert(!strcmp(b, "-0.184"));
	FormatDelta(0.3149, false, b, sizeof(b)); assert(!strcmp(b, "+0.31"));
	FormatDelta(0.0, true, b, sizeof(b));    assert(!strcmp(b, "+0.000"));
	FormatDelta(75.5, true, b, sizeof(b));   assert(!strcmp(b, "+1:15.500"));
	FormatPos(1284.314f, -512.06f, 64.03f, b, sizeof(b)); assert(!strcmp(b, "1284.31 -512.06 64.03"));
	FormatAng(12.4f, -87.13f, b, sizeof(b)); assert(!strcmp(b, "12.40 -87.13"));
	const char *st[] = {"AutoBhop", "LegacyJump"};
	FormatCourseLine("main", "CKZ", st, 2, b, sizeof(b)); assert(!strcmp(b, "MAIN \xC2\xB7 CKZ \xC2\xB7 AUTOBHOP \xC2\xB7 LEGACYJUMP"));
	FormatCourseLine("Bonus 1", "VNL", nullptr, 0, b, sizeof(b)); assert(!strcmp(b, "BONUS 1 \xC2\xB7 VNL \xC2\xB7 NORMAL"));
	// Длинное имя курса в маленький буфер: обрезка без выхода за буфер (n зажимается после snprintf).
	{
		char small[12];
		memset(small, 'Z', sizeof(small));
		const char *many[] = {"ABH", "LGJ", "SW", "HSW"};
		FormatCourseLine("a very long course name", "CKZ", many, 4, small, sizeof(small));
		assert(strlen(small) == sizeof(small) - 1 && !strcmp(small, "A VERY LONG"));
		char tiny[16];
		FormatCourseLine("main", "CKZ", many, 4, tiny, sizeof(tiny));
		assert(strlen(tiny) < sizeof(tiny) && !strncmp(tiny, "MAIN \xC2\xB7 CKZ", 10));
		char guard[24];
		memset(guard, 'Z', sizeof(guard));
		FormatCourseLine("main", "CKZ", many, 4, guard, 8);
		assert(strlen(guard) == 7 && guard[8] == 'Z' && guard[23] == 'Z');
	}
	FormatCheckpointLine(3, 2, b, sizeof(b)); assert(!strcmp(b, "CP 3 \xC2\xB7 TP 2"));
	FormatCheckpointLine(0, 0, b, sizeof(b)); assert(!strcmp(b, "CP 0 \xC2\xB7 TP 0"));
	FormatCheckpointLine(4, -1, b, sizeof(b)); assert(!strcmp(b, "CP 4 \xC2\xB7 AWR"));
	int x, y;
	// центр ячейки 0..63 x 0..35: x=round((c+0.5)*100/64), y=round((r+0.5)*100/36).
	// Плановое ожидание g0_0 (x==0) арифметически неверно: (0+0.5)*100/64=0.78125 → round=1.
	assert(GridCellToPercent("g0_0", 64, 36, x, y) && x == 1 && y == 1);
	assert(GridCellToPercent("g63_35", 64, 36, x, y) && x == 99 && y == 99);
	// Плановое ожидание g32_18 (x==50) арифметически неверно: (32+0.5)*100/64=50.78125 → round=51.
	assert(GridCellToPercent("g32_18", 64, 36, x, y) && x == 51 && y == 51);
	assert(!GridCellToPercent("g64_0", 64, 36, x, y));
	assert(!GridCellToPercent("e_timer", 64, 36, x, y));
	assert(!GridCellToPercent("g1", 64, 36, x, y));
	assert(!strcmp(NoTimePlaceholder(true), "--:--.---"));
	assert(!strcmp(NoTimePlaceholder(false), "--:--.--"));
	assert(!DeltaVisible(true, false, true, 1));
	assert(!DeltaVisible(true, true, false, 1));
	assert(!DeltaVisible(false, true, true, 1));
	assert(!DeltaVisible(true, true, true, 0));
	assert(DeltaVisible(true, true, true, 2));
	puts("hud_format: all tests passed");
	return 0;
}
