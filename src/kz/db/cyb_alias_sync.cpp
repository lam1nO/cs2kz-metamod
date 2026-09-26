// [cyb] НАШ файл, апстрима здесь нет. См. cyb_alias_sync.h.
#include "cyb_alias_sync.h"

#include "kz/kz.h"
#include "kz/db/kz_db.h"
#include "queries/players.h"
#include "kz/option/kz_option.h"
#include "utils/ctimer.h"
#include "utils/http.h"
#include "utils/json.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "tier0/memdbgon.h"

namespace
{
	// Первый запрос — через полминуты после загрузки: БД и опции к этому времени готовы.
	constexpr f64 ALIAS_SYNC_FIRST_DELAY = 30.0;
	// Шаг насоса. Запрос уходит раз в ALIAS_SYNC_PERIOD_TICKS шагов (5 мин: платформа меняет
	// ники не чаще прохода по живым сессиям и рефреша Steam, пять минут задержки в топе никого
	// не смутят), а при догоне — ответ упёрся в потолок api — на следующем же шаге.
	constexpr f64 ALIAS_SYNC_TICK = 5.0;
	constexpr u32 ALIAS_SYNC_PERIOD_TICKS = 60;
	constexpr u32 ALIAS_SYNC_STUCK_TICKS = 24;
	// Потолок строк одного ответа (NAMES_DELTA_LIMIT в api). Полный ответ = «есть ещё».
	constexpr size_t ALIAS_SYNC_API_LIMIT = 2000;
	// UPDATE'ов в одной транзакции: сбойная строка (ник, который БД не приняла) роняет только
	// свою пачку, а не весь синк.
	constexpr size_t ALIAS_SYNC_TXN_CHUNK = 50;
	// Alias — VARCHAR(32) в MySQL-схеме форка (queries/players.h); длина в СИМВОЛАХ.
	constexpr size_t ALIAS_MAX_CHARS = 32;

	// Курсор дельты от платформы, непрозрачный. Пустой — «с начала окна» (последние 7 суток):
	// так после рестарта плагина мы переигрываем недавние смены — UPDATE идемпотентен.
	std::string g_cursor;
	bool g_requestInFlight = false;
	bool g_catchUp = false;
	// Шагов с прошлого запроса; стартует «просроченным», чтобы первый запрос ушёл сразу.
	u32 g_ticksSinceRequest = ALIAS_SYNC_PERIOD_TICKS;

