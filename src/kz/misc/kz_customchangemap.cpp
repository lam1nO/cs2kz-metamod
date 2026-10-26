// Смена workshop-карты для GG1/!mcustom: заявка с ожиданием связи со Steam и повтором
// (см. блок ниже), плюс хуки «карта из общего кэша ноды = установлена».
#include "kz_customchangemap.h"

#include "kz/kz.h"
#include "kz/language/kz_language.h"
#include "utils/utils.h"
#include "utils/logging.h"

#include "public/steam/isteamugc.h"
#include "public/steam/isteamgameserver.h"
#include "public/steam/isteamclient.h"
#include "map_recovery_policy.h"
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
// MAM и наш SDK используют 019, а движок — отдельный адаптер 021. Хукаем обе таблицы.
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

// Версия интерфейса фиксирует ABI. В 021 перед GetItemState добавлены три метода:
// 76 GetItemState, 77 GetItemInstallInfo, 79 DownloadItem (019: 73, 74, 76).
// Указатель 021 используется только KHook с явным индексом, не методами SDK 019.
static KHook::Return<uint32> GetItemState021Pre(ISteamUGC *pThis, PublishedFileId_t id);
static KHook::Virtual<ISteamUGC, uint32, PublishedFileId_t> s_itemState021Hook(GetItemState021Pre, nullptr);
static KHook::Return<bool> GetItemInstallInfo021Pre(ISteamUGC *pThis, PublishedFileId_t id, uint64 *size, char *folder, uint32 length, uint32 *timestamp);
static KHook::Virtual<ISteamUGC, bool, PublishedFileId_t, uint64 *, char *, uint32, uint32 *> s_installInfo021Hook(GetItemInstallInfo021Pre, nullptr);
static KHook::Return<bool> DownloadItem021Pre(ISteamUGC *pThis, PublishedFileId_t id, bool highPriority);
static KHook::Virtual<ISteamUGC, bool, PublishedFileId_t, bool> s_downloadItem021Hook(DownloadItem021Pre, nullptr);

