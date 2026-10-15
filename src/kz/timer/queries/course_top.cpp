#include "base_request.h"
#include "kz/timer/kz_timer.h"
#include "kz/db/kz_db.h"
#include "kz/global/kz_global.h"

#include "utils/simplecmds.h"
#include "utils/ctimer.h"
#include "kz/hud/layout/gsmenu.h"
#include "kz/mode/kz_mode.h"
#include "kz/hud/kz_hud.h"
#include "utils/interfaces.h"

#include "vendor/sql_mm/src/public/sql_mm.h"
#include "utils/json.h"

#include <fstream>
#include <sstream>
// clang-format off
#define COURSE_TOP_TABLE_KEY "Course Top - Table Name (Server Overall)"
static_global const char *columnKeysLocal[] = {
	"#",
	"Course Top Header - Player Alias",
	"Course Top Header - Time",
	"Course Top Header - Teleports",
	"Course Top Header - SteamID64",
	"Course Top Header - Run ID"
};

#define COURSE_TOP_PRO_TABLE_KEY "Course Top - Table Name (Server Pro)"
static_global const char *columnKeysLocalPro[] = {
	"#",
	"Course Top Header - Player Alias",
	"Course Top Header - Time",
	"Course Top Header - SteamID64",
	"Course Top Header - Run ID"
};

#define COURSE_TOP_TABLE_KEY_GLOBAL "Course Top - Table Name (Global Overall)"
static_global const char *columnKeysGlobal[] = {
	"#",
	"Course Top Header - Player Alias",
	"Course Top Header - Time",
	"Course Top Header - Teleports",
	"Course Top Header - SteamID64",
	"Course Top Header - Points",
	"Course Top Header - Run ID"
};

#define COURSE_TOP_PRO_TABLE_KEY_GLOBAL "Course Top - Table Name (Global Pro)"
static_global const char *columnKeysGlobalPro[] = {
	"#",
	"Course Top Header - Player Alias",
	"Course Top Header - Time",
	"Course Top Header - SteamID64",
	"Course Top Header - Points",
	"Course Top Header - Run ID",
};

// clang-format on

struct CourseTopRequest : public BaseRequest
{
	using BaseRequest::BaseRequest;

	static constexpr u64 ctopFeatures =
		RequestFeature::Course | RequestFeature::Map | RequestFeature::Mode | RequestFeature::Offset | RequestFeature::Limit;

	struct RunStats
	{
		CUtlString runID;
		CUtlString name;
		u64 teleportsUsed;
		f64 time;
		u64 steamid64;
		u64 points {}; // 0 for local database
		CUtlString created; // «YYYY-MM-DD HH:MM:SS» из Times.Created (локальная БД), пусто — нет данных

		CUtlString GetName()
		{
			return name;
		}

		CUtlString GetRunID()
		{
			return runID;
		}

		CUtlString GetTeleportCount()
		{
			CUtlString fmt;
			fmt.Format("%llu", teleportsUsed);
			return fmt;
		}

		CUtlString GetTime()
		{
			return utils::FormatTime(time);
		}

		CUtlString GetSteamID64()
		{
			CUtlString fmt;
			fmt.Format("%llu", steamid64);
			return fmt;
		}

		CUtlString GetPoints()
		{
			CUtlString fmt;
			fmt.Format("%llu", points);
			return fmt;
		}
	};

	struct CourseTopData
	{
		CUtlVector<RunStats> overallData;
		CUtlVector<RunStats> proData;
	} srData, wrData;

