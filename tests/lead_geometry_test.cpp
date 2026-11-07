#include <cassert>
#include <cmath>
#include <iostream>

struct Vec
{
	float x, y, z;

	Vec operator+(Vec b) const
	{
		return {x + b.x, y + b.y, z + b.z};
	}

	Vec operator-(Vec b) const
	{
		return {x - b.x, y - b.y, z - b.z};
	}

	Vec operator*(float s) const
	{
		return {x * s, y * s, z * s};
	}

	float Length() const
	{
		return std::sqrt(x * x + y * y + z * z);
	}
};

#include "../src/kz/lead/lead_geometry.h"

static bool near(Vec a, Vec b)
{
	return (a - b).Length() < 1e-4f;
}

int main()
{
	using namespace leadgeometry;
	const Vec a {-20, 0, 0}, b {0, 0, 0}, c {0, 20, 0};
	auto q = RoundCorner(a, b, c, true);
	assert(q.rounded && near(q.entry, {-4, 0, 0}) && near(q.exit, {0, 4, 0}));
	// Quadratic tangent at either end is collinear with its adjoining straight.
	assert(near((q.control - q.entry) * .25f, {1, 0, 0}));
	assert(near((q.exit - q.control) * .25f, {0, 1, 0}));
	auto small = RoundCorner(Vec {-1, 0, 0}, b, Vec {0, 1, 0}, true);
	assert(near(small.entry, {-.45f, 0, 0}) && near(small.exit, {0, .45f, 0}));
	assert(!RoundCorner(a, b, c, false).rounded);
	assert(!RoundCorner(b, b, c, true).rounded);
	assert(!RoundCorner(a, b, a, true).rounded);
	assert(!RoundCorner(a, b, Vec {20, 0, 0}, true).rounded);
	// Unequal edges still use the SAME trim on each side, maintaining tangent joins.
	auto uneven = RoundCorner(Vec {-1, 0, 0}, b, Vec {0, 1000, 0}, true);
	assert(near(uneven.exit, {0, .45f, 0}));
	assert(EdgeBudget(1, true) == 1 && !RoundWithinBudget(1));
	assert(EdgeBudget(3, true) == 1 && EdgeBudget(512, true) == 256);
	assert(EdgeBudget(384, false) == 384);
	std::cout << "lead geometry PASS\n";
}
