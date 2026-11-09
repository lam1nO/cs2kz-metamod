#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace leadgeometry
{
	template<class V>
	struct Corner
	{
		std::array<V, 4> points;
		unsigned pieces = 0;
	};

	template<class V>
	Corner<V> ShapeCorner(const V &previous, const V &point, const V &next, bool sameSurface)
	{
		Corner<V> result {{point, point, point, point}};
		const V incoming = point - previous, outgoing = next - point;
		const float before = incoming.Length(), after = outgoing.Length();
		if (!std::isfinite(before) || !std::isfinite(after) || before < .001f || after < .001f)
		{
			return result;
		}
		const V u = incoming * (1.0f / before), v = outgoing * (1.0f / after);
		const float dot = u.x * v.x + u.y * v.y + u.z * v.z;
		if (dot <= -.95f || dot >= .9995f)
		{
			return result;
		}
		// Equal trimming prevents short edges from being smoothed more on one side.
		// Landings need a longer visible blend, still bounded by both recorded edges.
		const float trim = (std::min)(sameSurface ? 6.0f : 12.0f, .42f * (std::min)(before, after));
		const V displacement = (v - u) * (.25f * trim);
		if (displacement.Length() < .6f)
		{
			// Dense vertices only move. Never split a tiny bend into more entities.
			result.points[0] = point + displacement;
			return result;
		}

		result.pieces = 3;
		result.points[0] = point - u * trim;
		result.points[3] = point + v * trim;
		for (unsigned i = 1; i < result.pieces; ++i)
		{
			const float t = float(i) / float(result.pieces);
			const float s = 1.0f - t;
			result.points[i] = point + (v * (t * t) - u * (s * s)) * trim;
		}
		return result;
	}
} // namespace leadgeometry