	virtual void PrintInstructions() override
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(this->userID);
		if (!player)
		{
			return;
		}
		player->languageService->PrintChat(true, false, "Course Top Command Usage");
		player->languageService->PrintConsole(false, false, "Course Top Command Usage - Console");
	}

	virtual void QueryLocal()
	{
		if (this->requestingFirstCourse)
		{
			return;
		}
		if (this->localStatus == ResponseStatus::ENABLED)
		{
			this->localStatus = ResponseStatus::PENDING;

			KZPlayer *callingPlayer = g_pKZPlayerManager->ToPlayer(userID);
			if (!callingPlayer)
			{
				this->Invalidate();
				return;
			}

			this->localStatus = ResponseStatus::PENDING;

			u64 uid = this->uid;

			auto onQuerySuccess = [uid](std::vector<ISQLQuery *> queries)
			{
				CourseTopRequest *req = (CourseTopRequest *)CourseTopRequest::Find(uid);
				if (!req)
				{
					return;
				}

				req->localStatus = ResponseStatus::RECEIVED;
				ISQLResult *result = queries[0]->GetResultSet();
				if (result && result->GetRowCount() > 0)
				{
					while (result->FetchRow())
					{
						req->srData.overallData.AddToTail(
							{result->GetString(0), result->GetString(2), (u64)result->GetInt64(4), result->GetFloat(3), (u64)result->GetInt64(1), 0, result->GetString(5)});
					}
				}

				if ((result = queries[1]->GetResultSet()) && result->GetRowCount() > 0)
				{
					while (result->FetchRow())
					{
						req->srData.proData.AddToTail({result->GetString(0), result->GetString(2), 0, result->GetFloat(3), (u64)result->GetInt64(1), 0, result->GetString(5)});
					}
				}
			};

			auto onQueryFailure = [uid](std::string, int)
			{
				CourseTopRequest *req = (CourseTopRequest *)CourseTopRequest::Find(uid);
				if (req)
				{
					req->localStatus = ResponseStatus::DISABLED;
				}
			};

			KZDatabaseService::QueryRecords(this->mapName, this->courseName, this->localModeID, this->limit, this->offset, onQuerySuccess,
											onQueryFailure);
		}
	}

	virtual void QueryGlobal()
	{
		if (this->requestingFirstCourse)
		{
			return;
		}

		if (this->globalStatus == ResponseStatus::ENABLED)
		{
			this->globalStatus = ResponseStatus::PENDING;

			KZGlobalService::QueryCourseTopParams params;
			params.map = std::string_view(this->mapName.Get(), this->mapName.Length());
			params.course = std::string_view(this->courseName.Get(), this->courseName.Length());
			params.mode = this->apiMode;
			params.limit = this->limit;
			params.offset = this->offset;

			KZGlobalService::MessageCallback<KZ::api::messages::CourseTop> callback(CourseTopRequest::OnGlobalCourseTopQueried, this->uid);

			KZGlobalService::QueryCourseTop(std::move(params), std::move(callback));
		}
	}

	static void OnGlobalCourseTopQueried(const KZ::api::messages::CourseTop &ctop, u64 uid)
	{
		CourseTopRequest *req = (CourseTopRequest *)CourseTopRequest::Find(uid);
		if (!req)
		{
			return;
		}

		if (ctop.map.has_value() && ctop.course.has_value())
		{
			req->mapName = ctop.map->name.c_str();
			req->courseName = ctop.course->name.c_str();
			req->globalStatus = ResponseStatus::RECEIVED;
		}
		else
		{
			req->globalStatus = ResponseStatus::DISABLED;
			return;
		}
		for (const auto &record : ctop.overall)
		{
			CUtlString id;
			id.Format("%s", record.id.c_str());
			req->wrData.overallData.AddToTail(
				{id, record.player.name.c_str(), record.teleports, record.time, record.player.id, (u64)floor(record.nubPoints)});
		}
		for (const auto &record : ctop.pro)
		{
			CUtlString id;
			id.Format("%s", record.id.c_str());
			req->wrData.proData.AddToTail({id, record.player.name.c_str(), 0, record.time, record.player.id, (u64)floor(record.proPoints)});
		}
	}

	// Окно !maptop (layout/gsmenu.cpp) — false, если панорама недоступна: тогда остаётся консоль.
	bool ShowWindow();
	bool fromWindow = false; // перезапрос из окна (фильтр/листание), а не команда игрока

	virtual void Reply()
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
		if (!player)
		{
			return;
		}

		if (localStatus != ResponseStatus::RECEIVED && globalStatus != ResponseStatus::RECEIVED)
		{
			player->languageService->PrintChat(true, false, "Course Top Request - Failed (Generic)");
			return;
		}

		// Перезапрос из окна (фильтр/листание) — только окно, без консольной таблицы на каждый клик.
		if (this->fromWindow)
		{
			this->ShowWindow();
			return;
		}
		// Окно перед лицом; консольная таблица остаётся как была (её читают скриптами и копируют).
		if (!this->ShowWindow())
		{
			player->languageService->PrintChat(true, false, "Course Top - Check Console");
		}
		if (this->localStatus == ResponseStatus::RECEIVED)
		{
			this->ReplyLocal();
		}
		if (this->globalStatus == ResponseStatus::RECEIVED)
		{
			this->ReplyGlobal();
		}
	}

	void ReplyGlobal()
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
		CUtlString rank;

		// Overall table
		CUtlString headers[KZ_ARRAYSIZE(columnKeysGlobal)];
		for (u32 i = 0; i < KZ_ARRAYSIZE(columnKeysGlobal); i++)
		{
			headers[i] = player->languageService->PrepareMessage(columnKeysGlobal[i]).c_str();
		}
		utils::Table<KZ_ARRAYSIZE(columnKeysGlobal)> table(
			player->languageService->PrepareMessage(COURSE_TOP_TABLE_KEY_GLOBAL, mapName.Get(), courseName.Get(), modeName.Get()).c_str(), headers);
		FOR_EACH_VEC(wrData.overallData, i)
		{
			rank.Format("%llu", this->offset + i + 1);
			RunStats stats = wrData.overallData[i];
			table.SetRow(i, rank, stats.GetName(), stats.GetTime(), stats.GetTeleportCount(), stats.GetSteamID64(), stats.GetPoints(),
						 stats.GetRunID());
		}
		player->PrintConsole(false, false, table.GetSeparator("="));
		player->PrintConsole(false, false, table.GetTitle());
		player->PrintConsole(false, false, table.GetHeader());

		for (u32 i = 0; i < table.GetNumEntries(); i++)
		{
			player->PrintConsole(false, false, table.GetLine(i));
		}
		player->PrintConsole(false, false, table.GetSeparator("="));

		// Pro table
		CUtlString headersPro[KZ_ARRAYSIZE(columnKeysGlobalPro)];
		for (u32 i = 0; i < KZ_ARRAYSIZE(columnKeysGlobalPro); i++)
		{
			headersPro[i] = player->languageService->PrepareMessage(columnKeysGlobalPro[i]).c_str();
		}
		utils::Table<KZ_ARRAYSIZE(columnKeysGlobalPro)> tablePro(
			player->languageService->PrepareMessage(COURSE_TOP_PRO_TABLE_KEY_GLOBAL, mapName.Get(), courseName.Get(), modeName.Get()).c_str(),
			headersPro);
		FOR_EACH_VEC(wrData.proData, i)
		{
			rank.Format("%llu", this->offset + i + 1);
			RunStats stats = wrData.proData[i];
			tablePro.SetRow(i, rank, stats.GetName(), stats.GetTime(), stats.GetSteamID64(), stats.GetPoints(), stats.GetRunID());
		}
		player->PrintConsole(false, false, tablePro.GetSeparator("="));
		player->PrintConsole(false, false, tablePro.GetTitle());
		player->PrintConsole(false, false, tablePro.GetHeader());
		for (u32 i = 0; i < tablePro.GetNumEntries(); i++)
		{
			player->PrintConsole(false, false, tablePro.GetLine(i));
		}
		player->PrintConsole(false, false, tablePro.GetSeparator("="));
	}

	void ReplyLocal()
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
		CUtlString rank;

		// Overall table
		CUtlString headers[KZ_ARRAYSIZE(columnKeysLocal)];
		for (u32 i = 0; i < KZ_ARRAYSIZE(columnKeysLocal); i++)
		{
			headers[i] = player->languageService->PrepareMessage(columnKeysLocal[i]).c_str();
		}
		utils::Table<KZ_ARRAYSIZE(columnKeysLocal)> table(
			player->languageService->PrepareMessage(COURSE_TOP_TABLE_KEY, mapName.Get(), courseName.Get(), modeName.Get()).c_str(), headers);
		FOR_EACH_VEC(srData.overallData, i)
		{
			rank.Format("%llu", this->offset + i + 1);
			RunStats stats = srData.overallData[i];
			table.SetRow(i, rank, stats.GetName(), stats.GetTime(), stats.GetTeleportCount(), stats.GetSteamID64(), stats.GetRunID());
		}
		player->PrintConsole(false, false, table.GetSeparator("="));
		player->PrintConsole(false, false, table.GetTitle());
		player->PrintConsole(false, false, table.GetHeader());

		for (u32 i = 0; i < table.GetNumEntries(); i++)
		{
			player->PrintConsole(false, false, table.GetLine(i));
		}
		player->PrintConsole(false, false, table.GetSeparator("="));
		// Pro table
		CUtlString headersPro[KZ_ARRAYSIZE(columnKeysLocalPro)];
		for (u32 i = 0; i < KZ_ARRAYSIZE(columnKeysLocalPro); i++)
		{
			headersPro[i] = player->languageService->PrepareMessage(columnKeysLocalPro[i]).c_str();
		}
		utils::Table<KZ_ARRAYSIZE(columnKeysLocalPro)> tablePro(
			player->languageService->PrepareMessage(COURSE_TOP_PRO_TABLE_KEY, mapName.Get(), courseName.Get(), modeName.Get()).c_str(), headersPro);
		FOR_EACH_VEC(srData.proData, i)
		{
			rank.Format("%llu", this->offset + i + 1);
			RunStats stats = srData.proData[i];
			tablePro.SetRow(i, rank, stats.GetName(), stats.GetTime(), stats.GetSteamID64(), stats.GetRunID());
		}
		player->PrintConsole(false, false, tablePro.GetSeparator("="));
		player->PrintConsole(false, false, tablePro.GetTitle());
		player->PrintConsole(false, false, tablePro.GetHeader());
		for (u32 i = 0; i < tablePro.GetNumEntries(); i++)
		{
			player->PrintConsole(false, false, tablePro.GetLine(i));
		}
		player->PrintConsole(false, false, tablePro.GetSeparator("="));
	}
};

