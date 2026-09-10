
#define NOMINMAX
#include "cs2kz.h"
#include "kz/kz.h"
#include "kz/jumpstats/kz_jumpstats.h"
#include "kz/language/kz_language.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
#include "kz/option/kz_option.h"
#include "kz/db/kz_db.h"
#include "kz/timer/kz_timer.h"
#include "kz/mappingapi/kz_mappingapi.h" // GetCourseByCourseID — сверка курса окна рана с шапкой
#include "commands.h"
#include "data.h"
#include "bot.h"
#include "events.h"
#include "playback.h"
#include "watcher.h"
#include "cyb_replay_download.h"
#include "cyb_replay_common.h" // MapMode/IsValidMapName — общий маппинг режима и валидация карты
#include "menu.h"
#include "utils/uuid.h"
#include "utils/simplecmds.h"
#include "utils/http.h"       // поиск реплея по нику через наш api
#include "tier1/keyvalues3.h" // разбор JSON-ответа поиска
#include "kz/global/kz_global.h"
#include "vendor/sql_mm/src/public/sql_mm.h"
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>
extern ReplayWatcher g_ReplayWatcher;

namespace KZ::replaysystem::commands
{

	void NavigateReplay(KZPlayer *player, u32 targetTick)
	{
		auto replay = data::GetCurrentReplay();

		// Сик внутрь записанной паузы приземляем на кадр возобновления (её интерьер —
		// застывшие кадры, в skip-режиме не показываются). Снап ДО reprocess, чтобы
		// таймер-состояние считалось для того же тика, что и позиция бота.
		targetTick = playback::SnapSeekTargetOutOfPause(targetTick);

		// Reset replay state and reprocess events up to target tick
		data::ResetReplayState(replay);
		events::ReprocessEventsUpToTick(replay, targetTick);

		// Set current tick
		replay->currentTick = targetTick;
		// Курсор пропуска пауз — на первый сегмент, который ещё впереди цели.
		playback::ResetPauseCursor(targetTick);

		// Apply the target tick's state immediately
		auto bot = bot::GetBot();
		if (bot)
		{
			KZPlayer *botPlayer = g_pKZPlayerManager->ToPlayer(bot);
			if (botPlayer)
			{
				TickData *tickData = &replay->tickData[targetTick];
				playback::ApplyTickState(botPlayer, tickData);
			}
		}
	}

	// `!replay <подстрока>` — поиск по НИКУ среди держателей PB-реплеев на текущей карте
	// (курс main) и режиме, через наш api. Зовётся, только когда локальный поиск по
	// подстроке UUID не дал НИ ОДНОГО совпадения (см. LoadReplay): прежний путь на этом
	// месте просто ругался «Invalid UUID», поэтому расширение никого не ломает.
	//
	// Результат по количеству совпадений: 0 — отказ, 1 — сразу грузим, >1 — меню выбора
	// (топ-10, порядок api = по PB по возрастанию). Пустую строку сюда не пускает сам
	// `!replay` (ArgC < 2 → Usage), это осталось как было.
	// Percent-encoding значения query-параметра. Нужен именно здесь: HTTP::Request::SetQuery
	// склеивает "key=value" в URL СЫРЫМИ (см. utils/http.cpp) — всем прежним вызывающим это
	// сходило с рук, потому что они передают заведомо безопасное (имя карты по вайтлисту
	// [a-z0-9_-], steamID цифрами, литералы режима). Здесь же значение — НИК, введённый
	// игроком: '&' дописал бы лишний параметр, '#' обрубил бы запрос, пробел/кириллица дали
	// бы битый URL. Общий SetQuery не трогаем — это поведение всего форка, отдельная задача.
	static_function std::string UrlEncodeQueryValue(const std::string &value)
	{
		static const char *hex = "0123456789ABCDEF";
		std::string out;
		out.reserve(value.size() * 3);
		for (unsigned char c : value)
		{
			// Unreserved по RFC 3986 — остальное кодируем, включая '~' (безобидно) и
			// весь не-ASCII (ники в UTF-8 приезжают побайтово и кодируются как есть).
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
			{
				out += (char)c;
			}
			else
			{
				out += '%';
				out += hex[c >> 4];
				out += hex[c & 0x0F];
			}
		}
		return out;
	}

