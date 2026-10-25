#include "cs2kz.h"
#include "kz/kz.h"
#include "bot.h"
#include "session.h"
#include "kz_replay.h"
#include "data.h"
#include "item.h"
#include "kz/spec/kz_spec.h"
#include "kz/db/kz_db.h"
#include "vendor/sql_mm/src/public/sql_mm.h"
#include "utils/ctimer.h"

static_global CHandle<CCSPlayerController> g_replayBots[KZ::replaysystem::session::Count];
// Персистентный буфер кода прицела: schema-сеттер хранит сырой указатель,
// память хедера/cvar ему отдавать нельзя (умирает при смене реплея/значения).
static_global char g_botCrosshairCodes[KZ::replaysystem::session::Count][256];
extern CConVar<bool> kz_replay_playback_skins_enable;
extern CConVar<CUtlString> kz_replay_bot_default_crosshair;

static_function f64 SetBotModel(int index, CHandle<CCSPlayerController> handle, UUID_t uuid)
{
	KZ::replaysystem::session::Scope scope(index);
	if (!handle.Get() || g_replayBots[index] != handle.Get() || !(KZ::replaysystem::data::GetCurrentReplay()->uuid == uuid))
	{
		return -1.0;
	}
	auto bot = g_replayBots[KZ::replaysystem::session::Current()].Get();
	if (!bot)
	{
		return -1.0f;
	}
	ReplayHeader &header = KZ::replaysystem::data::GetCurrentReplay()->header;
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(bot);
	EconInfo glovesInfo = {};
	if (header.has_gloves())
	{
		if (header.gloves().has_main_info())
		{
			const auto &mainInfo = header.gloves().main_info();
			glovesInfo.mainInfo.itemDef = mainInfo.item_def();
			glovesInfo.mainInfo.quality = mainInfo.quality();
			glovesInfo.mainInfo.level = mainInfo.level();
			glovesInfo.mainInfo.accountID = mainInfo.account_id();
			glovesInfo.mainInfo.itemID = mainInfo.item_id();
			glovesInfo.mainInfo.inventoryPosition = mainInfo.inventory_position();
			V_strncpy(glovesInfo.mainInfo.customName, mainInfo.custom_name().c_str(), sizeof(glovesInfo.mainInfo.customName));
			V_strncpy(glovesInfo.mainInfo.customNameOverride, mainInfo.custom_name_override().c_str(),
					  sizeof(glovesInfo.mainInfo.customNameOverride));
		}

		glovesInfo.mainInfo.numAttributes = MIN(header.gloves().attributes_size(), 32);
		for (int i = 0; i < glovesInfo.mainInfo.numAttributes; i++)
		{
			const auto &attr = header.gloves().attributes(i);
			glovesInfo.attributes[i].defIndex = attr.def_index();
			glovesInfo.attributes[i].value = attr.value();
		}
	}
	KZ::replaysystem::item::ApplyModelAttributesToPawn(player->GetPlayerPawn(), glovesInfo, header.model_name().c_str());
	return -1.0f;
}

namespace KZ::replaysystem::bot
{
	void KickBot()
	{
		if (g_KZPlugin.unloading)
		{
			return;
		}
		auto bot = g_replayBots[KZ::replaysystem::session::Current()].Get();
		if (!bot)
		{
			return;
		}
		g_replayBots[KZ::replaysystem::session::Current()].Term();
		interfaces::pEngine->KickClient(bot->GetPlayerSlot(), "bye bot", NETWORK_DISCONNECT_KICKED);
	}

	void SpawnBot()
	{
		if (g_replayBots[KZ::replaysystem::session::Current()].Get())
		{
			return;
		}
		BotProfile *profile = BotProfile::PersistentProfile(CS_TEAM_T);
		if (!profile)
		{
			return;
		}
		CCSPlayerController *bot = g_pKZUtils->CreateBot(profile, CS_TEAM_T);
		if (!bot)
		{
			return;
		}

		g_replayBots[KZ::replaysystem::session::Current()] = bot;
	}

	void MakeBotAlive()
	{
		SpawnBot();
		auto bot = g_replayBots[KZ::replaysystem::session::Current()].Get();
		if (!bot)
		{
			return;
		}
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(bot);
		KZ::misc::JoinTeam(player, CS_TEAM_CT, false);
		// CS2 will kick bots that don't have spectator pending team when another player joins.
		player->GetController()->m_iPendingTeamNum(1);
		ReplayHeader &header = KZ::replaysystem::data::GetCurrentReplay()->header;

		bot->GetPlayerPawn()->m_flViewmodelOffsetX() = header.viewmodel_offset_x();
		bot->GetPlayerPawn()->m_flViewmodelOffsetY() = header.viewmodel_offset_y();
		bot->GetPlayerPawn()->m_flViewmodelOffsetZ() = header.viewmodel_offset_z();
		bot->GetPlayerPawn()->m_flViewmodelFOV() = header.viewmodel_fov();

		// Прицел из реплея; для старых реплеев — дефолт из cvar. Ставим безусловно:
		// пустая строка = дефолтный прицел клиента, и код прошлого реплея не протекает.
		const char *crosshairCode = header.crosshair_code().c_str();
		if (!crosshairCode[0])
		{
			crosshairCode = kz_replay_bot_default_crosshair.Get().Get();
		}
		V_strncpy(g_botCrosshairCodes[KZ::replaysystem::session::Current()], crosshairCode,
				  sizeof(g_botCrosshairCodes[KZ::replaysystem::session::Current()]));
		bot->SetCrosshairCodes(g_botCrosshairCodes[KZ::replaysystem::session::Current()]);
		// Клиент CS2 прицел ботов не рендерит (cl_observed_bot_crosshair, ветка «это бот»
		// раньше кодов) — код ставим на будущее и для внешних потребителей (сайт/демо);
		// зрителю даём подсказку в чате при загрузке реплея.
	}

