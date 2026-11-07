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
	const auto corner = ShapeCorner(a, b, c, true);
	assert(corner.pieces == 3);
	assert(near(corner.points[0], {-6, 0, 0}));
	assert(near(corner.points[3], {0, 6, 0}));
	assert(near(corner.points[1], {-8.0f / 3.0f, 2.0f / 3.0f, 0}));
	assert(near(corner.points[2], {-2.0f / 3.0f, 8.0f / 3.0f, 0}));

	const auto dense = ShapeCorner(Vec {-1, 0, 0}, b, Vec {0, 1, 0}, true);
	assert(dense.pieces == 0);
	assert(near(dense.points[0], {-.105f, .105f, 0}));
	assert(near(ShapeCorner(a, b, c, false).points[0], b));
	assert(near(ShapeCorner(a, b, a, true).points[0], b));
	assert(near(ShapeCorner(b, b, c, true).points[0], b));
	assert(near(ShapeCorner(a, b, Vec {20, 0, 0}, true).points[0], b));
	std::cout << "lead geometry PASS\n";
}