SCMD(kz_ctop, SCFL_RECORD | SCFL_GLOBAL)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	// Повторный !maptop закрывает окно: под захватом курсора это выход без клика по ✕.
	if (player && KZ::gsmenu::IsOpenBy(player, "maptop"))
	{
		KZ::gsmenu::Close(player, "command_toggle");
		return true;
	}
	CourseTopRequest::Create<CourseTopRequest>(player, CourseTopRequest::ctopFeatures, true, true, args);
	return true;
}

SCMD_LINK(kz_coursetop, kz_ctop);
// !maptop — единственный из тройки ctop/coursetop/maptop в whitelist !help.
// Показывает в консоль топ-10 PRO (0 телепортов) и топ-10 NUB (overall) текущего
// курса/карты из локальной БД (+ глобал, если режим поддержан api).
SCMD_LINK(kz_maptop, kz_ctop, SCFL_HELP);

SCMD(kz_gctop, SCFL_RECORD | SCFL_GLOBAL)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	CourseTopRequest::Create<CourseTopRequest>(player, CourseTopRequest::ctopFeatures, false, true, args);
	return true;
}

SCMD_LINK(kz_gcoursetop, kz_gctop);
SCMD_LINK(kz_gmaptop, kz_gctop);

SCMD(kz_sctop, SCFL_TIMER)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	CourseTopRequest::Create<CourseTopRequest>(player, CourseTopRequest::ctopFeatures, true, false, args);
	return true;
}

