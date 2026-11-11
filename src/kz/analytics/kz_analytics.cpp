#include "kz_analytics.h"
#include "jump_analytics.h"
#include "source_identity.h"
#include "kz/kz.h"
#include "kz/jumpstats/kz_jumpstats.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
#include "kz/prac/kz_prac.h"
#include "kz/option/kz_option.h"
#include "kz/outbox/kz_outbox.h"
#include "kz/timer/submission.h"
#include "kz/replays/cyb_replay_common.h"
#include "utils/utils.h"
#include "utils/json.h"
#include <chrono>
#include <ctime>
#include <cstdlib>

CConVar<bool> kz_analytics_enabled("kz_analytics_enabled", FCVAR_NONE, "Enable opt-in jump pattern analytics events.", false);

namespace KZ::analytics
{
	namespace
	{
		struct ActorState
		{
			State state;
			u64 steamId {};
			u64 generation {};
			u64 contextGeneration {};
			std::string context;
		};

		ActorState actors[MAXPLAYERS + 1];

		ActorState *Slot(KZPlayer *player)
		{
			int slot = player->GetPlayerSlot().Get();
			return slot >= 0 && slot <= MAXPLAYERS ? &actors[slot] : nullptr;
		}

		nlohmann::json Metadata(KZPlayer *player)
		{
			char mapHash[33] {};
			g_pKZUtils->GetCurrentMapMD5(mapHash, sizeof(mapHash));
			auto mode = KZ::mode::GetModeInfo(player->modeService);
			nlohmann::json styles = nlohmann::json::array();
			FOR_EACH_VEC(player->styleServices, i)
			styles.push_back(player->styleServices[i]->GetStyleShortName());
			return {{"playerName", player->GetName()}, {"mapName", g_pKZUtils->GetCurrentMapName().Get()},           {"mapHash", mapHash},
					{"physicsHash", mode.md5},         {"mode", CybReplayCommon::MapMode(mode.shortModeName.Get())}, {"styles", styles}};
		}

		ActorState *Current(KZPlayer *player)
		{
			ActorState *actor = Slot(player);
			if (!actor)
			{
				return nullptr;
			}
			auto *pawn = player->GetPlayerPawn();
			if (!kz_analytics_enabled.Get() || !pawn || pawn->IsBot() || !player->IsAuthenticated() || pawn->GetTeam() <= CS_TEAM_SPECTATOR
				|| !player->GetSteamId64())
			{
				actor->state.Reset();
				actor->context.clear();
				++actor->generation;
				++actor->contextGeneration;
				return nullptr;
			}
			auto metadata = Metadata(player);
			// Ник может меняться внутри рана. Совместимость задают карта, физика, режим и стили.
			metadata.erase("playerName");
			std::string context = metadata.dump();
			FOR_EACH_VEC(player->styleServices, i)
			{
				auto style = KZ::style::GetStyleInfo(player->styleServices[i]);
				context += std::string("|") + style.md5;
			}
			u64 steamId = player->GetSteamId64();
			if (actor->steamId != steamId || actor->context != context)
			{
				double lastAlert = actor->steamId == steamId ? actor->state.lastAlert : -1;
				actor->state.Reset();
				actor->state.lastAlert = lastAlert;
				actor->steamId = steamId;
				actor->context = context;
				++actor->generation;
				++actor->contextGeneration;
			}
			if (metadata["mode"] == "" || metadata["mapHash"] == "" || metadata["physicsHash"] == "")
			{
				actor->state.Stop();
				KZ_LOG_INFO(LogChannel::Global, "[analytics] evidence skipped reason=missing_compatibility_metadata\n");
				return nullptr;
			}
			return actor;
		}

		std::string Timestamp()
		{
			std::time_t now = std::time(nullptr);
			std::tm utc {};
#if defined(_WIN32)
			gmtime_s(&utc, &now);
#else
			gmtime_r(&now, &utc);
#endif
			char formatted[32];
			std::strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ", &utc);
			return formatted;
		}

