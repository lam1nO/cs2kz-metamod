#include "kz_db.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
#include "kz/timer/kz_timer.h"
#include "kz/hud/layout/gsmenu.h"
#include "queries/save_time.h"
#include "queries/times.h"
#include "utils/uuid.h"
#include "vendor/sql_mm/src/public/sql_mm.h"

using namespace KZ::Database;

void KZDatabaseService::SaveTime(const char *runUUID, u64 steamID, u32 courseID, i32 modeID, f64 time, u64 teleportsUsed, u64 styleIDs,
								 std::string_view metadata, TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure)
{
	if (!KZDatabaseService::IsReady())
	{
		return;
	}

	// Сетка карт GG1 (gsmenu_fill.cpp): новое время сбрасывает кэш PB игрока и, если это рекорд, WR режима.
	TransactionSuccessCallbackFunc inner = onSuccess;
	onSuccess = [inner, steamID, modeID, time, styleIDs](std::vector<ISQLQuery *> queries)
	{
		if (styleIDs == 0)
		{
			KZ::gsmenu::FillOnTimeSaved(steamID, modeID, time);
		}
		if (inner)
		{
			inner(queries);
		}
	};

	char query[2048];
	Transaction txn;

	// Always use UUID insert since all migrations must be applied for the plugin to run
	V_snprintf(query, sizeof(query), sql_times_insert, runUUID, steamID, courseID, modeID, styleIDs, time, teleportsUsed, metadata.data());
	txn.queries.push_back(query);
	// Rank every exact style combination independently.
	// Get Top 2 PRO PBs
	V_snprintf(query, sizeof(query), sql_getpb, courseID, steamID, modeID, styleIDs, 2);
	txn.queries.push_back(query);
	// Get Rank
	V_snprintf(query, sizeof(query), sql_getmaprank, courseID, modeID, styleIDs, steamID, courseID, modeID, styleIDs);
	txn.queries.push_back(query);
	// Get Number of Players with Times
	V_snprintf(query, sizeof(query), sql_getlowestmaprank, courseID, modeID, styleIDs);
	txn.queries.push_back(query);
	if (teleportsUsed == 0)
	{
		// Get Top 2 PRO PBs
		V_snprintf(query, sizeof(query), sql_getpbpro, courseID, steamID, modeID, styleIDs, 2);
		txn.queries.push_back(query);
		// Get PRO Rank
		V_snprintf(query, sizeof(query), sql_getmaprankpro, courseID, modeID, styleIDs, steamID, courseID, modeID, styleIDs);
		txn.queries.push_back(query);
		// Get Number of Players with Times
		V_snprintf(query, sizeof(query), sql_getlowestmaprankpro, courseID, modeID, styleIDs);
		txn.queries.push_back(query);
	}
	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}
