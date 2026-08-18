#include "cyb_replay_download.h"

#include "cs2kz.h"
#include "kz/kz.h"
#include "kz/mode/kz_mode.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/option/kz_option.h"
#include "kz/language/kz_language.h"
#include "kz/timer/kz_timer.h"
#include "kz/replays/kz_replay.h"
#include "kz/replays/commands.h"
#include "cyb_replay_common.h"
#include "utils/http.h"
#include "utils/json.h"
#include "utils/utils.h"
#include "utils/uuid.h"
#include "utils/logging.h"

#include <string>

namespace
{
	// Ключ резолва (см. заголовок cyb_replay_download.h — mode-гейт держится
	// именно на этом наборе полей).
	struct ResolveKey
	{
		std::string map;
		i32 course;
		std::string mode; // короткое api-имя (ckz/vnl/kzt), либо пусто — режим не поддержан
	};

	ResolveKey BuildKey(KZPlayer *player)
	{
		ResolveKey key;
		key.map = g_pKZUtils->GetCurrentMapName().Get();
		key.course = KZ::course::GetCyberCourseNumber(player->timerService->GetCourse());
		auto modeInfo = KZ::mode::GetModeInfo(player->modeService);
		key.mode = CybReplayCommon::MapMode(std::string(modeInfo.shortModeName.Get(), modeInfo.shortModeName.Length()));
		return key;
	}

	// Реальная докачка бинарника по публичному URL (selstorage, без auth) и запуск
	// существующего плейбека. replayUuid уже провалидирован как корректный UUID
	// вызывающим кодом (см. OnResolveResponse ниже).
	void DownloadAndPlay(CPlayerUserId userID, const std::string &replayUuid, const std::string &downloadUrl)
	{
		char cachedPath[512];
		V_snprintf(cachedPath, sizeof(cachedPath), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", replayUuid.c_str());

		// Уже скачан раньше (кэш в downloads/) — сети не дёргаем, играем сразу.
		if (g_pFullFileSystem->FileExists(cachedPath))
		{
			KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
			if (player)
			{
				KZ::replaysystem::commands::LoadReplay(player, replayUuid.c_str());
			}
			return;
		}

		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
		if (player)
		{
			player->languageService->PrintChat(true, false, "Replay - Central Downloading");
		}

		HTTP::Request req(HTTP::Method::GET, downloadUrl);
		// clang-format off
		req.Send(
			[userID, replayUuid](HTTP::Response resp)
			{
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				if (resp.status < 200 || resp.status >= 300)
				{
					KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] download HTTP %u for %s\n", (unsigned)resp.status, replayUuid.c_str());
					if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
					return;
				}

				std::optional<std::vector<char>> raw = resp.RawBody();
				if (!raw.has_value() || raw->empty())
				{
					KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] empty download body for %s\n", replayUuid.c_str());
					if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
					return;
				}

				char replayPath[512];
				V_snprintf(replayPath, sizeof(replayPath), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", replayUuid.c_str());

				// Синхронная запись (НЕ g_asyncFileIO->QueueWriteBuffer — тот fire-and-forget
				// без колбэка завершения): следующий вызов LoadReplay ниже сразу же читает
				// путь с диска (см. commands.cpp:52-98, data::LoadReplayAsync), файл обязан
				// физически существовать к этому моменту — гонка с асинхронной записью здесь
				// недопустима.
				if (!utils::WriteBufferToFile(replayPath, *raw))
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] failed to write downloaded replay to %s\n", replayPath);
					if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
					return;
				}

				if (player)
				{
					KZ::replaysystem::commands::LoadReplay(player, replayUuid.c_str());
				}
			},
			[userID, replayUuid]()
			{
				KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] network error downloading %s\n", replayUuid.c_str());
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
			});
		// clang-format on
	}

	void OnResolveResponse(CPlayerUserId userID, HTTP::Response resp, CybReplayDownload::Kind kind, bool targetIsSelf)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);

		if (resp.status == 404)
		{
			if (player)
			{
				// `!replay pbpro` РАБОТАЛ и до централизации: он не попадал под гейт
				// «Global Mode Only», а LoadPBReplay при недоступном глобале молча
				// фолбэчился на локальный серверный pro-PB. Поэтому центральный промах для
				// PBPro обязан вернуться в тот же локальный путь — иначе мы бы не починили
				// команду, а сломали работавшую. Только для СВОЕГО pro-PB: LoadSPBReplay
				// берёт steamid самого игрока и чужую цель не поддерживает.
				if (kind == CybReplayDownload::Kind::PBPro && targetIsSelf)
				{
					KZ::replaysystem::commands::LoadReplayForRecord(player, KZ::replaysystem::commands::RecordType::SPBPro, "", "");
					return;
				}
				// У pro-видов 404 означает не только «рекорда нет», но и частый случай
				// «рекорд есть, а файла под него нет»: реплей пишется лишь на overall/NUB-ран,
				// и если pro-рекорд игрока — другой ран, отдавать нечего (см. заголовок).
				// Общая фраза про «повтор не найден» тут вводила бы в заблуждение.
				const bool isPro = kind == CybReplayDownload::Kind::PBPro || kind == CybReplayDownload::Kind::WRPro;
				player->languageService->PrintChat(true, false, isPro ? "Replay - Pro Not Saved" : "Replay - Central Not Found");
			}
			return;
		}

		if (resp.status < 200 || resp.status >= 300)
		{
			KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] resolve HTTP %u\n", (unsigned)resp.status);
			if (player)
			{
				player->languageService->PrintChat(true, false, "Replay Request - Error");
			}
			return;
		}

		std::optional<std::string> bodyStr = resp.Body();
		if (!bodyStr.has_value())
		{
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] resolve 200 with no body\n");
			if (player)
			{
				player->languageService->PrintChat(true, false, "Replay Request - Error");
			}
			return;
		}

		Json json(*bodyStr);
		if (!json.IsValid())
		{
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] resolve returned invalid JSON\n");
			if (player)
			{
				player->languageService->PrintChat(true, false, "Replay Request - Error");
			}
			return;
		}

		std::string replayUuidStr, downloadUrl;
		if (!json.Get("replayUuid", replayUuidStr) || !json.Get("url", downloadUrl) || downloadUrl.empty())
		{
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] resolve JSON missing required fields\n");
			if (player)
			{
				player->languageService->PrintChat(true, false, "Replay Request - Error");
			}
			return;
		}

		// Защита от мусора/подмены в теле ответа — строим путь на диске только из
		// провалидированного UUID, не из сырой строки api.
		UUID_t parsedUuid;
		if (!UUID_t::FromString(replayUuidStr.c_str(), &parsedUuid))
		{
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] resolve returned malformed replayUuid '%s'\n", replayUuidStr.c_str());
			if (player)
			{
				player->languageService->PrintChat(true, false, "Replay Request - Error");
			}
			return;
		}

		DownloadAndPlay(userID, parsedUuid.ToString(), downloadUrl);
	}
} // namespace

