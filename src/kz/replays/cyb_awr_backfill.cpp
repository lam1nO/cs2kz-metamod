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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// Сколько файлов бэкфилл держит в полёте одновременно. Конвар, а не опция серверного cfg:
// опции читаются один раз на загрузке плагина, а этот параметр нужно крутить ЖИВЬЁМ под
// нагрузкой по RCON, не перезапуская прогон. Потолок и обоснование —
// AWR_BACKFILL_MAX_CONCURRENCY.
CConVar<i32> kz_awr_backfill_concurrency("kz_awr_backfill_concurrency", FCVAR_NONE,
										 "How many AWR backfill files to process concurrently (1-8; memory scales with it)", 1);

namespace
{
	// Темп насоса, когда есть работа: один файл в секунду (см. заголовок).
	constexpr f64 AWR_BACKFILL_BUSY_INTERVAL = 1.0;
	// Потолок аргумента команды: и от опечатки в разряде, и от усечения при (u32).
	constexpr i64 AWR_BACKFILL_MAX_COUNT = 100000;

	// ------------------------------------------------------------------
	// Состояние (главный поток; очередь результатов — под мьютексом)
	// ------------------------------------------------------------------

	// Файлов В ПОЛЁТЕ: от взятия из бэклога до POST результата (скачивание + разбор на
	// рабочем потоке + отправка). Было защёлкой на один файл; стало счётчиком, потому что
	// упор прогона — СЕТЬ, а не разбор: файл до 32 МБ качается ~секунду, и один файл за
	// раз давал 59 файлов в минуту (замер cyb.184: 17 748 файлов = 4.7 часа).
	u32 g_inFlight = 0;
	// Пик за прогон — печатается в строке завершения: по нему видно, дал ли конвар эффект.
	u32 g_peakInFlight = 0;
	// uuid файлов в полёте. В api они помечаются только POST'ом, поэтому до его доезда
	// бэклог отдаёт их снова — без этого множества N слотов взяли бы ОДИН И ТОТ ЖЕ файл N
	// раз (та же природа, что у g_softFailed и g_dryRunSeen).
	std::unordered_set<std::string> g_takenInFlight;
	// Запрос бэклога СЕРИАЛИЗОВАН: одновременно летит не больше одного. Курсор смещения
	// (g_backlogOffset) читается и двигается только в этом пути, поэтому гонки за него нет
	// по построению. Параллелятся скачивание и разбор — там и лежит время.
	bool g_backlogBusy = false;
	// Сколько попыток осталось в текущем прогоне (списывается на ВЗЯТИИ файла).
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
	// доезжают ПОСЛЕ неё — и, если слоты уже погашены (OnMapChanged), такой опоздавший
	// колбэк освободил бы ЧУЖОЙ слот или списал бы попытку нового прогона. Каждая из трёх
	// отправок (GET бэклога, GET файла, POST результата) и каждый её колбэк ошибки запоминают
	// эпоху и на чужой молча уходят, не трогая ни слоты, ни g_remaining, ни сцепку.
	u32 g_epoch = 0;
	// Рабочих потоков в полёте. Атомик, потому что декремент делает сам поток. Только
	// диагностика (печатается в логах): занятость слотов считается НЕ им, см. g_orphanResults.
	std::atomic<int> g_workersInFlight {0};
	// Разборы, чей результат ещё НЕ ПОТРЕБЛЁН главным потоком: инкремент в SpawnWorker,
	// декремент на каждый снятый очередью результат в Tick. Оба конца — на главном потоке и
	// попарны ПО ПОСТРОЕНИЮ: каждый запущенный воркер публикует ровно один результат, и
	// каждый результат снимается очередью ровно один раз. Единица одна и та же — «результат,
	// который ещё не забрали», поэтому счётчик не зависит ни от живости потока, ни от такта
	// насоса, ни от паузы changelevel (когда таймеры не тикают, а воркеры считают).
	u32 g_unconsumedResults = 0;
	// Разборы, ОСИРОТЕВШИЕ на смене карты: их слоты погашены (OnMapChanged), колбэки
	// отброшены по эпохе, но память (сырой буфер + дельта-буфер + тики) они держат. Это
	// ВТОРОЕ слагаемое занятости, и в нормальном прогоне оно равно нулю — поэтому файл
	// считается ровно один раз (складывать g_inFlight с g_workersInFlight нельзя: второй
	// счётчик строгий подынтервал первого, и файл на этапе разбора считался бы дважды,
	// вдвое занижая фактическую параллельность).
	//
	// Снимок берётся из g_unconsumedResults, а НЕ из числа живых потоков: инкремент по
	// живости и декремент по потреблению — разные единицы, между ними такт насоса и пауза
	// changelevel, и снимок расходился в обе стороны (воркер, вышедший до смены карты,
	// в снимок не попадал, но его результат списывал чужую долю — занижение занятости и
	// ложный underflow; обратный порядок давал завышение и ПОТЕРЯННЫЙ слот, при N=1 —
	// полный стоп прогона). В нынешней единице расходиться нечему: все непотреблённые на
	// момент смены карты результаты принадлежат прошлой эпохе, каждый будет снят ровно один
	// раз и ровно один раз уменьшит снимок.
	u32 g_orphanResults = 0;

