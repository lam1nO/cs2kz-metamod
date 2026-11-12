#include "cs2kz.h"
#include "cyb_emitter.h"
#include "cyb_ticket_lease.h"
#include <charconv>
#include <cstdlib>
#include "kz/analytics/source_identity.h"
#include "cyb_ticket_eligibility.h"

#include "utils/utils.h"
#include "utils/uuid.h"
#include "kz/option/kz_option.h"
#include "kz/outbox/kz_outbox.h"

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

	const char *legacyServerId = KZOptionService::GetOptionStr("cybServerId", "");
	const bool hasTicketProof = sub.ticketEligible && CybTickets::IsCampaignId(sub.ticketCampaign);
	const std::string serverId = hasTicketProof ? KZ::analytics::SourceServerId(std::getenv("CS2_INSTANCE_ID"), legacyServerId)
										  : (legacyServerId ? legacyServerId : "");

	// Маппинг режима
	const char *modeStr = MapMode(sub.mode.name);
	if (modeStr[0] == '\0')
	{
		KZ_LOG_INFO(LogChannel::Global, "[cyb_emit] unknown mode '%s', skipping\n", sub.mode.name.c_str());
		return;
	}

	// Event ID = UUID рана: ключ идемпотентности на платформе — ретрай из outbox
	// (kz/outbox) с тем же id не создаст дубликата события.
	std::string eventIdStr = sub.localUUID.ToString();

	// Timestamp
	char atBuf[40];
	FormatISO8601(atBuf, sizeof(atBuf));

	// Время в мс (cs2kz хранит в секундах как f64)
	long long timeMs = (long long)(sub.time * 1000.0 + 0.5);

	const std::string &campaign = sub.ticketCampaign;
	const std::string ticketProof =
		hasTicketProof ? ",\"eventTicket\":{\"campaignId\":\"" + campaign + "\",\"valid\":true}" : "";

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
		"\"course\":%d,"
		"\"courseName\":\"%s\","
		"\"mode\":\"%s\","
		"\"timeMs\":%lld,"
		"\"teleports\":%u,"
		"\"styles\":[]%s"
		"}"
		"}]}",
		eventIdStr.c_str(),
		atBuf,
		JsonEscape(serverId).c_str(),
		(unsigned long long)sub.player.steamid64,
		JsonEscape(sub.map.name).c_str(),
		(int)sub.course.number,
		JsonEscape(sub.course.name).c_str(),
		modeStr,
		timeMs,
		(unsigned)sub.teleports, ticketProof.c_str());

	// Write-ahead: тело события на диск ДО первой попытки отправки — при обрыве
	// сети до платформы ран не потеряется, ретраер outbox дошлёт его с тем же id.
	// Отправка (URL, заголовки, обработка ответа) — общая с ретраером функция;
	// 2xx удалит write-ahead файл, ран при любом исходе не блокируется.
	std::string bodyStr(body);
	KZOutboxService::EnqueueEvent(eventIdStr, bodyStr);
	KZOutboxService::SendEvent(eventIdStr, bodyStr);
}

namespace
{
	CybTickets::Lease eventTicketLease;

	std::int64_t LeaseNow()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
} // namespace

CConVar<i32> cyb_event_tickets_lease_version("cyb_event_tickets_lease_version", FCVAR_NONE, "Runtime event lease protocol version.", 1);

std::string CybEmitter::CaptureTicketCampaign()
{
	return eventTicketLease.Capture(LeaseNow());
}

CON_COMMAND_F(cyb_event_tickets_lease, "Trusted server event lease: campaign UUID or clear, TTL seconds0..90.", FCVAR_NONE)
{
	if (context.GetPlayerSlot().Get() != -1)
	{
		return;
	}
	int seconds = -1;
	if (args.ArgC() == 3)
	{
		const std::string value = args.Arg(2);
		auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
		if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size())
		{
			seconds = -1;
		}
	}
	eventTicketLease.Update(args.ArgC() == 3 ? args.Arg(1) : "invalid", static_cast<std::int64_t>(seconds) * 1000, LeaseNow(), -1);
}

CON_COMMAND_F(cyb_event_tickets_status, "Read the trusted event lease status.", FCVAR_NONE)
{
	if (context.GetPlayerSlot().Get() != -1)
	{
		return;
	}
	auto now = LeaseNow();
	auto id = eventTicketLease.Capture(now);
	Msg("[event_tickets] enabled=%d campaign=%s remaining_ms=%lld\n", id.empty() ? 0 : 1, id.empty() ? "none" : id.c_str(),
		static_cast<long long>(eventTicketLease.Remaining(now)));
}
