#include "base_request.h"
#include "kz/timer/kz_timer.h"
#include "kz/db/kz_db.h"
#include "kz/global/kz_global.h"

#include "utils/simplecmds.h"
#include "kz/hud/layout/gsmenu.h"
#include "kz/mode/kz_mode.h"
#include "kz/hud/kz_hud.h"
#include "utils/interfaces.h"

#include "vendor/sql_mm/src/public/sql_mm.h"
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
							{result->GetString(0), result->GetString(2), (u64)result->GetInt64(4), result->GetFloat(3), (u64)result->GetInt64(1)});
					}
				}

				if ((result = queries[1]->GetResultSet()) && result->GetRowCount() > 0)
				{
					while (result->FetchRow())
					{
						req->srData.proData.AddToTail({result->GetString(0), result->GetString(2), 0, result->GetFloat(3), (u64)result->GetInt64(1)});
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
	if (player && KZ::gsmenu::IsOpen(player))
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
// повтором запроса с теми же картой и курсом. Даты рана локальный запрос не отдаёт — колонки нет.

namespace
{
	// Окно грузит до 100 записей одним запросом и листает по 10 у себя: команда !maptop отдаёт 10
	// (консольная таблица), сразу за ней окно тихо дозапрашивает 100 — листание без запросов.
	constexpr u64 kMaptopLimit = 100;
	constexpr i32 kMaptopPage = KZ_GSMENU_TABLE_ROWS;

	struct MaptopView
	{
		CUtlString map, course, mode;
		i32 page = 0;
		bool pro = true;
		u64 pendingUid = 0;     // последний перезапрос из окна — показываем только его ответ
		f64 lastRequery = -1.0; // клики по режимам не чаще 2 в секунду
		std::vector<std::string> runIds; // строка на экране -> id рана (реплей)
		CourseTopRequest::CourseTopData data;
		bool global = false;
	};

	MaptopView g_maptop[MAXPLAYERS + 1];

	constexpr const char *kMaptopModes[] = {"KZT", "CKZ", "VNL", "SKZ"};

	void RenderMaptop(KZPlayer *player);

	void RequeryMaptop(KZPlayer *player, const char *mode, bool throttle)
	{
		MaptopView &v = g_maptop[player->GetPlayerSlot().Get()];
		const f64 now = g_pKZUtils->GetServerGlobals()->curtime;
		if (throttle && v.lastRequery >= 0.0 && now - v.lastRequery < 0.5)
		{
			return;
		}
		v.lastRequery = now;
		// Без кавычек: ParseArgsToKV3 берёт значение до следующего «ключ=» (пробелы в имени курса не
		// мешают), а кавычки он НЕ снимает — с ними курс искался как "Main" и таблица была пустой.
		CCommand args;
		char line[256];
		V_snprintf(line, sizeof(line), "kz_maptop map=%s course=%s mode=%s l=%llu", v.map.Get(), v.course.Get(), mode, kMaptopLimit);
		args.Tokenize(line);
		CourseTopRequest *req = CourseTopRequest::Create<CourseTopRequest>(player, CourseTopRequest::ctopFeatures, true, true, &args);
		req->fromWindow = true;
		v.pendingUid = req->uid;
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
					RequeryMaptop(player, mode, true);
					return;
				}
			}
			return;
		}
		if (KZ_STREQ(id, "gw_prev") || KZ_STREQ(id, "gw_next"))
		{
			const i32 pages = (MaptopList(v).Count() + kMaptopPage - 1) / kMaptopPage;
			v.page = KZ_STREQ(id, "gw_next") ? (std::min)(v.page + 1, (std::max)(pages - 1, 0)) : (std::max)(v.page - 1, 0);
			RenderMaptop(player);
			return;
		}
		if (sscanf(id, "gr%d_go", &row) != 1 || row < 0 || row >= (i32)v.runIds.size())
		{
			return;
		}
		// Реплей грузит команда !replay — с её проверками, ожиданием загрузки и сообщениями; зовём её
		// обработчик на сервере (как если бы игрок набрал её сам), без круга через клиента.
		KZ::gsmenu::Close(player, "watch_replay");
		CCommand args;
		char line[96];
		V_snprintf(line, sizeof(line), "kz_replay %s", v.runIds[row].c_str());
		args.Tokenize(line);
		CPlayerSlot slot = player->GetPlayerSlot();
		scmd::OnClientCommand(slot, args);
	}

	void RenderMaptop(KZPlayer *player)
	{
		MaptopView &v = g_maptop[player->GetPlayerSlot().Get()];
		CUtlVector<CourseTopRequest::RunStats> &list = MaptopList(v);
		const i32 pages = (list.Count() + kMaptopPage - 1) / kMaptopPage;
		v.page = (std::max)(0, (std::min)(v.page, pages - 1));

		KZ::gsmenu::Table t;
		t.title = v.map.Get();
		t.sub = std::string(v.course.Get()) + " · " + (v.global ? "мировой топ" : "топ сервера");
		t.cover = v.map.Get();
		t.noTp = v.pro;
		t.filters[1] = {{"PRO", v.pro}, {"NUB", !v.pro}};
		const CUtlString curShort = KZ::mode::GetModeInfo(v.mode).shortModeName;
		for (const char *mode : kMaptopModes)
		{
			if (KZ::mode::GetModeInfo(CUtlString(mode)).id >= 0)
			{
				t.filters[2].push_back({mode, !V_stricmp(curShort.Get(), mode)});
			}
		}
		t.headers[0] = "№";
		t.headers[1] = "Игрок";
		t.headers[2] = "Время";
		t.headers[3] = "TP";
		t.headers[4] = "До WR";

		v.runIds.clear();
		const u64 me = player->GetSteamId64();
		const f64 best = list.Count() > 0 ? list[0].time : 0.0;
		const i32 first = v.page * kMaptopPage;
		for (i32 i = first; i < list.Count() && i < first + kMaptopPage; i++)
		{
			CourseTopRequest::RunStats &r = list[i];
			KZ::gsmenu::Row row;
			const i32 rank = i + 1;
			row.cells[0] = std::to_string(rank);
			row.cells[1] = r.name.Get();
			row.cells[2] = r.GetTime().Get();
			row.cells[3] = v.pro ? "" : std::to_string(r.teleportsUsed);
			row.initial = KZ::gsmenu::Initial(r.name.Get());
			row.colour = (u32)(r.steamid64 % 8);
			row.wr = rank == 1;
			row.me = r.steamid64 == me;
			if (rank > 1 && best > 0.0)
			{
				row.cells[4] = std::string("+") + utils::FormatTime(r.time - best).Get();
			}
			t.rows.push_back(row);
			v.runIds.push_back(r.runID.Get());
		}
		if (t.rows.empty())
		{
			t.hint = v.pro ? "PRO-рекордов на этом курсе пока нет" : "Рекордов на этом курсе пока нет";
		}
		// Листание — только когда записей больше одной страницы.
		if (pages > 1)
		{
			char page[32];
			V_snprintf(page, sizeof(page), "%i / %i", v.page + 1, pages);
			t.page = page;
			t.prev = v.page > 0;
			t.next = v.page < pages - 1;
		}
		KZ::gsmenu::ShowTable(player, t, OnMaptopClick);
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
	if (req->fromWindow && (req->uid != v.pendingUid || !KZ::gsmenu::IsOpen(player)))
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
	}
	RenderMaptop(player);
	// Команда отдала первую страницу — дозапрашиваем полный топ для листания (тихо, без консоли).
	const bool full = req->limit >= kMaptopLimit;
	if (!req->fromWindow && !full && KZ::gsmenu::IsOpen(player)
		&& (src.overallData.Count() >= kMaptopPage || src.proData.Count() >= kMaptopPage))
	{
		RequeryMaptop(player, v.mode.Get(), false);
	}
	return KZ::gsmenu::IsOpen(player);
}