	// Хэндлы рабочих потоков: нужны, чтобы на выгрузке плагина сделать join, а не ждать
	// счётчик. Ожидание счётчика окно не закрывает: декремент стоит в деструкторе локальной
	// переменной внутри лямбды, а сама лямбда (с копией сырого буфера) уничтожается ПОЗЖЕ,
	// уже после возврата из неё, и её деструкторы лежат в нашей .so. join() возвращается
	// только когда поток отработал целиком, включая уничтожение замыкания.
	struct WorkerHandle
	{
		std::thread thread;
		// Поток дописал результат — можно джойнить без блокировки. shared_ptr, потому что
		// флаг переживает и хэндл (главный поток), и лямбду (рабочий).
		std::shared_ptr<std::atomic<bool>> done;
	};

	std::vector<WorkerHandle> g_workerThreads;
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
	// мягких отказов): иначе хватает одной свежайшей строки. 50 — не наше предпочтение, а
	// ЖЁСТКИЙ КАП api (`limit: z.coerce.number().int().min(1).max(50)`), поэтому неполная
	// страница означает «за ней записей нет», а не «мы попросили мало».
	constexpr size_t AWR_BACKFILL_PAGE_LIMIT = 50;
	constexpr const char *AWR_BACKFILL_PAGE_LIMIT_STR = "50";

	// Число и строка обязаны совпадать: по числу решается «страница неполная = конец
	// бэклога», строка уходит в запрос. Разъехались бы — прогон останавливался бы на
	// полной странице или ходил по кругу на неполной.
	constexpr size_t ParseDecimal(const char *text)
	{
		size_t value = 0;
		for (const char *c = text; *c != '\0'; c++)
		{
			value = value * 10 + (size_t)(*c - '0');
		}
		return value;
	}

	static_assert(ParseDecimal(AWR_BACKFILL_PAGE_LIMIT_STR) == AWR_BACKFILL_PAGE_LIMIT, "limit query string must match AWR_BACKFILL_PAGE_LIMIT");

	// Потолок одновременных файлов. Разбор держит на слот ТРИ буфера сразу: сырой файл,
	// дельта-буфер распаковки и вектор тиков. Три РАЗНЫЕ величины (двухчасовой ран,
	// ~460 000 кадров, файл 32 МБ):
	//   - типичный пик ~209 МБ (32 + 121 тики + ~56 дельта-буфер по оценке самого форка,
	//     `reserve(size * 120)` в compression.cpp);
	//   - худший ЛЕГИТИМНЫЙ ~275 МБ (дельта-буфер вырастает до 264 Б на кадр, если меняются
	//     все поля каждый кадр);
	//   - разрешённый ПРЕ-ВАЛИДАЦИЕЙ ~550 МБ (256 МиБ на секцию + миллион кадров по 264 Б +
	//     32 МБ сырых): 32-мегабайтный zstd законно разжимается в 256 МиБ.
	// При N=8 это 1.7 / 2.2 / 4.4 ГБ соответственно. Восьмёрка держится НЕ на объёме, а на
	// том, что файлы — вывод нашего же рекордера (лимит аплоада 32 МБ, кадры пишет он сам),
	// а не то, что прислал игрок; на ноде с игроками рекомендуется 4 (см. CYBER.md).
	constexpr i32 AWR_BACKFILL_MAX_CONCURRENCY = 8;
	// Про кламп предупреждаем один раз на процесс: конвар читается на каждое решение.
	bool g_concurrencyWarned = false;

