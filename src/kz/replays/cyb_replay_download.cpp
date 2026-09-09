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

// Ожидание AWR-режима (см. заголовок): ставится перед докачкой, снимается первым же
// LoadReplay ЭТОГО uuid. Один активный реплей на сервер — состояние глобальное, как и сам
// плейбек. Хранится строка uuid, а не флаг: чужой `!replay <uuid>` не должен его подобрать.
static_global char g_pendingAwrUuid[40] = {};
static_global u64 g_pendingAwrMs = 0;

void CybReplayDownload::SetPendingAwr(const char *uuid, u64 awrMs)
{
	if (!uuid || uuid[0] == '\0')
	{
		CybReplayDownload::ClearPendingAwr();
		return;
	}
	V_strncpy(g_pendingAwrUuid, uuid, sizeof(g_pendingAwrUuid));
	g_pendingAwrMs = awrMs;
}

void CybReplayDownload::ClearPendingAwr()
{
	g_pendingAwrUuid[0] = '\0';
	g_pendingAwrMs = 0;
}

bool CybReplayDownload::TakePendingAwr(const char *uuid, u64 &awrMs)
{
	const bool match = g_pendingAwrUuid[0] != '\0' && uuid && uuid[0] != '\0' && KZ_STREQI(g_pendingAwrUuid, uuid);
	awrMs = match ? g_pendingAwrMs : 0;
	// Гасим ВСЕГДА: ожидание одноразовое, и мимо своего uuid ему жить незачем.
	CybReplayDownload::ClearPendingAwr();
	return match;
}

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

	// Имена типов — контракт с api (resolveQuery в replays.controller.ts): pb|wr|pbpro|wrpro|awr.
	const char *ResolveTypeArg(CybReplayDownload::Kind kind)
	{
		switch (kind)
		{
			case CybReplayDownload::Kind::PB:
				return "pb";
			case CybReplayDownload::Kind::WR:
				return "wr";
			case CybReplayDownload::Kind::PBPro:
				return "pbpro";
			case CybReplayDownload::Kind::WRPro:
				return "wrpro";
			case CybReplayDownload::Kind::AWR:
				return "awr";
		}
		return "wr";
	}

	// Пролог резолва, общий для плейбека и для !lead: базовый URL, токен и ключ по текущему
	// курсу/режиму игрока. false — резолв невозможен по построению (центральные реплеи на
	// сервере выключены либо режим/карта не поддержаны ключом), сеть не дёргаем.
	bool PrepareResolve(KZPlayer *player, std::string &outUrl, std::string &outToken, ResolveKey &outKey)
	{
		const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
		if (!url || url[0] == '\0')
		{
			return false;
		}
		outKey = BuildKey(player);
		if (outKey.mode.empty() || !CybReplayCommon::IsValidMapName(outKey.map))
		{
			return false;
		}
		outUrl = url;
		if (!outUrl.empty() && outUrl.back() == '/')
		{
			outUrl.pop_back();
		}
		outUrl += "/replays/v1/resolve";
		const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");
		outToken = token ? token : "";
		return true;
	}

	// Файл реплея: кэш downloads/ или реальная докачка по публичному URL (selstorage, без
	// auth). replayUuid уже провалидирован как корректный UUID вызывающим кодом (см.
	// OnResolveResponse ниже).
	//
	// wantBytes — нужны ли байты в колбэке. Путь плейбека их НЕ берёт: LoadReplay читает
	// файл с диска сам и асинхронно, и синхронное чтение здесь было бы лишним хитчем на
	// десятки мегабайт. Путь !lead берёт (ему нужен разбор на своём потоке).
	// announce — печатать ли игроку служебные фразы («качаю», «ошибка запроса»). У !lead
	// свои фразы, дублировать незачем.
	void FetchReplayBytes(CPlayerUserId userID, const std::string &replayUuid, const std::string &downloadUrl, bool wantBytes,
						  bool announce, std::function<void(CPlayerUserId, bool, std::vector<char> &&)> onDone)
	{
		char cachedPath[512];
		V_snprintf(cachedPath, sizeof(cachedPath), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", replayUuid.c_str());

		// Уже скачан раньше (кэш в downloads/) — сети не дёргаем.
		if (g_pFullFileSystem->FileExists(cachedPath))
		{
			std::vector<char> cached;
			if (wantBytes && !utils::ReadBufferFromFile(cachedPath, cached))
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] failed to read cached replay %s\n", cachedPath);
				if (announce)
				{
					KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
					if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
				}
				onDone(userID, false, {});
				return;
			}
			onDone(userID, true, std::move(cached));
			return;
		}

		if (announce)
		{
			KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
			if (player)
			{
				player->languageService->PrintChat(true, false, "Replay - Central Downloading");
			}
		}

		HTTP::Request req(HTTP::Method::GET, downloadUrl);
		// clang-format off
		req.Send(
			[userID, replayUuid, wantBytes, announce, onDone](HTTP::Response resp)
			{
				KZPlayer *player = announce ? g_pKZPlayerManager->ToPlayer(userID) : nullptr;
				if (resp.status < 200 || resp.status >= 300)
				{
					KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] download HTTP %u for %s\n", (unsigned)resp.status, replayUuid.c_str());
					if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
					onDone(userID, false, {});
					return;
				}

				std::optional<std::vector<char>> raw = resp.RawBody();
				if (!raw.has_value() || raw->empty())
				{
					KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] empty download body for %s\n", replayUuid.c_str());
					if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
					onDone(userID, false, {});
					return;
				}

				char replayPath[512];
				V_snprintf(replayPath, sizeof(replayPath), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", replayUuid.c_str());

				// Синхронная запись (НЕ g_asyncFileIO->QueueWriteBuffer — тот fire-and-forget
				// без колбэка завершения): следующий вызов LoadReplay сразу же читает
				// путь с диска (см. commands.cpp:52-98, data::LoadReplayAsync), файл обязан
				// физически существовать к этому моменту — гонка с асинхронной записью здесь
				// недопустима.
				if (!utils::WriteBufferToFile(replayPath, *raw))
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] failed to write downloaded replay to %s\n", replayPath);
					if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
					onDone(userID, false, {});
					return;
				}

				onDone(userID, true, wantBytes ? std::move(*raw) : std::vector<char>());
			},
			[userID, replayUuid, announce, onDone]()
			{
				KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] network error downloading %s\n", replayUuid.c_str());
				if (announce)
				{
					KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
					if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
				}
				onDone(userID, false, {});
			});
		// clang-format on
	}

	// Докачка + запуск существующего плейбека. Поведение пути бота сохранено полностью,
	// включая семантику ожидания AWR: оно снимается в КАЖДОЙ ветке, где LoadReplay не будет
	// вызван (любой отказ докачки/чтения кэша — ok=false; игрок ушёл — звать некому).
	//
	// Гашение живёт ЗДЕСЬ, а не в FetchReplayBytes: тот общий с `!lead`, а `!lead` ожидания
	// AWR не ставит и трогать его не имеет права — иначе отказ загрузки луча съедал бы
	// ожидание одновременно запущенного `!replay awr`.
	void DownloadAndPlay(CPlayerUserId userID, const std::string &replayUuid, const std::string &downloadUrl)
	{
		FetchReplayBytes(userID, replayUuid, downloadUrl, false, true,
						 [replayUuid](CPlayerUserId uid, bool ok, std::vector<char> &&)
						 {
							 KZPlayer *player = ok ? g_pKZPlayerManager->ToPlayer(uid) : nullptr;
							 if (!player)
							 {
								 CybReplayDownload::ClearPendingAwr();
								 return;
							 }
							 KZ::replaysystem::commands::LoadReplay(player, replayUuid.c_str());
						 });
	}

	void OnResolveResponse(CPlayerUserId userID, HTTP::Response resp, CybReplayDownload::Kind kind, bool targetIsSelf)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);

		// Любой резолв начинается с чистого ожидания: привязка к uuid уже не даёт подобрать
		// чужое, но незачем и держать протухшее.
		CybReplayDownload::ClearPendingAwr();

		if (resp.status == 404)
		{
			if (player)
			{
				// AWR: фолбэка нет — в локальной БД плагина AWR не существует по построению.
				if (kind == CybReplayDownload::Kind::AWR)
				{
					player->languageService->PrintChat(true, false, "Replay - AWR Not Found");
					return;
				}
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

		if (kind == CybReplayDownload::Kind::AWR)
		{
			// Подпись до загрузки: истину (разрез по самому файлу) посчитает LoadReplay.
			// `awrMs: null` — штатный ответ (разрез ещё не считали), поэтому проверяем ключ
			// ТИХО: Get на null/отсутствующем ключе пишет WARN, а тревожиться тут не о чем.
			f64 awrMs = 0.0;
			if (json.HasValue("awrMs"))
			{
				json.Get("awrMs", awrMs);
			}
			CybReplayDownload::SetPendingAwr(parsedUuid.ToString().c_str(), awrMs > 0.0 ? (u64)awrMs : 0);
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

	std::string fullUrl, token;
	ResolveKey key;
	if (!PrepareResolve(player, fullUrl, token, key))
	{
		// Центральные реплеи выключены либо режим/карта не поддержаны ключом — с точки
		// зрения игрока неотличимо от «такого реплея нет».
		player->languageService->PrintChat(true, false, "Replay - Central Not Found");
		return;
	}

	HTTP::Request req(HTTP::Method::GET, fullUrl);
	req.SetQuery("map", key.map);
	req.SetQuery("course", std::to_string(key.course));
	req.SetQuery("mode", key.mode);
	req.SetQuery("type", ResolveTypeArg(kind));
	if (kind == Kind::PB || kind == Kind::PBPro)
	{
		req.SetQuery("steamId64", std::to_string(targetSteamId64));
	}
	if (!token.empty())
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

void CybReplayDownload::RequestFile(KZPlayer *player, Kind kind, u64 targetSteamId64, std::function<void(CPlayerUserId, std::vector<char>)> onReady)
{
	if (!player || !onReady)
	{
		return;
	}

	CPlayerUserId userID = player->GetClient()->GetUserID();

	std::string fullUrl, token;
	ResolveKey key;
	if (!PrepareResolve(player, fullUrl, token, key))
	{
		onReady(userID, {});
		return;
	}

	HTTP::Request req(HTTP::Method::GET, fullUrl);
	req.SetQuery("map", key.map);
	req.SetQuery("course", std::to_string(key.course));
	req.SetQuery("mode", key.mode);
	req.SetQuery("type", ResolveTypeArg(kind));
	if (kind == Kind::PB || kind == Kind::PBPro)
	{
		req.SetQuery("steamId64", std::to_string(targetSteamId64));
	}
	if (!token.empty())
	{
		req.SetHeader("Authorization", std::string("Bearer ") + token);
	}

	// clang-format off
	req.Send(
		[userID, onReady](HTTP::Response resp)
		{
			if (resp.status < 200 || resp.status >= 300)
			{
				// 404 — штатное «такой записи нет», в лог не пишем (это не отказ нашей стороны).
				if (resp.status != 404)
				{
					KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] file resolve HTTP %u\n", (unsigned)resp.status);
				}
				onReady(userID, {});
				return;
			}

			std::optional<std::string> bodyStr = resp.Body();
			if (!bodyStr.has_value())
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] file resolve 200 with no body\n");
				onReady(userID, {});
				return;
			}

			Json json(*bodyStr);
			std::string replayUuidStr, downloadUrl;
			UUID_t parsedUuid;
			// Путь на диске строим только из провалидированного UUID, не из сырой строки api.
			if (!json.IsValid() || !json.Get("replayUuid", replayUuidStr) || !json.Get("url", downloadUrl) || downloadUrl.empty()
				|| !UUID_t::FromString(replayUuidStr.c_str(), &parsedUuid))
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] file resolve returned unusable JSON\n");
				onReady(userID, {});
				return;
			}

			FetchReplayBytes(userID, parsedUuid.ToString(), downloadUrl, true, false,
							 [onReady](CPlayerUserId uid, bool ok, std::vector<char> &&bytes)
							 {
								 onReady(uid, ok ? std::move(bytes) : std::vector<char>());
							 });
		},
		[userID, onReady]()
		{
			KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] file resolve network error\n");
			onReady(userID, {});
		});
	// clang-format on
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
