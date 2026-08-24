#include "kz_outbox.h"

#include "utils/utils.h"
#include "utils/http.h"
#include "utils/ctimer.h"
#include "utils/async_file_io.h"
#include "utils/json.h"
#include "utils/uuid.h"
#include "kz/option/kz_option.h"
#include "kz/db/kz_db.h"
#include "kz/db/queries/times.h"
#include "kz/timer/submission.h"
#include "kz/replays/kz_replay.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unordered_set>

#define KZ_OUTBOX_PATH      "cyb_outbox"
#define KZ_OUTBOX_DEAD_PATH KZ_OUTBOX_PATH "/dead"

// Ретраер не трогает файлы моложе 30 с — их ещё пытается доставить онлайн-путь.
static_global constexpr f64 KZ_OUTBOX_TICK_INTERVAL = 60.0;
static_global constexpr i64 KZ_OUTBOX_MIN_AGE_SECONDS = 30;
// За тик обрабатываем не больше стольких файлов — не душим кадр отправками.
static_global constexpr u32 KZ_OUTBOX_MAX_PER_TICK = 20;
// ...и просматриваем не больше стольких записей каталога: сам скан (directory_iterator +
// last_write_time на файл) тоже идёт в игровом потоке, а после недельного обрыва в
// очереди могут лежать тысячи файлов — обрываем итерацию, хвост дойдёт следующими тиками.
static_global constexpr u32 KZ_OUTBOX_MAX_SCAN_PER_TICK = 200;

// Имена файлов, по которым отправка уже идёт (HTTP в полёте / транзакция БД /
// чтение реплея). Ретраер такие пропускает — защита от двойной отправки между тиками.
static_global std::unordered_set<std::string> g_outboxInFlight;

// ---------------------------------------------------------------------------
// Пути и файлы очереди
// ---------------------------------------------------------------------------

// Путь файла очереди относительно csgo/ (такой ждут utils::*File-хелперы).
static_function std::string QueueRelPath(const std::string &name)
{
	return std::string(KZ_OUTBOX_PATH "/") + name;
}

static_function void QueueAbsPath(char *buf, int bufLen, const char *relDir)
{
	V_snprintf(buf, bufLen, "%s/csgo/%s", Plat_GetGameDirectory(), relDir);
}

static_function bool QueueFileExists(const std::string &name)
{
	char absPath[1024];
	QueueAbsPath(absPath, sizeof(absPath), QueueRelPath(name).c_str());
	std::error_code ec;
	return std::filesystem::exists(absPath, ec);
}

static_function bool WriteQueueFile(const std::string &name, const std::string &content)
{
	std::vector<char> buffer(content.begin(), content.end());
	// WriteBufferToFile пишет атомарно (tmp + rename) и сам создаёт каталог.
	if (!utils::WriteBufferToFile(QueueRelPath(name).c_str(), buffer))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb_outbox] enqueue failed file=%s reason=write_failed\n", name.c_str());
		return false;
	}
	return true;
}

// Сток подтверждён или признан ненужным — снимаем write-ahead файл.
// verb: "flush" (доставлено) либо "drop" (доставка не нужна/невозможна).
static_function void FinishQueueFile(const char *verb, const char *kind, const std::string &runUuid, const std::string &name, const char *reason)
{
	if (!QueueFileExists(name))
	{
		return;
	}
	utils::RemoveFile(QueueRelPath(name).c_str());
	KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] %s kind=%s run=%s reason=%s\n", verb, kind, runUuid.c_str(), reason);
}

// Ответ api 4xx — платформа отвергла навсегда, ретраи бессмысленны: в dead/ на разбор руками.
static_function void MoveToDead(const char *kind, const std::string &runUuid, const std::string &name, const char *reason, u32 status)
{
	if (!utils::RenameFile(QueueRelPath(name).c_str(), (std::string(KZ_OUTBOX_DEAD_PATH "/") + name).c_str()))
	{
		// Каталог dead/ кто-то снёс на живом сервере — файл остаётся в очереди, увидим по логу.
		KZ_LOG_WARN(LogChannel::General, "[cyb_outbox] dead-move failed file=%s\n", name.c_str());
		return;
	}
	KZ_LOG_WARN(LogChannel::General, "[cyb_outbox] drop kind=%s run=%s reason=%s status=%u dest=dead\n", kind, runUuid.c_str(), reason, status);
}