namespace
{

bool s_ugcHooked = false;
bool s_ugc021Hooked = false;
std::unordered_set<PublishedFileId_t> s_loggedCacheItems;
std::unordered_set<PublishedFileId_t> s_loggedSuppressedDownloads;
std::thread::id s_mainThread;

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

template<typename Hook>
static KHook::Return<uint32> CachedItemState(Hook &hook, ISteamUGC *pThis, PublishedFileId_t id)
{
	uint32 state = hook.CallOriginal(pThis, id);
	if (std::this_thread::get_id() != s_mainThread)
	{
		return {KHook::Action::Supersede, state};
	}
	bool installed = (state & k_EItemStateInstalled) && !(state & k_EItemStateNeedsUpdate);
	if (installed || (state & k_EItemStateLegacyItem) || !IsSharedCacheMount() || !FindCachedItem(id, nullptr))
	{
		return {KHook::Action::Supersede, state};
	}
	LogCacheOverride(id, state);
	return {KHook::Action::Supersede, k_EItemStateInstalled};
}

template<typename Hook>
static KHook::Return<bool> CachedItemInstallInfo(Hook &hook, ISteamUGC *pThis, PublishedFileId_t id, uint64 *punSizeOnDisk, char *pchFolder, uint32 cchFolderSize,
                                                 uint32 *punTimeStamp)
{
	// Кэш спрашиваем ПЕРВЫМ, даже если Steam считает карту установленной: после обновления
	// карты сторожем ACF инстанса держит старую запись, и Steam отдаёт старое время установки.
	// CDedicatedServerWorkshopManager (libserver) сверяет его со временем публикации, видит
	// «устарела» и зовёт DownloadItem — а докачка внутри контейнера падает (EResult 37,
	// «Staging library folder not found»), смена карты молча не происходит (09.10, kz_dankality).
	CachedItem item;
	if (std::this_thread::get_id() != s_mainThread || !pchFolder || cchFolderSize == 0 || !IsSharedCacheMount() || !FindCachedItem(id, &item) || item.folder.size() >= cchFolderSize)
	{
		return {KHook::Action::Supersede, hook.CallOriginal(pThis, id, punSizeOnDisk, pchFolder, cchFolderSize, punTimeStamp)};
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
template<typename Hook>
static KHook::Return<bool> CachedDownloadItem(Hook &hook, ISteamUGC *pThis, PublishedFileId_t id, bool bHighPriority)
{
	if (std::this_thread::get_id() != s_mainThread || !IsSharedCacheMount() || !FindCachedItem(id, nullptr))
	{
		return {KHook::Action::Supersede, hook.CallOriginal(pThis, id, bHighPriority)};
	}
	if (s_loggedSuppressedDownloads.insert(id).second)
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb] workshop_download_suppressed id=%llu reason=in_shared_cache\n", id);
	}
	return {KHook::Action::Supersede, true};
}

static KHook::Return<uint32> GetItemStatePre(ISteamUGC *self, PublishedFileId_t id)
{
	return CachedItemState(s_itemStateHook, self, id);
}
static KHook::Return<uint32> GetItemState021Pre(ISteamUGC *self, PublishedFileId_t id)
{
	return CachedItemState(s_itemState021Hook, self, id);
}
static KHook::Return<bool> GetItemInstallInfoPre(ISteamUGC *self, PublishedFileId_t id, uint64 *size, char *folder, uint32 length, uint32 *timestamp)
{
	return CachedItemInstallInfo(s_installInfoHook, self, id, size, folder, length, timestamp);
}
static KHook::Return<bool> GetItemInstallInfo021Pre(ISteamUGC *self, PublishedFileId_t id, uint64 *size, char *folder, uint32 length, uint32 *timestamp)
{
	return CachedItemInstallInfo(s_installInfo021Hook, self, id, size, folder, length, timestamp);
}
static KHook::Return<bool> DownloadItemPre(ISteamUGC *self, PublishedFileId_t id, bool priority)
{
	return CachedDownloadItem(s_downloadItemHook, self, id, priority);
}
static KHook::Return<bool> DownloadItem021Pre(ISteamUGC *self, PublishedFileId_t id, bool priority)
{
	return CachedDownloadItem(s_downloadItem021Hook, self, id, priority);
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
constexpr f64 kStallSeconds = 60.0;   // идущая закачка без прироста байт дольше — сдаёмся
constexpr f64 kRetryNoticeSeconds = 60.0;
constexpr f64 kSteamNoticeSeconds = 3.0;
constexpr f64 kStartMapGraceSeconds = 45.0; // штатная стартовая смена из командной строки

// Причина немедленного отказа, выставленная колбэком закачки (карта удалена/скрыта).
const char *s_failNow = nullptr;

struct MapChangeRequest
{
	bool active = false;
	PublishedFileId_t id = 0;
	f64 since = 0;
	f64 lastIssue = 0;
	int issues = 0;
	bool steamNoticeShown = false;
	bool fromStartup = false;
	u64 mapAtRequest = 0;  // workshop-карта сервера в момент заявки
	u64 lastBytes = 0;     // прогресс закачки, которую ведёт Steam
	f64 lastProgress = 0;  // когда байты в последний раз росли (или заявка создана)
	f64 lastRetryNotice = 0;
};

MapChangeRequest s_req;

void StartRequest(PublishedFileId_t id, bool fromStartup)
{
	s_failNow = nullptr;
	s_req = {};
	s_req.active = true;
	s_req.id = id;
	s_req.since = Plat_FloatTime();
	s_req.lastProgress = s_req.since;
	s_req.fromStartup = fromStartup;
	s_req.mapAtRequest = g_pKZUtils->GetCurrentMapWorkshopID();
}

bool s_timerStarted = false;
// Карты, на которых этот процесс уже стоял: движок помнит их сведения и меняет на них без
// запроса в Steam (проверено 16.10 с отрезанным Steam) — для них связи не ждём.
std::unordered_set<u64> s_hostedMaps;
bool s_startMapRecoveryEnabled = false;
f64 s_startMapRetryAt = 0;
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

// Только CS:GO-версия айтема (<id>_legacy.bin без vpk): CS2 её не загрузит никогда.
bool IsLegacyOnlyItem(PublishedFileId_t id)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path dir = WorkshopContentDir() / std::to_string(id);
	if (WorkshopContentDir().empty() || !fs::is_directory(dir, ec) || FindCachedItem(id, nullptr))
	{
		return false;
	}
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
	{
		if (it->path().filename().string().find("_legacy.bin") != std::string::npos)
		{
			return true;
		}
	}
	return false;
}

void Finish(const char *outcome)
{
	KZ_LOG_INFO(LogChannel::General, "[cyb] mapchange_%s id=%llu issues=%d elapsed=%.0f startup=%d\n", outcome, s_req.id, s_req.issues,
				Now() - s_req.since, s_req.fromStartup);
	if (s_req.fromStartup)
	{
		s_startMapRetryAt = Now() + 60.0;
	}
	s_req = {};
}

// Стартовая карта идёт `+host_workshop_map` из командной строки — раньше, чем подняты хуки и
// связь со Steam. После неудачи повторяем с паузой: одноразовая проверка оставляла
// процесс на Dust2 навсегда (26.10 srv-23), несмотря на последующие перезагрузки карты.
void CheckStartMap()
{
	if (!KZ::misc::customchangemap::ShouldRecoverStartMap(s_startMapRecoveryEnabled, s_req.active,
            g_pKZUtils->GetCurrentMapWorkshopID(), Now(), s_loadedAt + kStartMapGraceSeconds, s_startMapRetryAt))
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
	StartRequest(id, true);
}

f64 MapChangeTick()
{
	CheckStartMap();
	if (u64 hosted = g_pKZUtils->GetCurrentMapWorkshopID())
	{
		s_hostedMaps.insert(hosted);
	}
	if (!s_req.active)
	{
		return kTickSeconds;
	}
	f64 now = Now();
	u64 current = g_pKZUtils->GetCurrentMapWorkshopID();
	if (s_failNow)
	{
		const char *reason = s_failNow;
		s_failNow = nullptr;
		if (!s_req.fromStartup)
		{
			KZLanguageService::PrintChatAll(true, "Mcustom - Unavailable", std::to_string(s_req.id).c_str());
		}
		KZ_LOG_WARN(LogChannel::General, "[cyb] mapchange_unavailable id=%llu reason=%s\n", s_req.id, reason);
		Finish("failed_unavailable");
		return kTickSeconds;
	}
	if (current == s_req.id)
	{
		Finish("done");
		return kTickSeconds;
	}
	// Карту сменили мимо заявки (админ, другая команда) — не тащим сервер обратно.
	if (current != 0 && current != s_req.mapAtRequest)
	{
		Finish("superseded_external");
		return kTickSeconds;
	}
	// Закачку ведёт Steam (карты нет в кэше): пока растут байты, не торопим и не повторяем —
	// повтор host_workshop_map поверх идущей закачки ничего не ускоряет.
	bool downloading = false;
	if (ISteamUGC *ugc = g_steamAPI.SteamUGC())
	{
		uint32 state = ugc->GetItemState(s_req.id);
		downloading = state & (k_EItemStateDownloading | k_EItemStateDownloadPending);
		uint64 done = 0, total = 0;
		if (downloading && ugc->GetItemDownloadInfo(s_req.id, &done, &total) && done != s_req.lastBytes)  // != : Steam может начать закачку заново
		{
			s_req.lastBytes = done;
			s_req.lastProgress = now;
		}
	}
	if (!downloading && IsLegacyOnlyItem(s_req.id))
	{
		s_failNow = "legacy_csgo_item"; // разберётся следующий тик тем же путём, что и отказ закачки
		return kTickSeconds;
	}
	bool stalled = now - s_req.lastProgress > kStallSeconds;
	if (now - s_req.since > kGiveUpSeconds && (!downloading || stalled))
	{
		if (!s_req.fromStartup)
		{
			// ID, а не имя: игроку предлагают повторить `!mcustom <id>`.
			KZLanguageService::PrintChatAll(true, "Mcustom - Exhausted", std::to_string(s_req.id).c_str());
		}
		Finish(!SteamLoggedOn() ? "failed_steam_offline" : downloading ? "failed_download_stalled" : "failed");
		return kTickSeconds;
	}
	if (!SteamLoggedOn() && !s_hostedMaps.count(s_req.id))
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
	// Стартовую карту движок уже качает по `+host_workshop_map` из командной строки.
	if (downloading && (s_req.issues > 0 || s_req.fromStartup))
	{
		return kTickSeconds;
	}
	if (s_req.issues == 0 || now - s_req.lastIssue >= kReissueSeconds)
	{
		s_req.issues++;
		s_req.lastIssue = now;
		KZ_LOG_INFO(LogChannel::General, "[cyb] mapchange_issue id=%llu attempt=%d\n", s_req.id, s_req.issues);
		if (s_req.issues > 1 && !s_req.fromStartup && now - s_req.lastRetryNotice >= kRetryNoticeSeconds)
		{
			s_req.lastRetryNotice = now;
			KZLanguageService::PrintChatAll(true, "Mcustom - Retry", s_req.issues);
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
		// Удалена или скрыта автором: ждать бессмысленно, говорим игрокам сразу (16.10: удалённая
		// 3809766386 отвечала 15, затем 2 — и заявка висела бы 3 минуты).
		// Только для карт вне кэша: vpk в кэше движок возьмёт через хук и без Steam.
		if ((pParam->m_eResult == k_EResultFileNotFound || pParam->m_eResult == k_EResultAccessDenied) && !FindCachedItem(s_req.id, nullptr))
		{
			s_failNow = pParam->m_eResult == k_EResultFileNotFound ? "file_not_found" : "access_denied";
		}
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
		StartRequest(id, false);
	}
	MapChangeTick();
}

void KZ::misc::customchangemap::Init(bool late)
{
	// Поздняя загрузка плагина (meta reload): сервер мог осознанно стоять на официальной карте.
	s_startMapRecoveryEnabled = !late;
	s_startMapRetryAt = 0;
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

	ISteamUGC *ugc021 = g_steamAPI.SteamClient()->GetISteamUGC(SteamGameServer_GetHSteamUser(), SteamGameServer_GetHSteamPipe(),
		                                                      "STEAMUGC_INTERFACE_VERSION021");
	if (!ugc021 || KHook::GetVtableIndex(&ISteamUGC::GetItemState) != 73
		   || KHook::GetVtableIndex(&ISteamUGC::GetItemInstallInfo) != 74
		   || KHook::GetVtableIndex(&ISteamUGC::DownloadItem) != 76
		   || *reinterpret_cast<void ***>(ugc021) == *reinterpret_cast<void ***>(ugc))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] workshop_cache_hook version=021 active=0 reason=interface_unavailable_or_aliased\n");
		return;
	}
	s_itemState021Hook.Configure(76);
	s_installInfo021Hook.Configure(77);
	s_downloadItem021Hook.Configure(79);
	s_itemState021Hook.AddGlobal(ugc021);
	s_installInfo021Hook.AddGlobal(ugc021);
	s_downloadItem021Hook.AddGlobal(ugc021);
	s_ugc021Hooked = true;
	KZ_LOG_INFO(LogChannel::General, "[cyb] workshop_cache_hook version=021 active=1\n");
}

void KZ::misc::customchangemap::Cleanup()
{
	s_downloadHandler.m_CallbackDownloadItemResult.Unregister();
	if (s_ugc021Hooked)
	{
		s_itemState021Hook.ClearHooks();
		s_installInfo021Hook.ClearHooks();
		s_downloadItem021Hook.ClearHooks();
		s_ugc021Hooked = false;
	}
	if (s_ugcHooked)
	{
		s_itemStateHook.ClearHooks();
		s_installInfoHook.ClearHooks();
		s_downloadItemHook.ClearHooks();
		s_ugcHooked = false;
	}
}
