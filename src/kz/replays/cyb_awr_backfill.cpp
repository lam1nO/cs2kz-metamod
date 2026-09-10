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

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
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
	// Эпоха, которой принадлежит текущая защёлка g_busy. Нужна, чтобы снять её мог только
	// тот, кто её ставил: после смены карты защёлку сбрасывает OnMapChanged, но если в этот
	// момент был жив рабочий поток, она остаётся до отбрасывания его результата (см. Tick).
	u32 g_busyEpoch = 0;
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
	// Сам таймер: нужен, чтобы на смене карты сбросить его `lastExecute` (см. OnMapChanged).
	// Указатель не повиснет: ProcessTimerList удаляет таймер только когда Execute вернул
	// interval <= 0, а Tick всегда возвращает либо AWR_BACKFILL_BUSY_INTERVAL, либо
	// g_autoIntervalSec > 0.
	CTimerBase *g_timer = nullptr;
	// ЭПОХА цикла: увеличивается на каждой смене карты. Плагин на changelevel не
	// выгружается, поэтому колбэки Steam-HTTP, отправленные до смены карты, спокойно
	// доезжают ПОСЛЕ неё — и, если защёлка g_busy уже снята (OnMapChanged), такой опоздавший
	// колбэк запустил бы ВТОРУЮ цепочку поверх идущей: `g_result` — одна ячейка, результаты
	// затирали бы друг друга, а `g_remaining` списывался бы вдвое. Каждая из трёх отправок
	// (GET бэклога, GET файла, POST результата) и каждый её колбэк ошибки запоминают эпоху и
	// на чужой молча уходят, не трогая ни g_busy, ни g_remaining, ни сцепку.
	u32 g_epoch = 0;
	// Рабочих потоков в полёте. Атомик, потому что декремент делает сам поток: главному
	// нужно знать, безопасно ли снимать защёлку g_busy на смене карты (иначе два файла
	// разбирались бы одновременно в одну ячейку результата).
	std::atomic<int> g_workersInFlight {0};
	// dry-run: uuid, уже взятые в ЭТОМ прогоне. В dry-run строка в api не помечается, а
	// `awr-backlog` отдаёт свежайшие записи, поэтому без этого множества `kz_awr_backfill 50
	// -dry-run` пережёвывал бы ОДИН И ТОТ ЖЕ файл все 50 раз (наблюдено на канарейке
	// kz 0.191.0). Живёт до следующего Run.
	std::unordered_set<std::string> g_dryRunSeen;
	// «Мягко» отказавшие uuid этого ПРОЦЕССА (см. IsSoftFailure): в api они не отправлены,
	// значит api их не помечает и бэклог отдаёт их снова — без этого множества обычный
	// прогон жевал бы один и тот же файл до исчерпания счётчика. Живёт до перезапуска
	// плагина намеренно: «мягкий» отказ снимает правка НАШЕГО кода, то есть новый бинарь.
	std::unordered_set<std::string> g_softFailed;

	// Потолок смещения бэклога: `offset` в api ограничен zod'ом (`.max(100000)`, граница
	// включительная), запрос с большим значением вернул бы 400.
	constexpr size_t AWR_BACKLOG_MAX_OFFSET = 100000;
	// Сколько записей просить у бэклога, когда нужен ВЫБОР (dry-run или непустой набор
	// мягких отказов): иначе хватает одной свежайшей строки.
	constexpr size_t AWR_BACKFILL_PAGE_LIMIT = 50;
	constexpr const char *AWR_BACKFILL_PAGE_LIMIT_STR = "50";

	// Идёт ЯВНЫЙ прогон (команда `kz_awr_backfill N`) или автоподбор по таймеру. Смещение
	// бэклога — свойство ТОЛЬКО явного прогона: автоподбор обязан брать свежие загрузки, а
	// они стоят в НАЧАЛЕ выдачи (created_at desc), то есть ровно там, куда смещение не
	// смотрит. С общим смещением автоподбор после первого же прогона не считал бы новые
	// рекорды до перезапуска плагина.
	bool g_explicitRun = false;
	// Курсор смещения ТЕКУЩЕГО явного прогона. Ведётся по ФАКТИЧЕСКОЙ странице: пока в
	// странице есть невиденный файл, курсор не двигается вовсе; если вся страница из
	// виденных — курсор прибавляет её размер и запрос повторяется. Так прогон не встаёт ни
	// на 50 мягких отказах, ни на 50 реплеях, залитых во время прогона (позиции 0..k-1 при
	// новых загрузках сдвигают набор, и «offset = |g_softFailed|» упёрся бы в ту же стену).
	//
	// Курсор монотонен внутри прогона, и это безопасно: он прибавляется только за страницу,
	// в которой ВСЁ виденное, а файлы уходят из бэклога только с позиций >= курсора (берём
	// их из страницы, начинающейся с курсора) — значит префикс [0, курсор) не теряет
	// записей и не может «подтянуть» под курсор невиденную.
	size_t g_backlogOffset = 0;
	// Про потолок смещения предупреждаем один раз на прогон: шаг у прогона на каждый файл,
	// и без защёлки лог залило бы одинаковыми строками. Снимается в Run() вместе с курсором
	// — потолок и существует только у явного прогона.
	bool g_offsetCapWarned = false;

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
		// Самый большой разрыв записи внутри вырезов, не покрытый паузой, и его кадр
		// (awr::CutResult::maxUncoveredGapTicks). Печатается на КАЖДОМ файле, где метрика
		// вообще посчитана (в том числе на успехе): распределение разрывов надо видеть до
		// того, как оно испортит awr_ms. На ранних отказах (parse_failed, not_a_run, empty,
		// no_run_window, dest_not_found, counter_mismatch) подсчёта не
		// было — печатаем max_gap=n/a, а не 0@0: ноль читался бы как «разрыва нет».
		bool maxGapMeasured = false;
		u64 maxGapTicks = 0;
		u32 maxGapFrame = 0;
		// Разбор отказа из awr::CutResult::detail (пусто при ok). Копия, а не указатель:
		// CutResult живёт на стеке рабочего потока, а строку печатает главный.
		std::string detail;
		// Снапшот режима на момент ВЗЯТИЯ файла. Пока файл в полёте, kz_awr_backfill может
		// переставить g_dryRun — и реальный файл ушёл бы как dry (или наоборот).
		bool dryRun = false;
		// Эпоха, в которой файл был взят (см. g_epoch). Поле нужно ТОЛЬКО для межпоточной
		// передачи: рабочий поток заполняет его, а Tick по нему отбрасывает результат,
		// приехавший из прошлой эпохи. Синтетические WorkerResult, которые собираются прямо
		// на главном потоке (отказы докачки http_4xx / empty_file), его не заполняют — им и
		// незачем, эпоху для POST берёт из g_epoch сама SendResult.
		u32 epoch = 0;
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

	// Колбэк принадлежит уже закончившейся эпохе (между отправкой и ответом сменилась
	// карта)? Тогда он обязан молча уйти: его цепочка мертва, а состояние принадлежит новой.
	bool StaleEpoch(u32 epoch, const char *what, const char *uuid)
	{
		if (epoch == g_epoch)
		{
			return false;
		}
		KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] callback dropped reason=stale_epoch what=%s uuid=%s epoch=%u now=%u\n", what,
					(uuid && uuid[0]) ? uuid : "<none>", (unsigned)epoch, (unsigned)g_epoch);
		return true;
	}

	// ------------------------------------------------------------------
	// Шаги цикла одного файла
	// ------------------------------------------------------------------

	void FetchOne(bool explicitRun);
	void StartDownload(const std::string &uuid, const std::string &url, bool dryRun);
	void SpawnWorker(const std::string &uuid, std::vector<char> data, bool dryRun);
	void PublishResult(const WorkerResult &res);
	// По значению: блокирующий инвариант шапки может понизить res.ok до отправки.
	void SendResult(WorkerResult res);

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
			FetchOne(g_explicitRun);
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
			// Результат потока, запущенного до смены карты. Разрез сам по себе верен (файл
			// от карты не зависит), но отправлять его отсюда нельзя: SendResult на всех
			// своих путях трогает общее состояние — FinishFileAndChain, а на
			// not_configured ещё и g_remaining/g_busy, — а это состояние принадлежит уже
			// НОВОЙ эпохе и, возможно, идущему прямо сейчас файлу. Заводить ради этого
			// вторую, «безсостояночную» ветку отправки — лишний путь ради одного файла.
			// Цена отказа мала и ограничена: строка в api не помечена, значит бэклог отдаст
			// её снова следующим же тиком, и файл досчитается в текущей эпохе.
			if (res.epoch != g_epoch)
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] callback dropped reason=stale_epoch what=worker uuid=%s epoch=%u now=%u\n",
							res.uuid.c_str(), (unsigned)res.epoch, (unsigned)g_epoch);
				// Защёлку снимаем ТОЛЬКО если она всё ещё принадлежит той, прошлой цепочке:
				// OnMapChanged её не тронул именно потому, что этот поток был в полёте.
				// Если новая эпоха уже взяла свой файл — защёлка её, и трогать нельзя.
				if (g_busy && g_busyEpoch != g_epoch)
				{
					g_busy = false;
				}
				return AWR_BACKFILL_BUSY_INTERVAL;
			}
			SendResult(res);
			return AWR_BACKFILL_BUSY_INTERVAL;
		}

		if (g_busy)
		{
			return AWR_BACKFILL_BUSY_INTERVAL;
		}

		if (g_remaining > 0)
		{
			FetchOne(g_explicitRun);
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
				// Период выждан — берём ОДИН файл, всегда «по-настоящему» (не dry-run) и
				// всегда БЕЗ смещения: автоподбор существует ради свежих загрузок.
				g_autoDue = false;
				g_remaining = 1;
				g_dryRun = false;
				g_explicitRun = false;
				FetchOne(false);
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
		g_timer = StartTimer(Tick, AWR_BACKFILL_BUSY_INTERVAL, true, true);
	}

	void FetchOne(bool explicitRun)
	{
		std::string url = ApiUrl("/replays/v1/awr-backlog");
		if (url.empty())
		{
			g_remaining = 0;
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backfill stopped reason=not_configured\n");
			return;
		}

		g_busy = true;
		g_busyEpoch = g_epoch;
		// Снапшот режима на момент взятия файла — см. WorkerResult::dryRun.
		const bool dryRun = g_dryRun;
		// Снапшот эпохи: ответ может приехать уже после смены карты.
		const u32 epoch = g_epoch;

		HTTP::Request req(HTTP::Method::GET, url);
		// Одной свежайшей строки хватает, пока выбирать не из чего: обработанная помечается
		// в api и в бэклог не возвращается. Выбор нужен в dry-run (пометки нет, см.
		// g_dryRunSeen) и при непустом наборе мягких отказов: они в api не помечены и
		// приезжают первыми.
		const bool needPage = dryRun || !g_softFailed.empty();
		req.SetQuery("limit", needPage ? AWR_BACKFILL_PAGE_LIMIT_STR : "1");

		// СМЕЩЕНИЕ — только на шаге ЯВНОГО прогона (см. g_explicitRun) и только реального:
		// dry-run ходит по первой странице и ограничен ей осознанно. Курсор ведётся по
		// фактической странице в колбэке ниже, здесь его лишь применяем.
		const size_t offset = (explicitRun && !dryRun) ? g_backlogOffset : 0;
		if (offset > 0)
		{
			req.SetQuery("offset", std::to_string(offset));
		}
		SetAuthHeader(req);
		// clang-format off
		req.Send(
			[dryRun, epoch, offset, explicitRun, needPage](HTTP::Response resp)
			{
				if (StaleEpoch(epoch, "backlog", nullptr))
				{
					return;
				}
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
					// Пусто — прогон закончен, следующий тик ничего не запросит. Причины
					// РАЗНЫЕ и различать их обязательно: при offset == 0 посчитано всё,
					// при offset > 0 за смещением строк не осталось, а хвост НАЧАЛА выдачи
					// (свежие загрузки и мягкие отказы) этот прогон не смотрел — его возьмёт
					// автоподбор или следующая команда.
					g_remaining = 0;
					g_busy = false;
					if (offset > 0)
					{
						KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill done reason=backlog_end_after_offset offset=%u\n",
									(unsigned)offset);
					}
					else
					{
						KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill done reason=empty_backlog\n");
					}
					return;
				}

				if (!dryRun)
				{
					for (const BacklogItem &item : items)
					{
						if (g_softFailed.count(item.replayUuid) != 0)
						{
							continue;
						}
						StartDownload(item.replayUuid, item.url, dryRun);
						return;
					}
					// Страница ЦЕЛИКОМ из уже виденных. Достижимо двумя способами: мягких
					// отказов накопилось на страницу, либо во время прогона залили пачку
					// реплеев и они сдвинули набор вправо. Оба лечит один приём — двигать
					// курсор по ФАКТИЧЕСКОЙ странице и повторять запрос.
					if (explicitRun && !dryRun)
					{
						if (items.size() < AWR_BACKFILL_PAGE_LIMIT)
						{
							// Страница неполная — за ней записей нет вовсе: это конец
							// бэклога, а не тупик.
							g_remaining = 0;
							g_busy = false;
							KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill done reason=backlog_tail_seen offset=%u items=%u\n",
										(unsigned)offset, (unsigned)items.size());
							return;
						}
						const size_t next = offset + items.size();
						if (next > AWR_BACKLOG_MAX_OFFSET)
						{
							// Дальше api не пустит (потолок offset). Прогон останавливаем:
							// молча ходить по кругу хуже, чем сказать, где встали.
							if (!g_offsetCapWarned)
							{
								g_offsetCapWarned = true;
								KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog offset capped offset=%u next=%u cap=%u\n",
											(unsigned)offset, (unsigned)next, (unsigned)AWR_BACKLOG_MAX_OFFSET);
							}
							g_remaining = 0;
							g_busy = false;
							KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backfill done reason=backlog_offset_cap offset=%u soft=%u\n",
										(unsigned)offset, (unsigned)g_softFailed.size());
							return;
						}
						g_backlogOffset = next;
						// Повтор в рамках ТОГО ЖЕ шага: файл не взят, поэтому ни g_remaining,
						// ни защёлку g_busy не трогаем (FinishAttempt здесь не зовём).
						// Стека не растим — Send асинхронный.
						FetchOne(true);
						return;
					}
					// Автоподбор (и dry-run) ходят только по первой странице: их дело —
					// свежие загрузки, а догон истории — дело явного прогона. Идти некуда.
					g_remaining = 0;
					g_busy = false;
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backfill done reason=soft_failed_exhausted soft=%u page=%u items=%u\n",
								(unsigned)g_softFailed.size(), (unsigned)(needPage ? AWR_BACKFILL_PAGE_LIMIT : 1), (unsigned)items.size());
					return;
				}

				// dry-run: первый, которого в этом прогоне ещё не брали.
				for (const BacklogItem &item : items)
				{
					if (g_dryRunSeen.count(item.replayUuid) == 0)
					{
						g_dryRunSeen.insert(item.replayUuid);
						StartDownload(item.replayUuid, item.url, dryRun);
						return;
					}
				}

				// Страница выбрана целиком. Дальше идти некуда: пометки в api нет, и
				// следующий запрос вернул бы ту же страницу — прогон остановится сам.
				g_remaining = 0;
				g_busy = false;
				KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill done reason=dry_run_exhausted seen=%u\n",
							(unsigned)g_dryRunSeen.size());
			},
			[epoch]()
			{
				if (StaleEpoch(epoch, "backlog", nullptr))
				{
					return;
				}
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog failed reason=network\n");
				FinishAttempt();
			});
		// clang-format on
	}

	void StartDownload(const std::string &uuid, const std::string &url, bool dryRun)
	{
		// Эпоха на момент отправки: файл может доехать уже после смены карты.
		const u32 epoch = g_epoch;
		// Ссылка выдана самим api (обычно presigned) — свой Bearer сюда не подставляем.
		HTTP::Request req(HTTP::Method::GET, url);
		// clang-format off
		req.Send(
			[uuid, dryRun, epoch](HTTP::Response resp)
			{
				if (StaleEpoch(epoch, "download", uuid.c_str()))
				{
					return;
				}
				if (resp.status >= 400 && resp.status < 500)
				{
					// 4xx — отказ ФАЙЛА, а не сети (403 протухшая ссылка, 404/410 объект
					// удалён из S3). Помечаем строку в api (awrMs:null): бэклог отдаётся
					// desc(createdAt) с limit=1, и непомеченный файл возвращался бы первым
					// вечно — весь бэклог встал бы намертво на одном мёртвом объекте.
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] download failed uuid=%s reason=http_%u\n", uuid.c_str(),
								(unsigned)resp.status);
					WorkerResult res;
					res.uuid = uuid;
					res.ok = false;
					res.reason = "http_4xx";
					res.dryRun = dryRun;
					SendResult(res);
					return;
				}
				if (resp.status < 200 || resp.status >= 300)
				{
					// 5xx и прочее — сетевой класс отказа: файл в бэклоге не помечаем,
					// вернёмся к нему позже.
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
			[uuid, epoch]()
			{
				if (StaleEpoch(epoch, "download", uuid.c_str()))
				{
					return;
				}
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
		g_workersInFlight++;
		// Эпоха фиксируется ЗДЕСЬ, на главном потоке: сам поток g_epoch читать не должен.
		const u32 epoch = g_epoch;
		std::thread worker(
			[uuid, dryRun, epoch, data = std::move(data)]()
			{
				// Декремент строго после публикации результата: главный поток по нулю
				// решает, что в полёте никого нет (OnMapChanged).
				struct InFlightGuard
				{
					~InFlightGuard()
					{
						g_workersInFlight--;
					}
				} guard;

				WorkerResult res;
				res.uuid = uuid;
				res.dryRun = dryRun;
				res.epoch = epoch;

				// Защита от битого файла — НЕ try/catch: форк собирается с
				// `-fno-exceptions` (AMBuildScript), исключение поймать нечем. Абсурдные
				// размеры из шапки секций (по ним аллоцирует compression.cpp:417/458)
				// отсекает пре-валидация внутри data::LoadCutSourceFromMemory — она отдаёт
				// valid=false, и это ровно та же причина отказа parse_failed ниже.
				KZ::replaysystem::data::CutSource src = KZ::replaysystem::data::LoadCutSourceFromMemory(data.data(), data.size());
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
				res.detail = cut.detail;
				res.awrMs = cut.awrMs;
				res.teleports = cut.teleports;
				res.maxGapMeasured = cut.maxUncoveredGapMeasured;
				res.maxGapTicks = cut.maxUncoveredGapTicks;
				res.maxGapFrame = cut.maxUncoveredGapFrame;
				PublishResult(res);
			});
		worker.detach();
	}

	// Отказ, который может исчезнуть после правки НАШЕГО кода, а не свойство файла. Такой
	// результат нельзя ни писать в api, ни помечать посчитанным — см. SendResult.
	// Детерминированные отказы (dest_not_found, counter_mismatch, header_tp_mismatch,
	// no_run_window, parse_failed, not_a_run, http_4xx, empty_file) сюда НЕ входят.
	bool IsSoftFailure(const char *reason)
	{
		return KZ_STREQ(reason, "awr_implausible") || KZ_STREQ(reason, "awr_record_gap");
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
	void SendResult(WorkerResult res)
	{
		// Инвариант шапки — БЛОКИРУЮЩИЙ, в отличие от двух остальных: число прибытий ТП,
		// найденных разрезом, обязано совпасть с num_teleports из шапки файла. Расхождение
		// означает, что окно рана выбрано неверно (лишний или пропущенный телепорт), то есть
		// awr_ms посчитан не по тому набору кадров. Записать такое время в api хуже, чем
		// пометить строку посчитанной с awrMs:null: ложный AWR попадёт в витрину и на него
		// будут смотреть как на правду. Ровно эта проверка ловит любую ошибку выбора окна.
		if (res.ok && res.headerTeleports >= 0 && (u32)res.headerTeleports != res.teleports)
		{
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] invariant uuid=%s reason=header_tp_mismatch tps=%u header_tps=%d\n",
						res.uuid.c_str(), (unsigned)res.teleports, res.headerTeleports);
			res.ok = false;
			res.reason = "header_tp_mismatch";
			res.awrMs = 0;
		}

		// detail — только на отказе: без него первый живой dry-run ответил только
		// «dest_not_found» на всех 50 файлах, и разбор стоил ещё одного цикла
		// сборка → канарейка. Пишем в ту же строку, чтобы её можно было грепать целиком.
		char detailSuffix[256] = {};
		if (!res.ok && !res.detail.empty())
		{
			V_snprintf(detailSuffix, sizeof(detailSuffix), " detail=%s", res.detail.c_str());
		}
		char maxGapText[32];
		if (res.maxGapMeasured)
		{
			V_snprintf(maxGapText, sizeof(maxGapText), "%llu@%u", (unsigned long long)res.maxGapTicks, res.maxGapFrame);
		}
		else
		{
			V_snprintf(maxGapText, sizeof(maxGapText), "n/a");
		}
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill uuid=%s time_ms=%llu awr_ms=%llu tps=%u max_gap=%s ok=%d reason=%s dry=%d%s\n",
					res.uuid.c_str(), (unsigned long long)res.timeMs, (unsigned long long)res.awrMs, (unsigned)res.teleports, maxGapText,
					res.ok ? 1 : 0, res.reason, res.dryRun ? 1 : 0, detailSuffix);

		if (res.ok)
		{
			// Остальные инварианты спеки §4.5: расхождение отправку НЕ блокирует (это
			// свойства самого рана, а не признак неверного окна), но должно быть видно.
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
		}

		if (res.dryRun)
		{
			FinishFileAndChain();
			return;
		}

		// «Мягкий» отказ — НЕ отправляем в api вовсе. Любой POST (даже с awrMs:null) ставит
		// строке awr_checked_at, и она навсегда выпадает из бэклога; для отказа, который
		// говорит «этому результату нельзя верить» (а не «файл такой»), это неверно: после
		// правки нашего кода тот же файл может посчитаться нормально. Файл остаётся в
		// бэклоге, а чтобы прогон не жевал его по кругу — помним uuid в процессе.
		if (!res.ok && IsSoftFailure(res.reason))
		{
			g_softFailed.insert(res.uuid);
			KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill soft_failed uuid=%s reason=%s not_posted=1 soft=%u\n", res.uuid.c_str(),
						res.reason, (unsigned)g_softFailed.size());
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
		// Эпоха для колбэков POST берётся ИЗ g_epoch, а НЕ из res.epoch. SendResult зовётся
		// только на главном потоке и только с путей, уже прошедших проверку эпохи (Tick —
		// для результата рабочего потока; колбэк докачки — для синтетических отказов
		// http_4xx / empty_file), поэтому текущая эпоха здесь и есть эпоха этой цепочки.
		//
		// res.epoch для этого не годится: синтетические WorkerResult в колбэке докачки его
		// не заполняют (и не должны — он про межпоточную передачу), там остаётся 0. После
		// первой же смены карты (g_epoch >= 1) оба колбэка POST такого файла отбрасывались
		// бы как stale_epoch, FinishFileAndChain не вызывался бы, а g_busy оставался бы true
		// с g_busyEpoch == g_epoch — то есть и спасательная ветка в Tick не сработала бы:
		// вечная защёлка, воркер молчит до рестарта сервера.
		const u32 epoch = g_epoch;
		HTTP::Request req(HTTP::Method::POST, url);
		req.SetHeader("Content-Type", "application/json");
		SetAuthHeader(req);
		req.SetBody(body.ToString());
		// clang-format off
		req.Send(
			[uuid, epoch](HTTP::Response resp)
			{
				if (resp.status < 200 || resp.status >= 300)
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] post failed uuid=%s reason=http_%u\n", uuid.c_str(),
								(unsigned)resp.status);
				}
				// Сам POST уже доехал (строка в api помечена — это полезно и от эпохи не
				// зависит), но сцепку дальше гнать нельзя: она принадлежит новой эпохе.
				if (StaleEpoch(epoch, "post", uuid.c_str()))
				{
					return;
				}
				// Файл обработан (попытка списана в любом случае) — сразу следующий.
				FinishFileAndChain();
			},
			[uuid, epoch]()
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] post failed uuid=%s reason=network\n", uuid.c_str());
				if (StaleEpoch(epoch, "post", uuid.c_str()))
				{
					return;
				}
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
	// Множество виденного — на прогон, а не на жизнь сервера: повторный `-dry-run` обязан
	// снова пройти по тем же файлам, иначе второй прогон печатал бы сразу exhausted.
	g_dryRunSeen.clear();
	// «Мягко» отказавшие — тоже на прогон. Иначе прогон застревает структурно: бэклог
	// отдаёт максимум 50 строк (жёсткий кап в api), сортировка createdAt DESC, а мягкий
	// отказ строку в api НЕ помечает — как только таких наберётся 50 и свежее них не
	// окажется непроверенных, каждый следующий шаг видел бы только их и уходил в
	// soft_failed_exhausted, а весь более старый бэклог был бы недостижим до перезапуска
	// плагина. Явный `kz_awr_backfill N` — осознанная просьба оператора повторить, и цена
	// повтора (трафик + счётчик на уже виденные файлы) приемлема. АВТОПОДБОР по таймеру
	// (Tick без Run) множество не чистит: там повтор был бы вечным циклом.
	g_softFailed.clear();
	// Курсор смещения и его защёлка — состояние ОДНОГО прогона.
	g_backlogOffset = 0;
	g_offsetCapWarned = false;
	// Этот прогон — явный: только ему разрешено смещение (автоподбор его сбрасывает сам).
	g_explicitRun = true;
	// Насос может быть не запущен (cybAwrBackfillIntervalSec 0) — команда обязана работать.
	EnsureTimer();
	KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill started count=%u dry=%d\n", (unsigned)count, dryRun ? 1 : 0);
	// ПЕРВЫЙ шаг — сразу, не дожидаясь тика насоса: команда должна начинать работать в ту
	// же секунду, а не через интервал таймера, каким бы он ни был. Дальше файлы гонит
	// сцепка FinishFileAndChain, тоже без ожидания тика.
	Tick();
}