	// Обрезка до N кодовых точек UTF-8 (не байт — иначе рвали бы кириллицу посреди символа).
	std::string TruncateUtf8(const std::string &text, size_t maxChars)
	{
		size_t chars = 0;
		for (size_t i = 0; i < text.size(); i++)
		{
			if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80)
			{
				if (chars == maxChars)
				{
					return text.substr(0, i);
				}
				chars++;
			}
		}
		return text;
	}

	void ApplyNames(const std::vector<std::pair<u64, std::string>> &names)
	{
		ISQLConnection *db = KZDatabaseService::GetDatabaseConnection();
		if (!db)
		{
			return;
		}
		for (size_t start = 0; start < names.size(); start += ALIAS_SYNC_TXN_CHUNK)
		{
			Transaction txn;
			size_t end = start + ALIAS_SYNC_TXN_CHUNK < names.size() ? start + ALIAS_SYNC_TXN_CHUNK : names.size();
			for (size_t i = start; i < end; i++)
			{
				std::string escaped = db->Escape(TruncateUtf8(names[i].second, ALIAS_MAX_CHARS).c_str());
				char query[1024];
				V_snprintf(query, sizeof(query), sql_players_update_alias, escaped.c_str(), (i64)names[i].first);
				txn.queries.push_back(query);
			}
			u32 count = (u32)(end - start);
			auto onFailure = [count](std::string error, int failIndex)
			{ KZ_LOG_WARN(LogChannel::DB, "[cyb] alias_sync_fail reason=db_txn rows=%u at=%d error=%s\n", count, failIndex, error.c_str()); };
			db->ExecuteTransaction(txn, [](std::vector<ISQLQuery *>) {}, onFailure);
		}
	}

	void OnResponse(HTTP::Response response)
	{
		g_requestInFlight = false;
		if (response.status < 200 || response.status >= 300)
		{
			KZ_LOG_WARN(LogChannel::DB, "[cyb] alias_sync_fail reason=http_%u\n", (u32)response.status);
			if (response.status == 400)
			{
				g_cursor.clear(); // битый курсор — начинаем с окна заново
			}
			return;
		}
		nlohmann::json json = nlohmann::json::parse(response.Body().value_or(""), nullptr, false);
		if (json.is_discarded() || !json.is_object() || !json.contains("next") || !json["next"].is_string() || !json.contains("names")
			|| !json["names"].is_array())
		{
			KZ_LOG_WARN(LogChannel::DB, "[cyb] alias_sync_fail reason=bad_body\n");
			return;
		}
		std::vector<std::pair<u64, std::string>> names;
		const auto &items = json["names"];
		for (const auto &item : items)
		{
			if (!item.is_object() || !item.contains("steamId64") || !item["steamId64"].is_string() || !item.contains("name")
				|| !item["name"].is_string())
			{
				continue;
			}
			const std::string &sid = item["steamId64"].get_ref<const std::string &>();
			char *endPtr = nullptr;
			u64 steamId64 = strtoull(sid.c_str(), &endPtr, 10);
			const std::string &name = item["name"].get_ref<const std::string &>();
			if (steamId64 == 0 || !endPtr || *endPtr != '\0' || name.empty())
			{
				continue;
			}
			names.emplace_back(steamId64, name);
		}
		// Курсор двигаем сразу: сбойная пачка UPDATE'ов не должна крутить один и тот же ответ
		// вечно, а ник, который БД не приняла, не примет и повтор.
		g_cursor = json["next"].get<std::string>();
		g_catchUp = items.size() >= ALIAS_SYNC_API_LIMIT;
		if (!names.empty())
		{
			ApplyNames(names);
			KZ_LOG_INFO(LogChannel::DB, "[cyb] alias_sync applied=%u more=%d\n", (u32)names.size(), g_catchUp ? 1 : 0);
		}
	}

	f64 Tick()
	{
		if (g_ticksSinceRequest < ALIAS_SYNC_PERIOD_TICKS)
		{
			g_ticksSinceRequest++;
		}
		// HTTP-колбэк может не прийти вовсе (смена карты, см. cyb_awr_backfill) — без сброса
		// насос замолчал бы до рестарта. Две минуты тишины = запрос потерян.
		if (g_requestInFlight && g_ticksSinceRequest >= ALIAS_SYNC_STUCK_TICKS)
		{
			g_requestInFlight = false;
			KZ_LOG_WARN(LogChannel::DB, "[cyb] alias_sync_fail reason=no_callback\n");
		}
		if (g_requestInFlight || !KZDatabaseService::IsReady() || (!g_catchUp && g_ticksSinceRequest < ALIAS_SYNC_PERIOD_TICKS))
		{
			return ALIAS_SYNC_TICK;
		}
		std::string apiURL = KZOptionService::GetOptionStr("cybEmitUrl", "");
		if (apiURL.empty())
		{
			return ALIAS_SYNC_TICK; // платформа не сконфигурирована — живём на нике с захода
		}
		if (apiURL.back() == '/')
		{
			apiURL.pop_back();
		}
		std::string url = apiURL + "/ingest/v1/players/names";
		if (!g_cursor.empty())
		{
			// Курсор — цифры и ':', в URL безопасен как есть.
			url += "?since=" + g_cursor;
		}
		HTTP::Request request(HTTP::Method::GET, url);
		const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");
		if (token && token[0] != '\0')
		{
			request.SetHeader("Authorization", std::string("Bearer ") + token);
		}
		g_requestInFlight = true;
		g_catchUp = false;
		g_ticksSinceRequest = 0;
		request.Send(OnResponse,
					 []()
					 {
						 g_requestInFlight = false;
						 KZ_LOG_WARN(LogChannel::DB, "[cyb] alias_sync_fail reason=network\n");
					 });
		return ALIAS_SYNC_TICK;
	}
} // namespace

void CybAliasSync::Init()
{
	StartTimer(Tick, ALIAS_SYNC_FIRST_DELAY, true, true);
}
