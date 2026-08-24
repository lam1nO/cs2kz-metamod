#include "cyb_replay_upload.h"

#include "utils/logging.h"
#include "kz/option/kz_option.h"
#include "cyb_replay_common.h"
#include "kz_replay.h"

#include <string>

bool CybReplayUpload::BuildMeta(const RunSubmission &sub, bool isServerRecord, bool unconfirmedPb, KZOutboxService::ReplayMeta &out)
{
	// Центральные реплеи — только для бесстилевых ранов (ключ api не содержит
	// styles). На практике этот вызов и так достижим только при styleIDs==0
	// (см. save_time.cpp/submission.cpp), но проверяем явно как гарантию
	// инварианта на случай будущих изменений вызывающего кода.
	if (!sub.styles.empty())
	{
		return false;
	}

	if (sub.replayBuffer.empty())
	{
		return false;
	}

	if (sub.replayBuffer.size() >= KZOutboxService::maxReplayBytes)
	{
		KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] replay too large (%zu bytes), skipping central upload\n", sub.replayBuffer.size());
		return false;
	}

	const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
	if (!url || url[0] == '\0')
	{
		return false; // Центральные реплеи выключены на этом сервере
	}

	const char *modeStr = CybReplayCommon::MapMode(sub.mode.name);
	if (modeStr[0] == '\0')
	{
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_replay] unknown mode '%s', skipping central upload\n", sub.mode.name.c_str());
		return false;
	}

	if (!CybReplayCommon::IsValidMapName(sub.map.name))
	{
		KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] map name '%s' fails api validation, skipping central upload\n", sub.map.name.c_str());
		return false;
	}

	if (sub.course.number < 0 || sub.course.number > 32767)
	{
		KZ_LOG_WARN(LogChannel::Replays, "[cyb_replay] course number %d out of range, skipping central upload\n", sub.course.number);
		return false;
	}

	out.runUuid = sub.finalUUID.ToString();
	out.steamId64 = sub.player.steamid64;
	out.map = sub.map.name;
	out.course = sub.course.number;
	out.mode = modeStr;
	out.isServerRecord = isServerRecord;
	out.unconfirmedPb = unconfirmedPb;
	out.timeMs = (u64)(sub.time * 1000.0 + 0.5);
	out.replayPath = std::string(KZ_REPLAY_PATH "/") + out.runUuid + ".replay";
	return true;
}

void CybReplayUpload::MaybeUpload(const RunSubmission &sub, bool isServerRecord)
{
	KZOutboxService::ReplayMeta meta;
	if (!BuildMeta(sub, isServerRecord, false, meta))
	{
		return; // причина в логе BuildMeta; write-ahead меты для такого рана и не писался
	}
	// Переход unconfirmed → confirmed: перезаписываем write-ahead (OnReplayReady писал
	// вариант без подтверждения PB), затем отправляем той же функцией, что и ретраер.
	KZOutboxService::EnqueueReplayMeta(meta);
	KZOutboxService::SendReplay(meta, sub.replayBuffer);
}
