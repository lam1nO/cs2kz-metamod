#pragma once
#include <cstddef>
#include <string>

namespace CybTickets
{
inline bool IsCampaignId(const std::string &value)
{
	if (value.size() != 36)
		return false;
	for (std::size_t i = 0; i < value.size(); ++i)
	{
		if (i == 8 || i == 13 || i == 18 || i == 23)
		{
			if (value[i] != '-')
				return false;
		}
		else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
			return false;
	}
	// Match the RFC UUID accepted by the API; a bad optional proof must never reject a normal run.
	return value[14] >= '1' && value[14] <= '8' && (value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b');
}

inline bool Eligible(bool enabled, bool authenticatedAndNotBanned, bool validTimer, bool practice, int course, std::size_t styles)
{
	return enabled && authenticatedAndNotBanned && validTimer && !practice && course == 0 && styles == 0;
}
} // namespace CybTickets
