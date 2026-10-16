// Смена workshop-карты для GG1/!mcustom: заявка с ожиданием связи со Steam и повтором
// (см. блок ниже), плюс хуки «карта из общего кэша ноды = установлена».
#include "kz_customchangemap.h"

#include "kz/kz.h"
#include "kz/language/kz_language.h"
#include "utils/utils.h"
#include "utils/logging.h"

#include "public/steam/isteamugc.h"
#include "public/steam/isteamgameserver.h"
#include "utils/ctimer.h"

#include <filesystem>
#include <system_error>
#include <thread>
#include <ctime>
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
//
// KHook (Metamod API 18): решение принимается в PRE — оригинал зовём сами через CallOriginal и
// отдаём Supersede со своим значением (тот же приём, что апстрим в ProcessUsercmds). Так не
// зависим от того, как KHook трактует Override в POST.
static KHook::Return<uint32> GetItemStatePre(ISteamUGC *pThis, PublishedFileId_t id);
static KHook::Virtual<ISteamUGC, uint32, PublishedFileId_t> s_itemStateHook(GetItemStatePre, nullptr);
static KHook::Return<bool> GetItemInstallInfoPre(ISteamUGC *pThis, PublishedFileId_t id, uint64 *punSizeOnDisk, char *pchFolder, uint32 cchFolderSize,
												 uint32 *punTimeStamp);
static KHook::Virtual<ISteamUGC, bool, PublishedFileId_t, uint64 *, char *, uint32, uint32 *> s_installInfoHook(GetItemInstallInfoPre, nullptr);
static KHook::Return<bool> DownloadItemPre(ISteamUGC *pThis, PublishedFileId_t id, bool bHighPriority);
static KHook::Virtual<ISteamUGC, bool, PublishedFileId_t, bool> s_downloadItemHook(DownloadItemPre, nullptr);

namespace
{

bool s_ugcHooked = false;
std::unordered_set<PublishedFileId_t> s_loggedCacheItems;
std::unordered_set<PublishedFileId_t> s_loggedSuppressedDownloads;
std::thread::id s_mainThread;
bool s_offThreadLogged = false;

// При SourceHook общий стек контекстов портился от вызова хука из чужого потока; KHook держит
// свои мьютексы, но наш FindCachedItem и s_loggedCacheItems не потокобезопасны. По открытому
// коду все вызовы UGC — из главного потока; libserver закрыт, поэтому ловим обратное в лог.
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

} // namespace

static KHook::Return<uint32> GetItemStatePre(ISteamUGC *pThis, PublishedFileId_t id)
{
	CheckHookThread("GetItemState");
	uint32 state = s_itemStateHook.CallOriginal(pThis, id);
	bool installed = (state & k_EItemStateInstalled) && !(state & k_EItemStateNeedsUpdate);
	if (installed || (state & k_EItemStateLegacyItem) || !IsSharedCacheMount() || !FindCachedItem(id, nullptr))
	{
		return {KHook::Action::Supersede, state};
	}
	LogCacheOverride(id, state);
	return {KHook::Action::Supersede, k_EItemStateInstalled};
}

static KHook::Return<bool> GetItemInstallInfoPre(ISteamUGC *pThis, PublishedFileId_t id, uint64 *punSizeOnDisk, char *pchFolder, uint32 cchFolderSize,
												 uint32 *punTimeStamp)
{
	CheckHookThread("GetItemInstallInfo");
	// Кэш спрашиваем ПЕРВЫМ, даже если Steam считает карту установленной: после обновления
	// карты сторожем ACF инстанса держит старую запись, и Steam отдаёт старое время установки.
	// CDedicatedServerWorkshopManager (libserver) сверяет его со временем публикации, видит
	// «устарела» и зовёт DownloadItem — а докачка внутри контейнера падает (EResult 37,
	// «Staging library folder not found»), смена карты молча не происходит (09.10, kz_dankality).
	CachedItem item;
	if (!pchFolder || cchFolderSize == 0 || !IsSharedCacheMount() || !FindCachedItem(id, &item) || item.folder.size() >= cchFolderSize)
	{
		return {KHook::Action::Supersede, s_installInfoHook.CallOriginal(pThis, id, punSizeOnDisk, pchFolder, cchFolderSize, punTimeStamp)};
	}
	// Время установки — «сейчас», а не mtime vpk: сторож кладёт новую сборку раньше, чем Steam
	// поднимает time_updated публикации (метаданные правятся позже контента), и любое
	// «честное» время из кэша менеджер всё равно счёл бы устаревшим. Кэш — истина (см. выше).
	item.timestamp = static_cast<uint32>(time(nullptr));
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
	return {KHook::Action::Supersede, true};
}

