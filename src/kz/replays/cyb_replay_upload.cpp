#include "cyb_replay_upload.h"

#include "utils/http.h"
#include "utils/logging.h"
#include "kz/option/kz_option.h"
#include "cyb_replay_common.h"

#include <string>

namespace
{
	// Лимит тела запроса на стороне api (см. план T4b, Global Constraints).
	constexpr size_t kMaxReplayBytes = 32u * 1024u * 1024u; // 32 МБ

	// Один POST на один тип (pb/wr) — по одному на каждый вызов.
	void DoUpload(const std::string &baseUrl, const std::string &token, u64 steamId64, const std::string &map, i32 course, const char *mode,
				  const char *type, const std::string &replayUuid, const std::vector<char> &buffer)
	{
		std::string fullUrl = baseUrl;
		if (!fullUrl.empty() && fullUrl.back() == '/')
		{
			fullUrl.pop_back();
		}
		fullUrl += "/replays/v1/upload";

		HTTP::Request req(HTTP::Method::POST, fullUrl);
		req.SetQuery("steamId64", std::to_string(steamId64));
		req.SetQuery("map", map);
		req.SetQuery("course", std::to_string(course));
		req.SetQuery("mode", mode);
		req.SetQuery("type", type);
		req.SetQuery("replayUuid", replayUuid);

		req.SetHeader("Content-Type", "application/octet-stream");
		if (!token.empty())
		{
			req.SetHeader("Authorization", "Bearer " + token);
		}

		// std::string корректно хранит бинарные данные с внутренними '\0' — длина
		// берётся из .size(), а не strlen; SetHTTPRequestRawPostBody (utils/http.cpp)
		// использует явную длину буфера, так что нулевые байты тело не обрезают.
		req.SetBody(std::string(buffer.begin(), buffer.end()));

		req.Send(
			[type](HTTP::Response resp)
			{
				if (resp.status < 200 || resp.status >= 300)
				{
					KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] upload (%s) returned HTTP %u\n", type, (unsigned)resp.status);
				}
			},
			[type]()
			{
				KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] upload (%s) HTTP error (network/timeout)\n", type);
			});
	}
} // namespace

void CybReplayUpload::MaybeUpload(const RunSubmission &sub, bool isServerRecord)
{
	// Центральные реплеи — только для бесстилевых ранов (ключ api не содержит
	// styles). На практике этот вызов и так достижим только при styleIDs==0
	// (см. save_time.cpp/submission.cpp), но проверяем явно как гарантию
	// инварианта на случай будущих изменений вызывающего кода.
	if (!sub.styles.empty())
	{
		return;
	}

	if (sub.replayBuffer.empty())
	{
		return;
	}

	if (sub.replayBuffer.size() >= kMaxReplayBytes)
	{
		KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] replay too large (%zu bytes), skipping central upload\n", sub.replayBuffer.size());
		return;
	}

	const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
	if (!url || url[0] == '\0')
	{
		return; // Центральные реплеи выключены на этом сервере
	}
	const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");

	const char *modeStr = CybReplayCommon::MapMode(sub.mode.name);
	if (modeStr[0] == '\0')
	{
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] unknown mode '%s', skipping central upload\n", sub.mode.name.c_str());
		return;
	}

	if (!CybReplayCommon::IsValidMapName(sub.map.name))
	{
		KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] map name '%s' fails api validation, skipping central upload\n", sub.map.name.c_str());
		return;
	}

	if (sub.course.number < 0 || sub.course.number > 32767)
	{
		KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] course number %d out of range, skipping central upload\n", sub.course.number);
		return;
	}

	std::string replayUuid = sub.finalUUID.ToString();
	std::string tokenStr = token ? token : "";

	DoUpload(url, tokenStr, sub.player.steamid64, sub.map.name, sub.course.number, modeStr, "pb", replayUuid, sub.replayBuffer);

	// WR — ДОПОЛНИТЕЛЬНО к pb (второй независимый POST), только если этот же
	// ран сделал игрока локальным рекордсменом сервера (ранг 1).
	if (isServerRecord)
	{
		DoUpload(url, tokenStr, sub.player.steamid64, sub.map.name, sub.course.number, modeStr, "wr", replayUuid, sub.replayBuffer);
	}
}
