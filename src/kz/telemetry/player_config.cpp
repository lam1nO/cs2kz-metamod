#include "player_config.h"
#include "kz/kz.h"
#include "kz/option/kz_option.h"
#include "utils/ctimer.h"
#include "utils/cvarquery.h"
#include "utils/http.h"
#include "utils/json.h"
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace KZ::player_config
{
	namespace
	{
		// Archived game preferences that are absent from userinfo. Values are accepted only
		// when the engine reports ValueIntact. The crosshair height is its reference scale.
		constexpr const char *queriedCvars[] = {
			"zoom_sensitivity_ratio", "viewmodel_presetpos", "fps_max", "fps_max_ui",
			"cl_crosshairstyle", "cl_crosshair_length", "cl_crosshair_thickness", "cl_crosshair_gap",
			"cl_crosshaircolor_r", "cl_crosshaircolor_g", "cl_crosshaircolor_b", "cl_crosshaircolor_a",
			"cl_crosshair_screen_height", "cl_crosshair_sniper_width", "cl_crosshair_friendly_warning",
			"cl_crosshair_dynamic_splitdist", "cl_crosshair_dynamic_maxdist_splitratio", "cl_crosshair_dynamic_spread_limit",
			"cl_crosshairoutline_r", "cl_crosshairoutline_g", "cl_crosshairoutline_b", "cl_crosshairoutline_a",
			"cl_ironsight_dot_scale", "cl_ironsight_usecrosshaircolor", "cl_teamid_overhead_fade_near_crosshair",
			"cl_crosshair_dynamic_splitalpha_innermod", "cl_crosshair_dynamic_splitalpha_outermod",
			"cl_crosshair_drawoutline", "cl_crosshairdot", "cl_crosshair_t", "cl_crosshair_recoil",
			"hud_scaling", "cl_hud_color", "cl_radar_scale", "cl_hud_radar_scale", "cl_radar_icon_scale_min",
			"cl_radar_always_centered", "cl_radar_rotate", "cl_radar_square_with_scoreboard", "cl_radar_scale_dynamic",
			"cl_showfps", "cl_teammate_colors_show", "safezonex", "safezoney", "r_fullscreen_gamma",
			"cl_allow_animated_avatars", "cl_show_clan_in_death_notice", "cl_showloadout",
			"cl_teamcounter_playercount_instead_of_avatars", "hud_showtargetid", "cl_draw_only_deathnotices",
		};

		struct Observation
		{
			CPlayerUserId userId;
			CPlayerSlot slot;
			u64 steamId;
			i64 observedAtMs;
			int remainingDeliveryRetries = 2;
			nlohmann::json settings = nlohmann::json::object();
			std::vector<i32> cookies;
		};

		// CTimer lifetime only owns a numeric key. Keep the observation out of its arguments.
		std::unordered_map<u64, std::shared_ptr<Observation>> pending;
		u64 nextObservationId = 0;

		f64 Finish(u64 observationId)
		{
			auto entry = pending.find(observationId);
			if (entry == pending.end())
			{
				return 0;
			}
			auto observation = std::move(entry->second);
			pending.erase(entry);
			for (auto cookie : observation->cookies)
			{
				cvarquery::Cancel(observation->slot, cookie);
			}
			observation->cookies.clear();
			auto *player = g_pKZPlayerManager->ToPlayer(observation->userId);
			if (!player || !player->IsInGame() || player->GetSteamId64() != observation->steamId || observation->settings.empty())
			{
				return 0;
			}
			std::string url = KZOptionService::GetOptionStr("cybEmitUrl", "");
			const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");
			if (url.empty() || !token || !*token)
			{
				return 0;
			}
			if (url.back() == '/')
			{
				url.pop_back();
			}
			url += "/ingest/v1/players/" + std::to_string(observation->steamId) + "/config-observation?source=kz_query&observedAt="
				+ std::to_string(observation->observedAtMs);
			HTTP::Request request(HTTP::Method::POST, url);
			request.SetSensitive();
			request.SetHeader("Authorization", std::string("Bearer ") + token);
			request.SetHeader("Content-Type", "application/json");
			const char *serverId = std::getenv("CS2_INSTANCE_ID");
			if (serverId && *serverId)
			{
				request.SetHeader("X-Server-Id", serverId);
			}
			request.SetBody(observation->settings.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
			auto retry = [observationId, observation]()
			{
				if (observation->remainingDeliveryRetries <= 0)
				{
					return;
				}
				--observation->remainingDeliveryRetries;
				pending.emplace(observationId, observation);
				StartTimer<u64>(Finish, observationId, 6.0f, true, true);
			};
			request.Send(
				[retry](HTTP::Response response)
				{
					if (response.status == 0 || response.status == 429 || response.status >= 500)
					{
						retry();
					}
				},
				retry);

			return 0;
		}

		f64 Query(CPlayerUserId userId, int remainingAuthRetries)
		{
			auto *player = g_pKZPlayerManager->ToPlayer(userId);
			if (!player || !player->IsInGame() || !*KZOptionService::GetOptionStr("cybEmitUrl", ""))
			{
				return 0;
			}
			const u64 steamId = player->GetSteamId64();
			if (!steamId)
			{
				if (remainingAuthRetries > 0)
				{
					StartTimer<CPlayerUserId, int>(Query, userId, remainingAuthRetries - 1, 2.0f, false, true);
				}
				return 0;
			}
			const auto slot = player->GetPlayerSlot();
			const auto observedAtMs =
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
			auto observation = std::make_shared<Observation>(Observation{userId, slot, steamId, observedAtMs});
			observation->cookies.reserve(std::size(queriedCvars));
			for (const char *cvar : queriedCvars)
			{
				i32 cookie;
				if (cvarquery::Query(
						slot, cvar,
						[observation, cvar](CPlayerSlot, cvarquery::Status status, const char *name, const char *value)
						{
							if (status == cvarquery::Status::ValueIntact && std::strcmp(cvar, name) == 0 && *value && std::strlen(value) <= 80)
							{
								observation->settings[cvar] = value;
							}
						},
						&cookie))
				{
					observation->cookies.push_back(cookie);
				}
			}
			// The timer also releases cookies from clients that never answer. Keep it through
			// a map change so the outstanding queries are always cancelled.
			if (!observation->cookies.empty())
			{
				const u64 observationId = ++nextObservationId;
				pending.emplace(observationId, std::move(observation));
				StartTimer<u64>(Finish, observationId, 4.0f, true, true);
			}
			return 0;
		}
	}

	void Observe(KZPlayer *player)
	{
		if (!player->IsFakeClient() && !player->IsCSTV())
		{
			StartTimer<CPlayerUserId, int>(Query, player->GetClient()->GetUserID(), 8, 2.0f, false, true);
		}
	}
}
