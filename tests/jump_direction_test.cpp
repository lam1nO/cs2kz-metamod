#include "../src/kz/jumpstats/jump_direction.h"
#include <cassert>
#include <cstdio>
using namespace KZ::jumpstats::direction;

static int Classify(float viewYaw, float velYawDeg, int n = 100)
{
	Accumulator acc;
	float r = velYawDeg / 57.29577951308232f;
	for (int i = 0; i < n; i++) acc.Add(viewYaw, 280.0f * std::cos(r), 280.0f * std::sin(r), 1.0f / 64);
	return acc.Result();
}

int main()
{
	// Углы и перенос через ±180.
	assert(std::fabs(YawDelta(10.0f, 1.0f, 0.0f) - 10.0f) < 1e-3f);
	assert(std::fabs(YawDelta(-170.0f, -1.0f, 0.01f) - 10.0f) < 0.6f); // скорость ~180°, взгляд -170 => +10
	assert(std::fabs(std::fabs(YawDelta(0.0f, -1.0f, 0.0f)) - 180.0f) < 1e-3f);

	// Сектора.
	assert(Classify(0.0f, 0.0f) == Forward);
	assert(Classify(44.0f, 0.0f) == Forward);
	assert(Classify(90.0f, 0.0f) == Sideways);
	assert(Classify(-90.0f, 0.0f) == Sideways);
	assert(Classify(130.0f, 0.0f) == Sideways);
	assert(Classify(180.0f, 0.0f) == Backwards);
	assert(Classify(-140.0f, 0.0f) == Backwards);
	assert(Classify(170.0f, -170.0f) == Forward); // перенос: разница 20°

	// Боком 80% времени — боком; 79% — вперёд (оттолкнулся боком и развернулся).
	{
		Accumulator acc;
		for (int i = 0; i < 80; i++) acc.Add(90.0f, 280.0f, 0.0f, 1.0f);
		for (int i = 0; i < 20; i++) acc.Add(0.0f, 280.0f, 0.0f, 1.0f);
		assert(acc.Result() == Sideways);
	}
	{
		Accumulator acc;
		for (int i = 0; i < 79; i++) acc.Add(90.0f, 280.0f, 0.0f, 1.0f);
		for (int i = 0; i < 21; i++) acc.Add(0.0f, 280.0f, 0.0f, 1.0f);
		assert(acc.Result() == Forward);
	}
	// Медленные вызовы, нулевая длительность, NaN — не считаются; пусто — вперёд.
	{
		Accumulator acc;
		acc.Add(90.0f, 10.0f, 0.0f, 1.0f);
		acc.Add(90.0f, 280.0f, 0.0f, 0.0f);
		acc.Add(NAN, 280.0f, 0.0f, 1.0f);
		assert(acc.Result() == Forward);
		acc.Add(180.0f, 280.0f, 0.0f, 1.0f);
		assert(acc.Result() == Backwards);
	}
	assert(Name(Forward)[0] == 'F' && Name(Sideways)[0] == 'S' && Name(Backwards)[0] == 'B' && Name(7)[0] == 'F');
	puts("jump_direction_test: ok");
	return 0;
}