		std::string Envelope(KZPlayer *player, const char *type, const std::string &id, const std::string &serverId, const nlohmann::json &data,
							 int typeV = 1)
		{
			nlohmann::json event = {{"v", 1},
									{"id", id},
									{"type", type},
									{"typeV", typeV},
									{"at", Timestamp()},
									{"serverId", serverId},
									{"actor", {{"steamId64", std::to_string(player->GetSteamId64())}}},
									{"data", data}};
			return nlohmann::json({{"events", nlohmann::json::array({event})}}).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
		}

		void Emit(KZPlayer *player, const char *type, nlohmann::json data, int typeV = 1)
		{
			const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
			if (!url || !*url)
			{
				return;
			}
			const std::string serverId = SourceServerId(std::getenv("CS2_INSTANCE_ID"), KZOptionService::GetOptionStr("cybServerId", ""));
			if (serverId.empty())
			{
				KZ_LOG_INFO(LogChannel::Global, "[analytics] event dropped reason=missing_server_identity\n");
				return;
			}
			// У аналитического события свой ключ outbox, отдельный от обычного события финиша.
			std::string id = UUID_t().ToString();
			std::string body = Envelope(player, type, id, serverId, data, typeV);
			if (body.size() > 512u * 1024u && data.contains("runId"))
			{
				data["jumps"] = nlohmann::json::array();
				data["truncated"] = true;
				body = Envelope(player, type, id, serverId, data, typeV);
				KZ_LOG_INFO(LogChannel::Global, "[analytics] run evidence omitted reason=body_limit\n");
			}
			if (body.size() > 512u * 1024u)
			{
				KZ_LOG_INFO(LogChannel::Global, "[analytics] event dropped reason=body_limit\n");
				return;
			}
			KZOutboxService::EnqueueEvent(id, body);
			KZOutboxService::SendEvent(id, body);
		}

		template<class Jumps>
		nlohmann::json JumpsJson(const Jumps &jumps, bool includeInputFeatures = false)
		{
			nlohmann::json result = nlohmann::json::array();
			for (const auto &jump : jumps)
			{
				result.push_back(nlohmann::json::parse(JumpJson(jump, includeInputFeatures)));
			}
			return result;
		}

