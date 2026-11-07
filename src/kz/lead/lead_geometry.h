#pragma once
#include <algorithm>
#include <cmath>

// Только геометрия отрисовки: исходные replay-вершины и время не меняются.
namespace leadgeometry
{
	template<class V>
	struct Corner
	{
		V entry, control, exit;
		bool rounded;
	};

	template<class V>
	Corner<V> RoundCorner(const V &previous, const V &point, const V &next, bool sameSurface)
	{
		Corner<V> result {point, point, point, false};
		const V incoming = point - previous, outgoing = next - point;
		const float before = incoming.Length(), after = outgoing.Length();
		if (!sameSurface || !std::isfinite(before) || !std::isfinite(after) || before < 0.001f || after < 0.001f)
		{
			return result;
		}
		const V u = incoming * (1.0f / before), v = outgoing * (1.0f / after);
		const float dot = u.x * v.x + u.y * v.y + u.z * v.z;
		// У разворота нет устойчивой касательной; прямой участок не требует второй сущности.
		if (dot <= -0.95f || dot >= 0.9995f)
		{
			return result;
		}
		const float trim = (std::min)(4.0f, .45f * (std::min)(before, after));
		result.entry = point - u * trim;
		result.exit = point + v * trim;
		result.rounded = true;
		return result;
	}

	inline bool RoundWithinBudget(unsigned cap)
	{
		return cap >= 2;
	}

	inline unsigned EdgeBudget(unsigned cap, bool ribbon)
	{
		return ribbon && cap >= 2 ? cap / 2 : cap;
	}
} // namespace leadgeometry
