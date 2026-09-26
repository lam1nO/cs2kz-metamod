#include "cyb_replay_download.h"

#include "cs2kz.h"
#include "kz/kz.h"
#include "kz/mode/kz_mode.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/option/kz_option.h"
#include "kz/language/kz_language.h"
#include "kz/timer/kz_timer.h"
#include "kz/spec/kz_spec.h"
#include "kz/replays/kz_replay.h"
#include "kz/replays/commands.h"
#include "cyb_replay_common.h"
#include "utils/http.h"
#include "utils/json.h"
#include "utils/utils.h"
#include "utils/uuid.h"
#include "utils/logging.h"

#include <cstdlib> // strtoull: steamId64 приезжает строкой (u64 не влезает в число JSON)
#include <string>

// Ожидание вида записи и AWR-режима (см. заголовок): ставится перед докачкой, снимается
// первым же LoadReplay ЭТОГО uuid. Один активный реплей на сервер — состояние глобальное, как
// и сам плейбек. Хранится строка uuid, а не флаг: чужой `!replay <uuid>` не должен его подобрать.
static_global char g_pendingUuid[40] = {};
static_global CybReplayDownload::Pending g_pending {};

void CybReplayDownload::SetPendingKind(const char *uuid, Kind kind)
{
	if (!uuid || uuid[0] == '\0')
	{
		CybReplayDownload::ClearPending();
		return;
	}
	V_strncpy(g_pendingUuid, uuid, sizeof(g_pendingUuid));
	g_pending.hasKind = true;
	g_pending.kind = kind;
}

void CybReplayDownload::SetPendingAwr(const char *uuid, u64 awrMs)
{
	if (!uuid || uuid[0] == '\0')
	{
		CybReplayDownload::ClearPending();
		return;
	}
	V_strncpy(g_pendingUuid, uuid, sizeof(g_pendingUuid));
	g_pending.awr = true;
	g_pending.awrMs = awrMs;
}

void CybReplayDownload::ClearPending()
{
	g_pendingUuid[0] = '\0';
	g_pending = CybReplayDownload::Pending();
}

