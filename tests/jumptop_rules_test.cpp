#include "../src/kz/jumpstats/jumptop_rules.h"
#include <cassert>
#include <cstdio>
using namespace KZ::jumptop::rules;

int main()
{
	// Типы: LJ..JB в топе, Fall/Other/Invalid — нет.
	for (int t = 0; t <= 6; t++) assert(IsTrackedJumpType(t));
	assert(!IsTrackedJumpType(-1) && !IsTrackedJumpType(7) && !IsTrackedJumpType(8) && !IsTrackedJumpType(9));

	// Дистанция: 186..500, LAJ от 50.
	assert(PassesDistanceRules(0, 186.0f, 0.0f));
	assert(!PassesDistanceRules(0, 185.99f, 0.0f));
	assert(PassesDistanceRules(0, 500.0f, 0.0f));
	assert(!PassesDistanceRules(0, 500.01f, 0.0f));
	assert(PassesDistanceRules(typeLadderJump, 50.0f, 0.0f));
	assert(!PassesDistanceRules(typeLadderJump, 49.9f, 0.0f));
	assert(!PassesDistanceRules(1, 100.0f, 0.0f)); // бхоп 100 — не LAJ, порог 186

	// Понижение: до -eps можно, ниже — нет; вверх можно; LAJ — исключение.
	assert(PassesDistanceRules(0, 250.0f, -0.03125f));
	assert(!PassesDistanceRules(0, 250.0f, -0.04f));
	assert(PassesDistanceRules(3, 270.0f, 8.0f));
	assert(PassesDistanceRules(typeLadderJump, 120.0f, -30.0f));
	assert(!PassesDistanceRules(5, 250.0f, -1.0f)); // LAH — не исключение

	// PB по дистанции — строго больше.
	assert(IsBetter(false, 0, 250.1, 0, 250.0));
	assert(!IsBetter(false, 0, 250.0, 0, 250.0));
	assert(!IsBetter(false, 0, 249.0, 0, 250.0));

	// Блочный PB — больше блок; при равном блоке — больше дистанция. Меньший блок с большой
	// дистанцией не бьёт больший блок.
	assert(IsBetter(true, 255, 256.0, 250, 270.0));
	assert(IsBetter(true, 250, 251.0, 250, 250.5));
	assert(!IsBetter(true, 250, 250.5, 250, 250.5));
	assert(!IsBetter(true, 245, 280.0, 250, 251.0));

	printf("jumptop_rules_test: OK\n");
	return 0;
}