		bool Capture(Jump *source, JumpRecord &out, bool &runEligible)
		{
			if (source->analyticsDiscontinuous || !source->IsValid() || source->strafes.Count() < 1 || source->strafes.Count() > 64)
			{
				return false;
			}
			runEligible = source->strafes.Count() <= 16;
			std::vector<InputStrafe> inputStrafes;
			std::vector<InputAA> inputCalls;
			out.id = UUID_t().ToString();
			out.endTick = source->serverTick;
			out.distance = source->GetDistance();
			out.airtimeMs = source->airtime * 1000.0;
			out.sync = source->sync * 100.0;
			out.takeoff = {source->takeoffOrigin.x, source->takeoffOrigin.y, source->takeoffOrigin.z};
			out.landing = {source->landingOrigin.x, source->landingOrigin.y, source->landingOrigin.z};
			out.valid = true;
			out.inPrac = source->analyticsInPrac || source->player->pracService->IsInPrac();
			double totalDuration = 0;
			const AACall *first = nullptr, *last = nullptr;
			FOR_EACH_VEC(source->strafes, i)
			{
				auto &strafe = source->strafes[i];
				if (strafe.collisionGain != 0 || strafe.collisionLoss != 0 || strafe.externalGain != 0 || strafe.externalLoss != 0)
				{
					// Keep legacy MMR exclusions separate from input-only detection.
					runEligible = false;
				}
				inputStrafes.push_back({strafe.width, strafe.duration, strafe.arStats.available, strafe.arStats.median, strafe.arStats.average});
				out.widthsDeg.push_back(strafe.width);
				out.durationsMs.push_back(strafe.duration * 1000.0);
				FOR_EACH_VEC(strafe.aaCalls, k)
				{
					const auto &call = strafe.aaCalls[k];
					if (!std::isfinite(call.duration) || call.duration <= 0 || !std::isfinite(call.prevYaw) || !std::isfinite(call.currentYaw))
					{
						return false;
					}
					if (!first)
					{
						first = &call;
					}
					inputCalls.push_back({i, call.prevYaw, call.currentYaw, call.duration, call.buttons[0]});
					last = &call;
					totalDuration += call.duration;
					out.hasTurnBind |= (call.buttons[0] & (IN_TURNLEFT | IN_TURNRIGHT)) != 0;
				}
			}
			if (!first || !last || !std::isfinite(totalDuration) || totalDuration <= 0)
			{
				return false;
			}
			out.takeoffYaw = WrapYaw(first->prevYaw);
			out.landingYaw = WrapYaw(last->currentYaw);
			// Равномерные отсчёты времени с интерполяцией yaw каждого subtick через границу180°.
			size_t sample = 0;
			double elapsed = 0;
			FOR_EACH_VEC(source->strafes, i)
			{
				FOR_EACH_VEC(source->strafes[i].aaCalls, k)
				{
					const auto &call = source->strafes[i].aaCalls[k];
					double end = elapsed + call.duration;
					while (sample < 32 && totalDuration * sample / 31.0 <= end + 1e-9)
					{
						double fraction = std::clamp((totalDuration * sample / 31.0 - elapsed) / call.duration, 0.0, 1.0);
						out.yawSamples[sample++] = WrapYaw(call.prevYaw + WrapYaw(call.currentYaw - call.prevYaw) * fraction - out.takeoffYaw);
					}
					elapsed = end;
				}
			}
			out.inputFeatures = InputFeatures(inputStrafes, inputCalls, out.valid, out.inPrac);
			return sample == 32 && Complete(out);
		}
	} // namespace

	CON_COMMAND_F(kz_analytics_status, "Print the current jump analytics collector and model status.", FCVAR_NONE)
	{
		// Diagnostics are limited to server console/RCON and never sent to player chat.
		if (utils::GetController(context.GetPlayerSlot()))
		{
			KZ_LOG_WARN(LogChannel::Global, "[analytics] cmd_denied cmd=kz_analytics_status reason=not_server slot=%d\n",
						context.GetPlayerSlot().Get());
			return;
		}
		Msg("[analytics] status modelId=%s enabled=%d\n", AHK_INPUT_MODEL_ID, kz_analytics_enabled.Get() ? 1 : 0);
		for (int slot = 0; slot <= MAXPLAYERS; ++slot)
		{
			const auto &actor = actors[slot];
			if (!actor.steamId || actor.context.empty())
			{
				continue;
			}
			size_t withFeatures = 0, positive = 0;
			nlohmann::json lastScore = nullptr;
			nlohmann::json recent = nlohmann::json::array();
			for (const auto &jump : actor.state.recent)
			{
				nlohmann::json score = nullptr;
				if (jump.inputFeatures)
				{
					++withFeatures;
					double value = InputScore(*jump.inputFeatures);
					score = value;
					positive += jump.valid && !jump.inPrac && !jump.hasTurnBind && value >= AHK_INPUT_THRESHOLD;
				}
				lastScore = score;
				recent.push_back({{"endTick", jump.endTick}, {"withFeatures", jump.inputFeatures.has_value()}, {"score", score}});
			}
			nlohmann::json status = {{"slot", slot},
									 {"steamId64", std::to_string(actor.steamId)},
									 {"runActive", actor.state.active},
									 {"recentValid", actor.state.recent.size()},
									 {"withFeatures", withFeatures},
									 {"modelPositive", positive},
									 {"lastScore", lastScore},
									 {"lastAlert", actor.state.lastAlert},
									 {"recent", recent}};
			Msg("[analytics] actor=%s\n", status.dump().c_str());
		}
	}

