#include "cyb_awr_backfill.h"

#include "kz/kz.h"
#include "kz/option/kz_option.h"
#include "kz/replays/data.h"
#include "kz/replays/playback.h"
#include "kz/replays/awr_cut.h"
#include "utils/ctimer.h"
#include "utils/http.h"
#include "utils/json.h"
#include "utils/logging.h"
#include "utils/utils.h"

#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
	// Темп насоса, когда есть работа: один файл в секунду (см. заголовок).
	constexpr f64 AWR_BACKFILL_BUSY_INTERVAL = 1.0;
	// Потолок аргумента команды: и от опечатки в разряде, и от усечения при (u32).
	constexpr i64 AWR_BACKFILL_MAX_COUNT = 100000;

	// ------------------------------------------------------------------
	// Состояние (главный поток; g_result* — под мьютексом)
	// ------------------------------------------------------------------

	// Один файл в работе: от GET бэклога до POST результата.
	bool g_busy = false;
	// Сколько попыток осталось в текущем прогоне.
	u32 g_remaining = 0;
	bool g_dryRun = false;
	// Период автоподбора из опции; 0 = автоматически не берём (только командой).
	i64 g_autoIntervalSec = 0;
	// Период автоподбора выждан — следующий холостой тик берёт файл. Без этого флага
	// автоподбор выгребал бы бэклог со скоростью насоса (1 файл/с), а не раз в период.
	bool g_autoDue = false;
	// Насос уже запущен — второй таймер на то же состояние не нужен.
	bool g_timerStarted = false;

	// Результат разбора одного файла: рабочий поток заполняет, главный забирает.
	struct WorkerResult
	{
		std::string uuid;
		bool ok = false;
		const char *reason = "";
		u64 timeMs = 0;
		u64 awrMs = 0;
		u32 teleports = 0;
		// Число ТП из шапки реплея (RunReplayData::num_teleports); -1 = поля нет.
		i32 headerTeleports = -1;
		// Снапшот режима на момент ВЗЯТИЯ файла. Пока файл в полёте, kz_awr_backfill может
		// переставить g_dryRun — и реальный файл ушёл бы как dry (или наоборот).
		bool dryRun = false;
	};

	std::mutex g_resultMutex;
	bool g_resultReady = false;
	WorkerResult g_result;

	// ------------------------------------------------------------------
	// HTTP-мелочи (тот же конфиг, что у cyb_outbox/cyb_emitter)
	// ------------------------------------------------------------------

	// Полный URL эндпоинта api; пустая строка = платформа не сконфигурирована.
	std::string ApiUrl(const char *path)
	{
		const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
		if (!url || url[0] == '\0')
		{
			return "";
		}
		std::string fullUrl = url;
		if (!fullUrl.empty() && fullUrl.back() == '/')
		{
			fullUrl.pop_back();
		}
		fullUrl += path;
		return fullUrl;
	}

	void SetAuthHeader(HTTP::Request &req)
	{
		const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");
		if (token && token[0] != '\0')
		{
			req.SetHeader("Authorization", std::string("Bearer ") + token);
		}
	}

	// Элемент `items` ответа бэклога. FromJson принимает nlohmann::json (а не Json):
	// именно такую сигнатуру ждёт Json::Get(key, std::vector<T> &) — единственный
	// доступ к массиву объектов в utils/json.h.
	struct BacklogItem
	{
		std::string replayUuid;
		std::string url;

		bool FromJson(const nlohmann::json &item)
		{
			if (!item.is_object() || !item.contains("replayUuid") || !item.contains("url"))
			{
				return false;
			}
			if (!item["replayUuid"].is_string() || !item["url"].is_string())
			{
				return false;
			}
			this->replayUuid = item["replayUuid"].get<std::string>();
			this->url = item["url"].get<std::string>();
			return true;
		}
	};

	// ------------------------------------------------------------------
	// Шаги цикла одного файла
	// ------------------------------------------------------------------

	void FetchOne();
	void StartDownload(const std::string &uuid, const std::string &url, bool dryRun);
	void SpawnWorker(const std::string &uuid, std::vector<char> data, bool dryRun);
	void PublishResult(const WorkerResult &res);
	void SendResult(const WorkerResult &res);

	// Попытка завершена (успешно или нет) — освобождаем слот и списываем одну из
	// запрошенных. Списываем и на отказе: иначе битая сеть крутила бы один файл
	// вечно, а `count` перестал бы что-либо ограничивать.
	void FinishAttempt()
	{
		g_busy = false;
		if (g_remaining > 0)
		{
			g_remaining--;
		}
	}

	// Файл ОБРАБОТАН (результат отправлен или посчитан вхолостую) — берём следующий сразу,
	// не дожидаясь тика: иначе на файл уходило бы два тика (взять → отдать) вместо одного.
	// Темп 1 файл/с сохраняется: забор результата у рабочего потока всё равно гейтится тиком.
	// Зовётся ТОЛЬКО с путей завершения файла: на отказе бэклога/докачки сцепки нет, иначе
	// битая сеть крутилась бы петлёй со скоростью HTTP-раундтрипа.
	void FinishFileAndChain()
	{
		FinishAttempt();
		if (!g_busy && g_remaining > 0)
		{
			FetchOne();
		}
	}

	// Насос: держим один файл в работе, забираем результат рабочего потока.
	f64 Tick()
	{
		WorkerResult res;
		bool haveResult = false;
		{
			std::lock_guard<std::mutex> lock(g_resultMutex);
			if (g_resultReady)
			{
				g_resultReady = false;
				res = g_result;
				g_result = {};
				haveResult = true;
			}
		}

		// Отправка — вне лока: колбэки HTTP и логи не должны держать мьютекс.
		if (haveResult)
		{
			SendResult(res);
			return AWR_BACKFILL_BUSY_INTERVAL;
		}

		if (g_busy)
		{
			return AWR_BACKFILL_BUSY_INTERVAL;
		}

		if (g_remaining > 0)
		{
			FetchOne();
			return AWR_BACKFILL_BUSY_INTERVAL;
		}

		// Холостой тик: работы нет и брать нечего — только здесь возвращается период
		// автоподбора. Пока файл в работе, интервал обязан быть коротким, иначе результат
		// разбора пролежал бы в g_result до конца периода и темп упал бы до одного файла
		// на два периода.
		if (g_autoIntervalSec > 0)
		{
			if (g_autoDue)
			{
				// Период выждан — берём ОДИН файл, всегда «по-настоящему» (не dry-run).
				g_autoDue = false;
				g_remaining = 1;
				g_dryRun = false;
				FetchOne();
				return AWR_BACKFILL_BUSY_INTERVAL;
			}
			g_autoDue = true;
			return (f64)g_autoIntervalSec;
		}

		// Автоподбор выключен — насос живёт только ради команды.
		return AWR_BACKFILL_BUSY_INTERVAL;
	}

	void EnsureTimer()
	{
		if (g_timerStarted)
		{
			return;
		}
		g_timerStarted = true;
		StartTimer(Tick, AWR_BACKFILL_BUSY_INTERVAL, true, true);
	}

	void FetchOne()
	{
		std::string url = ApiUrl("/replays/v1/awr-backlog");
		if (url.empty())
		{
			g_remaining = 0;
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backfill stopped reason=not_configured\n");
			return;
		}

		g_busy = true;
		// Снапшот режима на момент взятия файла — см. WorkerResult::dryRun.
		const bool dryRun = g_dryRun;

		HTTP::Request req(HTTP::Method::GET, url);
		req.SetQuery("limit", "1");
		SetAuthHeader(req);
		// clang-format off
		req.Send(
			[dryRun](HTTP::Response resp)
			{
				if (resp.status < 200 || resp.status >= 300)
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog failed reason=http_%u\n", (unsigned)resp.status);
					FinishAttempt();
					return;
				}

				std::optional<std::string> body = resp.Body();
				if (!body.has_value())
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog failed reason=empty_response\n");
					FinishAttempt();
					return;
				}

				Json json(*body);
				if (!json.IsValid())
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog failed reason=bad_json\n");
					FinishAttempt();
					return;
				}

				std::vector<BacklogItem> items;
				if (!json.Get("items", items))
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog failed reason=bad_items\n");
					FinishAttempt();
					return;
				}

				if (items.empty())
				{
					// Бэклог пуст — прогон закончен, следующий тик ничего не запросит.
					g_remaining = 0;
					g_busy = false;
					KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill done reason=empty_backlog\n");
					return;
				}

				StartDownload(items[0].replayUuid, items[0].url, dryRun);
			},
			[]()
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog failed reason=network\n");
				FinishAttempt();
			});
		// clang-format on
	}

	void StartDownload(const std::string &uuid, const std::string &url, bool dryRun)
	{
		// Ссылка выдана самим api (обычно presigned) — свой Bearer сюда не подставляем.
		HTTP::Request req(HTTP::Method::GET, url);
		// clang-format off
		req.Send(
			[uuid, dryRun](HTTP::Response resp)
			{
				if (resp.status < 200 || resp.status >= 300)
				{
					// Сетевой класс отказа: файл в бэклоге не помечаем, вернёмся к нему позже.
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] download failed uuid=%s reason=http_%u\n", uuid.c_str(),
								(unsigned)resp.status);
					FinishAttempt();
					return;
				}

				std::optional<std::vector<char>> raw = resp.RawBody();
				if (!raw.has_value() || raw->empty())
				{
					// 200 с пустым телом — отказ ФАЙЛА, а не сети: помечаем его в api
					// (awrMs:null), иначе он вечно возвращался бы первым в бэклоге.
					WorkerResult res;
					res.uuid = uuid;
					res.ok = false;
					res.reason = "empty_file";
					res.dryRun = dryRun;
					SendResult(res);
					return;
				}

				SpawnWorker(uuid, std::move(*raw), dryRun);
			},
			[uuid]()
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] download failed uuid=%s reason=network\n", uuid.c_str());
				FinishAttempt();
			});
		// clang-format on
	}

	void SpawnWorker(const std::string &uuid, std::vector<char> data, bool dryRun)
	{
		// Разбор файла и разрез — на рабочем потоке: data::LoadCutSourceFromMemory и
		// playback::ComputeCutFor глобального состояния не трогают (см. их комментарии),
		// а распаковка нескольких мегабайт в игровом потоке дала бы просадку кадра.
		std::thread worker(
			[uuid, dryRun, data = std::move(data)]()
			{
				WorkerResult res;
				res.uuid = uuid;
				res.dryRun = dryRun;

				// try/catch обязателен: разбор аллоцирует по размерам ИЗ ФАЙЛА
				// (compression.cpp:417 `new char[header.uncompressedSize]`,
				// compression.cpp:458 `resize(elementCount)`), и на битом файле прилетит
				// bad_alloc/length_error. Необработанное исключение в detached-потоке —
				// std::terminate, то есть падение сервера из-за одного мусорного реплея.
				try
				{
					KZ::replaysystem::data::CutSource src =
						KZ::replaysystem::data::LoadCutSourceFromMemory(data.data(), data.size());
					if (!src.valid)
					{
						res.reason = "parse_failed";
						PublishResult(res);
						return;
					}

					if (!src.header.has_run() || src.header.run().time() <= 0.0f)
					{
						// Не ран-реплей: времени рана нет, считать AWR не от чего.
						res.reason = "not_a_run";
						PublishResult(res);
						return;
					}

					res.timeMs = (u64)((f64)src.header.run().time() * 1000.0 + 0.5);
					res.headerTeleports = src.header.run().has_num_teleports() ? src.header.run().num_teleports() : -1;

					KZ::replaysystem::awr::CutResult cut =
						KZ::replaysystem::playback::ComputeCutFor(src.ticks.data(), (u32)src.ticks.size(), src.events.data(),
																  (u32)src.events.size(), res.timeMs);
					res.ok = cut.ok;
					res.reason = cut.ok ? "ok" : cut.reason;
					res.awrMs = cut.awrMs;
					res.teleports = cut.teleports;
					PublishResult(res);
				}
				catch (...)
				{
					// Битый файл: отдаём отказ, api пометит строку (awrMs:null) и файл
					// перестанет возвращаться в бэклог. Сервер при этом жив.
					res.ok = false;
					res.reason = "parse_failed";
					res.timeMs = 0;
					res.awrMs = 0;
					res.teleports = 0;
					res.headerTeleports = -1;
					PublishResult(res);
				}
			});
		worker.detach();
	}

	// Рабочий поток: отдать результат главному. Один файл в работе, поэтому очередь
	// вырождена в одну ячейку.
	void PublishResult(const WorkerResult &res)
	{
		std::lock_guard<std::mutex> lock(g_resultMutex);
		g_result = res;
		g_resultReady = true;
	}

	// Главный поток: лог, инварианты, отправка результата в api.
	void SendResult(const WorkerResult &res)
	{
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill uuid=%s time_ms=%llu awr_ms=%llu tps=%u ok=%d reason=%s dry=%d\n",
					res.uuid.c_str(), (unsigned long long)res.timeMs, (unsigned long long)res.awrMs, (unsigned)res.teleports,
					res.ok ? 1 : 0, res.reason, res.dryRun ? 1 : 0);

		if (res.ok)
		{
			// Инварианты спеки §4.5: расхождение не блокирует отправку, но должно быть видно.
			if (res.awrMs > res.timeMs)
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] invariant uuid=%s reason=awr_gt_time awr_ms=%llu time_ms=%llu\n",
							res.uuid.c_str(), (unsigned long long)res.awrMs, (unsigned long long)res.timeMs);
			}
			if (res.teleports == 0 && res.awrMs != res.timeMs)
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] invariant uuid=%s reason=no_tp_time_differs awr_ms=%llu time_ms=%llu\n",
							res.uuid.c_str(), (unsigned long long)res.awrMs, (unsigned long long)res.timeMs);
			}
			if (res.headerTeleports >= 0 && (u32)res.headerTeleports != res.teleports)
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] invariant uuid=%s reason=tp_count_mismatch tps=%u header_tps=%d\n",
							res.uuid.c_str(), (unsigned)res.teleports, res.headerTeleports);
			}
		}

		if (res.dryRun)
		{
			FinishFileAndChain();
			return;
		}

		std::string url = ApiUrl("/replays/v1/awr");
		if (url.empty())
		{
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] post failed uuid=%s reason=not_configured\n", res.uuid.c_str());
			g_remaining = 0;
			g_busy = false;
			return;
		}

		Json body;
		body.Set("replayUuid", res.uuid);
		// Не сошлось — отправляем null: строка в api помечается посчитанной и не
		// возвращается в бэклог, иначе битый файл крутился бы вечно.
		std::optional<u64> awrMs = res.ok ? std::make_optional(res.awrMs) : std::nullopt;
		body.Set("awrMs", awrMs);

		std::string uuid = res.uuid;
		HTTP::Request req(HTTP::Method::POST, url);
		req.SetHeader("Content-Type", "application/json");
		SetAuthHeader(req);
		req.SetBody(body.ToString());
		// clang-format off
		req.Send(
			[uuid](HTTP::Response resp)
			{
				if (resp.status < 200 || resp.status >= 300)
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] post failed uuid=%s reason=http_%u\n", uuid.c_str(),
								(unsigned)resp.status);
				}
				// Файл обработан (попытка списана в любом случае) — сразу следующий.
				FinishFileAndChain();
			},
			[uuid]()
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] post failed uuid=%s reason=network\n", uuid.c_str());
				FinishFileAndChain();
			});
		// clang-format on
	}
} // namespace