	static_function void SearchReplaysByNickname(KZPlayer *player, const std::string &query)
	{
		const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
		if (!url || url[0] == '\0')
		{
			player->languageService->PrintChat(true, false, "Replay - Invalid UUID");
			return;
		}

		std::string mapName = g_pKZUtils->GetCurrentMapName().Get();
		if (!CybReplayCommon::IsValidMapName(mapName))
		{
			player->languageService->PrintChat(true, false, "Replay - Invalid UUID");
			return;
		}

		// Режим — в api-нотацию общим маппингом; кастовый режим сверх ckz/vnl/kzt
		// центральное хранилище не знает, PB-реплеев там нет по определению.
		const char *mode = CybReplayCommon::MapMode(player->modeService->GetModeShortName());
		if (!mode || mode[0] == '\0')
		{
			player->languageService->PrintChat(true, false, "Replay - Search No Matches", query.c_str());
			return;
		}

		std::string fullUrl = url;
		if (!fullUrl.empty() && fullUrl.back() == '/')
		{
			fullUrl.pop_back();
		}
		fullUrl += "/v1/kz/maps/" + mapName + "/replays/search";

		HTTP::Request req(HTTP::Method::GET, fullUrl);
		req.SetQuery("mode", mode);
		// Контракт ограничивает query 64 символами — режем на своей стороне, иначе длинный
		// ввод вернулся бы 400-м и игрок увидел бы «нет связи» вместо честного «не нашли».
		// Режем ДО кодирования: считать надо исходные символы, а не percent-триплеты.
		// Срез — по границе UTF-8: ники кириллицей двухбайтовые, и обрыв на середине символа
		// дал бы битую последовательность, на которой fastify отвечает 400 «URI malformed»
		// (то есть ровно тем отказом, который мы этим клампом и пытаемся предотвратить).
		std::string trimmedQuery = query;
		if (trimmedQuery.size() > 64)
		{
			size_t cut = 64;
			// Continuation-байты UTF-8 имеют вид 10xxxxxx — откатываемся к началу символа.
			while (cut > 0 && ((unsigned char)trimmedQuery[cut] & 0xC0) == 0x80)
			{
				cut--;
			}
			trimmedQuery.resize(cut);
		}
		req.SetQuery("query", UrlEncodeQueryValue(trimmedQuery));
		// limit по контракту 1..10; просим ровно тот максимум, который показываем.
		req.SetQuery("limit", "10");

		CPlayerUserId userID = player->GetClient()->GetUserID();
		u64 requesterSteamId64 = player->GetSteamId64();
		std::string requestedQuery = query;
		std::string requestedMap = mapName;

		// clang-format off
		req.Send(
			[userID, requesterSteamId64, requestedQuery, requestedMap](HTTP::Response resp)
			{
				KZPlayer *pl = g_pKZPlayerManager->ToPlayer(userID);
				if (!pl)
				{
					return; // игрок вышел за время round-trip
				}
				// Гард переиспользования userID (тот же, что у PB-фетча в kz_timer.cpp): слот мог
				// освободиться и достаться другому игроку, пока запрос летел. Без сверки ему
				// открылось бы чужое меню, а выбор в нём запустил бы реплей, которого он не просил.
				if (pl->GetSteamId64() != requesterSteamId64)
				{
					return;
				}
				// Карта сменилась, пока запрос летел — реплеи относятся к другой карте.
				if (!KZ_STREQ(requestedMap.c_str(), g_pKZUtils->GetCurrentMapName().Get()))
				{
					return;
				}
				if (resp.status < 200 || resp.status >= 300)
				{
					KZ_LOG_INFO(LogChannel::General, "[cyb_replay_search] HTTP %u for map=%s\n", (unsigned)resp.status, requestedMap.c_str());
					pl->languageService->PrintChat(true, false, "Replay - Search Unavailable");
					return;
				}
				std::optional<std::string> body = resp.Body();
				if (!body.has_value())
				{
					pl->languageService->PrintChat(true, false, "Replay - Search Unavailable");
					return;
				}

				KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
				CUtlString error = "";
				LoadKV3FromJSON(&kv, &error, body->c_str(), "");
				if (!error.IsEmpty())
				{
					KZ_LOG_WARN(LogChannel::General, "[cyb_replay_search] failed to parse api response: %s\n", error.Get());
					pl->languageService->PrintChat(true, false, "Replay - Search Unavailable");
					return;
				}

				KeyValues3 *results = kv.FindMember("results");
				if (!results || results->GetType() != KV3_TYPE_ARRAY)
				{
					pl->languageService->PrintChat(true, false, "Replay - Search No Matches", requestedQuery.c_str());
					return;
				}

				// Порядок элементов НЕ трогаем — api уже отдал по PB по возрастанию.
				std::vector<KZ::replaysystem::menu::SearchHit> hits;
				int count = results->GetArrayElementCount();
				for (int i = 0; i < count; i++)
				{
					KeyValues3 *entry = results->GetArrayElement(i);
					if (!entry)
					{
						continue;
					}
					KeyValues3 *uuidMember = entry->FindMember("replayUuid");
					KeyValues3 *nickMember = entry->FindMember("nickname");
					// Без UUID пункт бесполезен (грузить нечего), без ника — неотличим в меню.
					if (!uuidMember || uuidMember->GetType() != KV3_TYPE_STRING || !nickMember || nickMember->GetType() != KV3_TYPE_STRING)
					{
						continue;
					}
					KZ::replaysystem::menu::SearchHit hit;
					hit.nickname = nickMember->GetString("");
					hit.replayUuid = uuidMember->GetString("");
					KeyValues3 *pbMember = entry->FindMember("pbTimeMs");
					if (pbMember)
					{
						KV3Type_t t = pbMember->GetType();
						if (t == KV3_TYPE_INT || t == KV3_TYPE_UINT || t == KV3_TYPE_DOUBLE)
						{
							hit.pbTimeMs = (u64)pbMember->GetDouble(0.0);
						}
					}
					if (hit.replayUuid.empty())
					{
						continue;
					}
					hits.push_back(hit);
				}

				if (hits.empty())
				{
					pl->languageService->PrintChat(true, false, "Replay - Search No Matches", requestedQuery.c_str());
					return;
				}
				// Однозначное совпадение — не мучаем игрока меню из одного пункта. Если меню
				// показать не вышло (cs2menus не загружен), грузим первый элемент — он же
				// лучший PB: поведение не хуже прежнего безусловного отказа (паттерн !spec).
				if (hits.size() == 1 || !KZ::replaysystem::menu::OpenReplaySearchMenu(pl, hits))
				{
					LoadReplay(pl, hits[0].replayUuid.c_str());
				}
			},
			[userID]()
			{
				KZPlayer *pl = g_pKZPlayerManager->ToPlayer(userID);
				if (pl)
				{
					pl->languageService->PrintChat(true, false, "Replay - Search Unavailable");
				}
				KZ_LOG_INFO(LogChannel::General, "[cyb_replay_search] network error\n");
			});
		// clang-format on
	}

