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
	// A landing is a visible turn even though it changes the surface and beam color.
	const auto landing = ShapeCorner(Vec {-24, 0, 16}, b, Vec {24, 0, 0}, false);
	assert(landing.pieces == 3);
	assert(landing.points[1].z > 0.0f);
	assert(landing.points[2].z > 0.0f);
	assert(landing.points[3].z == 0.0f);
	assert(landing.points[0].x < -6.0f);
	// The dense landing in the kz_bhop_nothing_go AWR must get a curve too.
	const Vec landingPoint {-13369.08496f, -2479.45435f, 192.03125f};
	const auto denseLanding = ShapeCorner(Vec {-13372.74609f, -2482.73169f, 197.80199f}, landingPoint,
										 Vec {-13365.99707f, -2477.12573f, 192.97578f}, false);
	assert(denseLanding.pieces == 3);
	assert(denseLanding.points[1].z > landingPoint.z);
	assert(denseLanding.points[2].z > landingPoint.z);
	assert(near(ShapeCorner(a, b, a, true).points[0], b));
	assert(near(ShapeCorner(b, b, c, true).points[0], b));
	assert(near(ShapeCorner(a, b, Vec {20, 0, 0}, true).points[0], b));
	std::cout << "lead geometry PASS\n";
}
