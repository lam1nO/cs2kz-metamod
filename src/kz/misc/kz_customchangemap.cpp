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
	// Имя карты для чата, если его передал вызывающий (GG1 знает display-имя пула, а мы —
	// нет: до конца докачки на диске нет ни .vpk, ни титула из Steam). Пустое = нечем.
	std::string displayName;
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

// Безопасное имя карты: только [A-Za-z0-9_-], минимум 3 символа. Во время докачки в папке
// может лежать частичный/битый .vpk со странным именем — такое имя в чат не выводим.
// Дефис допускаем: у части воркшоп-карт он в имени .vpk (иначе показали бы ID вместо названия).
bool IsSafeMapName(const std::string &name)
{
	if (name.size() < 3)
	{
		return false;
	}
	for (char c : name)
	{
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!ok)
		{
			return false;
		}
	}
	return true;
}

// Имя из АРГУМЕНТА команды (GG1 передаёт display-имя пула вторым аргументом). Санитайз
// шире, чем у IsSafeMapName: display-имя законно содержит пробелы и скобки
// («kz_bhop_slide [NO GLOBAL]»), но НЕ имеет права содержать `%` и `{`/`}`.
// Причина не косметическая: строка фразы прогоняется через FormatV ДВАЖДЫ (PrintType →
// player->PrintChat отдаёт уже подставленный текст как формат), а `{...}` — токен цвета.
// Одна такая пара из имени карты съела бы хвост сообщения или покрасила бы полстроки.
bool IsSafeDisplayName(const std::string &name)
{
	if (name.size() < 3 || name.size() > 64)
	{
		return false;
	}
	bool anyLetter = false;
	for (unsigned char c : name)
	{
		if (c < 0x20 || c == 0x7F || c == '%' || c == '{' || c == '}')
		{
			return false;
		}
		anyLetter = anyLetter || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
	}
	// Требуем хотя бы одну букву, а не просто «не все цифры»: имя из одних цифр — это
	// Workshop-ID (так выглядит и фолбэк GetDisplayName у GG1 при пустом display_name пула, и
	// .vpk, названный числом), а «не все цифры» пропускало ещё и имя из одних пробелов —
	// «Скачиваю карту   ». Инвариант «в чат не уходит ни число, ни пустота» держим ЗДЕСЬ, а
	// не только у вызывающего.
	return anyLetter;
}

// Название карты установленного айтема = имя .vpk в его папке. Титул из Steam здесь НЕ
// годится принципиально, а не из экономии: он несёт витринные суффиксы вроде
// «kz_bhop_slide [NO GLOBAL]», а нам нужно игровое имя карты.
// Пустая строка = имени нет (айтем не установлен, каталог не читается, .vpk не прошёл
// санитайз). Возвращать вместо имени workshop-ID нельзя — число в чате не название.
std::string GetInstalledMapName(PublishedFileId_t id)
{
	u64 sizeOnDisk = 0;
	char folder[512] = {};
	u32 timestamp = 0;
	if (!g_steamAPI.SteamUGC()->GetItemInstallInfo(id, &sizeOnDisk, folder, sizeof(folder), &timestamp) || folder[0] == '\0')
	{
		return std::string();
	}
	// Ручной инкремент через increment(ec): range-for operator++ бросает при
	// ошибке чтения каталога, а сборка с -fno-exceptions (AMBuildScript).
	std::string firstVpk;
	std::error_code ec;
	std::filesystem::directory_iterator it(folder, ec), end;
	while (!ec && it != end)
	{
		if (it->path().extension() == ".vpk")
		{
			std::string name = it->path().stem().string();
			// Split-vpk: предпочитаем «foo_dir» (индекс), а не случайный «foo_000».
			if (name.size() > 4 && name.compare(name.size() - 4, 4, "_dir") == 0)
			{
				name.resize(name.size() - 4);
				// Битый/частичный .vpk при докачке даёт мусорное имя — валидируем перед выводом.
				if (IsSafeMapName(name))
				{
					return name;
				}
			}
			else if (firstVpk.empty())
			{
				firstVpk = name;
			}
		}
		it.increment(ec);
	}
	// Не нашли валидного имени → пусто (безымянный вариант фразы), но не ID и не мусор.
	return IsSafeMapName(firstVpk) ? firstVpk : std::string();
}

void StartDownload(PublishedFileId_t id)
{
	g_steamAPI.SteamUGC()->DownloadItem(id, true);
}

// Имя карты для чата: имя из аргумента (единственный источник ДО докачки; GG1 передаёт
// display-имя пула) → имя .vpk установленного айтема. Пусто = имени НЕТ, и тогда зовущий
// печатает безымянный вариант фразы. Workshop-ID и мусор с диска сюда не попадают намеренно
// (решение пользователя 17.08): число в чате — не название карты.
std::string MapLabel(PublishedFileId_t id, const std::string &hint)
{
	if (IsSafeDisplayName(hint))
	{
		return hint;
	}
	std::string installed = GetInstalledMapName(id);
	return IsSafeDisplayName(installed) ? installed : std::string();
}

// Печать «скачиваю»/«готово» с именем или без него — ветка одна на оба места вызова, чтобы
// выбор фразы не разъехался между ними.
void PrintMapPhrase(const char *namedKey, const char *unnamedKey, const std::string &label)
{
	if (label.empty())
	{
		KZLanguageService::PrintChatAll(true, unnamedKey);
		return;
	}
	KZLanguageService::PrintChatAll(true, namedKey, label.c_str());
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
		// Докачка завершилась — .vpk на диске уже есть, но имя из аргумента всё равно
		// приоритетнее: у split-vpk стем бывает техническим («foo_dir»/«foo_000»).
		PrintMapPhrase("Mcustom - Ready", "Mcustom - Ready Unnamed", MapLabel(s_state.workshopId, s_state.displayName));
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
// Второй аргумент (необязательный) — display-имя карты для чата. Его знает вызывающий: GG1
// берёт его из пула (GGMCmaps.json), а сам форк до конца докачки не знает названия вовсе — на
// диске ещё нет .vpk, а титул из Steam доступен только асинхронным UGC-запросом. Раньше в чат
// поэтому уезжал сырой workshop-ID («Скачиваю карту 3760078813»), а через kz_customchangemap
// идёт КАЖДАЯ смена workshop-карты в ротации, не только ручной !mcustom.
// Обратная совместимость обязательна: Mcustom (CSSharp) зовёт команду одним аргументом,
// и раскатка DLL с конфигом/GG1 не одномоментна.
CON_COMMAND_F(kz_customchangemap, "Switch to a workshop map, downloading it with retries if needed (used by !mcustom). Usage: kz_customchangemap <workshop_id> [display_name]", FCVAR_NONE)
{
	// atoll — та же конвенция парсинга u64 из строкового аргумента, что в
	// src/kz/anticheat/kz_anticheat.cpp:27.
	PublishedFileId_t id = atoll(args.Arg(1));
	if (id == 0)
	{
		return;
	}
	// Arg(2) у отсутствующего аргумента отдаёт "", не nullptr; IsSafeDisplayName отсеет.
	std::string nameHint = args.Arg(2);

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
		PrintMapPhrase("Mcustom - Ready", "Mcustom - Ready Unnamed", MapLabel(id, nameHint));
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
	s_state.displayName = nameHint;
	PrintMapPhrase("Mcustom - Downloading", "Mcustom - Downloading Unnamed", MapLabel(id, nameHint));
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
