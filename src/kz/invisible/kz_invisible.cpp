#include "kz_invisible.h"
#include "kz/language/kz_language.h"
#include "kz/spec/kz_spec.h"

#include "sdk/services.h"
#include "utils/utils.h"
#include "utils/json.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "tier0/memdbgon.h"

#define KZ_INVISIBLE_LIST_FILE "cfg/cyb_invisible.json"

// Список невидимок (steamid64). Мутации только на игровом потоке (плагин-лоад,
// map start, ConCommand'ы), чтение — CheckTransmit/PostEvent там же.
static_global std::unordered_set<u64> s_invisibleSteamIds;

// Пересчитать кэш-флаги всем игрокам после смены списка.
static_function void RefreshAllPlayers()
{
	for (int i = 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (player && player->invisibleService)
		{
			player->invisibleService->RefreshFlag();
		}
	}
}

// Разбор {"steamids": ["7656119...", ...]}. Любая невалидная запись бракует весь файл
// (список не меняем): полуприменённый список хуже старого.
static_function bool ParseList(const std::string &text, std::unordered_set<u64> &out, const char **reason)
{
	nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
	if (json.is_discarded())
	{
		*reason = "invalid_json";
		return false;
	}
	if (!json.is_object() || !json.contains("steamids") || !json["steamids"].is_array())
	{
		*reason = "missing_steamids_array";
		return false;
	}
	for (const auto &item : json["steamids"])
	{
		if (!item.is_string())
		{
			*reason = "steamid_not_string";
			return false;
		}
		const std::string &str = item.get_ref<const std::string &>();
		char *end = nullptr;
		u64 steamId = strtoull(str.c_str(), &end, 10);
		if (steamId == 0 || !end || *end != '\0')
		{
			*reason = "bad_steamid";
			return false;
		}
		out.insert(steamId);
	}
	return true;
}

void KZInvisibleService::Init()
{
	KZInvisibleService::LoadFromFile("plugin_load");
}

void KZInvisibleService::OnActivateServer()
{
	KZInvisibleService::LoadFromFile("map_start");
}

void KZInvisibleService::LoadFromFile(const char *source)
{
	char absPath[1024];
	V_snprintf(absPath, sizeof(absPath), "%s/csgo/%s", Plat_GetGameDirectory(), KZ_INVISIBLE_LIST_FILE);

	FILE *fp = fopen(absPath, "rb");
	if (!fp)
	{
		// Файл отсутствует — легальное состояние: пустой список.
		s_invisibleSteamIds.clear();
		RefreshAllPlayers();
		KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_list_loaded count=0 source=%s file=absent\n", source);
		return;
	}
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	std::string text;
	if (size > 0)
	{
		text.resize(size);
		size_t bytesRead = fread(text.data(), 1, size, fp);
		text.resize(bytesRead);
	}
	fclose(fp);

	std::unordered_set<u64> parsed;
	const char *reason = "unknown";
	if (!ParseList(text, parsed, &reason))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_list_rejected reason=%s source=%s file=%s\n", reason, source, KZ_INVISIBLE_LIST_FILE);
		return;
	}

	s_invisibleSteamIds = std::move(parsed);
	RefreshAllPlayers();
	KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_list_loaded count=%u source=%s\n", (u32)s_invisibleSteamIds.size(), source);
}

bool KZInvisibleService::IsInvisibleSteamId(u64 steamID64)
{
	return steamID64 != 0 && s_invisibleSteamIds.count(steamID64) > 0;
}

bool KZInvisibleService::IsInvisible(KZPlayer *player)
{
	return player && player->invisibleService && player->invisibleService->IsInvisible();
}

bool KZInvisibleService::ShouldHideFrom(KZPlayer *subject, KZPlayer *viewer)
{
	if (!subject || !viewer || subject == viewer)
	{
		return false;
	}
	return IsInvisible(subject) && !IsInvisible(viewer);
}

void KZInvisibleService::Reset()
{
	this->steamId64 = 0;
	this->invisible = false;
}

void KZInvisibleService::OnPlayerConnect(u64 steamID64)
{
	// Эпоха OnClientConnect: xuid уже известен — невидимка скрыт с первой секунды.
	this->steamId64 = steamID64;
	this->invisible = IsInvisibleSteamId(steamID64);
}

void KZInvisibleService::OnPlayerFullyConnect()
{
	if (this->invisible && !this->player->IsFakeClient())
	{
		this->player->languageService->PrintChat(true, false, "Invisible - Active");
	}
}

void KZInvisibleService::RefreshFlag()
{
	// steamId64 эпохи коннекта; фолбэк для late load плагина на живом сервере.
	u64 steamId = this->steamId64 ? this->steamId64 : this->player->GetSteamId64(false);
	bool newInvisible = IsInvisibleSteamId(steamId);
	if (newInvisible == this->invisible)
	{
		return;
	}
	this->invisible = newInvisible;
	if (this->player->IsInGame() && !this->player->IsFakeClient())
	{
		this->player->languageService->PrintChat(true, false, newInvisible ? "Invisible - Active" : "Invisible - Inactive");
	}
}