SCMD_LINK(kz_scoursetop, kz_sctop);
SCMD_LINK(kz_smaptop, kz_sctop);

// === Окно !maptop (общее panorama-окно, layout/gsmenu.cpp) ====================================
//
// Показывает то же, что консольная таблица: локальный топ сервера (если его нет — глобальный),
// PRO или общий. PRO/NUB переключаются без запроса (оба списка уже пришли), режим и страница —
// повтором запроса с теми же картой и курсом. «Когда» — Times.Created локального топа (у глобального пусто).

namespace
{
	// Окно показывает топ-10 и свою строку: команда !maptop отдаёт 10 (консольная таблица), сразу за ней
	// окно тихо дозапрашивает 100 — чтобы найти свой рекорд ниже десятки.
	constexpr u64 kMaptopLimit = 100;
	constexpr i32 kMaptopShow = 10;

	struct MaptopView
	{
		CUtlString map, course, mode;
		i32 page = 0;
		bool pro = true;
		u64 pendingUid = 0;     // последний перезапрос из окна — показываем только его ответ
		bool qLocal = true, qGlobal = true; // откуда брала данные команда (!maptop/!gmaptop/!smaptop)
		u64 baseOffset = 0;                 // o= команды — места считаются от него
		f64 lastRequery = -1.0; // клики по режимам не чаще 2 в секунду
		std::vector<std::string> runIds; // строка на экране -> id рана (реплей)
		std::string meRunId;             // своя строка под топом (ран ниже десятки)
		std::vector<std::string> courseNames; // вкладка gf0_k -> имя курса для перезапроса (только текущая карта)
		CourseTopRequest::CourseTopData data;
		bool global = false;
	};