static_function bool HasSuffix(const std::string &s, const char *suffix)
{
	size_t sufLen = strlen(suffix);
	return s.size() >= sufLen && s.compare(s.size() - sufLen, sufLen, suffix) == 0;
}

// ---------------------------------------------------------------------------
// Общие HTTP-мелочи (тот же конфиг, что у cyb_emitter/cyb_records)
// ---------------------------------------------------------------------------

// Полный URL эндпоинта api; пустая строка = платформа не сконфигурирована.
static_function std::string ApiUrl(const char *path)
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

static_function void SetAuthHeader(HTTP::Request &req)
{
	const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");
	if (token && token[0] != '\0')
	{
		req.SetHeader("Authorization", std::string("Bearer ") + token);
	}
}

// ---------------------------------------------------------------------------
// Сериализация .replay.meta
// ---------------------------------------------------------------------------

static_function std::string SerializeReplayMeta(const KZOutboxService::ReplayMeta &meta)
{
	Json json;
	json.Set("runUuid", meta.runUuid);
	json.Set("steamId64", meta.steamId64);
	json.Set("map", meta.map);
	json.Set("course", (u64)meta.course);
	json.Set("mode", meta.mode);
	json.Set("type", std::string("pb"));
	json.Set("isServerRecord", meta.isServerRecord);
	json.Set("unconfirmed_pb", meta.unconfirmedPb);
	json.Set("timeMs", meta.timeMs);
	json.Set("replayPath", meta.replayPath);
	return json.ToString();
}

static_function bool ParseReplayMeta(const std::string &content, KZOutboxService::ReplayMeta &out)
{
	Json json(content);
	if (!json.IsValid())
	{
		return false;
	}
	u64 course = 0;
	// clang-format off
	bool ok = json.Get("runUuid", out.runUuid)
		&& json.Get("steamId64", out.steamId64)
		&& json.Get("map", out.map)
		&& json.Get("course", course)
		&& json.Get("mode", out.mode)
		&& json.Get("isServerRecord", out.isServerRecord)
		&& json.Get("unconfirmed_pb", out.unconfirmedPb)
		&& json.Get("timeMs", out.timeMs)
		&& json.Get("replayPath", out.replayPath);
	// clang-format on
	out.course = (i32)course;
	return ok;
}

// Одна запись из GET /ingest/v1/kz/records (тот же контракт, что читает cyb_records в kz_timer.cpp).
struct PlatformRecordJson
{
	f64 course {};
	std::string mode;
	std::optional<f64> pbTimeMs;
	std::optional<f64> wrTimeMs;

	bool FromJson(const Json &json)
	{
		// pb/wr опциональны: у записи может быть только одно из двух.
		return json.Get("course", this->course) && json.Get("mode", this->mode) && json.Get("pbTimeMs", this->pbTimeMs)
			   && json.Get("wrTimeMs", this->wrTimeMs);
	}
};

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void KZOutboxService::Init()
{
	char absPath[1024];
	QueueAbsPath(absPath, sizeof(absPath), KZ_OUTBOX_DEAD_PATH);
	std::error_code ec;
	// Создаёт и родительский cyb_outbox/.
	std::filesystem::create_directories(absPath, ec);
	if (ec)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb_outbox] init failed reason=create_dir path=%s error=%s\n", absPath, ec.message().c_str());
	}
	StartTimer(KZOutboxService::Tick, KZ_OUTBOX_TICK_INTERVAL, true, true);
}

// ---------------------------------------------------------------------------
// Write-ahead
// ---------------------------------------------------------------------------