	void MoveBotToSpec()
	{
		auto bot = g_replayBots[KZ::replaysystem::session::Current()].Get();
		if (!bot)
		{
			return;
		}
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(bot);
		KZ::misc::JoinTeam(player, CS_TEAM_SPECTATOR, false);
	}

	CCSPlayerController *GetBot()
	{
		return g_replayBots[KZ::replaysystem::session::Current()].Get();
	}

	int SessionForBot(CCSPlayerController *controller)
	{
		if (!controller)
		{
			return 0;
		}
		for (int i = 1; i < session::Count; ++i)
		{
			if (g_replayBots[i] == controller)
			{
				return i;
			}
		}
		return 0;
	}

	bool IsValidBot(CCSPlayerController *controller)
	{
		return SessionForBot(controller) != 0;
	}

	KZPlayer *GetBotPlayer()
	{
		auto bot = g_replayBots[KZ::replaysystem::session::Current()].Get();
		if (!bot)
		{
			return nullptr;
		}
		return g_pKZPlayerManager->ToPlayer(bot);
	}

	// [cyb] В шапке файла — ник на момент ЗАПИСИ рана: игрок, сменивший его после, крутился бы
	// под старым именем. Актуальный ник — `Players.Alias` общей БД (его пишет заход на сервер и
	// синк ников с платформы, db/cyb_alias_sync.cpp). Запрос асинхронный: бот сначала встаёт с
	// ником из шапки, потом переименовывается; ник подменяется и в шапке текущего реплея, чтобы
	// карточка меню показывала тот же. Ответ, опоздавший к другому реплею, отбрасывается по uuid.
	static void RefreshBotNameFromDatabase(u64 steamId64)
	{
		data::ReplayPlayback *replay = data::GetCurrentReplay();
		if (steamId64 == 0 || !replay || !KZDatabaseService::IsReady())
		{
			return;
		}
		const UUID_t uuid = replay->uuid;
		auto onSuccess = [uuid, steamId64, index = session::Current()](std::vector<ISQLQuery *> queries)
		{
			session::Scope scope(index);
			ISQLResult *result = (!queries.empty() && queries[0]) ? queries[0]->GetResultSet() : nullptr;
			if (!result || result->GetRowCount() == 0 || !result->FetchRow())
			{
				return;
			}
			const char *alias = result->GetString(0);
			data::ReplayPlayback *current = data::GetCurrentReplay();
			if (!alias || !alias[0] || !current || !(current->uuid == uuid) || !current->header.has_player()
				|| current->header.player().steamid64() != steamId64 || current->header.player().name() == alias)
			{
				return;
			}
			current->header.mutable_player()->set_name(alias);
			KZPlayer *botPlayer = GetBotPlayer();
			if (botPlayer)
			{
				botPlayer->SetName(alias);
			}
		};
		// Отказ БД — остаётся ник из шапки; саму ошибку печатает слой БД.
		KZDatabaseService::FindAliasBySteamID64(steamId64, onSuccess, [](std::string, int) {});
	}

	void InitializeBotForReplay(const ReplayHeader &header)
	{
		MakeBotAlive();
		auto bot = g_replayBots[KZ::replaysystem::session::Current()].Get();
		if (!bot)
		{
			return;
		}
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(bot);
		if (header.has_player())
		{
			player->SetName(header.player().name().c_str());
			RefreshBotNameFromDatabase(header.player().steamid64());
		}
		if (kz_replay_playback_skins_enable.Get())
		{
			const int index = session::Current();
			const auto handle = g_replayBots[index];
			const UUID_t uuid = data::GetCurrentReplay()->uuid;
			StartTimer<int, CHandle<CCSPlayerController>, UUID_t>(SetBotModel, index, handle, uuid, 0.05, false);
		}
	}

	// true — спектейт бота реально включился (SpectatePlayer гейтится CanSpectate).
	bool SpectateBot(KZPlayer *spectator)
	{
		KZPlayer *botPlayer = GetBotPlayer();
		if (!botPlayer || !spectator)
		{
			return false;
		}
		return spectator->specService->SpectatePlayer(botPlayer);
	}

} // namespace KZ::replaysystem::bot