	MaptopView g_maptop[MAXPLAYERS + 1];

	constexpr const char *kMaptopModes[] = {"CKZ", "KZT", "VNL", "SKZ"}; // порядок как в !jstop

	void RenderMaptop(KZPlayer *player);

	// Создание запроса — ТОЛЬКО отложенно, таймером на следующий тик. Ответ окна (ShowWindow) приходит
	// из BaseRequest::CheckRequests, который обходит BaseRequest::instances через remove_if: новый запрос,
	// созданный прямо там, делал push_back в тот же вектор посреди обхода — переаллокация, обход по
	// освобождённой памяти, segv (канарейка 13.10, 0.305.0, кадр CheckRequests::$_2).
	f64 DoRequeryMaptop(CPlayerUserId userID, CUtlString mode, CUtlString course)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
		if (!player)
		{
			return 0.0;
		}
		MaptopView &v = g_maptop[player->GetPlayerSlot().Get()];
		// Без кавычек: ParseArgsToKV3 берёт значение до следующего «ключ=» (пробелы в имени курса не
		// мешают), а кавычки он НЕ снимает — с ними курс искался как "Main" и таблица была пустой.
		CCommand args;
		char line[256];
		V_snprintf(line, sizeof(line), "kz_maptop map=%s course=%s mode=%s l=%llu", v.map.Get(), course.Get(), mode.Get(), kMaptopLimit);
		args.Tokenize(line);
		CourseTopRequest *req = CourseTopRequest::Create<CourseTopRequest>(player, CourseTopRequest::ctopFeatures, v.qLocal, v.qGlobal, &args);
		req->fromWindow = true;
		v.pendingUid = req->uid;
		return 0.0;
	}

	// course — курс перезапроса; v.course меняет только ответ (ShowWindow), иначе отброшенный троттлингом
	// клик по вкладке курса оставил бы окно с вкладкой нового курса над таблицей старого.
	void RequeryMaptop(KZPlayer *player, const char *mode, const char *course, bool throttle)
	{
		MaptopView &v = g_maptop[player->GetPlayerSlot().Get()];
		const f64 now = g_pKZUtils->GetServerGlobals()->curtime;
		if (throttle && v.lastRequery >= 0.0 && now - v.lastRequery < 0.5)
		{
			return;
		}
		if (throttle)
		{
			v.lastRequery = now;
		}
		if (!player->GetClient())
		{
			return;
		}
		// Ответы, пришедшие до отложенного создания, уже чужие.
		v.pendingUid = UINT64_MAX;
		StartTimer<CPlayerUserId, CUtlString, CUtlString>(DoRequeryMaptop, player->GetClient()->GetUserID(), CUtlString(mode), CUtlString(course), 0.0, false);
	}

	CUtlVector<CourseTopRequest::RunStats> &MaptopList(MaptopView &v)
	{
		return v.pro ? v.data.proData : v.data.overallData;
	}

	void OnMaptopClick(KZPlayer *player, const char *id)
	{
		MaptopView &v = g_maptop[player->GetPlayerSlot().Get()];
		i32 row = -1, k = -1;
		if (KZ_STREQ(id, "gf1_0") || KZ_STREQ(id, "gf1_1"))
		{
			v.pro = KZ_STREQ(id, "gf1_0");
			v.page = 0;
			RenderMaptop(player);
			return;
		}
		if (sscanf(id, "gf0_%d", &k) == 1 && k >= 0)
		{
			if (k < (i32)v.courseNames.size() && V_stricmp(v.courseNames[k].c_str(), v.course.Get()))
			{
				RequeryMaptop(player, v.mode.Get(), v.courseNames[k].c_str(), true);
			}
			return;
		}
		if (sscanf(id, "gf2_%d", &k) == 1 && k >= 0)
		{
			i32 shown = 0;
			for (const char *mode : kMaptopModes)
			{
				if (KZ::mode::GetModeInfo(CUtlString(mode)).id < 0)
				{
					continue;
				}
				if (shown++ == k)
				{
					v.page = 0;
					RequeryMaptop(player, mode, v.course.Get(), true);
					return;
				}
			}
			return;
		}
		// ▶ строки gr{i} или своей строки gm (ран ниже десятки).
		std::string runId;
		if (KZ_STREQ(id, "gm_go"))
		{
			runId = v.meRunId;
		}
		else if (sscanf(id, "gr%d_go", &row) == 1 && row >= 0 && row < (i32)v.runIds.size())
		{
			runId = v.runIds[row];
		}
		if (runId.empty())
		{
			return;
		}
		// Реплей грузит команда !replay — с её проверками, ожиданием загрузки и сообщениями; зовём её
		// обработчик на сервере (как если бы игрок набрал её сам), без круга через клиента.
		KZ::gsmenu::Close(player, "watch_replay");
		CCommand args;
		char line[96];
		V_snprintf(line, sizeof(line), "kz_replay %s", runId.c_str());
		args.Tokenize(line);
		CPlayerSlot slot = player->GetPlayerSlot();
		scmd::OnClientCommand(slot, args);
	}

	// «YYYY-MM-DD HH:MM:SS» (Times.Created) → «14.08.2026» — колонка «Дата» (решение владельца 14.10).
	std::string RecordDate(const char *ts)
	{
		int y = 0, mo = 0, d = 0;
		if (!ts || sscanf(ts, "%d-%d-%d", &y, &mo, &d) != 3 || mo < 1 || mo > 12 || d < 1 || d > 31)
		{
			return "";
		}
		char buf[16];
		V_snprintf(buf, sizeof(buf), "%02d.%02d.%04d", d, mo, y);
		return buf;
	}

	// Тир карты для бейджа в шапке окна — из пула GG1 (cfg/GGMCmaps.json, рендерит оркестратор):
	// {"<id>": {"display": "kz_x", "tier_kzt": 5, "tier_ckz": 4, "tier_vnl": null}}. 0 — тира нет.
	i32 MapTier(const char *map, const char *modeShort)
	{
		char path[512];
		g_SMAPI->PathFormat(path, sizeof(path), "%s/cfg/GGMCmaps.json", g_SMAPI->GetBaseDir());
		std::ifstream f(path);
		if (!f || !map || !modeShort)
		{
			return 0;
		}
		std::stringstream ss;
		ss << f.rdbuf();
		const nlohmann::json pool = nlohmann::json::parse(ss.str(), nullptr, false);
		if (!pool.is_object())
		{
			return 0;
		}
		std::string key = std::string("tier_") + modeShort;
		for (char &c : key)
		{
			c = (char)tolower((unsigned char)c);
		}
		for (const auto &[id, m] : pool.items())
		{
			if (!m.is_object() || !m.contains("display") || !m["display"].is_string() || V_stricmp(m["display"].get<std::string>().c_str(), map) != 0)
			{
				continue;
			}
			if (m.contains(key) && m[key].is_number_integer())
			{
				return m[key].get<i32>();
			}
			return 0;
		}
		return 0;
	}

	// Строка топа: место · ник · время · отставание от 1-го · когда.
	KZ::gsmenu::Row MaptopRow(CourseTopRequest::RunStats &r, i32 rank, f64 best, u64 me)
	{
		KZ::gsmenu::Row row;
		row.cells[0] = std::to_string(rank);
		row.cells[1] = r.name.Get();
		row.cells[2] = r.GetTime().Get();
		if (rank > 1 && best > 0.0)
		{
			row.cells[3] = std::string("+") + utils::FormatTime(r.time - best).Get();
		}
		row.cells[4] = RecordDate(r.created.Get());
		row.initial = KZ::gsmenu::Initial(r.name.Get());
		row.colour = (u32)(r.steamid64 % 8);
		row.wr = rank == 1;
		row.me = r.steamid64 == me;
		return row;
	}

	void RenderMaptop(KZPlayer *player)
	{
		MaptopView &v = g_maptop[player->GetPlayerSlot().Get()];
		CUtlVector<CourseTopRequest::RunStats> &list = MaptopList(v);
		v.page = 0; // топ-10 без листания (решение владельца 14.10), своя строка закреплена снизу

		KZ::gsmenu::Table t;
		t.title = v.map.Get();
		t.sub = std::string(v.course.Get()) + " · " + (v.global ? "мировой топ" : "топ сервера");
		t.cover = v.map.Get();
		t.filters[1] = {{"PRO", v.pro}, {"NUB", !v.pro}};
		// Курсы (Main / Bonus N) — вкладками слева в полосе режимов (не больше 8 — ёмкость разметки); список курсов есть только у карты,
		// что сейчас на сервере, у чужой карты (!maptop <карта>) полоса остаётся без них.
		v.courseNames.clear();
		if (!V_stricmp(v.map.Get(), g_pKZUtils->GetCurrentMapName().Get()))
		{
			KZ::course::CourseTab tabs[KZ_GSMENU_FILTER_CHIPS];
			const i32 n = KZ::course::GetCourseTabs(tabs, KZ_GSMENU_FILTER_CHIPS);
			for (i32 i = 0; i < n; i++)
			{
				t.filters[0].push_back({tabs[i].label, !V_stricmp(tabs[i].course->name, v.course.Get())});
				v.courseNames.push_back(tabs[i].course->name);
			}
		}
		const CUtlString curShort = KZ::mode::GetModeInfo(v.mode).shortModeName;
		t.tier = MapTier(v.map.Get(), curShort.Get());
		for (const char *mode : kMaptopModes)
		{
			if (KZ::mode::GetModeInfo(CUtlString(mode)).id >= 0)
			{
				t.filters[2].push_back({mode, !V_stricmp(curShort.Get(), mode)});
			}
		}
		t.headers[0] = "Место";
		t.headers[1] = "Игрок";
		t.headers[2] = "Время";
		t.headers[3] = "Отставание";
		t.headers[4] = "Дата";

		v.runIds.clear();
		v.meRunId.clear();
		const u64 me = player->GetSteamId64();
		const f64 best = list.Count() > 0 ? list[0].time : 0.0;
		bool meShown = false;
		for (i32 i = 0; i < list.Count() && i < kMaptopShow; i++)
		{
			KZ::gsmenu::Row row = MaptopRow(list[i], (i32)v.baseOffset + i + 1, best, me);
			meShown |= row.me;
			t.rows.push_back(row);
			v.runIds.push_back(list[i].runID.Get());
		}
		// Своя строка — когда свой рекорд есть в загруженном топе, но ниже десятки.
		for (i32 i = kMaptopShow; !meShown && i < list.Count(); i++)
		{
			if (list[i].steamid64 != me)
			{
				continue;
			}
			KZ::gsmenu::Row row = MaptopRow(list[i], (i32)v.baseOffset + i + 1, best, me);
			t.hasMe = true;
			for (i32 j = 0; j < KZ_GSMENU_TABLE_COLS; j++)
			{
				t.me.cells[j] = row.cells[j];
			}
			t.me.initial = row.initial;
			t.me.colour = row.colour;
			t.me.actions[0] = "▶"; // показать кнопку реплея своей строки (gm_go)
			v.meRunId = list[i].runID.Get();
			break;
		}
		t.empty = t.rows.empty();
		if (t.rows.empty())
		{
			t.hint = v.pro ? "PRO-рекордов на этом курсе пока нет" : "Рекордов на этом курсе пока нет";
		}
		KZ::gsmenu::ShowTable(player, t, OnMaptopClick, "maptop");
	}
} // namespace