void KZInvisibleService::FilterReceivers(const uint64 *clients, u32 emitterPlayerIndex)
{
	KZPlayer *emitter = g_pKZPlayerManager->ToPlayer(emitterPlayerIndex);
	if (!IsInvisible(emitter))
	{
		return;
	}
	for (i32 recipientPlayerIndex = 1; recipientPlayerIndex < MAXPLAYERS + 1; recipientPlayerIndex++)
	{
		if ((u32)recipientPlayerIndex == emitterPlayerIndex)
		{
			continue;
		}
		KZPlayer *recipient = g_pKZPlayerManager->ToPlayer(recipientPlayerIndex);
		if (IsInvisible(recipient))
		{
			continue;
		}
		*(uint64 *)clients &= ~(1ull << (recipientPlayerIndex - 1));
	}
}

void KZInvisibleService::OnGameFrame()
{
	// Общий случай флота — пустой список: бесплатный выход, ниже ничего не тикает.
	if (s_invisibleSteamIds.empty())
	{
		return;
	}
	for (i32 i = 1; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *viewer = g_pKZPlayerManager->ToPlayer((u32)i);
		if (!viewer || !viewer->IsInGame() || viewer->IsFakeClient() || viewer->IsCSTV() || viewer->invisibleService->IsInvisible())
		{
			continue;
		}
		KZPlayer *target = viewer->specService->GetSpectatedPlayer();
		if (!ShouldHideFrom(target, viewer))
		{
			continue;
		}
		// GetSpectatedPlayer вернул цель — controller/observer pawn/сервис валидны.
		CPlayer_ObserverServices *obsService = viewer->GetController()->m_hObserverPawn()->m_pObserverServices;
		if (!obsService)
		{
			continue;
		}
		// Следующая валидная цель, которую viewer имеет право видеть.
		KZPlayer *next = nullptr;
		for (i32 j = 1; j < MAXPLAYERS + 1; j++)
		{
			KZPlayer *candidate = g_pKZPlayerManager->ToPlayer((u32)j);
			if (candidate == viewer || !candidate->IsInGame() || !candidate->IsAlive() || !candidate->GetPlayerPawn())
			{
				continue;
			}
			if (ShouldHideFrom(candidate, viewer))
			{
				continue;
			}
			next = candidate;
			break;
		}
		// Наборы полей — зеркало KZSpecService::SpectatePlayer (kz_spec.cpp): in-eye на
		// другого игрока либо free roam без телепорта.
		if (next)
		{
			obsService->m_iObserverMode(OBS_MODE_IN_EYE);
			obsService->m_iObserverLastMode(OBS_MODE_NONE);
			obsService->m_hObserverTarget(next->GetPlayerPawn());
		}
		else
		{
			viewer->GetController()->m_DesiredObserverMode(OBS_MODE_ROAMING);
			viewer->GetController()->m_hDesiredObserverTarget(nullptr);
			obsService->m_iObserverMode(OBS_MODE_ROAMING);
			obsService->m_hObserverTarget(nullptr);
		}
	}
}

// Живое управление списком без файла. Настоящие ConCommand'ы (не SCMD) — зовутся с
// серверной консоли/RCON без игрока (урок cyb.33, см. kz_customchangemap.cpp).
// Игрокам команды недоступны: наличие controller у слота = отказ.

static_function bool DenyIfPlayer(const CCommandContext &context, const char *command)
{
	if (utils::GetController(context.GetPlayerSlot()))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_cmd_denied cmd=%s reason=not_server slot=%d\n", command, context.GetPlayerSlot().Get());
		return true;
	}
	return false;
}

CON_COMMAND_F(kz_invisible_reload, "Reload the invisible players list from cfg/cyb_invisible.json.", FCVAR_NONE)
{
	if (DenyIfPlayer(context, "kz_invisible_reload"))
	{
		return;
	}
	KZInvisibleService::LoadFromFile("rcon_reload");
}

CON_COMMAND_F(kz_invisible_add, "Add a SteamID64 to the invisible players list (runtime only, not persisted).", FCVAR_NONE)
{
	if (DenyIfPlayer(context, "kz_invisible_add"))
	{
		return;
	}
	u64 steamId = args.ArgC() >= 2 ? strtoull(args.Arg(1), nullptr, 10) : 0;
	if (steamId == 0)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_add_rejected reason=bad_steamid arg=%s\n", args.ArgC() >= 2 ? args.Arg(1) : "<none>");
		return;
	}
	s_invisibleSteamIds.insert(steamId);
	RefreshAllPlayers();
	KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_add steam_id=%llu count=%u\n", steamId, (u32)s_invisibleSteamIds.size());
}

CON_COMMAND_F(kz_invisible_remove, "Remove a SteamID64 from the invisible players list (runtime only, not persisted).", FCVAR_NONE)
{
	if (DenyIfPlayer(context, "kz_invisible_remove"))
	{
		return;
	}
	u64 steamId = args.ArgC() >= 2 ? strtoull(args.Arg(1), nullptr, 10) : 0;
	if (steamId == 0)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_remove_rejected reason=bad_steamid arg=%s\n", args.ArgC() >= 2 ? args.Arg(1) : "<none>");
		return;
	}
	if (s_invisibleSteamIds.erase(steamId) == 0)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_remove_rejected reason=not_in_list steam_id=%llu\n", steamId);
		return;
	}
	RefreshAllPlayers();
	KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_remove steam_id=%llu count=%u\n", steamId, (u32)s_invisibleSteamIds.size());
}