bool CybReplayDownload::TakePending(const char *uuid, Pending &out)
{
	const bool match = g_pendingUuid[0] != '\0' && uuid && uuid[0] != '\0' && KZ_STREQI(g_pendingUuid, uuid);
	out = match ? g_pending : CybReplayDownload::Pending();
	// Гасим ВСЕГДА: ожидание одноразовое, и мимо своего uuid ему жить незачем.
	CybReplayDownload::ClearPending();
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

	// followSpectated — ключ по курсу/режиму НАБЛЮДАЕМОГО, если игрок наблюдает (команда
	// !replay: смотрящий за чужим раном хочет реплеи его курса). !lead — всегда свой ключ.
	ResolveKey BuildKey(KZPlayer *player, bool followSpectated)
	{
		ResolveKey key;
		key.map = g_pKZUtils->GetCurrentMapName().Get();
		const KZCourseDescriptor *course = player->timerService->GetCourse();
		KZPlayer *modeOwner = player;
		if (followSpectated)
		{
			KZInfoSubject subject = player->specService->GetInfoSubject();
			course = subject.course;
			modeOwner = subject.player;
		}
		key.course = KZ::course::GetCyberCourseNumber(course);
		auto modeInfo = KZ::mode::GetModeInfo(modeOwner->modeService);
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
	// outReason — машинная причина такого отказа: без неё «повтор не найден» одинаково
	// покрывал и выключенный конфиг, и неподдержанный режим, и реальный промах по ключу.
	bool PrepareResolve(KZPlayer *player, std::string &outUrl, std::string &outToken, ResolveKey &outKey, const char **outReason = nullptr,
						bool followSpectated = false)
	{
		auto deny = [outReason](const char *reason)
		{
			if (outReason)
			{
				*outReason = reason;
			}
			return false;
		};
		const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
		if (!url || url[0] == '\0')
		{
			return deny("central_disabled");
		}
		outKey = BuildKey(player, followSpectated);
		if (outKey.mode.empty())
		{
			return deny("mode_unsupported");
		}
		if (!CybReplayCommon::IsValidMapName(outKey.map))
		{
			return deny("map_name_rejected");
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

	// Query-часть запроса резолва: ключ + вид записи + авторизация. Одна на все три входа
	// (плейбек, файл для !lead, метаданные для !awr) — расхождение здесь означало бы, что одна
	// из команд ходит не по тому ключу, и заметили бы мы это не сразу.
	void ApplyResolveQuery(HTTP::Request &req, CybReplayDownload::Kind kind, const ResolveKey &key, u64 targetSteamId64, const std::string &token)
	{
		req.SetQuery("map", key.map);
		req.SetQuery("course", std::to_string(key.course));
		req.SetQuery("mode", key.mode);
		req.SetQuery("type", ResolveTypeArg(kind));
		if (kind == CybReplayDownload::Kind::PB || kind == CybReplayDownload::Kind::PBPro)
		{
			req.SetQuery("steamId64", std::to_string(targetSteamId64));
		}
		if (!token.empty())
		{
			req.SetHeader("Authorization", std::string("Bearer ") + token);
		}
	}

	// Ключ резолва в отказе. Отказ обязан называть КУРС и РЕЖИМ: именно этот невидимый
	// фильтр 17.09 заставил владельца перебирать ник/SteamID/UUID на kz_angina_x, где
	// записи лежали на бонусе, а он стоял на main. `logKey` — машинная строка в лог,
	// `courseText`/`mode` — то же самое словами, для фразы в чат.
	struct ResolveContext
	{
		std::string logKey;
		std::string courseText;
		std::string mode;
		// Запрос шёл в /replays/v1/by-uuid: ключа курса/режима там нет вовсе, и «не найдено»
		// относится к самому UUID.
		bool byUuid {};
	};

	ResolveContext MakeContext(const ResolveKey &key)
	{
		ResolveContext ctx;
		ctx.logKey = "map=" + key.map + " course=" + std::to_string(key.course) + " mode=" + key.mode;
		ctx.courseText = CybReplayCommon::CourseText(key.course);
		ctx.mode = key.mode;
		return ctx;
	}

	// Машинная причина отказа из тела 404 api (ResolveFailReason в replays.service.ts).
	// Пустая строка — тела нет либо оно не разобралось (старая сборка api, прокси): тогда
	// печатаем прежнюю общую фразу, а не выдумываем причину.
	std::string ReadFailReason(const HTTP::Response &resp)
	{
		std::optional<std::string> body = resp.Body();
		if (!body.has_value() || body->empty())
		{
			return std::string();
		}
		Json json(*body);
		if (!json.IsValid() || !json.HasValue("error"))
		{
			return std::string();
		}
		std::string reason;
		return json.Get("error", reason) ? reason : std::string();
	}

	// Файл реплея: кэш downloads/ или реальная докачка по публичному URL (selstorage, без
	// auth). replayUuid уже провалидирован как корректный UUID вызывающим кодом (см.
	// OnResolveResponse ниже).
	//
	// Колбэк получает ПУТЬ (относительный, как его понимают utils::Read/WriteBufferToFile) к
	// файлу, который к этому моменту лежит на диске — и ничего не читает сам: реплей до
	// 32 МБ, чтение в тике было бы хитчем. Кто хочет содержимое, читает его на своём
	// рабочем потоке (так же устроен и путь плейбека: LoadReplay читает файл асинхронно).
	//
	// announce — печатать ли игроку служебные фразы («качаю», «ошибка запроса»). У !lead
	// свои фразы, дублировать незачем.
	void FetchReplayFile(CPlayerUserId userID, const std::string &replayUuid, const std::string &downloadUrl, bool announce,
						 std::function<void(CPlayerUserId, bool, std::string)> onDone)
	{
		char cachedPath[512];
		V_snprintf(cachedPath, sizeof(cachedPath), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", replayUuid.c_str());

		// Уже скачан раньше (кэш в downloads/) — сети не дёргаем.
		if (g_pFullFileSystem->FileExists(cachedPath))
		{
			onDone(userID, true, cachedPath);
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
			[userID, replayUuid, announce, onDone](HTTP::Response resp)
			{
				KZPlayer *player = announce ? g_pKZPlayerManager->ToPlayer(userID) : nullptr;
				if (resp.status < 200 || resp.status >= 300)
				{
					KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] download HTTP %u for %s\n", (unsigned)resp.status, replayUuid.c_str());
					if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
					onDone(userID, false, "");
					return;
				}

				std::optional<std::vector<char>> raw = resp.RawBody();
				if (!raw.has_value() || raw->empty())
				{
					KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] empty download body for %s\n", replayUuid.c_str());
					if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
					onDone(userID, false, "");
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
					onDone(userID, false, "");
					return;
				}

				onDone(userID, true, replayPath);
			},
			[userID, replayUuid, announce, onDone]()
			{
				KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] network error downloading %s\n", replayUuid.c_str());
				if (announce)
				{
					KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
					if (player) player->languageService->PrintChat(true, false, "Replay Request - Error");
				}
				onDone(userID, false, "");
			});
		// clang-format on
	}

	// Докачка + запуск существующего плейбека. Поведение пути бота сохранено полностью,
	// включая семантику ожидания AWR: оно снимается в КАЖДОЙ ветке, где LoadReplay не будет
	// вызван (любой отказ докачки/чтения кэша — ok=false; игрок ушёл — звать некому).
	//
	// Гашение живёт ЗДЕСЬ, а не в FetchReplayFile: тот общий с `!lead`, а `!lead` ожидания
	// AWR не ставит и трогать его не имеет права — иначе отказ загрузки луча съедал бы
	// ожидание одновременно запущенного `!replay awr`.
	void DownloadAndPlay(CPlayerUserId userID, const std::string &replayUuid, const std::string &downloadUrl)
	{
		FetchReplayFile(userID, replayUuid, downloadUrl, true,
						 [replayUuid](CPlayerUserId uid, bool ok, std::string)
						 {
							 KZPlayer *player = ok ? g_pKZPlayerManager->ToPlayer(uid) : nullptr;
							 if (!player)
							 {
								 CybReplayDownload::ClearPending();
								 return;
							 }
							 KZ::replaysystem::commands::LoadReplay(player, replayUuid.c_str());
						 });
	}

	void OnResolveResponse(CPlayerUserId userID, HTTP::Response resp, CybReplayDownload::Kind kind, bool targetIsSelf, ResolveContext ctx)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);

		// Любой резолв начинается с чистого ожидания: привязка к uuid уже не даёт подобрать
		// чужое, но незачем и держать протухшее.
		CybReplayDownload::ClearPending();

		if (resp.status == 404)
		{
			// Отказ игроку логируем ВСЕГДА и с машинной причиной: до 17.09 промах резолва не
			// оставлял в логе сервера ни строки, и разбор жалобы «не запускается реплей»
			// начинался с гадания, дошёл ли запрос до api вообще.
			const std::string reason = ReadFailReason(resp);
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] resolve_denied type=%s %s reason=%s\n", ResolveTypeArg(kind),
						ctx.byUuid ? "by_uuid" : ctx.logKey.c_str(), reason.empty() ? "-" : reason.c_str());
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
				// `!replay <uuid>`: ключа курса/режима в запросе нет вовсе, и «не найдено»
				// здесь про САМ UUID. Частая причина — игрок взял со страницы карты UUID
				// РАНА: это другой идентификатор, записи под ним нет.
				if (ctx.byUuid)
				{
					player->languageService->PrintChat(true, false, "Replay - Uuid Not Found");
					return;
				}
				// У pro-видов 404 означает не только «рекорда нет», но и частый случай
				// «рекорд есть, а файла под него нет»: реплей пишется лишь на overall/NUB-ран,
				// и если pro-рекорд игрока — другой ран, отдавать нечего (см. заголовок).
				// Общая фраза про «повтор не найден» тут вводила бы в заблуждение.
				const bool isPro = kind == CybReplayDownload::Kind::PBPro || kind == CybReplayDownload::Kind::WRPro;
				if (isPro)
				{
					player->languageService->PrintChat(true, false, "Replay - Pro Not Saved");
					return;
				}
				// Причину api называет машинно — переводим её в фразу. Ключ (курс/режим) в
				// фразе обязателен: он и есть невидимый фильтр, из-за которого отказ
				// выглядел как «такого игрока нет».
				if (reason == "no_replay")
				{
					player->languageService->PrintChat(true, false, "Replay - Record Without File", ctx.courseText.c_str(), ctx.mode.c_str());
					return;
				}
				if (reason == "no_record")
				{
					player->languageService->PrintChat(true, false, "Replay - No Record Here", ctx.courseText.c_str(), ctx.mode.c_str());
					return;
				}
				if (reason == "map_unknown")
				{
					player->languageService->PrintChat(true, false, "Replay - Map Unknown");
					return;
				}
				player->languageService->PrintChat(true, false, "Replay - Central Not Found");
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

		// Вид запроса — бейджу карточки меню реплея (в файле записи его нет). Ставим ДО
		// докачки, как и ожидание AWR: путь загрузки общий и донести туда вид иначе нечем.
		CybReplayDownload::SetPendingKind(parsedUuid.ToString().c_str(), kind);

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
	const char *denyReason = "-";
	if (!PrepareResolve(player, fullUrl, token, key, &denyReason, true))
	{
		// Отказ ДО сети: центральные реплеи выключены на сервере либо режим не поддержан
		// центральным хранилищем. Раньше игрок и лог видели тут то же «повтор не найден»,
		// что и при промахе по ключу, — две разные проблемы под одной фразой.
		KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] resolve_skipped type=%s reason=%s\n", ResolveTypeArg(kind), denyReason);
		player->languageService->PrintChat(true, false,
										   KZ_STREQ(denyReason, "mode_unsupported") ? "Replay - Mode Unsupported" : "Replay - Central Disabled");
		return;
	}

	HTTP::Request req(HTTP::Method::GET, fullUrl);
	ApplyResolveQuery(req, kind, key, targetSteamId64, token);

	CPlayerUserId userID = player->GetClient()->GetUserID();
	// Фолбэк на локальный pro-PB возможен только для СВОЕГО реплея (см. OnResolveResponse).
	const bool targetIsSelf = targetSteamId64 != 0 && targetSteamId64 == player->GetSteamId64();

	const ResolveContext ctx = MakeContext(key);

	req.Send([userID, kind, targetIsSelf, ctx](HTTP::Response resp) { OnResolveResponse(userID, resp, kind, targetIsSelf, ctx); },
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

void CybReplayDownload::RequestFile(KZPlayer *player, Kind kind, u64 targetSteamId64, std::function<void(CPlayerUserId, std::string)> onReady)
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
		onReady(userID, "");
		return;
	}

	HTTP::Request req(HTTP::Method::GET, fullUrl);
	ApplyResolveQuery(req, kind, key, targetSteamId64, token);

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
				onReady(userID, "");
				return;
			}

			std::optional<std::string> bodyStr = resp.Body();
			if (!bodyStr.has_value())
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] file resolve 200 with no body\n");
				onReady(userID, "");
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
				onReady(userID, "");
				return;
			}

			FetchReplayFile(userID, parsedUuid.ToString(), downloadUrl, false,
							 [onReady](CPlayerUserId uid, bool ok, std::string path)
							 {
								 onReady(uid, ok ? std::move(path) : std::string());
							 });
		},
		[userID, onReady]()
		{
			KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] file resolve network error\n");
			onReady(userID, "");
		});
	// clang-format on
}

