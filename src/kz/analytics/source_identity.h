#pragma once

#include <string>
#include <string_view>
#include <vendor/json/single_include/nlohmann/json.hpp>

namespace KZ::analytics
{
	inline bool ValidSourceServerId(std::string_view id)
	{
		if (id.empty() || id.size() > 32)
		{
			return false;
		}
		for (char c : id)
		{
			if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
			{
				return false;
			}
		}
		return true;
	}

	inline std::string SourceServerId(const char *instanceId, const char *legacyId)
	{
		// При отсутствии корректного ID ноды действует настройка профиля.
		if (instanceId && ValidSourceServerId(instanceId))
		{
			return instanceId;
		}
		return legacyId ? legacyId : "";
	}

	inline std::string MovementSourceHeader(const std::string &body)
	{
		// Заголовок проверенных KZ-типов берётся из сохранённого envelope.
		// Это сохраняет атрибуцию при ретраях после изменения окружения процесса.
		if (body.find("kz.ahk_suspected") == std::string::npos && body.find("kz.movement_run_finished") == std::string::npos
			&& body.find("kz.run_finished") == std::string::npos)
		{
			return "";
		}
		auto payload = nlohmann::json::parse(body, nullptr, false);
		if (!payload.is_object() || !payload.contains("events") || !payload["events"].is_array() || payload["events"].size() != 1
			|| !payload["events"][0].is_object())
		{
			return "";
		}
		const auto &event = payload["events"][0];
		if (!event.contains("type") || !event["type"].is_string() || !event.contains("serverId") || !event["serverId"].is_string())
		{
			return "";
		}
		const auto &type = event["type"].get_ref<const std::string &>();
		if (type != "kz.ahk_suspected" && type != "kz.movement_run_finished" && type != "kz.run_finished")
		{
			return "";
		}
		const auto &id = event["serverId"].get_ref<const std::string &>();
		return ValidSourceServerId(id) ? id : "";
	}
} // namespace KZ::analytics