	// Сколько файлов разрешено держать в полёте. Читается на каждое решение, поэтому
	// правится ЖИВЬЁМ по RCON (`kz_awr_backfill_concurrency 4`) — без пересборки и без
	// перезапуска прогона. Именно конвар, а не опция серверного cfg: `pServerCfgKeyValues`
	// грузится один раз в KZPlugin::Load, и менять N в ходе прогона через него нельзя.
	// Дефолт 1 = прежнее поведение.
	u32 Concurrency()
	{
		i32 value = kz_awr_backfill_concurrency.Get();
		if (value < 1)
		{
			value = 1;
		}
		if (value > AWR_BACKFILL_MAX_CONCURRENCY)
		{
			if (!g_concurrencyWarned)
			{
				g_concurrencyWarned = true;
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] concurrency capped requested=%d cap=%d\n", value, AWR_BACKFILL_MAX_CONCURRENCY);
			}
			value = AWR_BACKFILL_MAX_CONCURRENCY;
		}
		return (u32)value;
	}

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
	// Курсор монотонен внутри прогона, и ПОТЕРИ это не даёт — но не потому, что префикс
	// [0, курсор) якобы состоит только из виденного. Записи входят в бэклог именно В НАЧАЛО:
	// и свежий аплоад, и ПЕРЕЗАПИСЬ pb ставят created_at = now() и awr_checked_at = null
	// (apps/api/src/modules/replays/replays.service.ts, upsert pb), а сортировка выдачи —
	// created_at desc. То есть под курсор невиденные записи попадают штатно, и этот прогон
	// их не увидит. Не страшно ровно потому, что помеченными они не становятся: их возьмёт
	// автоподбор по таймеру (он ходит БЕЗ смещения, то есть как раз по началу выдачи) или
	// следующая команда. Курсор — способ дойти до хвоста истории за один прогон, а не
	// обещание обойти весь бэклог.
	size_t g_backlogOffset = 0;
	// Про потолок смещения предупреждаем один раз на прогон: шаг у прогона на каждый файл,
	// и без защёлки лог залило бы одинаковыми строками. Снимается в Run() вместе с курсором
	// — потолок и существует только у явного прогона.
	bool g_offsetCapWarned = false;
	// Защёлка на «вся первая страница уже виденная» у автоподбора. Это СТАЦИОНАРНОЕ
	// состояние, а не патология: мягкие отказы не помечаются никогда, причины отказа
	// детерминированы, а их фонд (≈7 % от 17 714 строк) заведомо больше страницы — значит
	// автоподбор будет попадать в него каждые cybAwrBackfillIntervalSec (дефолт 60) на
	// КАЖДОМ инстансе, и без защёлки лог получал бы одинаковый warn раз в минуту вечно.
	// Снимается на первом же ВЗЯТОМ файле (StartDownload): состояние изменилось.
	bool g_autoPageSeenWarned = false;

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
		// Самый большой разрыв записи внутри вырезов и его кадр — диагностика доверия
		// (awr::CutResult::maxRecordGapTicks). Печатается на КАЖДОМ файле, где метрика
		// вообще посчитана (в том числе на успехе): распределение разрывов надо видеть до
		// того, как оно испортит awr_ms. На ранних отказах (parse_failed, not_a_run, empty,
		// no_run_window, dest_not_found, counter_mismatch) подсчёта не
		// было — печатаем max_gap=n/a, а не 0@0: ноль читался бы как «разрыва нет».
		bool maxGapMeasured = false;
		u64 maxGapTicks = 0;
		u32 maxGapFrame = 0;
		// Прямая сверка тождества меры мёртвого времени: записанные не паузные кадры окна
		// против тиков таймера (awr::CutResult::timerFrames*). Расхождение сверх допуска —
		// отдельный warn с числами; отказом НЕ является (нужен живой разброс, а не отсев).
		bool timerFramesChecked = false;
		u64 timerFramesRecorded = 0;
		u64 timerFramesExpected = 0;
		bool timerFramesMismatch = false;
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

	// ОЧЕРЕДЬ результатов, а не одна ячейка: при N файлах в полёте два разбора могут
	// закончиться в одном такте, и вторая публикация затёрла бы первую.
	std::mutex g_resultMutex;
	std::vector<WorkerResult> g_results;

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
	void MaybeFetchNext();
	void StartDownload(const std::string &uuid, const std::string &url, bool dryRun);
	void SpawnWorker(const std::string &uuid, std::vector<char> data, bool dryRun);
	void PublishResult(const WorkerResult &res);
	// По значению: блокирующий инвариант шапки может понизить res.ok до отправки.
	void SendResult(WorkerResult res);

	// Попытка ВЗЯТИЯ файла не удалась на уровне БЭКЛОГА (сеть, битый json, http) —
	// освобождаем сериализованный запрос и списываем одну из запрошенных попыток. Списываем
	// и на отказе: иначе битая сеть крутила бы запрос вечно, а `count` перестал бы
	// что-либо ограничивать.
	void FinishBacklogAttempt()
	{
		g_backlogBusy = false;
		if (g_remaining > 0)
		{
			g_remaining--;
		}
	}

	// Файл в полёте закончился (результат отправлен, посчитан вхолостую или отброшен) —
	// освобождаем слот. `g_remaining` здесь НЕ трогаем: она списывается на ВЗЯТИИ файла,
	// иначе при N в полёте прогон взял бы больше `count` файлов.
	void FinishFile(const std::string &uuid)
	{
		g_takenInFlight.erase(uuid);
		if (g_inFlight > 0)
		{
			g_inFlight--;
		}
		// ПРОГОН ЗАКОНЧЕН = брать больше нечего И все полёты слились. Отдельная строка нужна
		// именно потому, что `done reason=…` печатается раньше — в момент, когда бэклог
		// исчерпан, а файлы ещё летят; при N > 1 между этими событиями секунды.
		// Только для ЯВНОГО прогона: у автоподбора «прогон» — это один файл, и он уже
		// напечатал свою строку; лишняя строка раз в минуту была бы шумом.
		if (g_explicitRun && g_remaining == 0 && g_inFlight == 0 && g_peakInFlight > 0)
		{
			KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill finished peak_in_flight=%u concurrency=%u soft=%u\n", (unsigned)g_peakInFlight,
						(unsigned)Concurrency(), (unsigned)g_softFailed.size());
			g_peakInFlight = 0;
		}
	}

	// Запросить следующий файл, если есть право брать и свободен слот. Запрос бэклога
	// сериализован (g_backlogBusy): курсор смещения живёт только в этом пути, и внахлёст
	// идущие запросы гоняли бы его наперегонки. Зовётся из завершения файла и из взятия
	// (чтобы N слотов заполнились без ожидания тика), а тик работает страховкой.
	void MaybeFetchNext()
	{
		// ЗАНЯТОСТЬ = слоты живых файлов + осиротевшие разборы прошлой эпохи. Слот живёт всю
		// жизнь файла (скачивание → разбор → POST), поэтому в нормальном прогоне файл
		// считается РОВНО ОДИН РАЗ и N слотов дают N одновременных скачиваний. Второе
		// слагаемое в норме ноль и нужно только после смены карты: там слоты гасятся, а
		// разборы прошлой эпохи ещё держат сырой буфер и распакованные тики, и без него сразу
		// после changelevel брались бы до N новых файлов ПРИ до N ещё считающихся.
		const u32 busy = g_inFlight + g_orphanResults;
		if (g_remaining == 0 || g_backlogBusy || busy >= Concurrency())
		{
			return;
		}
		FetchOne(g_explicitRun);
	}

	// ВЗЯТЬ файл: занять слот, списать попытку, начать скачивание и сразу попробовать занять
	// следующий слот — чтобы N скачиваний шли внахлёст, а не по одному за тик.
	void TakeFile(const std::string &uuid, const std::string &url, bool dryRun)
	{
		if (g_remaining > 0)
		{
			g_remaining--;
		}
		g_inFlight++;
		g_takenInFlight.insert(uuid);
		if (g_inFlight > g_peakInFlight)
		{
			g_peakInFlight = g_inFlight;
		}
		// Запрос бэклога отработал — освобождаем его для следующего слота.
		g_backlogBusy = false;
		StartDownload(uuid, url, dryRun);
		MaybeFetchNext();
	}

	// Файл ОБРАБОТАН — освобождаем слот и сразу пробуем взять следующий, не дожидаясь тика:
	// иначе на файл уходило бы два тика (взять → отдать) вместо одного.
	void FinishFileAndChain(const std::string &uuid)
	{
		FinishFile(uuid);
		MaybeFetchNext();
	}

	// Насос: держим один файл в работе, забираем результат рабочего потока.
	f64 Tick()
	{
		// Забираем ВСЕ готовые результаты: при N файлах в полёте их может накопиться
		// несколько за такт, и оставлять их в очереди до следующего тика значит терять темп.
		std::vector<WorkerResult> ready;
		{
			std::lock_guard<std::mutex> lock(g_resultMutex);
			ready.swap(g_results);
		}

		// Отправка — вне лока: колбэки HTTP и логи не должны держать мьютекс.
		for (WorkerResult &res : ready)
		{
			// Результат снят с очереди — он больше не «непотреблённый», независимо от того,
			// свой он или чужой по эпохе. Это второй конец пары к инкременту в SpawnWorker.
			if (g_unconsumedResults > 0)
			{
				g_unconsumedResults--;
			}
			else
			{
				// Результатов снято больше, чем запущено разборов: либо воркер опубликовал
				// дважды, либо очередь прочитали мимо этого места. Молчать нельзя — на этом
				// счётчике стоит снимок занятости после смены карты.
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] invariant reason=unconsumed_counter_underflow uuid=%s in_flight=%u epoch=%u\n",
							res.uuid.c_str(), (unsigned)g_inFlight, (unsigned)g_epoch);
			}

			// Результат потока, запущенного до смены карты. Разрез сам по себе верен (файл
			// от карты не зависит), но отправлять его отсюда нельзя: SendResult на всех
			// своих путях трогает общее состояние — слоты и сцепку, а на not_configured ещё
			// и g_remaining, — а это состояние принадлежит уже НОВОЙ эпохе и, возможно,
			// идущим прямо сейчас файлам. Цена отказа мала и ограничена: строка в api не
			// помечена, значит бэклог отдаст её снова, и файл досчитается в текущей эпохе.
			//
			// Слот такого файла НЕ освобождаем: OnMapChanged обнулил счётчик полётов
			// целиком, и декремент здесь уехал бы в чужой, уже новый файл.
			if (res.epoch != g_epoch)
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] callback dropped reason=stale_epoch what=worker uuid=%s epoch=%u now=%u orphans=%u\n",
							res.uuid.c_str(), (unsigned)res.epoch, (unsigned)g_epoch, (unsigned)g_orphanResults);
				// Осиротевший разбор отдал результат — освобождаем ЕГО долю занятости.
				// Снимок брался в тех же единицах (непотреблённые результаты), поэтому
				// декрементов будет ровно столько, сколько было в снимке.
				if (g_orphanResults > 0)
				{
					g_orphanResults--;
				}
				else
				{
					// Рассинхрон: чужих по эпохе результатов пришло больше, чем было в
					// снимке. Молча превращать в ноль нельзя — это означало бы, что снимок
					// в OnMapChanged врёт, и занятость слотов считается неверно.
					KZ_LOG_WARN(LogChannel::Replays,
								"[cyb_awr] invariant reason=orphan_counter_underflow uuid=%s in_flight=%u unconsumed=%u epoch=%u\n",
								res.uuid.c_str(), (unsigned)g_inFlight, (unsigned)g_unconsumedResults, (unsigned)g_epoch);
				}
				continue;
			}
			SendResult(res);
		}

		// Приборка отработавших потоков: join только по выставленному флагу, поэтому такт не
		// блокируется. Без неё хэндлы копились бы до выгрузки плагина.
		for (size_t i = 0; i < g_workerThreads.size();)
		{
			if (g_workerThreads[i].done && g_workerThreads[i].done->load())
			{
				if (g_workerThreads[i].thread.joinable())
				{
					g_workerThreads[i].thread.join();
				}
				g_workerThreads.erase(g_workerThreads.begin() + (ptrdiff_t)i);
				continue;
			}
			i++;
		}

		// Заполняем свободные слоты. Запрос бэклога сериализован, поэтому за такт уходит
		// один — остальные слоты набираются сцепкой из колбэков (взятие файла и завершение
		// файла зовут MaybeFetchNext сами), а тик остаётся страховкой на случай, когда
		// сцепка оборвалась (например отказ бэклога).
		if (g_remaining > 0 || g_inFlight > 0)
		{
			MaybeFetchNext();
			return AWR_BACKFILL_BUSY_INTERVAL;
		}

		// Холостой тик: работы нет и брать нечего — только здесь возвращается период
		// автоподбора. Пока файлы в работе, интервал обязан быть коротким, иначе результат
		// разбора пролежал бы в очереди до конца периода и темп упал бы до одного файла на
		// два периода.
		if (g_autoIntervalSec > 0)
		{
			if (g_autoDue)
			{
				// Период выждан — берём ОДИН файл, всегда «по-настоящему» (не dry-run),
				// всегда БЕЗ смещения (автоподбор существует ради свежих загрузок) и всегда
				// в одном экземпляре: конвар параллельности — про догон истории.
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
			// Сериализованный запрос освобождаем обязательно: сюда попадает и ПОВТОР из
			// колбэка (курсор сдвинулся), где флаг уже поднят, — а с поднятым флагом насос
			// молчал бы до смены карты. Тот же порядок, что на пути not_configured в
			// SendResult.
			g_remaining = 0;
			g_backlogBusy = false;
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backfill stopped reason=not_configured\n");
			return;
		}

		g_backlogBusy = true;
		// Снапшот режима на момент взятия файла — см. WorkerResult::dryRun.
		const bool dryRun = g_dryRun;
		// Снапшот эпохи: ответ может приехать уже после смены карты.
		const u32 epoch = g_epoch;

		HTTP::Request req(HTTP::Method::GET, url);
		// Одной свежайшей строки хватает, пока выбирать не из чего: обработанная помечается
		// в api и в бэклог не возвращается. Выбор нужен в dry-run (пометки нет, см.
		// g_dryRunSeen), при непустом наборе мягких отказов (они в api не помечены и приезжают
		// первыми) и при ПАРАЛЛЕЛЬНОМ прогоне: летящие файлы тоже ещё не помечены и стоят в
		// начале выдачи, а с limit=1 страница состояла бы ровно из одного такого — прогон
		// ждал бы освобождения слота и выродился в один файл за раз.
		const bool needPage = dryRun || !g_softFailed.empty() || Concurrency() > 1 || g_inFlight > 0;
		// Запрошенный лимит держим числом РЯДОМ со строкой запроса: колбэк сравнивает размер
		// страницы именно с ним (см. «страница неполная = конец бэклога»).
		const size_t limit = needPage ? AWR_BACKFILL_PAGE_LIMIT : 1;
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
			[dryRun, epoch, offset, explicitRun, limit](HTTP::Response resp)
			{
				if (StaleEpoch(epoch, "backlog", nullptr))
				{
					return;
				}
				if (resp.status < 200 || resp.status >= 300)
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog failed reason=http_%u\n", (unsigned)resp.status);
					FinishBacklogAttempt();
					return;
				}

				std::optional<std::string> body = resp.Body();
				if (!body.has_value())
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog failed reason=empty_response\n");
					FinishBacklogAttempt();
					return;
				}

				Json json(*body);
				if (!json.IsValid())
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog failed reason=bad_json\n");
					FinishBacklogAttempt();
					return;
				}

				std::vector<BacklogItem> items;
				if (!json.Get("items", items))
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog failed reason=bad_items\n");
					FinishBacklogAttempt();
					return;
				}

				if (items.empty())
				{
					// Пусто — брать больше нечего. Причины РАЗНЫЕ и различать их обязательно:
					// при offset == 0 посчитано всё, при offset > 0 за смещением строк не
					// осталось, а хвост НАЧАЛА выдачи (свежие загрузки и мягкие отказы) этот
					// прогон не смотрел — его возьмёт автоподбор или следующая команда.
					// Файлы, уже летящие в этот момент, продолжают считаться: «прогон
					// закончен» печатает FinishFile, когда сольётся последний (см. там).
					g_remaining = 0;
					g_backlogBusy = false;
					if (offset > 0)
					{
						KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill done reason=backlog_end_after_offset offset=%u in_flight=%u\n",
									(unsigned)offset, (unsigned)g_inFlight);
					}
					else
					{
						KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill done reason=empty_backlog in_flight=%u\n", (unsigned)g_inFlight);
					}
					return;
				}

				if (!dryRun)
				{
					// Пропускаем и мягко отказавших, и УЖЕ ЛЕТЯЩИХ: в api файл помечается
					// только POST'ом, поэтому до его доезда бэклог отдаёт взятый файл снова, и
					// без этой проверки N слотов взяли бы один и тот же файл N раз.
					bool blockedByInFlight = false;
					for (const BacklogItem &item : items)
					{
						if (g_softFailed.count(item.replayUuid) != 0)
						{
							continue;
						}
						if (g_takenInFlight.count(item.replayUuid) != 0)
						{
							blockedByInFlight = true;
							continue;
						}
						TakeFile(item.replayUuid, item.url, dryRun);
						return;
					}

					// Страница целиком из виденных. Если её занимают ЛЕТЯЩИЕ файлы — курсор
					// двигать НЕЛЬЗЯ: они уйдут из бэклога, как только доедет POST, выдача
					// сдвинется влево, и смещение перешагнуло бы живые строки. Просто ждём
					// освобождения слота: следующий MaybeFetchNext придёт из FinishFile (а
					// тик — страховка).
					if (blockedByInFlight)
					{
						g_backlogBusy = false;
						return;
					}

					// Дальше — страница целиком из МЯГКО ОТКАЗАВШИХ. Достижимо двумя
					// способами: их накопилось на страницу, либо во время прогона залили
					// пачку реплеев и они сдвинули набор вправо. Оба лечит один приём —
					// двигать курсор по ФАКТИЧЕСКОЙ странице и повторять запрос.
					if (explicitRun)
					{
						if (items.size() < limit)
						{
							// Страница неполная — за ней записей нет вовсе: это конец
							// бэклога, а не тупик.
							g_remaining = 0;
							g_backlogBusy = false;
							KZ_LOG_INFO(LogChannel::Replays,
										"[cyb_awr] backfill done reason=backlog_tail_seen offset=%u items=%u in_flight=%u\n", (unsigned)offset,
										(unsigned)items.size(), (unsigned)g_inFlight);
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
							g_backlogBusy = false;
							KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backfill done reason=backlog_offset_cap offset=%u soft=%u\n",
										(unsigned)offset, (unsigned)g_softFailed.size());
							return;
						}
						// Одна строка на сдвиг — не шум: сдвиг случается только на странице
						// ЦЕЛИКОМ из виденных, то есть раз на 50 таких записей, а без следа в
						// логах прошлый тупик (`soft_failed_exhausted` после 706 файлов) искать
						// было бы нечем.
						KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backlog offset advanced from=%u to=%u page=%u soft=%u\n", (unsigned)offset,
									(unsigned)next, (unsigned)items.size(), (unsigned)g_softFailed.size());
						g_backlogOffset = next;
						// Повтор в рамках ТОГО ЖЕ шага: файл не взят, поэтому ни g_remaining,
						// ни счётчик полётов не трогаем, а сериализованный запрос остаётся
						// занятым — им же и повторяем. Стека не растим: Send асинхронный.
						FetchOne(true);
						return;
					}
					// Автоподбор ходит только по первой странице (dry-run сюда не заходит):
					// его дело — свежие загрузки, догон истории — дело явного прогона.
					// Идти некуда.
					// Печатаем ОДИН раз (см. g_autoPageSeenWarned): для автоподбора это
					// стационарное состояние, и одинаковый warn раз в минуту вечно — шум, в
					// котором тонет всё остальное.
					g_remaining = 0;
					g_backlogBusy = false;
					if (!g_autoPageSeenWarned)
					{
						g_autoPageSeenWarned = true;
						KZ_LOG_WARN(LogChannel::Replays,
									"[cyb_awr] backfill done reason=soft_failed_exhausted soft=%u limit=%u items=%u latched=1\n",
									(unsigned)g_softFailed.size(), (unsigned)limit, (unsigned)items.size());
					}
					return;
				}

				// dry-run: первый, которого в этом прогоне ещё не брали.
				for (const BacklogItem &item : items)
				{
					if (g_dryRunSeen.count(item.replayUuid) == 0)
					{
						g_dryRunSeen.insert(item.replayUuid);
						TakeFile(item.replayUuid, item.url, dryRun);
						return;
					}
				}

				// Страница выбрана целиком. Дальше идти некуда: пометки в api нет, и
				// следующий запрос вернул бы ту же страницу — прогон остановится сам.
				g_remaining = 0;
				g_backlogBusy = false;
				KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill done reason=dry_run_exhausted seen=%u in_flight=%u\n",
							(unsigned)g_dryRunSeen.size(), (unsigned)g_inFlight);
			},
			[epoch]()
			{
				if (StaleEpoch(epoch, "backlog", nullptr))
				{
					return;
				}
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] backlog failed reason=network\n");
				FinishBacklogAttempt();
			});
		// clang-format on
	}

	void StartDownload(const std::string &uuid, const std::string &url, bool dryRun)
	{
		// Файл ВЗЯТ — состояние изменилось, значит следующее «вся страница виденная» у
		// автоподбора снова стоит напечатать (см. g_autoPageSeenWarned).
		g_autoPageSeenWarned = false;
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
					FinishFileAndChain(uuid);
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
				FinishFileAndChain(uuid);
			});
		// clang-format on
	}

	void SpawnWorker(const std::string &uuid, std::vector<char> data, bool dryRun)
	{
		// Разбор файла и разрез — на рабочем потоке: data::LoadCutSourceFromMemory и
		// playback::ComputeCutFor глобального состояния не трогают (см. их комментарии),
		// а распаковка нескольких мегабайт в игровом потоке дала бы просадку кадра.
		g_workersInFlight++;
		// Второй счётчик — в единицах «результат, который ещё не забрали» (см.
		// g_unconsumedResults): именно из него берётся снимок занятости на смене карты.
		g_unconsumedResults++;
		// Эпоха фиксируется ЗДЕСЬ, на главном потоке: сам поток g_epoch читать не должен.
		const u32 epoch = g_epoch;
		// Флаг «поток отработал» — для приборки хэндлов без блокировки такта (см. Tick).
		auto done = std::make_shared<std::atomic<bool>>(false);
		std::thread worker(
			[uuid, dryRun, epoch, done, data = std::move(data)]()
			{
				// Декремент строго после публикации результата: главный поток по нулю
				// решает, что в полёте никого нет (OnMapChanged).
				struct InFlightGuard
				{
					std::shared_ptr<std::atomic<bool>> done;

					~InFlightGuard()
					{
						g_workersInFlight--;
						// Флаг для приборки. Он НЕ гарантия «замыкание уничтожено» — оно
						// уничтожается позже, уже вне лямбды; гарантию даёт только join(),
						// которым и закрывается выгрузка плагина (Shutdown).
						done->store(true);
					}
				} guard {done};

				WorkerResult res;
				res.uuid = uuid;
				res.dryRun = dryRun;
				res.epoch = epoch;

				// Защита от битого файла — НЕ try/catch: форк собирается с
				// `-fno-exceptions` (AMBuildScript), исключение поймать нечем. Абсурдные
				// размеры из шапки секций (по ним аллоцируют ReadTickSection и ReadEventsCompressed)
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
				res.timerFramesChecked = cut.timerFramesChecked;
				res.timerFramesRecorded = cut.timerFramesRecorded;
				res.timerFramesExpected = cut.timerFramesExpected;
				res.timerFramesMismatch = cut.timerFramesMismatch;
				res.maxGapMeasured = cut.maxRecordGapMeasured;
				res.maxGapTicks = cut.maxRecordGapTicks;
				res.maxGapFrame = cut.maxRecordGapFrame;
				PublishResult(res);
			});
		// НЕ detach: хэндл нужен, чтобы на выгрузке плагина дождаться потока join'ом.
		g_workerThreads.push_back({std::move(worker), done});
	}

	// Отказ, который может исчезнуть после правки НАШЕГО кода, а не свойство файла. Такой
	// результат нельзя ни писать в api, ни помечать посчитанным — см. SendResult.
	// Детерминированные отказы (dest_not_found, counter_mismatch, header_tp_mismatch,
	// no_run_window, parse_failed, not_a_run, http_4xx, empty_file) сюда НЕ входят.
	bool IsSoftFailure(const char *reason)
	{
		return KZ_STREQ(reason, "awr_implausible") || KZ_STREQ(reason, "awr_record_gap");
	}

	// Рабочий поток: отдать результат главному. Файлов в полёте до N, поэтому это ОЧЕРЕДЬ:
	// два разбора, закончившиеся в одном такте, обязаны доехать оба.
	void PublishResult(const WorkerResult &res)
	{
		std::lock_guard<std::mutex> lock(g_resultMutex);
		g_results.push_back(res);
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
		// frames=<записанные не паузные>/<ожидаемые таймером>: n/a, если до сверки не дошли.
		char framesText[48];
		if (res.timerFramesChecked)
		{
			V_snprintf(framesText, sizeof(framesText), "%llu/%llu", (unsigned long long)res.timerFramesRecorded,
					   (unsigned long long)res.timerFramesExpected);
		}
		else
		{
			V_snprintf(framesText, sizeof(framesText), "n/a");
		}
		KZ_LOG_INFO(LogChannel::Replays,
					"[cyb_awr] backfill uuid=%s time_ms=%llu awr_ms=%llu tps=%u max_gap=%s frames=%s ok=%d reason=%s dry=%d%s\n",
					res.uuid.c_str(), (unsigned long long)res.timeMs, (unsigned long long)res.awrMs, (unsigned)res.teleports, maxGapText,
					framesText, res.ok ? 1 : 0, res.reason, res.dryRun ? 1 : 0, detailSuffix);

		// Сверка кадров с таймером — независимо от ok: она про сам файл, а не про разрез, и
		// на отказе тоже говорит, можно ли верить его числам. Порога-отказа тут нет
		// намеренно (см. CutResult::timerFramesChecked), поэтому единственный способ увидеть
		// класс «мёртвое завышено на 5-50 %» — читать эти строки.
		if (res.timerFramesChecked && res.timerFramesMismatch)
		{
			const i64 delta = (i64)res.timerFramesRecorded - (i64)res.timerFramesExpected;
			KZ_LOG_WARN(LogChannel::Replays,
						"[cyb_awr] invariant uuid=%s reason=timer_frames_mismatch recorded=%llu expected=%llu delta=%lld max_gap=%s\n",
						res.uuid.c_str(), (unsigned long long)res.timerFramesRecorded, (unsigned long long)res.timerFramesExpected,
						(long long)delta, maxGapText);
		}

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
			FinishFileAndChain(res.uuid);
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
			FinishFileAndChain(res.uuid);
			return;
		}

		std::string url = ApiUrl("/replays/v1/awr");
		if (url.empty())
		{
			KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] post failed uuid=%s reason=not_configured\n", res.uuid.c_str());
			// Прогон дальше не идёт, но СЛОТ освободить обязаны: иначе счётчик полётов не
			// сойдётся к нулю и насос не увидит завершения.
			g_remaining = 0;
			FinishFile(res.uuid);
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
		// бы как stale_epoch, FinishFileAndChain не вызывался бы, и слот навсегда остался бы
		// занятым: при N=1 воркер молчит до рестарта сервера.
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
				// Файл обработан (попытка списана при взятии) — сразу следующий.
				FinishFileAndChain(uuid);
			},
			[uuid, epoch]()
			{
				KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] post failed uuid=%s reason=network\n", uuid.c_str());
				if (StaleEpoch(epoch, "post", uuid.c_str()))
				{
					return;
				}
				FinishFileAndChain(uuid);
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
	// Пик — величина ЭТОГО прогона, поэтому обнуляем. Полёты прошлого прогона при этом
	// могут быть ещё живы (оператор вправе запустить команду повторно, не дожидаясь конца):
	// их слоты продолжают считаться, и в пик нового прогона они войдут — это честно, память
	// сервер держит одну на всех.
	g_peakInFlight = 0;
	// Курсор смещения и его защёлка — состояние ОДНОГО прогона.
	g_backlogOffset = 0;
	g_offsetCapWarned = false;
	// Этот прогон — явный: только ему разрешено смещение (автоподбор его сбрасывает сам).
	g_explicitRun = true;
	// Насос может быть не запущен (cybAwrBackfillIntervalSec 0) — команда обязана работать.
	EnsureTimer();
	KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] backfill started count=%u dry=%d concurrency=%u in_flight=%u\n", (unsigned)count, dryRun ? 1 : 0,
				(unsigned)Concurrency(), (unsigned)g_inFlight);
	// ПЕРВЫЙ шаг — сразу, не дожидаясь тика насоса: команда должна начинать работать в ту
	// же секунду, а не через интервал таймера, каким бы он ни был. Дальше файлы гонит
	// сцепка FinishFileAndChain, тоже без ожидания тика.
	Tick();
}