void KZOutboxService::EnqueueEvent(const std::string &runUuid, const std::string &body)
{
	if (WriteQueueFile(runUuid + ".event.json", body))
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] enqueue kind=event run=%s\n", runUuid.c_str());
	}
}

void KZOutboxService::EnqueueTimeInsert(const std::string &runUuid, u64 steamID64, u32 courseID, i32 modeID, f64 time, u64 teleports, u64 styleIDs,
										const std::string &metadata)
{
	// Тот же рендер, что в save_time.cpp: единый шаблон sql_times_insert (queries/times.h).
	char query[2048];
	V_snprintf(query, sizeof(query), sql_times_insert, runUuid.c_str(), steamID64, courseID, modeID, styleIDs, time, teleports, metadata.c_str());

	// IGNORE-семантика: Times.ID — PRIMARY KEY, повторная вставка при ретрае
	// (или после ретрая, догнанного онлайн-путём) должна быть безвредной.
	std::string q = query;
	size_t pos = q.find("INSERT INTO");
	if (pos == std::string::npos)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb_outbox] enqueue failed kind=sql run=%s reason=render_failed\n", runUuid.c_str());
		return;
	}
	bool mysql = KZDatabaseService::GetDatabaseType() == KZ::Database::DatabaseType::MySQL;
	q.replace(pos, strlen("INSERT INTO"), mysql ? "INSERT IGNORE INTO" : "INSERT OR IGNORE INTO");

	if (WriteQueueFile(runUuid + ".sql", q))
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] enqueue kind=sql run=%s\n", runUuid.c_str());
	}
}

void KZOutboxService::EnqueueReplayMeta(const ReplayMeta &meta)
{
	if (WriteQueueFile(meta.runUuid + ".replay.meta", SerializeReplayMeta(meta)))
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] enqueue kind=replay run=%s unconfirmed_pb=%d server_record=%d\n", meta.runUuid.c_str(),
					(int)meta.unconfirmedPb, (int)meta.isServerRecord);
	}
}

// ---------------------------------------------------------------------------
// Подтверждения
// ---------------------------------------------------------------------------

void KZOutboxService::AckEvent(const std::string &runUuid, const char *reason)
{
	FinishQueueFile("flush", "event", runUuid, runUuid + ".event.json", reason);
}

void KZOutboxService::AckSql(const std::string &runUuid, const char *reason)
{
	FinishQueueFile("flush", "sql", runUuid, runUuid + ".sql", reason);
}

void KZOutboxService::AckReplay(const std::string &runUuid, const char *reason)
{
	FinishQueueFile("flush", "replay", runUuid, runUuid + ".replay.meta", reason);
}

void KZOutboxService::DropReplay(const std::string &runUuid, const char *reason)
{
	FinishQueueFile("drop", "replay", runUuid, runUuid + ".replay.meta", reason);
}

void KZOutboxService::RenameReplayMeta(const std::string &oldUuid, const std::string &newUuid)
{
	std::string oldName = oldUuid + ".replay.meta";
	if (!QueueFileExists(oldName))
	{
		return;
	}
	std::vector<char> buffer;
	if (!utils::ReadBufferFromFile(QueueRelPath(oldName).c_str(), buffer))
	{
		return;
	}
	ReplayMeta meta;
	if (!ParseReplayMeta(std::string(buffer.begin(), buffer.end()), meta))
	{
		MoveToDead("replay", oldUuid, oldName, "bad_meta", 0);
		return;
	}
	meta.runUuid = newUuid;
	meta.replayPath = std::string(KZ_REPLAY_PATH "/") + newUuid + ".replay";
	EnqueueReplayMeta(meta);
	utils::RemoveFile(QueueRelPath(oldName).c_str());
	KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] rename kind=replay run=%s new=%s reason=late_api_uuid\n", oldUuid.c_str(), newUuid.c_str());
}

// ---------------------------------------------------------------------------
// Отправка: событие ingest
// ---------------------------------------------------------------------------