bool CourseTopRequest::ShowWindow()
{
	CourseTopRequest *req = this;
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(req->userID);
	if (!player || !KZ::gsmenu::Available(player))
	{
		return false;
	}
	MaptopView &v = g_maptop[player->GetPlayerSlot().Get()];
	// Ответ на перезапрос из окна — только последний и только пока окно открыто (игрок мог его
	// закрыть или нащёлкать новые фильтры, пока шёл запрос). true — ответ «обработан», в чат не пишем.
	if (req->fromWindow && (req->uid != v.pendingUid || !KZ::gsmenu::IsOpenBy(player, "maptop")))
	{
		return true;
	}
	const bool local = req->localStatus == ResponseStatus::RECEIVED;
	v.map = req->mapName;
	v.course = req->courseName;
	v.mode = req->modeName;
	v.global = !local;
	v.data = CourseTopRequest::CourseTopData();
	const CourseTopRequest::CourseTopData &src = local ? req->srData : req->wrData;
	v.data.overallData = src.overallData;
	v.data.proData = src.proData;
	if (!req->fromWindow)
	{
		v.page = 0;
		v.pendingUid = 0; // окно открыто командой — ответы прежних перезапросов из окна уже чужие
		v.qLocal = req->localStatus != ResponseStatus::DISABLED;
		v.qGlobal = req->globalStatus != ResponseStatus::DISABLED;
	}
	v.baseOffset = req->offset; // перезапрос из окна идёт с нуля — места тоже с нуля
	RenderMaptop(player);
	// Команда отдала первую страницу — дозапрашиваем полный топ для листания (тихо, без консоли).
	const bool full = req->limit >= kMaptopLimit;
	// Дозапрос не трогает троттлинг кликов и не делается при o= (там листать от нуля незачем).
	if (!req->fromWindow && !full && req->offset == 0 && KZ::gsmenu::IsOpenBy(player, "maptop")
		&& (src.overallData.Count() >= kMaptopShow || src.proData.Count() >= kMaptopShow))
	{
		RequeryMaptop(player, v.mode.Get(), v.course.Get(), false);
	}
	return KZ::gsmenu::IsOpen(player);
}
