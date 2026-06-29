#include "cyb_emitter.h"

#include "utils/http.h"
#include "utils/utils.h"
#include "utils/uuid.h"
#include "kz/option/kz_option.h"

#include <chrono>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Вспомогательные функции
// ---------------------------------------------------------------------------

// Маппинг cs2kz short mode name → наш API
// CKZ→ckz, VNL→vnl, KZT→kzt; всё остальное — пустая строка (не шлём).
static const char *MapMode(const std::string &shortName)
{
	if (shortName == "CKZ" || shortName == "ckz")
		return "ckz";
	if (shortName == "VNL" || shortName == "vnl")
		return "vnl";
	if (shortName == "KZT" || shortName == "kzt")
		return "kzt";
	return "";
}

// ISO-8601 UTC timestamp: "2026-06-30T12:00:00.000Z"
static void FormatISO8601(char *buf, size_t bufLen)
{
	using namespace std::chrono;
	auto now = system_clock::now();
	auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
	std::time_t t = system_clock::to_time_t(now);
	struct tm utc {};
#if defined(_WIN32)
	gmtime_s(&utc, &t);
#else
	gmtime_r(&t, &utc);
#endif
	char base[32];
	strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc);
	snprintf(buf, bufLen, "%s.%03ldZ", base, (long)ms.count());
}

// Экранирование строки для JSON (минимальное: кавычки и обратный слэш).
static std::string JsonEscape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s)
	{
		if (c == '"')
			out += "\\\"";
		else if (c == '\\')
			out += "\\\\";
		else
			out += c;
	}
	return out;
}

// ---------------------------------------------------------------------------
// Основная функция
// ---------------------------------------------------------------------------

void CybEmitter::Emit(const RunSubmission &sub)
{
	// Читаем конфиг
	const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
	if (!url || url[0] == '\0')
		return; // эмиттер выключен

	const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");
	const char *serverId = KZOptionService::GetOptionStr("cybServerId", "");

	// Маппинг режима
	const char *modeStr = MapMode(sub.mode.name);
	if (modeStr[0] == '\0')
	{
		KZ_LOG_INFO(LogChannel::Global, "[cyb_emit] unknown mode '%s', skipping\n", sub.mode.name.c_str());
		return;
	}

	// Генерируем event ID (UUIDv7)
	UUID_t eventId;
	std::string eventIdStr = eventId.ToString();

	// Timestamp
	char atBuf[40];
	FormatISO8601(atBuf, sizeof(atBuf));

	// Время в мс (cs2kz хранит в секундах как f64)
	long long timeMs = (long long)(sub.time * 1000.0 + 0.5);

	// Строим JSON payload
	// Формат: { "events": [ { envelope + data } ] }
	char body[2048];
	snprintf(body, sizeof(body),
		"{\"events\":[{"
		"\"v\":1,"
		"\"id\":\"%s\","
		"\"type\":\"kz.run_finished\","
		"\"typeV\":1,"
		"\"at\":\"%s\","
		"\"serverId\":\"%s\","
		"\"actor\":{\"steamId64\":\"%llu\"},"
		"\"data\":{"
		"\"mapName\":\"%s\","
		"\"course\":0,"
		"\"mode\":\"%s\","
		"\"timeMs\":%lld,"
		"\"teleports\":%u,"
		"\"styles\":[]"
		"}"
		"}]}",
		eventIdStr.c_str(),
		atBuf,
		JsonEscape(std::string(serverId ? serverId : "")).c_str(),
		(unsigned long long)sub.player.steamid64,
		JsonEscape(sub.map.name).c_str(),
		modeStr,
		timeMs,
		(unsigned)sub.teleports);

	// Строим URL: убираем trailing slash если есть
	std::string fullUrl = std::string(url);
	if (!fullUrl.empty() && fullUrl.back() == '/')
		fullUrl.pop_back();
	fullUrl += "/ingest/v1/events";

	HTTP::Request req(HTTP::Method::POST, fullUrl);
	req.SetHeader("Content-Type", "application/json");
	if (token && token[0] != '\0')
	{
		std::string auth = std::string("Bearer ") + token;
		req.SetHeader("Authorization", auth);
	}
	req.SetBody(std::string(body));

	// Fire-and-forget: ошибка только в лог, ран не блокируется
	req.Send(
		[](HTTP::Response resp)
		{
			if (resp.status < 200 || resp.status >= 300)
			{
				KZ_LOG_INFO(LogChannel::Global, "[cyb_emit] ingest returned HTTP %u\n", (unsigned)resp.status);
			}
		},
		[]()
		{
			KZ_LOG_INFO(LogChannel::Global, "[cyb_emit] ingest HTTP error (network/timeout)\n");
		});
}
