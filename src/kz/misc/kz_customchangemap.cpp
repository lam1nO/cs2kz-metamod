// !mcustom при сбое связи со Steam раньше молча зависал (докачка через host_workshop_map
// не даёт сигнала об ошибке). Здесь — явный ретрай на реальном ответе ISteamUGC, без
// таймаутов: паттерн подсмотрен в KZRacingService (src/kz/racing/map.cpp), которая тем
// же способом ждёт готовности воркшоп-карты перед сменой.
#include "kz_customchangemap.h"

#include "kz/kz.h"
#include "kz/language/kz_language.h"
#include "utils/utils.h"
#include "utils/logging.h"

#include "public/steam/isteamugc.h"

#include <filesystem>

#include "tier0/memdbgon.h"

extern CSteamGameServerAPIContext g_steamAPI;

namespace
{
constexpr int CUSTOMMAP_MAX_ATTEMPTS = 6;

struct CustomMapState
{
	bool pending = false;
	PublishedFileId_t workshopId = 0;
	int attempt = 0;
};

CustomMapState s_state;

bool IsMapReady(PublishedFileId_t id)
{
	auto state = g_steamAPI.SteamUGC()->GetItemState(id);
	return (state & (k_EItemStateInstalled | k_EItemStateNeedsUpdate | k_EItemStateDownloading | k_EItemStateDownloadPending))
		   == k_EItemStateInstalled;
}

void SwitchToMap(PublishedFileId_t id)
{
	std::string command = "host_workshop_map " + std::to_string(id);
	interfaces::pEngine->ServerCommand(command.c_str());
}

// Название карты установленного айтема = имя .vpk в его папке (титул из Steam
// доступен только асинхронным UGC-запросом — не тянем ради строки в чате).
// Фолбэк — сам workshop-ID строкой.
std::string GetInstalledMapName(PublishedFileId_t id)
{
	u64 sizeOnDisk = 0;
	char folder[512] = {};
	u32 timestamp = 0;
	if (!g_steamAPI.SteamUGC()->GetItemInstallInfo(id, &sizeOnDisk, folder, sizeof(folder), &timestamp) || folder[0] == '\0')
	{
		return std::to_string(id);
	}
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(folder, ec))
	{
		if (entry.path().extension() == ".vpk")
		{
			std::string name = entry.path().stem().string();
			if (name.size() > 4 && name.compare(name.size() - 4, 4, "_dir") == 0)
			{
				name.resize(name.size() - 4);
			}
			return name;
		}
	}
	return std::to_string(id);
}

void StartDownload(PublishedFileId_t id)
{
	g_steamAPI.SteamUGC()->DownloadItem(id, true);
}

static_function struct CustomMapDownloadHandler
{
	STEAM_GAMESERVER_CALLBACK_MANUAL(CustomMapDownloadHandler, OnDownloadResult, DownloadItemResult_t, m_CallbackDownloadItemResult);
} s_downloadHandler;

void CustomMapDownloadHandler::OnDownloadResult(DownloadItemResult_t *pParam)
{
	// Колбэк общий на весь сервер (в т.ч. чужие докачки, если появятся) — фильтруем по ID.
	if (!s_state.pending || pParam->m_nPublishedFileId != s_state.workshopId)
	{
		return;
	}

	if (pParam->m_eResult == k_EResultOK)
	{
		KZ_LOG_INFO(LogChannel::General, "kz_customchangemap: попытка %d/%d для %llu — успех\n", s_state.attempt,
					CUSTOMMAP_MAX_ATTEMPTS, s_state.workshopId);
		KZLanguageService::PrintChatAll(true, "Mcustom - Ready", GetInstalledMapName(s_state.workshopId).c_str());
		SwitchToMap(s_state.workshopId);
		s_state = {};
		return;
	}

	KZ_LOG_WARN(LogChannel::General, "kz_customchangemap: попытка %d/%d для %llu — провал, EResult=%d\n", s_state.attempt,
				CUSTOMMAP_MAX_ATTEMPTS, s_state.workshopId, pParam->m_eResult);

	if (s_state.attempt >= CUSTOMMAP_MAX_ATTEMPTS)
	{
		KZLanguageService::PrintChatAll(true, "Mcustom - Exhausted", std::to_string(s_state.workshopId).c_str());
		s_state = {};
		return;
	}

	s_state.attempt++;
	KZLanguageService::PrintChatAll(true, "Mcustom - Retry", s_state.attempt, CUSTOMMAP_MAX_ATTEMPTS);
	StartDownload(s_state.workshopId);
}
} // namespace

// Настоящий движковый ConCommand, а НЕ SCMD: команду зовёт mcustom (CSSharp) через
// Server.ExecuteCommand, т.е. серверным контекстом без игрока. SCMD-диспатч живёт в хуке
// DispatchConCommand и требует controller — из серверной консоли/RCON такая команда
// давала "Unknown command" (подтверждено вживую на srv-1, cyb.32).
CON_COMMAND_F(kz_customchangemap, "Switch to a workshop map, downloading it with retries if needed (used by !mcustom).", FCVAR_NONE)
{
	// atoll — та же конвенция парсинга u64 из строкового аргумента, что в
	// src/kz/anticheat/kz_anticheat.cpp:27.
	PublishedFileId_t id = atoll(args.Arg(1));
	if (id == 0)
	{
		return;
	}

	if (IsMapReady(id))
	{
		bool isCurrent = g_pKZUtils->GetCurrentMapWorkshopID() == static_cast<u32>(id);
		if (isCurrent)
		{
			KZLanguageService::PrintChatAll(true, "Mcustom - Already Playing");
			return;
		}
		// Карта уже готова — переключаемся сразу. Обнуляем state, чтобы более ранний
		// pending-запрос (другой ID, чья докачка ещё идёт) не был подхвачен колбэком
		// и не откатил это переключение на свою карту при своём успехе.
		s_state = {};
		KZLanguageService::PrintChatAll(true, "Mcustom - Ready", GetInstalledMapName(id).c_str());
		SwitchToMap(id);
		return;
	}

	if (s_state.pending && s_state.workshopId == id)
	{
		// Повторный вызов на уже идущий запрос — просто печатаем текущий статус,
		// попытки не сбрасываем и докачку не перезапускаем.
		KZLanguageService::PrintChatAll(true, "Mcustom - Retry", s_state.attempt, CUSTOMMAP_MAX_ATTEMPTS);
		return;
	}

	s_state.pending = true;
	s_state.workshopId = id;
	s_state.attempt = 1;
	KZLanguageService::PrintChatAll(true, "Mcustom - Downloading", std::to_string(id).c_str());
	StartDownload(id);
}

void KZ::misc::customchangemap::Init()
{
	s_downloadHandler.m_CallbackDownloadItemResult.Register(&s_downloadHandler, &CustomMapDownloadHandler::OnDownloadResult);
}

void KZ::misc::customchangemap::Cleanup()
{
	s_downloadHandler.m_CallbackDownloadItemResult.Unregister();
}