void CybAwrBackfill::OnMapChanged()
{
	// ЭПОХА растёт ВСЕГДА и ПЕРВЫМ делом. HTTP-колбэки, отправленные до changelevel,
	// доезжают в любом случае — плагин на смене карты не выгружается. Любой такой колбэк,
	// сработав уже по новому состоянию, увёл бы цикл в ДВЕ параллельные цепочки: ячейка
	// g_result одна (результаты затирали бы друг друга), а g_remaining списывался бы вдвое.
	// С этой строки все они молча уходят (StaleEpoch), не трогая ни g_busy, ни g_remaining,
	// ни сцепку.
	g_epoch++;

	// Защёлка «файл в работе». HTTP-запрос, начатый до changelevel, может не довести ни
	// колбэк ответа, ни колбэк ошибки — а теперь ещё и сознательно отбрасывается по эпохе;
	// в обоих случаях g_busy остался бы true навсегда, и Tick выходил бы на первой же
	// проверке: `kz_awr_backfill 1` печатает started и больше ничего не делает (наблюдено
	// на канарейке kz 0.191.0 после смены карты).
	//
	// Снимаем ТОЛЬКО когда в полёте нет рабочего потока: иначе его результат приехал бы в
	// одну ячейку g_result с результатом нового файла, и один из двух потерялся бы молча.
	// Если поток жив — защёлку снимет Tick, отбросив его результат как чужой по эпохе.
	if (g_busy && g_workersInFlight.load() == 0)
	{
		g_busy = false;
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] map_changed reason=busy_latch_cleared remaining=%u epoch=%u\n",
					(unsigned)g_remaining, (unsigned)g_epoch);
	}
	// Период автоподбора начинается заново: отсчитывать его от прошлой карты смысла нет.
	g_autoDue = false;
	// Страховка от дефекта ctimer (память проекта fork-timers-stall-after-map-change):
	// lastExecute == -1 заставляет ProcessTimerList взять текущее время за точку отсчёта,
	// то есть таймер точно проснётся через свой интервал, а не через длительность прошлой
	// карты. Нашему таймеру это, по идее, не нужно (он заведён с useRealTime = true, а
	// realtime на changelevel не обнуляется, в отличие от curtime), но проверить это
	// вживую я не могу, а цена страховки — одно присваивание на смену карты.
	if (g_timer)
	{
		g_timer->lastExecute = -1;
	}
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