void CybAwrBackfill::Shutdown()
{
	// Выгрузка плагина. Рабочие потоки держат указатели в НАШ .so
	// (data::LoadCutSourceFromMemory, playback::ComputeCutFor, аллокаторы, деструкторы
	// шаблонов замыкания): если .so выгрузят под живым потоком, это вызов в выгруженный код,
	// то есть падение сервера.
	//
	// Ждём join'ом и БЕЗ таймаута. Ожидание счётчика g_workersInFlight окно не закрывало:
	// декремент стоит в деструкторе локальной переменной внутри лямбды, а само замыкание
	// (с копией сырого буфера) уничтожается ПОЗЖЕ, уже после возврата из лямбды. Таймаута
	// нет намеренно: выгрузиться по истечении срока значит выгрузиться под живым потоком —
	// ровно то падение, от которого мы защищаемся. Разбор ограничен размером файла (32 МБ,
	// секунды), поэтому пауза конечна; чтобы она не выглядела зависанием, пишем строку ДО
	// ожидания и после.
	g_epoch++;
	g_remaining = 0;
	g_inFlight = 0;
	g_orphanResults = 0;
	g_unconsumedResults = 0;
	g_takenInFlight.clear();
	g_backlogBusy = false;

	const int live = g_workersInFlight.load();
	const size_t handles = g_workerThreads.size();
	if (live > 0)
	{
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] shutdown waiting workers=%d handles=%zu\n", live, handles);
	}
	size_t joined = 0;
	for (WorkerHandle &handle : g_workerThreads)
	{
		if (handle.thread.joinable())
		{
			handle.thread.join();
			joined++;
		}
	}
	g_workerThreads.clear();
	if (live > 0)
	{
		// Печатаем ЧИСЛО ДОЖДАННЫХ потоков: g_workersInFlight здесь уже ноль по построению
		// (все джойны прошли), и печатать его значило бы печатать константу.
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] shutdown joined=%zu of_handles=%zu\n", joined, handles);
	}
}