// Докачку карты, которая уже лежит в общем кэше, не запускаем вовсе: её держит актуальной
// сторож ноды, а внутри игрового процесса докачка в общий кэш падает (см. GetItemInstallInfoPre).
// Отвечаем «запрос принят» — тот же ответ, что Steam даёт на уже установленный предмет.
static KHook::Return<bool> DownloadItemPre(ISteamUGC *pThis, PublishedFileId_t id, bool bHighPriority)
{
	CheckHookThread("DownloadItem");
	if (!IsSharedCacheMount() || !FindCachedItem(id, nullptr))
	{
		return {KHook::Action::Supersede, s_downloadItemHook.CallOriginal(pThis, id, bHighPriority)};
	}
	if (s_loggedSuppressedDownloads.insert(id).second)
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb] workshop_download_suppressed id=%llu reason=in_shared_cache\n", id);
	}
	return {KHook::Action::Supersede, true};
}

namespace
{

// Смена workshop-карты (16.10, проверено опытами на канарейке srv-26 с отрезанным Steam):
//  * host_workshop_map ВСЕГДА спрашивает Steam о последней версии карты; без связи команда —
//    безопасная пустышка: сервер остаётся на текущей карте, отвечает, игроков не трогает;
//  * карта, которую этот процесс уже загружал, грузится и без Steam;
//  * обход Steam (`map <имя> customgamemode=<id> nomapvalidation=true`) вешает загрузку уровня.
// Поэтому: одна заявка на смену; пока Steam недоступен — ждём связи и говорим об этом игрокам;
// со связью — host_workshop_map и повтор, если карта не встала. Голосование не блокируем:
// новая заявка заменяет старую. Обрывы связи со Steam на флоте — секунды, изредка ~2 мин.
constexpr f64 kTickSeconds = 1.0;
constexpr f64 kReissueSeconds = 20.0; // карта не встала за это время — команду повторяем
constexpr f64 kGiveUpSeconds = 180.0; // покрывает обрыв связи со Steam до трёх минут
constexpr f64 kSteamNoticeSeconds = 3.0;
constexpr f64 kStartMapGraceSeconds = 45.0; // штатная стартовая смена из командной строки

struct MapChangeRequest
{
	bool active = false;
	PublishedFileId_t id = 0;
	f64 since = 0;
	f64 lastIssue = 0;
	int issues = 0;
	bool steamNoticeShown = false;
	bool fromStartup = false;
};

MapChangeRequest s_req;
bool s_timerStarted = false;
bool s_startMapChecked = false;
f64 s_loadedAt = 0;

f64 Now()
{
	return Plat_FloatTime();
}

bool SteamLoggedOn()
{
	return g_steamAPI.SteamGameServer() && g_steamAPI.SteamGameServer()->BLoggedOn();
}

void IssueHostWorkshopMap(PublishedFileId_t id)
{
	std::string command = "host_workshop_map " + std::to_string(id);
	interfaces::pEngine->ServerCommand(command.c_str());
}

void Finish(const char *outcome)
{
	KZ_LOG_INFO(LogChannel::General, "[cyb] mapchange_%s id=%llu issues=%d elapsed=%.0f startup=%d\n", outcome, s_req.id, s_req.issues,
				Now() - s_req.since, s_req.fromStartup);
	s_req = {};
}

// Стартовая карта идёт `+host_workshop_map` из командной строки — раньше, чем подняты хуки и
// связь со Steam; при сбое (EResult 2, 16.10 srv-26) сервер так и стоял на de_dust2.
void CheckStartMap()
{
	if (s_startMapChecked || s_req.active || Now() - s_loadedAt < kStartMapGraceSeconds)
	{
		return;
	}
	s_startMapChecked = true;
	if (g_pKZUtils->GetCurrentMapWorkshopID() != 0)
	{
		return;
	}
	const char *env = getenv("CS2_START_WORKSHOP_MAP");
	PublishedFileId_t id = env ? strtoull(env, nullptr, 10) : 0;
	if (id == 0)
	{
		return;
	}
	KZ_LOG_WARN(LogChannel::General, "[cyb] mapchange_start_map_missing id=%llu reason=still_on_official_map\n", id);
	s_req = {true, id, Now(), 0, 0, false, true};
}

f64 MapChangeTick()
{
	CheckStartMap();
	if (!s_req.active)
	{
		return kTickSeconds;
	}
	f64 now = Now();
	if (g_pKZUtils->GetCurrentMapWorkshopID() == s_req.id)
	{
		Finish("done");
		return kTickSeconds;
	}
	if (now - s_req.since > kGiveUpSeconds)
	{
		if (!s_req.fromStartup)
		{
			// ID, а не имя: игроку предлагают повторить `!mcustom <id>`.
			KZLanguageService::PrintChatAll(true, "Mcustom - Exhausted", std::to_string(s_req.id).c_str());
		}
		Finish(SteamLoggedOn() ? "failed" : "failed_steam_offline");
		return kTickSeconds;
	}
	if (!SteamLoggedOn())
	{
		if (!s_req.steamNoticeShown && now - s_req.since >= kSteamNoticeSeconds)
		{
			s_req.steamNoticeShown = true;
			KZ_LOG_WARN(LogChannel::General, "[cyb] mapchange_wait id=%llu reason=steam_offline\n", s_req.id);
			if (!s_req.fromStartup)
			{
				KZLanguageService::PrintChatAll(true, "Mcustom - Waiting Steam");
			}
		}
		return kTickSeconds;
	}
	if (s_req.issues == 0 || now - s_req.lastIssue >= kReissueSeconds)
	{
		s_req.issues++;
		s_req.lastIssue = now;
		KZ_LOG_INFO(LogChannel::General, "[cyb] mapchange_issue id=%llu attempt=%d\n", s_req.id, s_req.issues);
		if (s_req.issues > 1 && !s_req.fromStartup)
		{
			KZLanguageService::PrintChatAll(true, "Mcustom - Retry", s_req.issues, static_cast<int>(kGiveUpSeconds / kReissueSeconds));
		}
		IssueHostWorkshopMap(s_req.id);
	}
	return kTickSeconds;
}

void EnsureTimer()
{
	if (!s_timerStarted)
	{
		s_timerStarted = true;
		// preserveMapChange: заявка переживает смену карты; realtime — тик и в паузе/гибернации.
		StartTimer(MapChangeTick, kTickSeconds, true, true);
	}
}

// Сбой закачки сам по себе состояние заявки не меняет (повтор — по таймеру), но EResult нужен в логе.
static_function struct CustomMapDownloadHandler
{
	STEAM_GAMESERVER_CALLBACK_MANUAL(CustomMapDownloadHandler, OnDownloadResult, DownloadItemResult_t, m_CallbackDownloadItemResult);
} s_downloadHandler;

void CustomMapDownloadHandler::OnDownloadResult(DownloadItemResult_t *pParam)
{
	if (s_req.active && pParam->m_nPublishedFileId == s_req.id && pParam->m_eResult != k_EResultOK)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] mapchange_download_failed id=%llu eresult=%d\n", s_req.id, pParam->m_eResult);
	}
}
} // namespace

