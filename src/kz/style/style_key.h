#pragma once

#include "nopre_rules.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace KZ::style
{
	inline constexpr char AutoBhopName[] = "ABH";
	inline constexpr char LegacyJumpName[] = "LGJ";

	// Callers pass the canonical short names from the mode and style registries.
	inline bool CanUseStyleInMode(std::string_view style, std::string_view mode)
	{
		if (style == LegacyJumpName)
		{
			return mode == "VNL";
		}
		return style != NopreName || mode == "CKZ";
	}

	inline bool IsPlatformStyle(std::string_view name)
	{
		return name == AutoBhopName || name == "AUD" || name == LegacyJumpName || name == NopreName;
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
