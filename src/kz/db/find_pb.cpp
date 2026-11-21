#include "kz_db.h"
#include "kz/mode/kz_mode.h"
#include "vendor/sql_mm/src/public/sql_mm.h"
#include "queries/personal_best.h"

void KZDatabaseService::QueryPB(u64 steamID64, CUtlString mapName, CUtlString courseName, u32 modeID, TransactionSuccessCallbackFunc onSuccess,
								TransactionFailureCallbackFunc onFailure, u64 styleMask)
{
	std::string cleanedMapName = KZDatabaseService::GetDatabaseConnection()->Escape(mapName.Get());
	std::string cleanedCourseName = KZDatabaseService::GetDatabaseConnection()->Escape(courseName.Get());

	modeID = KZ::mode::GetRecordModeID(modeID, styleMask);
	Transaction txn;

	char query[2048];
	// Get PB
	V_snprintf(query, sizeof(query), sql_getpb, steamID64, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID, styleMask, 1);
	txn.queries.push_back(query);

	// Get Rank
	V_snprintf(query, sizeof(query), sql_getmaprank, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID, styleMask, steamID64, cleanedMapName.c_str(),
			   cleanedCourseName.c_str(), modeID, styleMask);
	txn.queries.push_back(query);

	// Get Number of Players with Times
	V_snprintf(query, sizeof(query), sql_getlowestmaprank, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID, styleMask);
	txn.queries.push_back(query);

	// Get PRO PB
	V_snprintf(query, sizeof(query), sql_getpbpro, steamID64, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID, styleMask, 1);
	txn.queries.push_back(query);

	// Get PRO Rank
	V_snprintf(query, sizeof(query), sql_getmaprankpro, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID, styleMask, steamID64, cleanedMapName.c_str(),
			   cleanedCourseName.c_str(), modeID, styleMask);
	txn.queries.push_back(query);

	// Get Number of Players with Times
	V_snprintf(query, sizeof(query), sql_getlowestmaprankpro, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID, styleMask);
	txn.queries.push_back(query);

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}

void KZDatabaseService::QueryPBRankless(u64 steamID64, CUtlString mapName, CUtlString courseName, u32 modeID, u64 styleIDFlags,
										TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure)
{
	std::string cleanedMapName = KZDatabaseService::GetDatabaseConnection()->Escape(mapName.Get());

	std::string cleanedCourseName = KZDatabaseService::GetDatabaseConnection()->Escape(courseName.Get());

	modeID = KZ::mode::GetRecordModeID(modeID, styleIDFlags);
	char query[2048];
	Transaction txn;
	// Get PB
	V_snprintf(query, sizeof(query), sql_getpb, steamID64, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID, styleIDFlags, 1);
	txn.queries.push_back(query);
	// Get PRO PB
	V_snprintf(query, sizeof(query), sql_getpbpro, steamID64, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID, styleIDFlags, 1);
	txn.queries.push_back(query);

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}

void KZDatabaseService::QueryAllPBs(u64 steamID64, CUtlString mapName, TransactionSuccessCallbackFunc onSuccess,
									TransactionFailureCallbackFunc onFailure)
{
	std::string cleanedMapName = KZDatabaseService::GetDatabaseConnection()->Escape(mapName.Get());

	Transaction txn;

	char query[2048];
	// Get PB
	V_snprintf(query, sizeof(query), sql_getpbs, steamID64, cleanedMapName.c_str());
	txn.queries.push_back(query);
	// Get PRO PB
	V_snprintf(query, sizeof(query), sql_getpbspro, steamID64, cleanedMapName.c_str());
	txn.queries.push_back(query);

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}