void CybReplayDownload::RequestInfo(KZPlayer *player, Kind kind, u64 targetSteamId64, std::function<void(CPlayerUserId, Info)> onDone)
{
	if (!player || !onDone)
	{
		return;
	}

	CPlayerUserId userID = player->GetClient()->GetUserID();

	std::string fullUrl, token;
	ResolveKey key;
	if (!PrepareResolve(player, fullUrl, token, key))
	{
		// Сети не было: центральные реплеи выключены либо режим/карта не поддержаны ключом.
		// Отличать это от сетевого отказа обязан вызывающий — фраза игроку тут другая.
		Info info;
		info.status = -1;
		onDone(userID, info);
		return;
	}

	HTTP::Request req(HTTP::Method::GET, fullUrl);
	ApplyResolveQuery(req, kind, key, targetSteamId64, token);

	// Актор строки лога. Снимается ЗДЕСЬ, а не в колбэке: к моменту ответа игрок мог уйти, и
	// тогда об отказе не осталось бы даже того, кому отказали. Аргумент false — по той же
	// причине: актор нужен и у неаутентифицированного игрока (канон форка).
	const u64 askerSteamId64 = player->GetSteamId64(false);
	const char *typeArg = ResolveTypeArg(kind);

	// clang-format off
	req.Send(
		[userID, askerSteamId64, typeArg, onDone](HTTP::Response resp)
		{
			Info info;
			// Статус НЕ понижается ни на одной ветке ниже: вызывающий может спрашивать только
			// его (уточняющий запрос `!awr` по wr), и подмена статуса из-за неожиданного тела
			// увела бы его в «не удалось» вместо верного ответа. Про тело говорит bodyUsable.
			info.status = (int)resp.status;
			if (resp.status < 200 || resp.status >= 300)
			{
				// 404 — штатное «такой записи нет»: что это значит, решает вызывающий (см. !awr),
				// и в лог оно не идёт — это не отказ нашей стороны. Всё остальное — отказ,
				// который увидит игрок, значит warn с машинной причиной и актором.
				if (resp.status != 404)
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] info_resolve_failed reason=http_status status=%u type=%s steam_id=%llu\n",
								(unsigned)resp.status, typeArg, (unsigned long long)askerSteamId64);
				}
				onDone(userID, info);
				return;
			}

			std::optional<std::string> bodyStr = resp.Body();
			if (!bodyStr.has_value())
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] info_resolve_failed reason=empty_body type=%s steam_id=%llu\n", typeArg,
							(unsigned long long)askerSteamId64);
				onDone(userID, info);
				return;
			}
			Json json(*bodyStr);
			if (!json.IsValid())
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] info_resolve_failed reason=invalid_json type=%s steam_id=%llu\n", typeArg,
							(unsigned long long)askerSteamId64);
				onDone(userID, info);
				return;
			}
			info.bodyUsable = true;

			// steamId64 api отдаёт СТРОКОЙ (u64 не влезает в число JSON без потерь) и кладёт его
			// в КАЖДЫЙ успешный ответ resolve. Отсутствие — расхождение с контрактом, но НЕ повод
			// объявлять весь ответ негодным: статус (и, если есть, awrMs) в нём по-прежнему
			// настоящие. Вызывающий сам решает, обязателен ли ему держатель.
			std::string steamIdStr;
			if (json.Get("steamId64", steamIdStr))
			{
				info.steamId64 = strtoull(steamIdStr.c_str(), nullptr, 10);
			}
			else
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] info_resolve_partial reason=no_steam_id type=%s steam_id=%llu\n", typeArg,
							(unsigned long long)askerSteamId64);
			}

			// `awrMs: null` — штатный ответ (разрез ещё не считали), поэтому ключ проверяем ТИХО:
			// Get на null пишет WARN, а тревожиться тут не о чем.
			if (json.HasValue("awrMs"))
			{
				f64 awrMs = 0.0;
				json.Get("awrMs", awrMs);
				info.awrMs = awrMs > 0.0 ? (u64)awrMs : 0;
			}
			onDone(userID, info);
		},
		[userID, askerSteamId64, typeArg, onDone]()
		{
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] info_resolve_failed reason=network type=%s steam_id=%llu\n", typeArg,
						(unsigned long long)askerSteamId64);
			Info info;
			// Ответа не было вовсе — это и есть ноль (см. заголовок): отличается от «пришёл 2xx
			// с нечитаемым телом», где статус настоящий, а негодно только тело.
			info.status = 0;
			onDone(userID, info);
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
	ResolveContext ctx;
	ctx.byUuid = true;

	req.Send([userID, ctx](HTTP::Response resp) { OnResolveResponse(userID, resp, CybReplayDownload::Kind::PB, false, ctx); },
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