	void LoadReplay(KZPlayer *player, const char *uuid)
	{
		if (!player)
		{
			return;
		}

		// Check if already loading
		if (data::IsLoading())
		{
			// Ожидание AWR снимаем: этот вызов реплей НЕ загрузит, а протухший uuid
			// достался бы следующему `!replay <тот же uuid>` с диска — и обычный реплей
			// сыграл бы как AWR.
			CybReplayDownload::ClearPendingAwr();
			player->languageService->PrintChat(true, false, "Replay - Loading Already");
			return;
		}

		UUID_t parsedUuid;
		if (!UUID_t::FromString(uuid, &parsedUuid))
		{
			// Try to find replays matching the UUID substring
			auto matches = g_ReplayWatcher.FindReplaysByUUIDSubstring(uuid);
			if (matches.empty())
			{
				// Ни одного локального реплея с такой подстрокой UUID — трактуем ввод как НИК
				// и спрашиваем платформу (п.7 пакета 15.08). Раньше здесь был безусловный
				// отказ, так что этот путь ничего не отнимает: он либо найдёт игрока, либо
				// напечатает свой отказ. Ответ асинхронный — дальше по нему идёт либо
				// LoadReplay, либо меню выбора.
				SearchReplaysByNickname(player, uuid);
				return;
			}
			else if (matches.size() > 1)
			{
				player->languageService->PrintChat(true, false, "Replay - Multiple Matches", uuid);
				return;
			}
			// Exactly one match found
			parsedUuid = matches[0];
		}

		// Validate uuid format
		char replayPath[512];
		V_snprintf(replayPath, sizeof(replayPath), KZ_REPLAY_PATH "/%s.replay", parsedUuid.ToString().c_str());

		if (!g_pFullFileSystem->FileExists(replayPath))
		{
			// Also check the downloads directory.
			V_snprintf(replayPath, sizeof(replayPath), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", parsedUuid.ToString().c_str());
			if (!g_pFullFileSystem->FileExists(replayPath))
			{
				// Глобал у нас отключён (KZGlobalService::RequestReplay — мёртвый путь,
				// из-за него !replay <id> не работал) — качаем из центрального хранилища.
				CybReplayDownload::RequestAndPlayByUuid(player, parsedUuid.ToString().c_str());
				return;
			}
		}

		// Show loading message
		player->languageService->PrintChat(true, false, "Replay - Loading");

		// Get player user ID for thread-safe callback access
		CPlayerUserId playerUserID = player->GetClient()->GetUserID();
		// UUID именно этой загрузки — по нему колбэк забирает ожидание AWR (оно привязано к
		// uuid: `!replay <uuid>` с диска идёт сюда напрямую, мимо резолва).
		const std::string loadedUuid = parsedUuid.ToString();

		// Start async loading
		// clang-format off
		data::LoadReplayAsync(
			replayPath,
			// Success callback (runs on main thread via ProcessAsyncLoadCompletion)
			data::LoadSuccessCallback([playerUserID, loadedUuid]() {
				// AWR (`!replay awr`): вид записи доносит сюда одноразовое ожидание резолва —
				// путь загрузки общий для всех видов и донести его иначе нечем. Ожидание
				// привязано к uuid, поэтому чужой реплей его не подберёт; забираем всё равно
				// ПЕРВОЙ строкой, до любого раннего выхода (нет игрока, чужая карта), чтобы
				// оно не осталось висеть на неудавшейся загрузке.
				u64 pendingAwrMs = 0;
				const bool pendingAwr = CybReplayDownload::TakePendingAwr(loadedUuid.c_str(), pendingAwrMs);
				KZPlayer* player = g_pKZPlayerManager->ToPlayer(playerUserID);
				if (!player)
				{
					return;
				}
				if (!KZ_STREQI(data::GetCurrentReplay()->header.map().name().c_str(), g_pKZUtils->GetCurrentMapName().Get()))
				{
					player->languageService->PrintChat(true, false, "Replay - Wrong Map", data::GetCurrentReplay()->header.map().name().c_str(), g_pKZUtils->GetCurrentMapName().Get());
					return;
				}
				for (u32 i = 0; i < data::GetCurrentReplay()->numEvents; i++)
				{
					auto& event = data::GetCurrentReplay()->events[i];
					switch (event.type)
					{
						case RPEVENT_MODE_CHANGE:
						{
							if (KZ::mode::GetModeInfo(CUtlString(event.data.modeChange.name)).id < 0)
							{
								player->languageService->PrintChat(true, false, "Replay - Unknown Mode", event.data.modeChange.name);
							}
							break;
						}
						case RPEVENT_STYLE_CHANGE:
						{
							if (event.data.styleChange.name[0] != '\0'
								&& KZ::style::GetStyleInfo(CUtlString(event.data.styleChange.name)).id < 0)
							{
								player->languageService->PrintChat(true, false, "Replay - Unknown Style", event.data.styleChange.name);
							}
							break;
						}
					}
				}
				player->languageService->PrintChat(true, false, "Replay - Loaded Successfully");
				// Клиент не рендерит прицел ботов — подсказываем, как видеть свой.
				player->languageService->PrintChat(true, false, "Replay - Bot Crosshair Hint");

				// Initialize bot and start playback (safe to call on main thread)
				// The replay data is already stored in the global g_currentReplay
				auto replay = data::GetCurrentReplay();

				// Разрез считаем ЗДЕСЬ, до playback::StartReplay: он строит пропускаемые
				// сегменты уже с учётом awrMode/awrDead.
				replay->awrMode = pendingAwr;
				replay->awrMs = pendingAwrMs;
				if (replay->awrMode)
				{
					// Время рана — из шапки; у не-ранового реплея его нет, резать нечего.
					const bool isRun = replay->header.has_run() && replay->header.run().time() > 0.0f;
					// Сверка курса окна с шапкой. Делается ЗДЕСЬ, а не внутри ComputeCutFor:
					// в событиях курс — это id (`timer.index`), в шапке — имя, и разрешает id
					// в имя только реестр курсов главного потока (ComputeCutFor обязан
					// оставаться пригодным для рабочего потока бэкфилла). Окно уже выбрано по
					// последней паре START/END (см. RunWindowFromEvents), но если её курс не
					// тот, что в шапке, значит в окно попал не наш ран — не режем.
					bool courseOk = false;
					if (isRun)
					{
						u32 winStart = 0, winEnd = 0;
						i32 winCourseId = -1;
						if (playback::RunWindowFromEvents(replay->tickData, replay->tickCount, replay->events, replay->numEvents, winStart, winEnd,
														  winCourseId))
						{
							const KZCourseDescriptor *winCourse = KZ::course::GetCourseByCourseID(winCourseId);
							const std::string &headerCourse = replay->header.run().course_name();
							// Пустое имя в шапке (старые файлы) сверять нечем — довольствуемся
							// проверкой пары START/END внутри RunWindowFromEvents.
							courseOk = headerCourse.empty()
									   || (winCourse && !winCourse->GetName().IsEmpty()
										   && winCourse->GetName().IsEqual_FastCaseInsensitive(headerCourse.c_str()));
						}
					}
					awr::CutResult cut;
					if (!isRun)
					{
						cut.reason = "not_a_run";
					}
					else if (!courseOk)
					{
						cut.reason = "course_mismatch";
					}
					else
					{
						cut = playback::ComputeCutFor(replay->tickData, replay->tickCount, replay->events, replay->numEvents,
													  (u64)(replay->header.run().time() * 1000.0 + 0.5));
					}
					if (!cut.ok)
					{
						// Ложная сшивка хуже отказа (спека §4): играем как обычный реплей.
						KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] awr_cut_failed reason=%s uuid=%s\n", cut.reason,
									replay->uuid.ToString().c_str());
						replay->awrMode = false;
						replay->awrMs = 0;
						player->languageService->PrintChat(true, false, "Replay - AWR Cut Failed");
					}
					else
					{
						// Истина — файл; значение из api было только подписью до загрузки.
						// delete перед присваиванием — страховка: прошлый разрез уже снят
						// вместе с прошлыми кадрами (FreeReplayData в ProcessAsyncLoadCompletion).
						delete replay->awrDead;
						replay->awrDead = new std::vector<awr::Interval>(std::move(cut.dead));
						replay->awrMs = cut.awrMs;
					}
				}

				bot::InitializeBotForReplay(replay->header);
				playback::StartReplay();
				playback::InitializeWeapons();
				// Меню управления реплеем открывать здесь нечем: смену команды движок применит
				// позже, а panorama-меню (hud/layout/rpmenu.cpp) само открывается тиком худа, как
				// только игрок фактически наблюдает бота, и живёт, пока он его наблюдает.
				bot::SpectateBot(player);
			}),
			// Failure callback (runs on main thread via ProcessAsyncLoadCompletion)
			data::LoadFailureCallback([playerUserID](const char* error) {
				KZPlayer* player = g_pKZPlayerManager->ToPlayer(playerUserID);
				if (player)
				{
					player->languageService->PrintChat(true, false, error);
				}
			})
		);
		// clang-format on
	}

	void JumpToReplayTime(KZPlayer *player, const char *input)
	{
		if (!player)
		{
			return;
		}

		if (!data::IsReplayPlaying())
		{
			player->languageService->PrintChat(true, false, "Replay - No Replay Playing");
			return;
		}

		auto replay = data::GetCurrentReplay();
		// Вся арифметика — в ЭФФЕКТИВНОЙ шкале (без вырезанных пауз, см. playback.h): иначе «−10 с»
		// из кадра сразу после паузы приземлялось внутрь неё и снапом возвращалось на тот же кадр.
		const u32 effectiveCount = playback::EffectiveTickCount();
		u32 effectiveTarget;
		bool isRelative = (input[0] == '+' || input[0] == '-');

		if (isRelative)
		{
			// Relative time seeking
			f64 seekSeconds = -1.0;

			if (!utils::ParseTimeString(input + 1, &seekSeconds)) // Skip the +/- sign
			{
				player->languageService->PrintChat(true, false, "Replay - Invalid Relative Time");
				return;
			}

			if (input[0] == '-')
			{
				seekSeconds = -seekSeconds;
			}

			// Calculate target tick based on current replay position + seek amount
			i64 ticksToSeek = (i64)(seekSeconds / ENGINE_FIXED_TICK_INTERVAL);
			i64 newTarget = (i64)playback::RawTickToEffective(replay->currentTick) + ticksToSeek;

			// Clamp to valid range
			if (newTarget < 0)
			{
				effectiveTarget = 0;
			}
			else if (newTarget >= (i64)effectiveCount)
			{
				effectiveTarget = effectiveCount > 0 ? effectiveCount - 1 : 0;
			}
			else
			{
				effectiveTarget = (u32)newTarget;
			}
		}
		else
		{
			// Absolute time navigation
			f64 targetSeconds = -1.0f;
			if (!utils::ParseTimeString(input, &targetSeconds))
			{
				player->languageService->PrintChat(true, false, "Replay - Invalid Absolute Time");
				return;
			}
			effectiveTarget = (u32)(targetSeconds / ENGINE_FIXED_TICK_INTERVAL);
		}

		char time[32];
		utils::FormatTime(effectiveTarget * ENGINE_FIXED_TICK_INTERVAL, time, sizeof(time), false);
		char maxTime[32];
		utils::FormatTime((effectiveCount > 0 ? effectiveCount - 1 : 0) * ENGINE_FIXED_TICK_INTERVAL, maxTime, sizeof(maxTime), false);
		if (effectiveTarget >= effectiveCount)
		{
			player->languageService->PrintChat(true, false, "Replay - Time Out Of Range", time, maxTime);
			return;
		}

		const u32 targetTick = playback::EffectiveTickToRaw(effectiveTarget);
		NavigateReplay(player, targetTick);

		if (isRelative)
		{
			f64 seekSeconds = -1.0f;
			utils::ParseTimeString(input + 1, &seekSeconds);
			if (input[0] == '-')
			{
				seekSeconds = -seekSeconds;
			}
			player->languageService->PrintChat(true, false, "Replay - Seeked To Tick", seekSeconds, effectiveTarget, time);
		}
		else
		{
			player->languageService->PrintChat(true, false, "Replay - Jumped To Tick", targetTick, time);
		}
	}

	void JumpToReplayTick(KZPlayer *player, const char *input)
	{
		if (!player)
		{
			return;
		}

		if (!data::IsReplayPlaying())
		{
			player->languageService->PrintChat(true, false, "Replay - No Replay Playing");
			return;
		}

		auto replay = data::GetCurrentReplay();
		// Номера тиков здесь — ЭФФЕКТИВНЫЕ (без вырезанных пауз), те же, что печатает !rpinfo; в сырой
		// индекс переводим только перед NavigateReplay (см. playback.h).
		const u32 effectiveCount = playback::EffectiveTickCount();
		u32 targetTick;
		bool isRelative = (input[0] == '+' || input[0] == '-');

		if (isRelative)
		{
			// Relative tick seeking
			char *endPtr;
			long tickOffset = strtol(input + 1, &endPtr, 10); // Skip the +/- sign
			if (*endPtr != '\0' || tickOffset < 0)
			{
				player->languageService->PrintChat(true, false, "Replay - Invalid Tick Number");
				return;
			}

			if (input[0] == '-')
			{
				tickOffset = -tickOffset;
			}

			// Calculate target tick based on current replay position + tick offset
			i64 newTargetTick = (i64)playback::RawTickToEffective(replay->currentTick) + (i64)tickOffset;

			// Clamp to valid range
			if (newTargetTick < 0)
			{
				targetTick = 0;
			}
			else if (newTargetTick >= (i64)effectiveCount)
			{
				targetTick = effectiveCount > 0 ? effectiveCount - 1 : 0;
			}
			else
			{
				targetTick = (u32)newTargetTick;
			}
		}
		else
		{
			// Absolute tick navigation
			char *endPtr;
			long tickValue = strtol(input, &endPtr, 10);
			if (*endPtr != '\0' || tickValue < 0)
			{
				player->languageService->PrintChat(true, false, "Replay - Invalid Tick Number");
				return;
			}
			targetTick = (u32)tickValue;
		}

		if (targetTick >= effectiveCount)
		{
			player->languageService->PrintChat(true, false, "Replay - Tick Out Of Range", targetTick, effectiveCount > 0 ? effectiveCount - 1 : 0);
			return;
		}

		NavigateReplay(player, playback::EffectiveTickToRaw(targetTick));
		char time[32];
		utils::FormatTime(targetTick * ENGINE_FIXED_TICK_INTERVAL, time, sizeof(time), false);
		player->languageService->PrintChat(true, false, "Replay - Jumped To Tick", targetTick, time);
	}

	void GetReplayInfo(KZPlayer *player)
	{
		if (!player)
		{
			return;
		}

		if (!data::IsReplayPlaying())
		{
			player->languageService->PrintChat(true, false, "Replay - No Replay Playing");
			return;
		}

		auto replay = data::GetCurrentReplay();

		// Позиция/длительность — в эффективной шкале без вырезанных пауз (см. playback.h).
		const u32 effectiveTick = playback::RawTickToEffective(replay->currentTick);
		const u32 effectiveLast = playback::EffectiveTickCount() > 0 ? playback::EffectiveTickCount() - 1 : 0;
		char timeStr[64], maxTime[64];
		utils::FormatTime(effectiveTick * ENGINE_FIXED_TICK_INTERVAL, timeStr, sizeof(timeStr), false);
		utils::FormatTime(effectiveLast * ENGINE_FIXED_TICK_INTERVAL, maxTime, sizeof(maxTime), false);
		char timestamp[64];
		time_t time = replay->header.timestamp();
		strftime(timestamp, 64, "%Y-%m-%d %H:%M:%S", localtime(&time));
		player->languageService->PrintChat(true, false, "Replay - Current Info", effectiveTick, effectiveLast, timeStr, maxTime);
		if (replay->awrMode)
		{
			// Подпись AWR: время рана БЕЗ вырезанных петель (посчитано по самому файлу).
			// Точность как у соседних времён этой же строки (!rpinfo) — до десятых.
			CUtlString awrTime = utils::FormatTime((f64)replay->awrMs / 1000.0, false);
			player->languageService->PrintChat(true, false, "Replay - AWR Label", awrTime.Get());
		}
		player->languageService->PrintConsole(false, false, "Replay - General Info Console", replay->uuid.ToString().c_str(),
											  replay->header.player().name().c_str(), replay->header.player().steamid64(), timestamp,
											  replay->header.server_version(), replay->header.plugin_version());
		switch (static_cast<ReplayType>(replay->header.type()))
		{
			case ReplayType::RP_CHEATER:
			{
				if (replay->header.has_cheater())
				{
					player->languageService->PrintConsole(false, false, "Replay - Cheater Info Console", replay->header.cheater().reason().c_str());
				}
				break;
			}
			case ReplayType::RP_RUN:
			{
				if (replay->header.has_run())
				{
					char modeStr[128];
					auto &run = replay->header.run();
					i32 styleCount = run.styles_size();
					V_snprintf(modeStr, sizeof(modeStr), "%s%s", run.mode().name().c_str(), styleCount ? "*" : "");
					CUtlString timeString = utils::FormatTime(run.time());
					player->languageService->PrintConsole(false, false, "Replay - Run Info Console", run.course_name().c_str(), modeStr,
														  timeString.Get(), run.num_teleports());
				}
				break;
			}
			case ReplayType::RP_JUMPSTATS:
			{
				if (replay->header.has_jump())
				{
					auto &jump = replay->header.jump();
					u8 jt = static_cast<u8>(jump.jump_type());
					if (jump.block_distance() <= 0)
					{
						player->languageService->PrintConsole(false, false, "Replay - Jump Info Console", jumpTypeStr[jt], jump.distance(),
															  jump.sync() * 100.0f, jump.pre(), jump.max(), jump.air_time());
					}
					else
					{
						player->languageService->PrintConsole(false, false, "Replay - Jump Info Console", jumpTypeStr[jt], jump.distance(),
															  jump.block_distance(), jump.sync() * 100.0f, jump.pre(), jump.max(), jump.air_time());
					}
				}
				break;
			}
			case ReplayType::RP_MANUAL:
			{
				if (replay->header.has_manual())
				{
					auto &manual = replay->header.manual();
					if (manual.has_saved_by())
					{
						player->languageService->PrintConsole(false, false, "Replay - Manual Info Console", manual.saved_by().name().c_str(),
															  manual.saved_by().steamid64());
					}
				}
				break;
			}
		}
	}

	void ToggleReplayPause(KZPlayer *player)
	{
		if (!player)
		{
			return;
		}

		if (!data::IsReplayPlaying())
		{
			player->languageService->PrintChat(true, false, "Replay - No Replay Playing");
			return;
		}

		auto replay = data::GetCurrentReplay();

		// Toggle pause state
		replay->replayPaused = !replay->replayPaused;

		if (replay->replayPaused)
		{
			player->languageService->PrintChat(true, false, "Replay - Paused");
		}
		else
		{
			player->languageService->PrintChat(true, false, "Replay - Resumed");
		}
	}

	// Формат скорости для чата и меню: 0.25, 1, 1.75 — без хвостовых нулей, потому что
	// «1.00» в строке меню читается хуже, чем «1». Точность 3 знака после запятой:
	// %.2g округлял бы 1.75 до «1.8», то есть показывал НЕ то, что применено.
	// Суффикс «x» дописывают фразы перевода, а не эта функция.
	void FormatReplaySpeed(f32 speed, char *out, size_t size)
	{
		V_snprintf(out, (int)size, "%.3f", speed);
		char *dot = strchr(out, '.');
		if (!dot)
		{
			return;
		}
		char *end = out + strlen(out) - 1;
		while (end > dot && *end == '0')
		{
			*end-- = '\0';
		}
		if (end == dot)
		{
			*dot = '\0';
		}
	}

	f32 GetReplaySpeed()
	{
		return data::IsReplayPlaying() ? data::GetCurrentReplay()->playbackSpeed : 1.0f;
	}

	void SetReplaySpeed(KZPlayer *player, f32 speed, bool announce)
	{
		if (!player)
		{
			return;
		}

		if (!data::IsReplayPlaying())
		{
			player->languageService->PrintChat(true, false, "Replay - No Replay Playing");
			return;
		}

		speed = (std::min)((std::max)(speed, data::KZ_REPLAY_SPEED_MIN), data::KZ_REPLAY_SPEED_MAX);

		auto replay = data::GetCurrentReplay();
		replay->playbackSpeed = speed;

		if (announce)
		{
			char speedText[16];
			FormatReplaySpeed(speed, speedText, sizeof(speedText));
			player->languageService->PrintChat(true, false, "Replay - Speed Set", speedText);
		}
	}

	void StepReplay(KZPlayer *player, i32 frames, bool announce)
	{
		if (!player)
		{
			return;
		}

		if (!data::IsReplayPlaying())
		{
			player->languageService->PrintChat(true, false, "Replay - No Replay Playing");
			return;
		}

		auto replay = data::GetCurrentReplay();
		if (replay->tickCount == 0)
		{
			// Индексировать нечего. Остальные сик-пути отсекают это проверкой
			// targetTick >= tickCount, здесь цель считается от currentTick — гард свой.
			return;
		}

		// Шаг по тикам имеет смысл только на стопкадре — иначе следующий же тик
		// плейбека затрёт результат. Не в паузе — встаём в неё сами, это и есть намерение.
		// О самом факте остановки сообщаем ВСЕГДА, даже при announce=false: первое A/D по
		// строке паузы в !rpmenu иначе молча замораживает реплей, и это читается как «завис».
		if (!replay->replayPaused)
		{
			player->languageService->PrintChat(true, false, "Replay - Paused");
		}
		// Арифметика в i64: frames приходит из чата, i32-сложение переполнялось бы (UB). Шаг — в
		// эффективной шкале: шаг назад с кадра после паузы уходит на кадр ДО неё, а не снапается
		// обратно (см. playback.h).
		const i64 effectiveLast = (i64)playback::EffectiveTickCount() - 1;
		i64 target = (i64)playback::RawTickToEffective(replay->currentTick) + (i64)frames;
		target = (std::min)((std::max)(target, (i64)0), (std::max)(effectiveLast, (i64)0));

		NavigateReplay(player, playback::EffectiveTickToRaw((u32)target));
		// NavigateReplay проходит через ResetReplayState, а тот снимает паузу — для сика
		// это верно (зритель перематывает и смотрит дальше), для шага нет: стопкадр обязан
		// остаться стопкадром. Возвращаем паузу после навигации, а не до.
		replay->replayPaused = true;

		if (announce)
		{
			char time[32];
			const u32 effectiveTick = playback::RawTickToEffective(replay->currentTick);
			utils::FormatTime(effectiveTick * ENGINE_FIXED_TICK_INTERVAL, time, sizeof(time), false);
			player->languageService->PrintChat(true, false, "Replay - Stepped", frames, effectiveTick, time);
		}
	}

	void StopReplay(KZPlayer *player)
	{
		if (!data::IsReplayPlaying())
		{
			if (player)
			{
				player->languageService->PrintChat(true, false, "Replay - No Replay Playing");
			}
			return;
		}

		// Та же последовательность, что при естественном конце плейбека
		// (playback.cpp: KickBot + снять флаг playingReplay). Данные реплея не
		// выгружаем — его можно запустить снова.
		bot::KickBot();
		data::GetCurrentReplay()->playingReplay = false;
		playback::ClearPauseSegments();

		if (player)
		{
			player->languageService->PrintChat(true, false, "Replay - Ended");
		}
	}

	void CheckReplayLoadProgress(KZPlayer *player)
	{
		if (!player)
		{
			return;
		}

		auto status = data::GetLoadStatus();

		switch (status->state.load())
		{
			case data::LoadingState::Idle:
				player->languageService->PrintChat(true, false, "Replay - No Loading Progress");
				break;

			case data::LoadingState::Loading:
			{
				float progress = status->progress.load() * 100.0f;
				player->languageService->PrintChat(true, false, "Replay - Loading Progress", progress);
				break;
			}

			case data::LoadingState::Completed:
				player->languageService->PrintChat(true, false, "Replay - Loading Completed");
				break;

			case data::LoadingState::Failed:
			{
				std::lock_guard<std::mutex> lock(status->errorMutex);
				player->languageService->PrintChat(true, false, status->errorMessage.c_str());
				break;
			}
		}
	}

	void CancelReplayLoad(KZPlayer *player)
	{
		if (!player)
		{
			return;
		}

		if (!data::IsLoading())
		{
			player->languageService->PrintChat(true, false, "Replay - No Loading Progress");
			return;
		}

		data::CancelAsyncLoad();
		player->languageService->PrintChat(true, false, "Replay - Loading Cancelled");
	}

	void ListReplays(KZPlayer *player, const char *filter)
	{
		if (!player)
		{
			return;
		}
		if (!filter || filter[0] == '\0')
		{
			g_ReplayWatcher.PrintUsage(player);
		}
		g_ReplayWatcher.FindReplaysMatchingCriteria(filter, player);
	}

	void ToggleLegsVisibility(KZPlayer *player)
	{
		if (!player)
		{
			return;
		}
		if (!data::IsReplayPlaying())
		{
			player->languageService->PrintChat(true, false, "Replay - No Replay Playing");
			return;
		}
		bot::GetBotPlayer()->ToggleHideLegs();
		if (bot::GetBotPlayer()->optionService->GetPreferenceBool("hideLegs"))
		{
			player->languageService->PrintChat(true, false, "Replay - Hide Player Legs - Enable");
		}
		else
		{
			player->languageService->PrintChat(true, false, "Replay - Hide Player Legs - Disable");
		}
	}

	static void PlayReplayByID(KZPlayer *player, const char *idStr)
	{
		UUID_t uuid;
		if (!UUID_t::FromString(idStr, &uuid))
		{
			player->languageService->PrintChat(true, false, "Replay - Not Found");
			return;
		}
		char replayPath[512];
		V_snprintf(replayPath, sizeof(replayPath), KZ_REPLAY_PATH "/%s.replay", uuid.ToString().c_str());
		if (g_pFullFileSystem->FileExists(replayPath))
		{
			LoadReplay(player, uuid.ToString().c_str());
			return;
		}

		char downloadPath[512];
		V_snprintf(downloadPath, sizeof(downloadPath), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", uuid.ToString().c_str());
		if (g_pFullFileSystem->FileExists(downloadPath))
		{
			LoadReplay(player, uuid.ToString().c_str());
			return;
		}

		if (KZGlobalService::IsAvailable())
		{
			KZGlobalService::RequestReplay(player, uuid);
			return;
		}

		player->languageService->PrintChat(true, false, "Replay - Not Found");
	}

	struct RecordContext
	{
		CPlayerUserId playerUserID;
		CUtlString mapName;
		CUtlString courseName;
		KZ::api::Mode apiMode;
		u32 localModeID;

		RecordContext(CPlayerUserId uid, CUtlString map, CUtlString course, KZ::api::Mode mode, u32 modeID)
			: playerUserID(uid), mapName(std::move(map)), courseName(std::move(course)), apiMode(mode), localModeID(modeID)
		{
		}
	};

	static void LoadWRReplay(KZPlayer *player, bool isPro, const RecordContext &ctx)
	{
		if (!KZGlobalService::IsAvailable())
		{
			player->languageService->PrintChat(true, false, "Replay - API Not Available");
			return;
		}
		player->languageService->PrintChat(true, false, "Replay - Querying WR");
		// clang-format off
		KZGlobalService::MessageCallback<KZ::api::messages::WorldRecords> callback(
			[userID = ctx.playerUserID, isPro](const KZ::api::messages::WorldRecords &records)
			{
				KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
				if (!p) return;
				const auto &record = isPro ? records.pro : records.overall;
				if (!record.has_value())
				{
					p->languageService->PrintChat(true, false, "Replay - WR Not Found");
					return;
				}
				PlayReplayByID(p, record->id.c_str());
			});
		callback.OnError([userID = ctx.playerUserID](const KZ::api::messages::Error &)
		{
			KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
			if (p) p->languageService->PrintChat(true, false, "Replay - WR Not Found");
		});
		callback.OnCancelled([userID = ctx.playerUserID](KZGlobalService::MessageCallbackCancelReason)
		{
			KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
			if (p) p->languageService->PrintChat(true, false, "Replay - WR Not Found");
		});
		// clang-format on
		KZGlobalService::QueryWorldRecords(std::string_view(ctx.mapName.Get(), ctx.mapName.Length()),
										   std::string_view(ctx.courseName.Get(), ctx.courseName.Length()), ctx.apiMode, std::move(callback));
	}

	static void LoadSRReplay(KZPlayer *player, bool isPro, const RecordContext &ctx)
	{
		// clang-format off
		auto onSuccess = [userID = ctx.playerUserID, isPro](std::vector<ISQLQuery *> queries)
		{
			KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
			if (!p) return;
			ISQLResult *result = (isPro ? queries[1] : queries[0])->GetResultSet();
			if (result && result->GetRowCount() > 0 && result->FetchRow())
				PlayReplayByID(p, result->GetString(0));
			else
				p->languageService->PrintChat(true, false, "Replay - SR Not Found");
		};
		auto onFailure = [userID = ctx.playerUserID](std::string, int)
		{
			KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
			if (p) p->languageService->PrintChat(true, false, "Replay - SR Not Found");
		};
		// clang-format on
		KZDatabaseService::QueryRecords(ctx.mapName, ctx.courseName, ctx.localModeID, 1, 0, onSuccess, onFailure);
	}

	static void LoadGPBReplay(KZPlayer *player, bool isPro, const RecordContext &ctx)
	{
		if (!KZGlobalService::IsAvailable())
		{
			player->languageService->PrintChat(true, false, "Replay - API Not Available");
			return;
		}
		player->languageService->PrintChat(true, false, "Replay - Querying GPB");
		KZGlobalService::QueryPBParams params;
		params.player = KZGlobalService::PlayerIdentifier::SteamID(player->GetSteamId64());
		params.map = std::string_view(ctx.mapName.Get(), ctx.mapName.Length());
		params.course = std::string_view(ctx.courseName.Get(), ctx.courseName.Length());
		params.mode = ctx.apiMode;
		// clang-format off
		KZGlobalService::MessageCallback<KZ::api::messages::PersonalBest> callback(
			[userID = ctx.playerUserID, isPro](const KZ::api::messages::PersonalBest &pb)
			{
				KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
				if (!p) return;
				const auto &record = isPro ? pb.pro : pb.overall;
				if (!record.has_value())
				{
					p->languageService->PrintChat(true, false, "Replay - GPB Not Found");
					return;
				}
				PlayReplayByID(p, record->id.c_str());
			});
		callback.OnError([userID = ctx.playerUserID](const KZ::api::messages::Error &)
		{
			KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
			if (p) p->languageService->PrintChat(true, false, "Replay - GPB Not Found");
		});
		callback.OnCancelled([userID = ctx.playerUserID](KZGlobalService::MessageCallbackCancelReason)
		{
			KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
			if (p) p->languageService->PrintChat(true, false, "Replay - GPB Not Found");
		});
		// clang-format on
		KZGlobalService::QueryPB(params, std::move(callback));
	}

	static void LoadSPBReplay(KZPlayer *player, bool isPro, const RecordContext &ctx)
	{
		u64 steamID64 = player->GetSteamId64();
		// clang-format off
		auto onSuccess = [userID = ctx.playerUserID, isPro](std::vector<ISQLQuery *> queries)
		{
			KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
			if (!p) return;
			// queries[0] = sql_getpb (ID col 0), queries[1] = sql_getpbpro (ID col 0)
			ISQLResult *result = (isPro ? queries[1] : queries[0])->GetResultSet();
			if (result && result->GetRowCount() > 0 && result->FetchRow())
				PlayReplayByID(p, result->GetString(0));
			else
				p->languageService->PrintChat(true, false, "Replay - SPB Not Found");
		};
		auto onFailure = [userID = ctx.playerUserID](std::string, int)
		{
			KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
			if (p) p->languageService->PrintChat(true, false, "Replay - SPB Not Found");
		};
		// clang-format on
		KZDatabaseService::QueryPBRankless(steamID64, ctx.mapName, ctx.courseName, ctx.localModeID, 0, onSuccess, onFailure);
	}

	static void LoadPBReplay(KZPlayer *player, bool isPro, bool isGlobalMode, const RecordContext &ctx)
	{
		if (!isGlobalMode || !KZGlobalService::IsAvailable())
		{
			LoadSPBReplay(player, isPro, ctx);
			return;
		}
		player->languageService->PrintChat(true, false, "Replay - Querying GPB");
		KZGlobalService::QueryPBParams params;
		params.player = KZGlobalService::PlayerIdentifier::SteamID(player->GetSteamId64());
		params.map = std::string_view(ctx.mapName.Get(), ctx.mapName.Length());
		params.course = std::string_view(ctx.courseName.Get(), ctx.courseName.Length());
		params.mode = ctx.apiMode;
		// clang-format off
		KZGlobalService::MessageCallback<KZ::api::messages::PersonalBest> callback(
			[userID = ctx.playerUserID, isPro, mapName = ctx.mapName, courseName = ctx.courseName, localModeID = ctx.localModeID]
			(const KZ::api::messages::PersonalBest &pb)
			{
				KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
				if (!p) return;
				const auto &record = isPro ? pb.pro : pb.overall;
				if (record.has_value())
				{
					PlayReplayByID(p, record->id.c_str());
					return;
				}
				// GPB not found — fall back to server PB
				RecordContext spbCtx(userID, mapName, courseName, {}, localModeID);
				LoadSPBReplay(p, isPro, spbCtx);
			});
		auto spbFallback = [userID = ctx.playerUserID, isPro, mapName = ctx.mapName, courseName = ctx.courseName, localModeID = ctx.localModeID]()
		{
			KZPlayer *p = g_pKZPlayerManager->ToPlayer(userID);
			if (!p) return;
			RecordContext spbCtx(userID, mapName, courseName, {}, localModeID);
			LoadSPBReplay(p, isPro, spbCtx);
		};
		callback.OnError([spbFallback](const KZ::api::messages::Error &) { spbFallback(); });
		callback.OnCancelled([spbFallback](KZGlobalService::MessageCallbackCancelReason) { spbFallback(); });
		// clang-format on
		KZGlobalService::QueryPB(params, std::move(callback));
	}

	// Ищет ПОДКЛЮЧЁННОГО игрока по подстроке ника (без учёта регистра, первое
	// совпадение — как kz_playercheck в kz/misc/kz_misc.cpp). Без обращения к БД:
	// `!replay pb <ник>` работает только для игроков, которые прямо сейчас на
	// сервере (не резолвит офлайн-игроков по нику — для них нужен steamid64).
	static_function KZPlayer *FindOnlinePlayerByName(const char *nameQuery)
	{
		if (!nameQuery || nameQuery[0] == '\0')
		{
			return nullptr;
		}

		char needle[256];
		V_strncpy(needle, nameQuery, sizeof(needle));
		V_strlower(needle);

		for (i32 i = 0; i <= MAXPLAYERS; i++)
		{
			CBasePlayerController *controller = g_pKZPlayerManager->players[i]->GetController();
			if (!controller)
			{
				continue;
			}
			// Боты/SourceTV и не-аутентифицированные (SteamID64==0) не годятся в цель PB —
			// иначе матч по подстроке уйдёт в api со steamId64=0 и вернёт невнятный отказ.
			if (g_pKZPlayerManager->players[i]->IsFakeClient() || g_pKZPlayerManager->players[i]->GetSteamId64() == 0)
			{
				continue;
			}

			char haystack[256];
			V_strncpy(haystack, g_pKZPlayerManager->players[i]->GetName(), sizeof(haystack));
			V_strlower(haystack);

			if (V_strstr(haystack, needle))
			{
				return g_pKZPlayerManager->ToPlayer(i);
			}
		}

		return nullptr;
	}

	// Строгий парсинг steamid64: ровно 17 цифр (api валидирует так же —
	// apps/api/src/modules/replays/replays.controller.ts, steamId64Schema). Не
	// делаем более "умную" эвристику намеренно: цель — быстро отличить явный
	// steamid64-аргумент от опечатки в нике, а не принять любую цифровую строку.
	static_function bool ParseStrictSteamId64(const char *arg, u64 &out)
	{
		if (!arg || arg[0] == '\0')
		{
			return false;
		}

		size_t len = strlen(arg);
		if (len != 17)
		{
			return false;
		}

		for (size_t i = 0; i < len; i++)
		{
			if (!isdigit((unsigned char)arg[i]))
			{
				return false;
			}
		}

		out = strtoull(arg, nullptr, 10);
		return true;
	}

	void LoadReplayForRecord(KZPlayer *player, RecordType type, const char *courseArg, const char *modeArg)
	{
		if (!player)
		{
			return;
		}

		const KZCourseDescriptor *course = nullptr;
		if (courseArg && courseArg[0] != '\0')
		{
			course = KZ::course::GetCourse(courseArg, false, true);
		}
		if (!course)
		{
			course = player->timerService->GetCourse();
		}
		if (!course)
		{
			course = KZ::course::GetFirstCourse();
		}
		if (!course)
		{
			player->languageService->PrintChat(true, false, "Replay - No Course");
			return;
		}

		KZModeManager::ModePluginInfo modeInfo;
		if (modeArg && modeArg[0] != '\0')
		{
			modeInfo = KZ::mode::GetModeInfo(CUtlString(modeArg));
		}
		else
		{
			modeInfo = KZ::mode::GetModeInfo(player->modeService);
		}
		if (modeInfo.id == -2)
		{
			player->languageService->PrintChat(true, false, "Replay - Invalid Mode Arg");
			return;
		}

		KZ::api::Mode apiMode {};
		bool isGlobalMode = KZ::api::DecodeModeString(std::string_view(modeInfo.shortModeName.Get(), modeInfo.shortModeName.Length()), apiMode);

		if (!isGlobalMode && (type == RecordType::WR || type == RecordType::WRPro || type == RecordType::GPB || type == RecordType::GPBPro))
		{
			player->languageService->PrintChat(true, false, "Replay - Global Mode Only");
			return;
		}

		RecordContext ctx(player->GetClient()->GetUserID(), g_pKZUtils->GetCurrentMapName(), course->GetName(), apiMode, (u32)modeInfo.databaseID);

		switch (type)
		{
				// clang-format off
			case RecordType::WR:     LoadWRReplay(player, false, ctx); break;
			case RecordType::WRPro:  LoadWRReplay(player, true,  ctx); break;
			case RecordType::SR:     LoadSRReplay(player, false, ctx); break;
			case RecordType::SRPro:  LoadSRReplay(player, true,  ctx); break;
			case RecordType::GPB:    LoadGPBReplay(player, false, ctx); break;
			case RecordType::GPBPro: LoadGPBReplay(player, true,  ctx); break;
			case RecordType::SPB:    LoadSPBReplay(player, false, ctx); break;
			case RecordType::SPBPro: LoadSPBReplay(player, true,  ctx); break;
			// clang-format on
			case RecordType::PB:
			case RecordType::PBPro:
			{
				bool isPro = (type == RecordType::PBPro);
				LoadPBReplay(player, isPro, isGlobalMode, ctx);
				break;
			}
		}
	}
} // namespace KZ::replaysystem::commands

