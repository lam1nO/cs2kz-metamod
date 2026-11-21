#include "kz_db.h"
#include "vendor/sql_mm/src/public/sql_mm.h"
#include "queries/course_top.h"

void KZDatabaseService::QueryAllRecords(CUtlString mapName, TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure)
{
	std::string cleanedMapName = KZDatabaseService::GetDatabaseConnection()->Escape(mapName.Get());

	Transaction txn;

	char query[2048];
	// Get PB
	V_snprintf(query, sizeof(query), sql_getsrs, cleanedMapName.c_str());
	txn.queries.push_back(query);

	// Get Rank
	V_snprintf(query, sizeof(query), sql_getsrspro, cleanedMapName.c_str());
	txn.queries.push_back(query);

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}

void KZDatabaseService::QueryRecords(CUtlString mapName, CUtlString courseName, u32 modeID, u32 count, u32 offset,
									 TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure, u64 styleMask, bool includeStyleCounts, u64 personalSteamId)
{
	std::string cleanedMapName = KZDatabaseService::GetDatabaseConnection()->Escape(mapName.Get());
	std::string cleanedCourseName = KZDatabaseService::GetDatabaseConnection()->Escape(courseName.Get());

	Transaction txn;

	char query[2048];
	// Get PB
	V_snprintf(query, sizeof(query), sql_getcoursetop, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID, styleMask, count, offset);
	txn.queries.push_back(query);

	// Get Rank
	V_snprintf(query, sizeof(query), sql_getcoursetoppro, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID, styleMask, count, offset);
	txn.queries.push_back(query);

	if (includeStyleCounts)
	{
		V_snprintf(query, sizeof(query), sql_get_course_styles, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID);
		txn.queries.push_back(query);
	}
	if (personalSteamId)
	{
		for (int pro = 0; pro <= 1; ++pro)
		{
			V_snprintf(query, sizeof(query), sql_get_course_personal, cleanedMapName.c_str(), cleanedCourseName.c_str(), modeID,
					   styleMask, pro, personalSteamId);
			txn.queries.push_back(query);
		}
	}

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}
