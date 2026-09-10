/*
 * cyb_awr_debug.cpp — `kz_awr_debug <uuid>`: разбор AWR-разреза одного файла в консоль.
 *
 * Зачем отдельная команда: живьём разрез виден только через awr_ms и «дёргается ли бот у
 * чекпоинта», а причина расхождения лежит в отдельных прибытиях телепорта. Первый живой
 * dry-run бэкфилла ответил «dest_not_found» на 50 файлах из 50, и разбор стоил ещё одного
 * цикла сборка → канарейка. Команда печатает по строке на каждое прибытие: чем сшили, куда,
 * на каком расстоянии был чекпоинт, сколько игрок стоял, какой вырез объявлен.
 *
 * Куда что печатается: построчная трасса и объявленные вырезы уходят в ЛОГ
 * (KZ_LOG_INFO, LogChannel::Replays — читается bin/logs.sh, как строки `[cyb_awr] backfill`),
 * а в ответ команды идёт только сводка. Причина: в RCON-контексте Msg уходит в ОТВЕТ пакетом,
 * и на 81 прибытии клиент получает `bad packet size 10330`, а в логах при этом не остаётся
 * ничего — то есть на длинном ране трасса терялась целиком.
 *
 * Только серверная консоль/RCON (как kz_awr_backfill): это операторская диагностика.
 * Разбор СИНХРОННЫЙ, на главном потоке — секция тиков большого реплея распаковывается
 * десятки миллисекунд, то есть команда крадёт кадр. Для ручного вызова это приемлемо, в
 * автоматику её ставить нельзя (для неё есть воркер бэкфилла со своим потоком).
 */

#include "cs2kz.h"
#include "filesystem.h"
#include "kz/kz.h"
#include "kz/timer/kz_timer.h"
#include "kz/replays/awr_cut.h"
#include "kz/replays/data.h"
#include "kz/replays/kz_replay.h"
#include "kz/replays/playback.h"
#include "utils/logging.h"
#include "utils/utils.h"
#include "utils/uuid.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
	// Потолок печати: на ране с тысячами телепортов полная трасса заливает лог, а для
	// разбора хватает начала (трасса упорядочена от старта к финишу).
	constexpr size_t AWR_DEBUG_MAX_ARRIVALS = 200;

	// Три РАЗНЫХ исхода чтения: «файла нет» и «файл есть, но прочитать нечего» — разные
	// диагнозы, и путать их нельзя (пустой или обрезанный файл диагностировался бы как
	// «сначала проиграйте реплей», хотя играть нечего).
	enum class ReadResult
	{
		Ok,
		NotFound,
		ReadFailed,
	};

	// Прочитать файл реплея целиком. Путь строится ТОЛЬКО из провалидированного UUID
	// (как в cyb_replay_download): аргумент команды в путь на диске не попадает.
	ReadResult ReadReplayFile(const char *uuid, std::vector<char> &out, std::string &usedPath)
	{
		char path[512];
		V_snprintf(path, sizeof(path), KZ_REPLAY_PATH "/%s.replay", uuid);
		if (!g_pFullFileSystem->FileExists(path))
		{
			V_snprintf(path, sizeof(path), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", uuid);
			if (!g_pFullFileSystem->FileExists(path))
			{
				return ReadResult::NotFound;
			}
		}
		usedPath = path;

		FileHandle_t file = g_pFullFileSystem->Open(path, "rb");
		if (!file)
		{
			return ReadResult::ReadFailed;
		}
		const size_t size = g_pFullFileSystem->Size(file);
		out.resize(size);
		const bool read = size > 0 && g_pFullFileSystem->Read(out.data(), (int)size, file) == (int)size;
		g_pFullFileSystem->Close(file);
		return read ? ReadResult::Ok : ReadResult::ReadFailed;
	}

	// Есть ли на сервере живой ран. Разбор синхронный и крадёт кадр — оператор должен
	// понимать, что именно он сейчас испортит чужой ран, но ОТКАЗЫВАТЬ не за что: это его
	// сервер и его решение.
	bool AnyTimerRunning()
	{
		// Граница цикла — как в KZ::zones::ResetEditors: перегрузка ToPlayer(CPlayerSlot)
		// внутри делает index = slot.Get() + 1 по массиву players[MAXPLAYERS + 1], поэтому
		// строго `i < MAXPLAYERS`.
		for (i32 i = 0; i < MAXPLAYERS; i++)
		{
			KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(i));
			if (player && player->timerService && player->timerService->GetTimerRunning())
			{
				return true;
			}
		}
		return false;
	}
} // namespace