SCMD(kz_replay, SCFL_REPLAY | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!g_pFullFileSystem || !player)
	{
		return MRES_SUPERCEDE;
	}

	if (args->ArgC() < 2)
	{
		player->languageService->PrintChat(true, false, "Replay - Usage Command");
		return MRES_SUPERCEDE;
	}

	using namespace KZ::replaysystem::commands;
	using RT = RecordType;

	const char *arg1 = args->Arg(1);

	// Cyber-платформа: центральное (кросс-серверное) хранилище через api, ПОДМЕНЯЕТ
	// upstream-обработку шести ключевых слов — `pb`, `wr`, `wrpro`, `pbpro`, `gpb`,
	// `gpbpro` (upstream роутил их в глобальный cs2kz-api, недоступный этой сети и не
	// работающий для кастомных режимов вроде kzt, см. refs/cs2kz-api в CLAUDE.md).
	// `gpb`/`gpbpro` схлопнуты в наши PB/PBPro: отдельного глобального хранилища у нас
	// нет, наша сеть и есть источник. Апстримными остались серверные виды из локальной
	// БД плагина — sr/srpro/spb/spbpro, см. recordKeywords ниже.
	// Ключ резолва — ТЕКУЩИЙ курс/режим игрока (без аргументов курса/режима, в отличие
	// от upstream-варианта): это и есть mode-гейт спеки.
	// Личные виды (свой/чужой PB) — с разбором необязательной цели вторым аргументом;
	// `gpb`/`gpbpro` тоже сюда: «глобальный PB» у нас и есть PB нашей сети, отдельного
	// глобального хранилища нет. Ветка одна на все четыре, чтобы разбор цели не разъехался.
	const bool isPbKind = KZ_STREQI(arg1, "pb") || KZ_STREQI(arg1, "gpb");
	const bool isPbProKind = KZ_STREQI(arg1, "pbpro") || KZ_STREQI(arg1, "gpbpro");
	if (isPbKind || isPbProKind)
	{
		u64 targetSteamId64 = player->GetSteamId64();
		// До завершения Steam-auth свой steamid == 0 — честный отказ вместо api-400.
		if (targetSteamId64 == 0 && args->ArgC() < 3)
		{
			player->languageService->PrintChat(true, false, "Error Message (Player Not Found)", player->GetName());
			return MRES_SUPERCEDE;
		}
		if (args->ArgC() >= 3)
		{
			const char *targetArg = args->Arg(2);
			KZPlayer *target = FindOnlinePlayerByName(targetArg);
			if (target)
			{
				targetSteamId64 = target->GetSteamId64();
			}
			else if (!ParseStrictSteamId64(targetArg, targetSteamId64))
			{
				player->languageService->PrintChat(true, false, "Error Message (Player Not Found)", targetArg);
				return MRES_SUPERCEDE;
			}
		}
		CybReplayDownload::RequestAndPlay(player, isPbProKind ? CybReplayDownload::Kind::PBPro : CybReplayDownload::Kind::PB, targetSteamId64);
		return MRES_SUPERCEDE;
	}
	// AWR — рекорд сети ПОСЛЕ вырезки телепорт-петель. Аргументов курса/режима в v1 нет:
	// ключ, как у `wr`, — текущий курс и режим игрока.
	if (KZ_STREQI(arg1, "awr"))
	{
		CybReplayDownload::RequestAndPlay(player, CybReplayDownload::Kind::AWR, 0);
		return MRES_SUPERCEDE;
	}
	if (KZ_STREQI(arg1, "wr"))
	{
		CybReplayDownload::RequestAndPlay(player, CybReplayDownload::Kind::WR, 0);
		return MRES_SUPERCEDE;
	}
	if (KZ_STREQI(arg1, "wrpro"))
	{
		CybReplayDownload::RequestAndPlay(player, CybReplayDownload::Kind::WRPro, 0);
		return MRES_SUPERCEDE;
	}

	// Остались ТОЛЬКО серверные виды (локальная БД плагина) — они и раньше работали без
	// глобального api. Из перехваченных выше трое (wrpro/gpb/gpbpro) требовали
	// KZGlobalService и давали игроку «Глобальный API недоступен»/«только для глобальных
	// режимов»; `pbpro` же работал — он фолбэчился на локальный SPBPro, и этот фолбэк
	// сохранён на 404 центрального резолва (см. cyb_replay_download.cpp).
	static const struct
	{
		const char *keyword;
		RT type;
	} recordKeywords[] = {
		{"sr", RT::SR},
		{"srpro", RT::SRPro},
		{"spb", RT::SPB},
		{"spbpro", RT::SPBPro},
	};

	for (const auto &kw : recordKeywords)
	{
		if (KZ_STREQI(arg1, kw.keyword))
		{
			LoadReplayForRecord(player, kw.type, args->ArgC() >= 3 ? args->Arg(2) : "", args->ArgC() >= 4 ? args->Arg(3) : "");
			return MRES_SUPERCEDE;
		}
	}

	LoadReplay(player, arg1);
	return MRES_SUPERCEDE;
}

