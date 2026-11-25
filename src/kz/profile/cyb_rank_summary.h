#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace CybRankSummary
{
inline constexpr int RankCount = 23;

struct Rating
{
	int points;
	int index;
	std::string name;
	std::optional<int> nextPoints;
};

std::optional<Rating> Parse(std::string_view body);
} // namespace CybRankSummary