// Только серверная консоль/RCON: операторская диагностика, не действие игрока (та же
// защита, что у kz_awr_backfill; урок cyb.33 — SCMD-диспатч требует controller и из RCON
// не работает).
CON_COMMAND_F(kz_awr_debug, "Explain the AWR cut of one replay file. Usage: kz_awr_debug <uuid>", FCVAR_NONE)
{
	if (utils::GetController(context.GetPlayerSlot()))
	{
		KZ_LOG_WARN(LogChannel::Replays, "[cyb_awr] cmd_denied cmd=kz_awr_debug reason=not_server slot=%d\n", context.GetPlayerSlot().Get());
		return;
	}

	UUID_t parsedUuid;
	if (args.ArgC() < 2 || !UUID_t::FromString(args.Arg(1), &parsedUuid))
	{
		Msg("[cyb_awr] kz_awr_debug: usage: kz_awr_debug <uuid>\n");
		fflush(stdout);
		return;
	}
	const std::string uuid = parsedUuid.ToString();

	std::vector<char> raw;
	std::string path;
	const ReadResult read = ReadReplayFile(uuid.c_str(), raw, path);
	// Печать команды — ASCII: кодировка серверной консоли и RCON-клиента ненадёжна,
	// кириллица в них приезжает мусором (комментарии по-русски, вывод — нет).
	if (read == ReadResult::NotFound)
	{
		Msg("[cyb_awr] kz_awr_debug uuid=%s: file not found locally (neither " KZ_REPLAY_PATH " nor " KZ_REPLAY_DOWNLOADS_PATH
			"). Play it once first: !replay %s\n",
			uuid.c_str(), uuid.c_str());
		fflush(stdout);
		return;
	}
	if (read == ReadResult::ReadFailed)
	{
		Msg("[cyb_awr] kz_awr_debug uuid=%s: read failed path=%s size=%zu (empty or truncated file)\n", uuid.c_str(), path.c_str(), raw.size());
		fflush(stdout);
		return;
	}

	if (AnyTimerRunning())
	{
		Msg("[cyb_awr] kz_awr_debug: WARNING a player has a run in progress; this command parses the replay synchronously and will steal a "
			"frame\n");
	}

	KZ::replaysystem::data::CutSource src = KZ::replaysystem::data::LoadCutSourceFromMemory(raw.data(), raw.size());
	if (!src.valid)
	{
		Msg("[cyb_awr] kz_awr_debug uuid=%s: parse failed (path=%s size=%zu)\n", uuid.c_str(), path.c_str(), raw.size());
		fflush(stdout);
		return;
	}

	const bool isRun = src.header.has_run() && src.header.run().time() > 0.0f;
	const u64 timeMs = isRun ? (u64)((f64)src.header.run().time() * 1000.0 + 0.5) : 0;

	// Шапка — и в лог (чтобы блок трассы был самодостаточен), и в ответ команды.
	KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] debug uuid=%s path=%s ticks=%zu events=%zu is_run=%d time_ms=%llu header_tps=%d\n", uuid.c_str(),
				path.c_str(), src.ticks.size(), src.events.size(), isRun ? 1 : 0, (unsigned long long)timeMs,
				isRun && src.header.run().has_num_teleports() ? src.header.run().num_teleports() : -1);

	u32 runStart = 0, runEnd = 0;
	i32 runCourseId = -1;
	const bool hasWindow = KZ::replaysystem::playback::RunWindowFromEvents(src.ticks.data(), (u32)src.ticks.size(), src.events.data(),
																		   (u32)src.events.size(), runStart, runEnd, runCourseId);
	if (hasWindow)
	{
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] debug uuid=%s window=%u..%u course_id=%d start_tick=%u end_tick=%u\n", uuid.c_str(), runStart,
					runEnd, runCourseId, src.ticks[runStart].serverTick, src.ticks[runEnd].serverTick);
	}
	else
	{
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] debug uuid=%s window=none reason=no_timer_start_end_pair_or_course_mismatch\n", uuid.c_str());
	}

	std::vector<KZ::replaysystem::awr::ArrivalTrace> trace;
	KZ::replaysystem::awr::CutResult cut = KZ::replaysystem::playback::ComputeCutForTraced(
		src.ticks.data(), (u32)src.ticks.size(), src.events.data(), (u32)src.events.size(), timeMs, &trace);

	KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] debug uuid=%s arrivals=%zu teleports=%u ok=%d reason=%s awr_ms=%llu detail=%s\n", uuid.c_str(),
				trace.size(), cut.teleports, cut.ok ? 1 : 0, cut.ok ? "ok" : cut.reason, (unsigned long long)cut.awrMs, cut.detail);

	for (size_t i = 0; i < trace.size(); i++)
	{
		if (i >= AWR_DEBUG_MAX_ARRIVALS)
		{
			KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] debug uuid=%s ... %zu more arrivals not shown\n", uuid.c_str(),
						trace.size() - AWR_DEBUG_MAX_ARRIVALS);
			break;
		}
		const KZ::replaysystem::awr::ArrivalTrace &tr = trace[i];
		char cpText[96];
		if (tr.cpFrame >= 0)
		{
			V_snprintf(cpText, sizeof(cpText), "S=%lld post_d=%.1f pre_d=%.1f", (long long)tr.cpFrame, tr.cpPostDist, tr.cpPreDist);
		}
		else
		{
			V_snprintf(cpText, sizeof(cpText), "S=none");
		}
		char deadText[48];
		if (tr.dest >= 0)
		{
			V_snprintf(deadText, sizeof(deadText), "dead=%u..%u", tr.deadFrom, tr.deadTo);
		}
		else
		{
			V_snprintf(deadText, sizeof(deadText), "dead=none");
		}
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] debug uuid=%s T=%u tick=%u cp=%d/%d tp=%d pos=%.1f/%.1f/%.1f D=%lld via=%c %s stand=%u %s\n",
					uuid.c_str(), tr.frame, tr.serverTick, tr.cpIndex, tr.cpCount, tr.tpCount, tr.origin[0], tr.origin[1], tr.origin[2],
					(long long)tr.dest, tr.method, cpText, tr.standTicks, deadText);
	}

	for (const KZ::replaysystem::awr::Interval &d : cut.dead)
	{
		KZ_LOG_INFO(LogChannel::Replays, "[cyb_awr] debug uuid=%s cut %u..%u\n", uuid.c_str(), d.from, d.to);
	}

	// СВОДКА в ответ команды. Больше в ответ не печатаем: в RCON-контексте вывод уходит
	// клиенту одним пакетом, и трасса на десятки прибытий его рвёт (bad packet size).
	Msg("[cyb_awr] kz_awr_debug uuid=%s path=%s ticks=%zu is_run=%d time_ms=%llu\n", uuid.c_str(), path.c_str(), src.ticks.size(), isRun ? 1 : 0,
		(unsigned long long)timeMs);
	if (hasWindow)
	{
		Msg("[cyb_awr]   window=%u..%u (ticks %u..%u) course_id=%d\n", runStart, runEnd, src.ticks[runStart].serverTick,
			src.ticks[runEnd].serverTick, runCourseId);
	}
	else
	{
		Msg("[cyb_awr]   window=<none> (no TIMER_START/TIMER_END pair, or the pair has different course ids)\n");
	}
	Msg("[cyb_awr]   arrivals=%zu teleports=%u ok=%d reason=%s awr_ms=%llu time_ms=%llu cuts=%zu\n", trace.size(), cut.teleports, cut.ok ? 1 : 0,
		cut.ok ? "ok" : cut.reason, (unsigned long long)cut.awrMs, (unsigned long long)timeMs, cut.dead.size());
	if (!cut.ok && cut.detail[0] != '\0')
	{
		Msg("[cyb_awr]   detail=%s\n", cut.detail);
	}
	Msg("[cyb_awr]   per-arrival trace is in the server log: grep '[cyb_awr] debug uuid=%s'\n", uuid.c_str());

	// stdout контейнера буферизуется — без flush вывод команды приходит рывками (урок
	// kzt-саги, тот же fflush стоит у диагностики скинов бота).
	fflush(stdout);
}