SCMD(kz_rpgoto, SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}

	if (args->ArgC() < 2)
	{
		player->languageService->PrintChat(true, false, "Replay - Usage Goto Time");
		return MRES_SUPERCEDE;
	}

	KZ::replaysystem::commands::JumpToReplayTime(player, args->ArgS());
	return MRES_SUPERCEDE;
}

SCMD(kz_rpgototick, SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}

	if (args->ArgC() < 2)
	{
		player->languageService->PrintChat(true, false, "Replay - Usage Goto Tick");
		return MRES_SUPERCEDE;
	}

	KZ::replaysystem::commands::JumpToReplayTick(player, args->Arg(1));
	return MRES_SUPERCEDE;
}

SCMD(kz_rpspeed, SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}

	if (args->ArgC() < 2)
	{
		// Без аргумента — показываем текущую скорость и границы, а не ругаемся:
		// команда чаще зовётся «а сейчас сколько?», чем по ошибке.
		char speedText[16], minText[16], maxText[16];
		KZ::replaysystem::commands::FormatReplaySpeed(KZ::replaysystem::commands::GetReplaySpeed(), speedText, sizeof(speedText));
		KZ::replaysystem::commands::FormatReplaySpeed(KZ::replaysystem::data::KZ_REPLAY_SPEED_MIN, minText, sizeof(minText));
		KZ::replaysystem::commands::FormatReplaySpeed(KZ::replaysystem::data::KZ_REPLAY_SPEED_MAX, maxText, sizeof(maxText));
		player->languageService->PrintChat(true, false, "Replay - Speed Current", speedText, minText, maxText);
		return MRES_SUPERCEDE;
	}

	char *endPtr = nullptr;
	f32 speed = (f32)strtod(args->Arg(1), &endPtr);
	if (endPtr == args->Arg(1) || *endPtr != '\0' || !(speed > 0.0f))
	{
		player->languageService->PrintChat(true, false, "Replay - Invalid Speed");
		return MRES_SUPERCEDE;
	}

	KZ::replaysystem::commands::SetReplaySpeed(player, speed);
	return MRES_SUPERCEDE;
}

