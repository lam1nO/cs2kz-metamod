#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace leadgeometry
{
	inline constexpr unsigned MaxCornerPieces = 8;
	inline constexpr unsigned StripPieces = 6;
	inline constexpr float TargetJoinAngle = .20943951f; // 12 degrees
	inline constexpr float StripMinCut = .125f; // Half the default path simplification tolerance.

	template<class V>
	struct Corner
	{
		std::array<V, MaxCornerPieces + 1> points;
		unsigned pieces = 0;
	};

	template<class V>
	struct CornerSpec
	{
		V incoming, outgoing;
		float cosine, trim;
		unsigned pieces = 0;
	};

	template<class V>
	CornerSpec<V> AnalyzeCorner(const V &previous, const V &point, const V &next, bool sameSurface,
							  unsigned pieceLimit, bool continuous)
	{
		const V incoming = point - previous, outgoing = next - point;
		const float before = incoming.Length(), after = outgoing.Length();
		if (!std::isfinite(before) || !std::isfinite(after) || before < .001f || after < .001f)
		{
			return {};
		}
		const V u = incoming * (1.0f / before), v = outgoing * (1.0f / after);
		const float dot = u.x * v.x + u.y * v.y + u.z * v.z;
		if (dot <= -.95f || dot >= .9995f)
		{
			return {};
		}
		// Adjacent fillets consume at most 48% each of their shared edge.
		const float trim = (std::min)(sameSurface ? 12.0f : 16.0f, .48f * (std::min)(before, after));
		if (sameSurface)
		{
			const float minCut = continuous ? StripMinCut : .16f;
			// Compare (trim * tan(turn/4))² via cos(turn/2), without solving for the angle.
			const float halfCosine = std::sqrt(.5f * (1.0f + dot));
			if (trim * trim * (1.0f - halfCosine) < minCut * minCut * (1.0f + halfCosine))
			{
				return {};
			}
		}
		const unsigned pieces = continuous ? StripPieces
										   : std::clamp(static_cast<unsigned>(std::ceil(std::acos(dot) / TargetJoinAngle)), 2u, pieceLimit);
		return {u, v, dot, trim, pieces};
	}

	template<class V>
	Corner<V> ShapeCorner(const V &point, const CornerSpec<V> &spec)
	{
		Corner<V> result {{point}};
		if (!spec.pieces)
		{
			return result;
		}
		const auto &[u, v, dot, trim, pieces] = spec;
		// The circle is tangent to both edges. In the basis (u, normal),
		// its center is one radius inward from the first tangent point.
		const float turn = std::acos(dot);
		const float sinTurn = std::sqrt(1.0f - dot * dot);
		const V normal = (v - u * dot) * (1.0f / sinTurn);
		const float radius = trim * (1.0f + dot) / sinTurn;
		result.pieces = pieces;
		result.points[0] = point - u * trim;
		result.points[result.pieces] = point + v * trim;
		for (unsigned i = 1; i < result.pieces; ++i)
		{
			const float angle = turn * float(i) / float(result.pieces);
			const float halfSine = std::sin(.5f * angle);
			result.points[i] = point + u * (radius * std::sin(angle) - trim) + normal * (radius * 2.0f * halfSine * halfSine);
		}
		return result;
	}

	template<class V>
	Corner<V> ShapeCorner(const V &previous, const V &point, const V &next, bool sameSurface,
						  unsigned pieceLimit = MaxCornerPieces, bool continuous = false)
	{
		return ShapeCorner(point, AnalyzeCorner(previous, point, next, sameSurface, pieceLimit, continuous));
	}
} // namespace leadgeometry