void KZOutboxService::SendEvent(const std::string &runUuid, const std::string &body)
{
	std::string fileName = runUuid + ".event.json";
	std::string url = ApiUrl("/ingest/v1/events");
	if (url.empty())
	{
		return; // платформа не сконфигурирована — файл остаётся до TTL
	}
	g_outboxInFlight.insert(fileName);

	HTTP::Request req(HTTP::Method::POST, url);
	req.SetHeader("Content-Type", "application/json");
	SetAuthHeader(req);
	req.SetBody(body);

	// clang-format off
	req.Send(
		[runUuid, fileName](HTTP::Response resp)
		{
			g_outboxInFlight.erase(fileName);
			if (resp.status >= 200 && resp.status < 300)
			{
				AckEvent(runUuid, "ingest_2xx");
			}
			else if (resp.status >= 400 && resp.status < 500)
			{
				MoveToDead("event", runUuid, fileName, "rejected", resp.status);
			}
			else
			{
				// 5xx и прочее — временное, остаёмся в очереди
				KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] event send HTTP %u run=%s (stays queued)\n", (unsigned)resp.status, runUuid.c_str());
			}
		},
		[runUuid, fileName]()
		{
			g_outboxInFlight.erase(fileName);
			KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] event send network error run=%s (stays queued)\n", runUuid.c_str());
		});
	// clang-format on
}

// ---------------------------------------------------------------------------
// Отправка: отложенный INSERT в Times (только ретраер, см. заголовок)
// ---------------------------------------------------------------------------

void KZOutboxService::SendTimeInsert(const std::string &runUuid, const std::string &query)
{
	if (!KZDatabaseService::IsReady())
	{
		return; // БД так и не поднялась — файл остаётся до TTL/следующего тика
	}
	std::string fileName = runUuid + ".sql";
	g_outboxInFlight.insert(fileName);

	Transaction txn;
	txn.queries.push_back(query);
	// clang-format off
	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn,
		[runUuid, fileName](std::vector<ISQLQuery *>)
		{
			g_outboxInFlight.erase(fileName);
			AckSql(runUuid, "db_txn_ok");
		},
		[runUuid, fileName](std::string error, int)
		{
			g_outboxInFlight.erase(fileName);
			// Duplicate-key = вставка уже есть (файлы старых версий без IGNORE-семантики) — успех.
			if (error.find("Duplicate entry") != std::string::npos || error.find("UNIQUE constraint failed") != std::string::npos)
			{
				AckSql(runUuid, "duplicate");
				return;
			}
			KZ_LOG_WARN(LogChannel::General, "[cyb_outbox] sql retry failed run=%s reason=%s (stays queued)\n", runUuid.c_str(), error.c_str());
		});
	// clang-format on
}

// ---------------------------------------------------------------------------
// Отправка: центральный реплей
// ---------------------------------------------------------------------------

// Один POST /replays/v1/upload заданного типа (pb/wr); done(status), status=0 — сетевая ошибка.
static_function void PostReplay(const KZOutboxService::ReplayMeta &meta, const std::shared_ptr<std::string> &body, const char *type,
								std::function<void(u32 status)> done)
{
	std::string url = ApiUrl("/replays/v1/upload");
	HTTP::Request req(HTTP::Method::POST, url);
	req.SetQuery("steamId64", std::to_string(meta.steamId64));
	req.SetQuery("map", meta.map);
	req.SetQuery("course", std::to_string(meta.course));
	req.SetQuery("mode", meta.mode);
	req.SetQuery("type", type);
	req.SetQuery("replayUuid", meta.runUuid);
	req.SetHeader("Content-Type", "application/octet-stream");
	SetAuthHeader(req);
	// std::string корректно хранит бинарные данные с внутренними '\0' — длина
	// берётся из .size() (SetHTTPRequestRawPostBody в utils/http.cpp).
	req.SetBody(*body);
	req.Send([done](HTTP::Response resp) { done((u32)resp.status); }, [done]() { done(0); });
}