SCMD(kz_rpstep, SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}

	// Без аргумента — шаг на один кадр вперёд: это основной жест покадрового просмотра.
	i32 frames = 1;
	if (args->ArgC() >= 2)
	{
		char *endPtr = nullptr;
		long parsed = strtol(args->Arg(1), &endPtr, 10);
		if (endPtr == args->Arg(1) || *endPtr != '\0' || parsed == 0)
		{
			player->languageService->PrintChat(true, false, "Replay - Invalid Step");
			return MRES_SUPERCEDE;
		}
		// Кламп ДО приведения к i32: без него !rpstep 99999999999 (strtol → LONG_MAX)
		// после среза давал бы −1, то есть шаг НАЗАД вместо «в конец записи».
		const long limit = (long)KZ::replaysystem::data::GetTickCount();
		parsed = (std::min)((std::max)(parsed, -limit), limit);
		frames = (i32)parsed;
	}

	KZ::replaysystem::commands::StepReplay(player, frames);
	return MRES_SUPERCEDE;
}

SCMD(kz_rpinfo, SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}

	KZ::replaysystem::commands::GetReplayInfo(player);
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_rpseek, kz_rpgoto);

SCMD(kz_rppause, SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}

	KZ::replaysystem::commands::ToggleReplayPause(player);
	return MRES_SUPERCEDE;
}

SCMD(kz_rploadprogress, SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}

	KZ::replaysystem::commands::CheckReplayLoadProgress(player);
	return MRES_SUPERCEDE;
}

SCMD(kz_rpcancelload, SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}

	KZ::replaysystem::commands::CancelReplayLoad(player);
	return MRES_SUPERCEDE;
}

SCMD(kz_replays, SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}

	KZ::replaysystem::commands::ListReplays(player, args->ArgS());
	return MRES_SUPERCEDE;
}

SCMD(kz_rphidelegs, SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	KZ::replaysystem::commands::ToggleLegsVisibility(player);
	return MRES_SUPERCEDE;
}