void CybAwrBackfill::OnMapChanged()
{
	// ЭПОХА растёт ВСЕГДА и ПЕРВЫМ делом. HTTP-колбэки, отправленные до changelevel,
	// доезжают в любом случае — плагин на смене карты не выгружается. Любой такой колбэк,
	// сработав уже по новому состоянию, увёл бы цикл в ДВЕ параллельные цепочки: ячейка
	// g_result одна (результаты затирали бы друг друга), а g_remaining списывался бы вдвое.
	// С этой строки все они молча уходят (StaleEpoch), не трогая ни слоты, ни g_remaining,
	// ни сцепку.
	g_epoch++;

	// Слоты «файл в работе». HTTP-запрос, начатый до changelevel, может не довести ни
	// колбэк ответа, ни колбэк ошибки — а теперь ещё и сознательно отбрасывается по эпохе;
	// в обоих случаях слот остался бы занятым навсегда, и при N=1 Tick выходил бы на первой
	// же проверке: `kz_awr_backfill 1` печатает started и больше ничего не делает (наблюдено
	// на канарейке kz 0.191.0 после смены карты).
	//
	// Гасим ВСЕ полёты сразу, а не по одному: их колбэки уже отброшены по эпохе и слот сами
	// не освободят. Рабочие потоки, если они ещё живы, свои результаты опубликуют — Tick
	// отбросит их как чужие по эпохе и слот НЕ тронет (см. там), поэтому обнуление здесь
	// безопасно и с живым потоком: ячейка результата больше не одна, затирать нечего.
	if (g_inFlight > 0 || g_backlogBusy)
	{
		KZ_LOG_INFO(LogChannel::Replays,
					"[cyb_awr] map_changed reason=flights_cleared in_flight=%u orphans=%u workers=%d remaining=%u epoch=%u\n",
					(unsigned)g_inFlight, (unsigned)g_unconsumedResults, g_workersInFlight.load(), (unsigned)g_remaining, (unsigned)g_epoch);
	}
	// Снимок ОСИРОТЕВШИХ разборов: слоты гасим, но эти разборы память держат, значит держат
	// и занятость (см. g_orphanResults). Берём его из g_unconsumedResults — то есть в тех же
	// единицах, в которых он потом уменьшается (снятый с очереди чужой результат).
	// Присваивание, а не +=: после этой строки ВСЕ непотреблённые результаты принадлежат
	// прошлой эпохе, то есть осиротели все до одного.
	g_orphanResults = g_unconsumedResults;
	g_inFlight = 0;
	g_takenInFlight.clear();
	g_backlogBusy = false;
	g_peakInFlight = 0;
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