void CybReplayDownload::RequestAndPlay(KZPlayer *player, Kind kind, u64 targetSteamId64)
{
	if (!player)
	{
		return;
	}

	const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
	if (!url || url[0] == '\0')
	{
		// Центральные реплеи выключены на этом сервере — с точки зрения игрока
		// неотличимо от "такого реплея нет".
		player->languageService->PrintChat(true, false, "Replay - Central Not Found");
		return;
	}
	const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");

	ResolveKey key = BuildKey(player);
	if (key.mode.empty() || !CybReplayCommon::IsValidMapName(key.map))
	{
		// Режим/карта не поддержаны центральным хранилищем по построению ключа —
		// данных там нет и быть не может, сеть не дёргаем.
		player->languageService->PrintChat(true, false, "Replay - Central Not Found");
		return;
	}

	std::string fullUrl = url;
	if (!fullUrl.empty() && fullUrl.back() == '/')
	{
		fullUrl.pop_back();
	}
	fullUrl += "/replays/v1/resolve";

	HTTP::Request req(HTTP::Method::GET, fullUrl);
	req.SetQuery("map", key.map);
	req.SetQuery("course", std::to_string(key.course));
	req.SetQuery("mode", key.mode);
	// Имена типов — контракт с api (resolveQuery в replays.controller.ts): pb|wr|pbpro|wrpro.
	const char *typeArg = "wr";
	switch (kind)
	{
		case Kind::PB:
			typeArg = "pb";
			break;
		case Kind::WR:
			typeArg = "wr";
			break;
		case Kind::PBPro:
			typeArg = "pbpro";
			break;
		case Kind::WRPro:
			typeArg = "wrpro";
			break;
	}
	req.SetQuery("type", typeArg);
	if (kind == Kind::PB || kind == Kind::PBPro)
	{
		req.SetQuery("steamId64", std::to_string(targetSteamId64));
	}
	if (token && token[0] != '\0')
	{
		req.SetHeader("Authorization", std::string("Bearer ") + token);
	}

	CPlayerUserId userID = player->GetClient()->GetUserID();
	// Фолбэк на локальный pro-PB возможен только для СВОЕГО реплея (см. OnResolveResponse).
	const bool targetIsSelf = targetSteamId64 != 0 && targetSteamId64 == player->GetSteamId64();

	req.Send([userID, kind, targetIsSelf](HTTP::Response resp) { OnResolveResponse(userID, resp, kind, targetIsSelf); },
			 [userID]()
			 {
				 KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] resolve network error\n");
				 KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				 if (player)
				 {
					 player->languageService->PrintChat(true, false, "Replay Request - Error");
				 }
			 });
}

void CybReplayDownload::RequestAndPlayByUuid(KZPlayer *player, const char *uuid)
{
	if (!player || !uuid || uuid[0] == '\0')
	{
		return;
	}

	const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
	if (!url || url[0] == '\0')
	{
		// Центральные реплеи выключены на этом сервере.
		player->languageService->PrintChat(true, false, "Replay - Central Not Found");
		return;
	}
	const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");

	std::string fullUrl = url;
	if (!fullUrl.empty() && fullUrl.back() == '/')
	{
		fullUrl.pop_back();
	}
	fullUrl += "/replays/v1/by-uuid";

	HTTP::Request req(HTTP::Method::GET, fullUrl);
	req.SetQuery("uuid", uuid);
	if (token && token[0] != '\0')
	{
		req.SetHeader("Authorization", std::string("Bearer ") + token);
	}

	CPlayerUserId userID = player->GetClient()->GetUserID();

	// Формат ответа идентичен resolve (url + replayUuid) — общий обработчик.
	req.Send([userID](HTTP::Response resp) { OnResolveResponse(userID, resp, CybReplayDownload::Kind::PB, false); },
			 [userID]()
			 {
				 KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] by-uuid network error\n");
				 KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				 if (player)
				 {
					 player->languageService->PrintChat(true, false, "Replay Request - Error");
				 }
			 });
}
