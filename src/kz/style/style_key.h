#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace KZ::style
{
	inline bool IsPlatformStyle(std::string_view name)
	{
		return name == "ABH" || name == "AUD" || name == "LGJ" || name == "NOPRE";
	}

	inline std::string StyleKey(std::vector<std::string_view> names)
	{
		std::sort(names.begin(), names.end());
		const auto end = std::unique(names.begin(), names.end());
		std::string key;
		for (auto it = names.begin(); it != end; ++it)
		{
			if (!key.empty())
			{
				key += '+';
			}
			key += *it;
		}
		return key;
	}

	// Canonical style keys contain ASCII tokens and '+'. Encode only the query delimiter.
	inline std::string EncodeStyleKey(std::string_view style)
	{
		std::string encoded;
		for (char c : style)
		{
			if (c == '+')
			{
				encoded += "%2B";
			}
			else
			{
				encoded += c;
			}
		}
		return encoded;
	}
}
