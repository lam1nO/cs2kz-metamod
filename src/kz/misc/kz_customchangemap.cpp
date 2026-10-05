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
#include <system_error>
#include <thread>
#include <sys/stat.h>
#include <unordered_set>

#include "tier0/memdbgon.h"

extern CSteamGameServerAPIContext g_steamAPI;

// Карта, лежащая в общем кэше ноды, но неизвестная Steam'у этого процесса. Steam узнаёт
// об установленных картах только из ACF, а ACF инстанса агент собирает лишь перед
// docker run: карта, добавленная в пул или обновлённая сторожем после старта, для
// сервера «не установлена», и смена на неё уходила в DownloadItem — а он внутри
// игрового процесса отвечает EResult 2/3/37 (srv-12, kz_imaginary, 05.10). Сторож
// держит кэш равным публикации, поэтому кэш — истина: если vpk на месте, отвечаем
// «установлена» сами. Хук на vtable ISteamUGC, а не на наш указатель: путь к аддону
// при смене карты спрашивает libserver (CCSAddonManager), состояние — ещё и MAM;
// все они на STEAMUGC_INTERFACE_VERSION019, то есть на одной таблице.
SH_DECL_HOOK1(ISteamUGC, GetItemState, SH_NOATTRIB, 0, uint32, PublishedFileId_t);
SH_DECL_HOOK5(ISteamUGC, GetItemInstallInfo, SH_NOATTRIB, 0, bool, PublishedFileId_t, uint64 *, char *, uint32, uint32 *);

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

int s_itemStateHook = 0;
int s_installInfoHook = 0;
std::unordered_set<PublishedFileId_t> s_loggedCacheItems;
std::thread::id s_mainThread;
bool s_offThreadLogged = false;

// Стек контекстов SourceHook общий: вызов хука из чужого потока параллельно с главным
// его портит. По открытому коду все вызовы UGC — из главного потока; libserver закрыт,
// поэтому на канарейке ловим обратное в лог, а не предполагаем.
void CheckHookThread(const char *fn)
{
	if (!s_offThreadLogged && std::this_thread::get_id() != s_mainThread)
	{
		s_offThreadLogged = true;
		KZ_LOG_WARN(LogChannel::General, "[cyb] workshop_cache_hook_off_main_thread fn=%s\n", fn);
	}
}

struct CachedItem
{
	std::string folder;
	uint64 size = 0;
	uint32 timestamp = 0;
};

// Каталог воркшопа относительно рабочего каталога процесса: у выделенного сервера он
// лежит там (см. BuildAddonPath в MAM), cwd = game/bin/linuxsteamrt64.
const std::filesystem::path &WorkshopContentDir()
{
	static std::filesystem::path s_dir = []
	{
		std::error_code ec;
		std::filesystem::path cwd = std::filesystem::current_path(ec);
		return ec ? std::filesystem::path() : cwd / "steamapps" / "workshop" / "content" / "730";
	}();
	return s_dir;
}

// Подмена допустима ТОЛЬКО в режиме общего кэша: агент монтирует кэш ноды поверх
// content/730, и его держит равным публикации сторож. В legacy-режиме (srv-3, DM) этот
// каталог свой у инстанса, обновления качает сам Steam — там «vpk на месте» не значит
// «актуален», и подмена NeedsUpdate навсегда оставила бы сервер на старой версии.
bool IsSharedCacheMount()
{
	static int s_shared = -1;
	if (s_shared != -1)
	{
		return s_shared == 1;
	}
	s_shared = 0;
#ifdef _LINUX
	std::error_code ec;
	std::filesystem::path dir = std::filesystem::canonical(WorkshopContentDir(), ec);
	FILE *f = ec ? nullptr : fopen("/proc/self/mountinfo", "r");
	if (f)
	{
		// Поле 5 — точка монтирования. Пробелы в путях экранируются (\040), у нас их нет.
		char line[4096];
		while (fgets(line, sizeof(line), f))
		{
			char mountPoint[2048];
			if (sscanf(line, "%*s %*s %*s %*s %2047s", mountPoint) == 1 && dir == mountPoint)
			{
				s_shared = 1;
				break;
			}
		}
		fclose(f);
	}
#endif
	KZ_LOG_INFO(LogChannel::General, "[cyb] workshop_cache_mode shared=%d dir=%s\n", s_shared, WorkshopContentDir().string().c_str());
	return s_shared == 1;
}

bool FindCachedItem(PublishedFileId_t id, CachedItem *out)
{
	namespace fs = std::filesystem;
	if (WorkshopContentDir().empty())
	{
		return false;
	}
	std::error_code ec;
	fs::path dir = WorkshopContentDir() / std::to_string(id);

	// Только Source 2: многочанковый <id>_dir.vpk или одиночный <id>.vpk. Айтемы
	// CS:GO (_legacy.bin) движок всё равно не смонтирует.
	fs::path vpk = dir / (std::to_string(id) + "_dir.vpk");
	if (!fs::is_regular_file(vpk, ec))
	{
		vpk = dir / (std::to_string(id) + ".vpk");
		if (!fs::is_regular_file(vpk, ec))
		{
			return false;
		}
	}
	if (!out)
	{
		return true;
	}

	out->folder = dir.string();
	out->size = 0;
	// increment(ec), а не range-for: operator++ бросает filesystem_error, и при
	// -fno-exceptions это std::terminate прямо внутри хука.
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
	{
		std::error_code fileEc;
		uintmax_t size = it->file_size(fileEc);
		if (!fileEc && it->is_regular_file(fileEc))
		{
			out->size += size;
		}
	}
	// stat, а не fs::last_write_time: эпоха file_clock в libstdc++ — 2174 год.
	struct stat st;
	out->timestamp = stat(vpk.string().c_str(), &st) == 0 && st.st_mtime > 0 ? static_cast<uint32>(st.st_mtime) : 0;
	return true;
}

