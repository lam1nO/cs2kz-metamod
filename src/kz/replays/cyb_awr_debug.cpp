/*
 * cyb_awr_debug.cpp — `kz_awr_debug <uuid>`: разбор AWR-разреза одного файла в консоль.
 *
 * Зачем отдельная команда: живьём разрез виден только через awr_ms и «дёргается ли бот у
 * чекпоинта», а причина расхождения лежит в отдельных прибытиях телепорта. Первый живой
 * dry-run бэкфилла ответил «dest_not_found» на 50 файлах из 50, и разбор стоил ещё одного
 * цикла сборка → канарейка. Команда печатает по строке на каждое прибытие: чем сшили, куда,
 * на каком расстоянии был чекпоинт, сколько игрок стоял, какой вырез объявлен.
 *
 * Только серверная консоль/RCON (как kz_awr_backfill): это операторская диагностика.
 * Разбор СИНХРОННЫЙ, на главном потоке — секция тиков большого реплея распаковывается
 * десятки миллисекунд, то есть команда крадёт кадр. Для ручного вызова это приемлемо, в
 * автоматику её ставить нельзя (для неё есть воркер бэкфилла со своим потоком).
 */

#include "cs2kz.h"
#include "filesystem.h"
#include "kz/kz.h"
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
	// Потолок печати: на ране с сотнями телепортов полная трасса вытеснит из консоли всё
	// остальное, а для разбора хватает начала (обход идёт от старта к финишу).
	constexpr size_t AWR_DEBUG_MAX_ARRIVALS = 200;

	// Прочитать файл реплея целиком. Путь строится ТОЛЬКО из провалидированного UUID
	// (как в cyb_replay_download): аргумент команды в путь на диске не попадает.
	bool ReadReplayFile(const char *uuid, std::vector<char> &out, std::string &usedPath)
	{
		char path[512];
		V_snprintf(path, sizeof(path), KZ_REPLAY_PATH "/%s.replay", uuid);
		if (!g_pFullFileSystem->FileExists(path))
		{
			V_snprintf(path, sizeof(path), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", uuid);
			if (!g_pFullFileSystem->FileExists(path))
			{
				return false;
			}
		}

		FileHandle_t file = g_pFullFileSystem->Open(path, "rb");
		if (!file)
		{
			return false;
		}
		const size_t size = g_pFullFileSystem->Size(file);
		out.resize(size);
		const bool read = size > 0 && g_pFullFileSystem->Read(out.data(), (int)size, file) == (int)size;
		g_pFullFileSystem->Close(file);
		if (!read)
		{
			return false;
		}
		usedPath = path;
		return true;
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
	if (!ReadReplayFile(uuid.c_str(), raw, path))
	{
		// Печать команды — ASCII: кодировка серверной консоли и RCON-клиента ненадёжна,
		// кириллица в них приезжает мусором (комментарии по-русски, вывод — нет).
		Msg("[cyb_awr] kz_awr_debug uuid=%s: file not found locally (neither " KZ_REPLAY_PATH " nor " KZ_REPLAY_DOWNLOADS_PATH
			"). Play it once first: !replay %s\n",
			uuid.c_str(), uuid.c_str());
		fflush(stdout);
		return;
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

	Msg("[cyb_awr] kz_awr_debug uuid=%s path=%s ticks=%zu events=%zu is_run=%d time_ms=%llu header_tps=%d\n", uuid.c_str(), path.c_str(),
		src.ticks.size(), src.events.size(), isRun ? 1 : 0,
		(unsigned long long)timeMs, isRun && src.header.run().has_num_teleports() ? src.header.run().num_teleports() : -1);

	u32 runStart = 0, runEnd = 0;
	i32 runCourseId = -1;
	if (KZ::replaysystem::playback::RunWindowFromEvents(src.ticks.data(), (u32)src.ticks.size(), src.events.data(), (u32)src.events.size(), runStart,
														runEnd, runCourseId))
	{
		Msg("[cyb_awr]   window=%u..%u course_id=%d start_tick=%u end_tick=%u\n", runStart, runEnd, runCourseId,
			src.ticks[runStart].serverTick, src.ticks[runEnd].serverTick);
	}
	else
	{
		Msg("[cyb_awr]   window=<none> (no TIMER_START/TIMER_END pair, or the pair has different course ids)\n");
	}

	std::vector<KZ::replaysystem::awr::ArrivalTrace> trace;
	KZ::replaysystem::awr::CutResult cut = KZ::replaysystem::playback::ComputeCutForTraced(
		src.ticks.data(), (u32)src.ticks.size(), src.events.data(), (u32)src.events.size(), timeMs, &trace);

	Msg("[cyb_awr]   arrivals=%zu teleports=%u ok=%d reason=%s awr_ms=%llu detail=%s\n", trace.size(), cut.teleports, cut.ok ? 1 : 0,
		cut.ok ? "ok" : cut.reason, (unsigned long long)cut.awrMs, cut.detail);

	for (size_t i = 0; i < trace.size(); i++)
	{
		if (i >= AWR_DEBUG_MAX_ARRIVALS)
		{
			Msg("[cyb_awr]   ... %zu more arrivals not shown\n", trace.size() - AWR_DEBUG_MAX_ARRIVALS);
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
		Msg("[cyb_awr]   T=%u tick=%u cp=%d/%d tp=%d pos=%.1f/%.1f/%.1f D=%lld via=%c %s stand=%u %s\n", tr.frame, tr.serverTick, tr.cpIndex,
			tr.cpCount, tr.tpCount, tr.origin[0], tr.origin[1], tr.origin[2], (long long)tr.dest, tr.method, cpText, tr.standTicks, deadText);
	}

	for (const KZ::replaysystem::awr::Interval &d : cut.dead)
	{
		Msg("[cyb_awr]   cut %u..%u\n", d.from, d.to);
	}

	// stdout контейнера буферизуется — без flush вывод команды приходит рывками (урок
	// kzt-саги, тот же fflush стоит у диагностики скинов бота).
	fflush(stdout);
}
