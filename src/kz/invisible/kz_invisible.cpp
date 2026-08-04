#include "kz_invisible.h"
#include "kz/language/kz_language.h"

#include "sdk/serversideclient.h"
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

// Сколько невидимок сейчас ОНЛАЙН (кэш-флаги игроков). Гейт горячих путей: файл со
// списком админов на проде непуст всегда, а вот невидимка на сервере — редкость.
static_global i32 s_onlineInvisibleCount = 0;

// Пересчитать кэш-флаги всем игрокам после смены списка. Счётчик онлайн-невидимок
// пересобирается с нуля — защита от дрейфа инкрементальной арифметики (сиротский флаг
// отклонённого коннекта чинит декремент в OnPlayerConnect при переиспользовании слота).
// Возвращает, стал ли кто-то из игроков В ИГРЕ видимым — тогда вызывающий должен
// разослать один сетевой full update (BroadcastFullUpdate).
static_function bool RefreshAllPlayers()
{
	bool anyBecameVisible = false;
	i32 onlineInvisibleCount = 0;
	for (int i = 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (!player || !player->invisibleService)
		{
			continue;
		}
		anyBecameVisible |= player->invisibleService->RefreshFlag();
		if (player->invisibleService->IsInvisible())
		{
			onlineInvisibleCount++;
		}
	}
	s_onlineInvisibleCount = onlineInvisibleCount;
	return anyBecameVisible;
}

// Возврат видимости: клиентам нужен полный снапшот, иначе pawn может не пересоздаться
// у них сразу. ТОЛЬКО сетевой ForceFullUpdate + снап углов, БЕЗ SetAngles/Teleport:
// телепорт тащит побочку на непричастных (lastTeleportTime блокирует TimerStart,
// джампстаты рвут прыжок, триггеры с cancelOnTeleport; предупреждение в kz_player.cpp:
// «Using SetAngles, which uses Teleport makes player movement really weird»).
// Дедуп на вызывающем: один бродкаст на мутацию списка, не на каждого снятого.
static_function void BroadcastFullUpdate()
{
	for (i32 i = 1; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *viewer = g_pKZPlayerManager->ToPlayer((u32)i);
		if (!viewer || !viewer->IsInGame() || viewer->IsFakeClient() || viewer->IsCSTV())
		{
			continue;
		}
		// Невидимкам (в т.ч. вернувшемуся) апдейт не нужен — они субъекта и так видели.
		if (KZInvisibleService::IsInvisible(viewer))
		{
			continue;
		}
		CServerSideClient *client = g_pKZUtils->GetClientBySlot(viewer->GetPlayerSlot());
		if (!client)
		{
			continue;
		}
		client->ForceFullUpdate();
		// Сохраняем вид (full update может дёрнуть углы) — прецедент DisableTurnbinds.
		// Только живым: GetAngles() читает moveDataPost, который у не-симулируемых
		// (мёртвый/спектатор/после смены карты) хранит углы прошлой жизни; им снап и не
		// нужен — они смотрят через observer pawn.
		if (viewer->IsAlive())
		{
			// IsAlive() уже гарантирует ненулевой pawn.
			QAngle angles;
			viewer->GetAngles(&angles);
			g_pKZUtils->SnapViewAngles(viewer->GetPlayerPawn(), angles);
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
		if (RefreshAllPlayers())
		{
			BroadcastFullUpdate();
		}
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
	if (RefreshAllPlayers())
	{
		BroadcastFullUpdate();
	}
	KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_list_loaded count=%u source=%s\n", (u32)s_invisibleSteamIds.size(), source);
}

void KZInvisibleService::OnAllPluginsLoaded()
{
	// Late load: наш Init() уже применил список, но ResetPlayers() из AllPluginsLoaded
	// обнуляет сервисы ПОСЛЕ него — без повторного применения meta reload на живом
	// сервере молча снимал бы невидимость до смены карты. Заодно пересобирается счётчик.
	RefreshAllPlayers();
}

bool KZInvisibleService::IsInvisibleSteamId(u64 steamID64)
{
	return steamID64 != 0 && s_invisibleSteamIds.count(steamID64) > 0;
}

bool KZInvisibleService::HasOnlineInvisibles()
{
	return s_onlineInvisibleCount > 0;
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
	if (this->invisible)
	{
		s_onlineInvisibleCount--;
	}
	this->steamId64 = 0;
	this->invisible = false;
}

void KZInvisibleService::OnPlayerConnect(u64 steamID64)
{
	// Слот мог остаться «сиротой» после отклонённого коннекта (OnClientDisconnect не
	// приходил, Reset не звался) — сперва снимаем его вклад из счётчика.
	if (this->invisible)
	{
		s_onlineInvisibleCount--;
	}
	// Эпоха OnClientConnect: xuid уже известен — невидимка скрыт с первой секунды.
	this->steamId64 = steamID64;
	this->invisible = IsInvisibleSteamId(steamID64);
	if (this->invisible)
	{
		s_onlineInvisibleCount++;
	}
}

void KZInvisibleService::OnPlayerActive()
{
	if (this->invisible && !this->player->IsFakeClient())
	{
		this->player->languageService->PrintChat(true, false, "Invisible - Active");
	}
}

bool KZInvisibleService::RefreshFlag()
{
	// steamId64 эпохи коннекта; фолбэк для late load плагина на живом сервере.
	// Пересчёт зависит ТОЛЬКО от steamId64 + списка; любой гейт по состоянию
	// клиента/сущностей на границах сессии/смены карты необратимо гасит флаг до
	// следующей мутации списка. Сиротский флаг (коннект отклонён) безвреден: pawn нет,
	// максимум держит горячий гейт открытым; вклад в счётчик снимает декремент в
	// OnPlayerConnect при переиспользовании слота.
	u64 steamId = this->steamId64 ? this->steamId64 : this->player->GetSteamId64(false);
	bool newInvisible = IsInvisibleSteamId(steamId);
	if (newInvisible == this->invisible)
	{
		return false;
	}
	this->invisible = newInvisible;
	// Счётчик здесь не трогаем: единственный вызывающий — RefreshAllPlayers, он
	// пересобирает s_onlineInvisibleCount целиком после обхода.
	if (this->player->IsInGame() && !this->player->IsFakeClient())
	{
		this->player->languageService->PrintChat(true, false, newInvisible ? "Invisible - Active" : "Invisible - Inactive");
		// Возврат видимости игрока в игре — сигнал вызывающему на один BroadcastFullUpdate.
		return !newInvisible;
	}
	return false;
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
	if (RefreshAllPlayers())
	{
		BroadcastFullUpdate();
	}
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
	if (RefreshAllPlayers())
	{
		BroadcastFullUpdate();
	}
	KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_remove steam_id=%llu count=%u\n", steamId, (u32)s_invisibleSteamIds.size());
}
