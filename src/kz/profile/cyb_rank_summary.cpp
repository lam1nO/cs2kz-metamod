#include "cyb_rank_summary.h"

#include <cstdint>
#include <limits>

#include <vendor/json/single_include/nlohmann/json.hpp>

namespace CybRankSummary
{
std::optional<Rating> Parse(std::string_view body)
{
	auto json = nlohmann::json::parse(body, nullptr, false);
	if (!json.is_object())
	{
		return std::nullopt;
	}

	auto readInt = [&json](const char *key, int max) -> std::optional<int>
	{
		const auto field = json.find(key);
		if (field == json.end() || !field->is_number_integer())
		{
			return std::nullopt;
		}
		const auto value = field->get<std::uint64_t>();
		if (value > static_cast<std::uint64_t>(max))
		{
			return std::nullopt;
		}
		return static_cast<int>(value);
	};

	const auto points = readInt("totalPoints", std::numeric_limits<int>::max());
	const auto index = readInt("rankIndex", RankCount - 1);
	const auto nameField = json.find("rankName");
	if (!points || !index || nameField == json.end() || !nameField->is_string())
	{
		return std::nullopt;
	}

	const auto &name = nameField->get_ref<const std::string &>();
	// Звание попадает в скорборд и чат. Допускаем буквы, пробел и знаки ступеней.
	if (name.empty() || name.size() > 24
		|| name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz +-") != std::string::npos)
	{
		return std::nullopt;
	}

	std::optional<int> nextPoints;
	const auto nextField = json.find("rankNextPoints");
	if (nextField != json.end() && !nextField->is_null())
	{
		nextPoints = readInt("rankNextPoints", std::numeric_limits<int>::max());
		if (!nextPoints || *nextPoints <= *points || *index == RankCount - 1)
		{
			return std::nullopt;
		}
	}
	return Rating {*points, *index, name, nextPoints};
}
} // namespace CybRankSummary