void KZOutboxService::SendReplay(const ReplayMeta &meta, const std::vector<char> &buffer)
{
	std::string fileName = meta.runUuid + ".replay.meta";
	if (buffer.size() >= KZOutboxService::maxReplayBytes)
	{
		DropReplay(meta.runUuid, "too_large");
		return;
	}
	std::string url = ApiUrl("/replays/v1/upload");
	if (url.empty())
	{
		return;
	}
	g_outboxInFlight.insert(fileName);

	auto body = std::make_shared<std::string>(buffer.begin(), buffer.end());
	// clang-format off
	PostReplay(meta, body, "pb",
		[meta, body, fileName](u32 status)
		{
			if (status == 0)
			{
				g_outboxInFlight.erase(fileName);
				KZ_LOG_INFO(LogChannel::Replays, "[cyb_outbox] replay upload (pb) network error run=%s (stays queued)\n", meta.runUuid.c_str());
				return;
			}
			if (status >= 400 && status < 500)
			{
				g_outboxInFlight.erase(fileName);
				MoveToDead("replay", meta.runUuid, fileName, "rejected", status);
				return;
			}
			if (status < 200 || status >= 300)
			{
				g_outboxInFlight.erase(fileName);
				KZ_LOG_INFO(LogChannel::Replays, "[cyb_outbox] replay upload (pb) HTTP %u run=%s (stays queued)\n", status, meta.runUuid.c_str());
				return;
			}
			if (!meta.isServerRecord)
			{
				g_outboxInFlight.erase(fileName);
				AckReplay(meta.runUuid, "upload_2xx");
				return;
			}
			// WR — дополнительный независимый POST для локального рекордсмена сервера.
			// pb уже доехал; при ретрае pb перезальётся (upsert по тому же ключу) — безвредно.
			PostReplay(meta, body, "wr",
				[meta, fileName](u32 status)
				{
					g_outboxInFlight.erase(fileName);
					if (status == 0)
					{
						KZ_LOG_INFO(LogChannel::Replays, "[cyb_outbox] replay upload (wr) network error run=%s (stays queued)\n", meta.runUuid.c_str());
						return;
					}
					if (status >= 400 && status < 500)
					{
						MoveToDead("replay", meta.runUuid, fileName, "rejected_wr", status);
						return;
					}
					if (status < 200 || status >= 300)
					{
						KZ_LOG_INFO(LogChannel::Replays, "[cyb_outbox] replay upload (wr) HTTP %u run=%s (stays queued)\n", status, meta.runUuid.c_str());
						return;
					}
					AckReplay(meta.runUuid, "upload_2xx");
				});
		});
	// clang-format on
}

// ---------------------------------------------------------------------------
// Ретраер
// ---------------------------------------------------------------------------

f64 KZOutboxService::Tick()
{
	ProcessQueue();
	return KZ_OUTBOX_TICK_INTERVAL;
}