// Настоящий движковый ConCommand, а НЕ SCMD: его зовут GG1 и mcustom (CSSharp) серверным
// контекстом без игрока, SCMD требует controller (cyb.33). Второй аргумент (имя карты)
// принимается и игнорируется — GG1/Mcustom его передают.
CON_COMMAND_F(kz_customchangemap, "Switch to a workshop map: waits for Steam if it is offline, retries until the map starts. Usage: kz_customchangemap <workshop_id> [display_name (ignored)]", FCVAR_NONE)
{
	PublishedFileId_t id = strtoull(args.Arg(1), nullptr, 10);
	if (id == 0)
	{
		return;
	}
	EnsureTimer();
	if (g_pKZUtils->GetCurrentMapWorkshopID() == id)
	{
		KZLanguageService::PrintChatAll(true, "Mcustom - Already Playing");
		return;
	}
	if (s_req.active && s_req.id != id)
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb] mapchange_superseded id=%llu by=%llu\n", s_req.id, id);
	}
	if (!(s_req.active && s_req.id == id))
	{
		s_req = {true, id, Now(), 0, 0, false, false};
	}
	MapChangeTick();
}

void KZ::misc::customchangemap::Init()
{
	s_downloadHandler.m_CallbackDownloadItemResult.Register(&s_downloadHandler, &CustomMapDownloadHandler::OnDownloadResult);
	s_loadedAt = Now();
	EnsureTimer();
}

void KZ::misc::customchangemap::OnSteamAPIActivated()
{
	ISteamUGC *ugc = g_steamAPI.SteamUGC();
	if (!ugc || s_ugcHooked)
	{
		return;
	}
	s_mainThread = std::this_thread::get_id();
	// AddGlobal — хук на втаблицу (как SH_ADD_VPHOOK): libserver и MAM держат свои указатели.
	s_itemStateHook.Configure(&ISteamUGC::GetItemState);
	s_itemStateHook.AddGlobal(ugc);
	s_installInfoHook.Configure(&ISteamUGC::GetItemInstallInfo);
	s_installInfoHook.AddGlobal(ugc);
	s_downloadItemHook.Configure(&ISteamUGC::DownloadItem);
	s_downloadItemHook.AddGlobal(ugc);
	s_ugcHooked = true;
}

void KZ::misc::customchangemap::Cleanup()
{
	s_downloadHandler.m_CallbackDownloadItemResult.Unregister();
	if (s_ugcHooked)
	{
		s_itemStateHook.ClearHooks();
		s_installInfoHook.ClearHooks();
		s_downloadItemHook.ClearHooks();
		s_ugcHooked = false;
	}
}