void LogCacheOverride(PublishedFileId_t id, uint32 steamState)
{
	if (s_loggedCacheItems.insert(id).second)
	{
		// Раз на карту за жизнь процесса: GetItemState зовут на каждой смене карты и в MAM.
		KZ_LOG_INFO(LogChannel::General, "[cyb] workshop_cache_override id=%llu steam_state=0x%x\n", id, steamState);
	}
}

uint32 Hook_GetItemState(PublishedFileId_t id)
{
	CheckHookThread("GetItemState");
	uint32 state = META_RESULT_ORIG_RET(uint32);
	bool installed = (state & k_EItemStateInstalled) && !(state & k_EItemStateNeedsUpdate);
	if (installed || (state & k_EItemStateLegacyItem) || !IsSharedCacheMount() || !FindCachedItem(id, nullptr))
	{
		RETURN_META_VALUE(MRES_IGNORED, state);
	}
	LogCacheOverride(id, state);
	RETURN_META_VALUE(MRES_OVERRIDE, k_EItemStateInstalled);
}

bool Hook_GetItemInstallInfo(PublishedFileId_t id, uint64 *punSizeOnDisk, char *pchFolder, uint32 cchFolderSize, uint32 *punTimeStamp)
{
	CheckHookThread("GetItemInstallInfo");
	if (META_RESULT_ORIG_RET(bool))
	{
		RETURN_META_VALUE(MRES_IGNORED, true);
	}
	CachedItem item;
	if (!pchFolder || cchFolderSize == 0 || !IsSharedCacheMount() || !FindCachedItem(id, &item) || item.folder.size() >= cchFolderSize)
	{
		RETURN_META_VALUE(MRES_IGNORED, false);
	}
	V_strncpy(pchFolder, item.folder.c_str(), cchFolderSize);
	if (punSizeOnDisk)
	{
		*punSizeOnDisk = item.size;
	}
	if (punTimeStamp)
	{
		*punTimeStamp = item.timestamp;
	}
	LogCacheOverride(id, 0);
	RETURN_META_VALUE(MRES_OVERRIDE, true);
}

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
		SwitchToMap(s_state.workshopId);
		s_state = {};
		return;
	}

	KZ_LOG_WARN(LogChannel::General, "kz_customchangemap: попытка %d/%d для %llu — провал, EResult=%d\n", s_state.attempt,
				CUSTOMMAP_MAX_ATTEMPTS, s_state.workshopId, pParam->m_eResult);

	if (s_state.attempt >= CUSTOMMAP_MAX_ATTEMPTS)
	{
		// Здесь подставляется аргумент команды `!mcustom <...>`, которую игроку предлагают
		// повторить, — поэтому ID, а не имя: по имени команда не сработает.
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
// Второй аргумент (display-имя карты) команда ПРИНИМАЕТ и игнорирует: он был нужен только
// фразам «Скачиваю карту»/«Карта готова», а их убрали (#8). Через kz_customchangemap идёт
// КАЖДАЯ смена workshop-карты (!rtv, !nominate, конец голосования), карта в норме уже лежит
// на диске: «скачиваю» игроки читали как реальную закачку, а «готово» дублировало «Следующая
// карта X» от GG1. Аргумент оставлен принимаемым намеренно: GG1/Mcustom его всё ещё передают,
// и раскатка DLL с конфигом/GG1 не одномоментна.
CON_COMMAND_F(kz_customchangemap, "Switch to a workshop map, downloading it with retries if needed (used by !mcustom). Usage: kz_customchangemap <workshop_id> [display_name (ignored)]", FCVAR_NONE)
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
	StartDownload(id);
}

void KZ::misc::customchangemap::Init()
{
	s_downloadHandler.m_CallbackDownloadItemResult.Register(&s_downloadHandler, &CustomMapDownloadHandler::OnDownloadResult);
}

void KZ::misc::customchangemap::OnSteamAPIActivated()
{
	ISteamUGC *ugc = g_steamAPI.SteamUGC();
	if (!ugc || s_itemStateHook)
	{
		return;
	}
	s_mainThread = std::this_thread::get_id();
	s_itemStateHook = SH_ADD_VPHOOK(ISteamUGC, GetItemState, ugc, SH_STATIC(Hook_GetItemState), true);
	s_installInfoHook = SH_ADD_VPHOOK(ISteamUGC, GetItemInstallInfo, ugc, SH_STATIC(Hook_GetItemInstallInfo), true);
}

void KZ::misc::customchangemap::Cleanup()
{
	s_downloadHandler.m_CallbackDownloadItemResult.Unregister();
	if (s_itemStateHook)
	{
		SH_REMOVE_HOOK_ID(s_itemStateHook);
		s_itemStateHook = 0;
	}
	if (s_installInfoHook)
	{
		SH_REMOVE_HOOK_ID(s_installInfoHook);
		s_installInfoHook = 0;
	}
}