void KZOutboxService::ProcessQueue()
{
	namespace fs = std::filesystem;

	char absDir[1024];
	QueueAbsPath(absDir, sizeof(absDir), KZ_OUTBOX_PATH);

	std::error_code ec;
	const auto now = fs::file_time_type::clock::now();
	const i64 ttlHours = KZOptionService::GetOptionInt("cybOutboxTtlHours", 168);

	u32 pending = 0;
	u32 processed = 0;
	u32 scanned = 0;
	bool scanTruncated = false;
	for (const auto &entry : fs::directory_iterator(absDir, ec))
	{
		if (++scanned > KZ_OUTBOX_MAX_SCAN_PER_TICK)
		{
			scanTruncated = true; // pending в логе ниже становится нижней оценкой («+»)
			break;
		}
		std::error_code entryEc;
		if (!entry.is_regular_file(entryEc))
		{
			continue; // dead/ и прочие каталоги
		}
		std::string name = entry.path().filename().string();
		// .tmp — недописанный WriteBufferToFile (например, после краша посреди записи).
		if (name.find(".tmp") != std::string::npos)
		{
			continue;
		}

		const char *kind = nullptr;
		if (HasSuffix(name, ".event.json"))
		{
			kind = "event";
		}
		else if (HasSuffix(name, ".sql"))
		{
			kind = "sql";
		}
		else if (HasSuffix(name, ".replay.meta"))
		{
			kind = "replay";
		}
		else
		{
			continue; // чужой файл — не трогаем
		}

		auto mtime = fs::last_write_time(entry.path(), entryEc);
		if (entryEc)
		{
			continue;
		}
		auto age = now - mtime;
		if (age < std::chrono::seconds(KZ_OUTBOX_MIN_AGE_SECONDS))
		{
			continue; // слишком свежий — не гоняемся с онлайн-путём (и не считаем застрявшим)
		}
		pending++;

		if (processed >= KZ_OUTBOX_MAX_PER_TICK)
		{
			continue; // только досчитываем pending для лога
		}
		if (g_outboxInFlight.count(name))
		{
			continue; // отправка предыдущего тика ещё в полёте
		}

		std::string runUuid = name.substr(0, name.find('.'));

		if (age > std::chrono::hours(ttlHours))
		{
			processed++;
			utils::RemoveFile(QueueRelPath(name).c_str());
			KZ_LOG_WARN(LogChannel::General, "[cyb_outbox] drop kind=%s run=%s reason=expired ttl_hours=%lld\n", kind, runUuid.c_str(),
						(long long)ttlHours);
			continue;
		}

		// Онлайн-путь этого рана ещё жив (например, ждёт очереди глобального API) —
		// не соревнуемся с ним; после смены карты RunSubmission::Clear() снимет блок.
		UUID_t uuid;
		if (UUID_t::FromString(runUuid.c_str(), &uuid) && RunSubmission::GetByUUID(uuid))
		{
			continue;
		}

		processed++;
		if (KZ_STREQ(kind, "event"))
		{
			RetryEventFile(name, runUuid);
		}
		else if (KZ_STREQ(kind, "sql"))
		{
			RetrySqlFile(name, runUuid);
		}
		else
		{
			RetryReplayFile(name, runUuid);
		}
	}
	if (ec)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb_outbox] scan failed reason=%s\n", ec.message().c_str());
		return;
	}
	if (pending > 0 || scanTruncated)
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] tick pending=%u%s processed=%u\n", pending, scanTruncated ? "+" : "", processed);
	}
}

void KZOutboxService::RetryEventFile(const std::string &name, const std::string &runUuid)
{
	std::vector<char> buffer;
	if (!utils::ReadBufferFromFile(QueueRelPath(name).c_str(), buffer) || buffer.empty())
	{
		return;
	}
	KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] retry kind=event run=%s\n", runUuid.c_str());
	SendEvent(runUuid, std::string(buffer.begin(), buffer.end()));
}

void KZOutboxService::RetrySqlFile(const std::string &name, const std::string &runUuid)
{
	if (!KZDatabaseService::IsReady())
	{
		return;
	}
	std::vector<char> buffer;
	if (!utils::ReadBufferFromFile(QueueRelPath(name).c_str(), buffer) || buffer.empty())
	{
		return;
	}
	KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] retry kind=sql run=%s\n", runUuid.c_str());
	SendTimeInsert(runUuid, std::string(buffer.begin(), buffer.end()));
}