// === Пройденные карты игрока для фильтра «Не пройденные» GG1 (сетки !nominate/!maps) ============
//
// GG1 (CSSharp) спрашивает `kz_gsm_done <slot>` с консоли сервера; ответ — серия серверных команд
// `css_gg1_done <slot> <карта,карта,...>` (пачками, чтобы не упереться в длину команды) и в конце
// `css_gg1_done_end <slot>`. Карты — с любым временем в текущем режиме игрока (любой курс, любой стиль).
CON_COMMAND_F(kz_gsm_done, "kz_gsm_done <slot> — пройденные игроком карты в его режиме (для фильтра GG1)", FCVAR_NONE)
{
	if (utils::GetController(context.GetPlayerSlot()) || args.ArgC() < 2)
	{
		return;
	}
	const i32 slot = V_StringToInt32(args.Arg(1), -1);
	KZPlayer *player = (slot >= 0 && slot < MAXPLAYERS) ? g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot)) : nullptr;
	auto sendEnd = [slot]()
	{
		char end[48];
		V_snprintf(end, sizeof(end), "css_gg1_done_end %i\n", slot);
		interfaces::pEngine->ServerCommand(end);
	};
	if (!player || !player->IsInGame() || !player->GetClient() || !KZDatabaseService::IsReady() || !player->modeService)
	{
		sendEnd();
		return;
	}
	const i32 modeID = KZ::mode::GetModeInfo(player->modeService).databaseID;
	char query[512];
	V_snprintf(query, sizeof(query),
			   "SELECT DISTINCT m.Name FROM Times t INNER JOIN MapCourses mc ON mc.ID = t.MapCourseID "
			   "INNER JOIN Maps m ON m.ID = mc.MapID WHERE t.SteamID64 = %llu AND t.ModeID = %d",
			   player->GetSteamId64(), modeID);
	Transaction txn;
	txn.queries.push_back(query);
	const CPlayerUserId uid = player->GetClient()->GetUserID();
	auto onSuccess = [slot, uid, sendEnd](std::vector<ISQLQuery *> queries)
	{
		KZPlayer *p = g_pKZPlayerManager->ToPlayer(uid);
		if (!p || p->GetPlayerSlot().Get() != slot)
		{
			return; // игрок ушёл — слот чужой, ответ некому
		}
		std::string chunk;
		auto flush = [&]()
		{
			if (chunk.empty())
			{
				return;
			}
			char cmd[512];
			V_snprintf(cmd, sizeof(cmd), "css_gg1_done %i %s\n", slot, chunk.c_str());
			interfaces::pEngine->ServerCommand(cmd);
			chunk.clear();
		};
		ISQLResult *result = queries.empty() ? nullptr : queries[0]->GetResultSet();
		while (result && result->FetchRow())
		{
			const char *name = result->GetString(0);
			// В команду — только безопасные имена карт (буквы, цифры, _ - .), без разделителей консоли.
			if (!name || !name[0] || strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != strlen(name))
			{
				continue;
			}
			if (chunk.size() + strlen(name) + 1 > 380)
			{
				flush();
			}
			if (!chunk.empty())
			{
				chunk += ',';
			}
			chunk += name;
		}
		flush();
		sendEnd();
	};
	auto onFailure = [sendEnd](std::string, int) { sendEnd(); };
	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}