void CybAwrBackfill::Init()
{
	g_autoIntervalSec = KZOptionService::GetOptionInt("cybAwrBackfillIntervalSec", 60);
	if (g_autoIntervalSec > 0)
	{
		EnsureTimer();
	}
}

void CybAwrBackfill::Run(u32 count, bool dryRun)
{
	if (count == 0)
	{
		g_remaining = 0;
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill stopped reason=manual\n");
		return;
	}

	g_remaining = count;
	// Режим меняется только для файлов, взятых ПОСЛЕ этой строки: у файла в полёте свой
	// снапшот в WorkerResult::dryRun.
	g_dryRun = dryRun;
	// Насос может быть не запущен (cybAwrBackfillIntervalSec 0) — команда обязана работать.
	EnsureTimer();
	KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill started count=%u dry=%d\n", (unsigned)count, dryRun ? 1 : 0);
}

// Только серверная консоль/RCON: массовый прогон — не действие игрока (та же защита, что
// у kz_invisible_*, урок cyb.33 — SCMD-диспатч требует controller и из RCON не работает).
CON_COMMAND_F(kz_awr_backfill, "Compute AWR cut for N replays from the platform backlog. Usage: kz_awr_backfill <count> [-dry-run]", FCVAR_NONE)
{
	if (utils::GetController(context.GetPlayerSlot()))
	{
		KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] cmd_denied cmd=kz_awr_backfill reason=not_server slot=%d\n",
					context.GetPlayerSlot().Get());
		return;
	}

	// endptr обязателен: strtoll("abc") вернул бы 0, а 0 — это «остановить прогон», т.е.
	// опечатка молча гасила бы воркер. Верхняя граница — чтобы (u32) ничего не усёк.
	const char *countArg = args.ArgC() >= 2 ? args.Arg(1) : "";
	char *countEnd = nullptr;
	const i64 count = strtoll(countArg, &countEnd, 10);
	if (countArg[0] == '\0' || !countEnd || *countEnd != '\0' || count < 0 || count > AWR_BACKFILL_MAX_COUNT)
	{
		KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] cmd_rejected reason=bad_count arg=%s\n", args.ArgC() >= 2 ? args.Arg(1) : "<none>");
		return;
	}

	bool dryRun = false;
	for (int i = 2; i < args.ArgC(); i++)
	{
		if (!V_stricmp(args.Arg(i), "-dry-run"))
		{
			dryRun = true;
		}
	}

	CybAwrBackfill::Run((u32)count, dryRun);
}