void KZOutboxService::RetryReplayFile(const std::string &name, const std::string &runUuid)
{
	std::vector<char> buffer;
	if (!utils::ReadBufferFromFile(QueueRelPath(name).c_str(), buffer) || buffer.empty())
	{
		return;
	}
	ReplayMeta meta;
	if (!ParseReplayMeta(std::string(buffer.begin(), buffer.end()), meta))
	{
		MoveToDead("replay", runUuid, name, "bad_meta", 0);
		return;
	}
	// runUuid внутри меты обязан совпадать с именем файла: по нему строятся ключи
	// in-flight/ack, и расхождение (файл руками переименовали) зависило бы очередь.
	if (meta.runUuid + ".replay.meta" != name)
	{
		MoveToDead("replay", runUuid, name, "uuid_mismatch", 0);
		return;
	}
	std::string url = ApiUrl("/ingest/v1/kz/records");
	if (url.empty())
	{
		return;
	}
	KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] retry kind=replay run=%s unconfirmed_pb=%d\n", runUuid.c_str(), (int)meta.unconfirmedPb);
	g_outboxInFlight.insert(name);

	// Перед аплоадом сверяемся с платформой — И для unconfirmed-мет (БД не подтвердила PB),
	// И для confirmed-остатков после краша: за время простоя игрок мог поставить более
	// свежий PB/WR, а платформенный upload — upsert по ключу и перетёр бы его вслепую.
	HTTP::Request req(HTTP::Method::GET, url);
	req.SetQuery("map", meta.map);
	req.SetQuery("steamId64", std::to_string(meta.steamId64));
	SetAuthHeader(req);

	// clang-format off
	req.Send(
		[meta, name](HTTP::Response resp)
		{
			if (resp.status < 200 || resp.status >= 300)
			{
				g_outboxInFlight.erase(name);
				KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] pb check HTTP %u run=%s (stays queued)\n", (unsigned)resp.status, meta.runUuid.c_str());
				return;
			}
			std::optional<std::string> body = resp.Body();
			if (!body.has_value())
			{
				g_outboxInFlight.erase(name);
				return;
			}
			Json json(*body);
			std::vector<PlatformRecordJson> records;
			if (!json.IsValid() || !json.Get("records", records))
			{
				g_outboxInFlight.erase(name);
				KZ_LOG_WARN(LogChannel::General, "[cyb_outbox] pb check parse failed run=%s (stays queued)\n", meta.runUuid.c_str());
				return;
			}
			ReplayMeta adjusted = meta;
			for (const PlatformRecordJson &rec : records)
			{
				if ((i32)rec.course != meta.course || rec.mode != meta.mode)
				{
					continue;
				}
				// Платформа уже знает время лучше нашего — этот реплей устарел, грузить нечего.
				if (rec.pbTimeMs.has_value() && (f64)meta.timeMs > *rec.pbTimeMs + 0.5)
				{
					g_outboxInFlight.erase(name);
					DropReplay(meta.runUuid, "superseded");
					return;
				}
				// WR на платформе свежее нашего — pb грузим, wr не перетираем.
				if (adjusted.isServerRecord && rec.wrTimeMs.has_value() && (f64)meta.timeMs > *rec.wrTimeMs + 0.5)
				{
					adjusted.isServerRecord = false;
				}
				break;
			}
			RetryReplayRead(adjusted);
		},
		[name, meta]()
		{
			g_outboxInFlight.erase(name);
			KZ_LOG_INFO(LogChannel::General, "[cyb_outbox] pb check network error run=%s (stays queued)\n", meta.runUuid.c_str());
		});
	// clang-format on
}

void KZOutboxService::RetryReplayRead(const ReplayMeta &meta)
{
	std::string fileName = meta.runUuid + ".replay.meta";
	if (!g_asyncFileIO)
	{
		g_outboxInFlight.erase(fileName);
		return;
	}
	ReplayMeta metaCopy = meta;
	g_asyncFileIO->QueueRead(meta.replayPath,
							 [metaCopy, fileName](bool success, std::vector<char> &&buffer)
							 {
								 g_outboxInFlight.erase(fileName);
								 if (!success || buffer.empty())
								 {
									 // Файл реплея не нашёлся (переименован поздним ответом API или удалён
									 // ретенцией) — оставляем мету до TTL, вдруг файл появится после rename.
									 KZ_LOG_WARN(LogChannel::General, "[cyb_outbox] retry kind=replay run=%s reason=replay_file_missing path=%s\n",
												 metaCopy.runUuid.c_str(), metaCopy.replayPath.c_str());
									 return;
								 }
								 SendReplay(metaCopy, buffer);
							 });
}