	void Reset(KZPlayer *player)
	{
		if (auto *actor = Slot(player))
		{
			actor->state.Reset();
			actor->context.clear();
			actor->steamId = 0;
			++actor->generation;
			++actor->contextGeneration;
		}
	}

	void Start(KZPlayer *player)
	{
		if (auto *actor = Current(player))
		{
			actor->state.Start();
			++actor->generation;
		}
	}

	void Stop(KZPlayer *player)
	{
		if (auto *actor = Slot(player))
		{
			actor->state.Stop();
			++actor->generation;
		}
	}

	void EnterPrac(KZPlayer *player)
	{
		if (auto *actor = Slot(player))
		{
			actor->state.EnterPrac();
		}
		auto &jumps = player->jumpstatsService->jumps;
		if (jumps.Count() && !jumps.Tail().AlreadyEnded())
		{
			jumps.Tail().analyticsInPrac = true;
		}
	}

	void Teleport(KZPlayer *player)
	{
		if (auto *actor = Slot(player))
		{
			actor->state.Teleport();
		}
		auto &jumps = player->jumpstatsService->jumps;
		if (jumps.Count() && !jumps.Tail().AlreadyEnded())
		{
			jumps.Tail().analyticsDiscontinuous = true;
		}
	}

	void OnJumpStart(Jump *jump)
	{
		jump->analyticsGeneration = 0;
		jump->analyticsContextGeneration = 0;
		jump->analyticsDiscontinuous = false;
		jump->analyticsInPrac = jump->player->pracService->IsInPrac();
		if (auto *actor = Current(jump->player))
		{
			jump->analyticsContextGeneration = actor->contextGeneration;
			if (actor->state.active)
			{
				jump->analyticsGeneration = actor->generation;
			}
		}
	}

	void OnJumpFinish(Jump *jump)
	{
		auto *actor = Current(jump->player);
		if (!actor)
		{
			return;
		}
		if (jump->analyticsContextGeneration != actor->contextGeneration)
		{
			actor->state.OmitJump();
			return;
		}
		JumpRecord record;
		bool runEligible = false;
		if (!Capture(jump, record, runEligible))
		{
			actor->state.OmitJump();
			return;
		}
		// Прыжок через границу старта или сброса таймера исключается из обоих ранов.
		bool belongsToRun = jump->analyticsGeneration != 0 && jump->analyticsGeneration == actor->generation;
		if (!runEligible)
		{
			actor->state.OmitJump();
		}
		actor->state.Add(std::move(record), belongsToRun && runEligible);
		double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		if (actor->state.ShouldAlert(now))
		{
			auto data = Metadata(jump->player);
			data["detectorVersion"] = "ahk-input-v2";
			data["modelId"] = AHK_INPUT_MODEL_ID;
			data["jumps"] = JumpsJson(actor->state.recent, true);
			Emit(jump->player, "kz.ahk_suspected", std::move(data), 2);
		}
	}

	void OnRunFinished(KZPlayer *player, const RunSubmission &submission)
	{
		auto *actor = Current(player);
		if (!actor || !actor->state.Finish())
		{
			return;
		}
		auto data = Metadata(player);
		data["runId"] = submission.localUUID.ToString();
		data["course"] = submission.course.number;
		data["timeMs"] = (long long)std::llround(submission.time * 1000.0);
		data["finished"] = true;
		data["usedPrac"] = actor->state.usedPrac;
		data["truncated"] = actor->state.truncated;
		data["jumps"] = JumpsJson(actor->state.run);
		if (actor->state.truncated)
		{
			KZ_LOG_INFO(LogChannel::Global, "[analytics] run excluded reason=incomplete_jump_evidence\n");
		}
		Emit(player, "kz.movement_run_finished", std::move(data));
		actor->state.run.clear();
	}
} // namespace KZ::analytics
