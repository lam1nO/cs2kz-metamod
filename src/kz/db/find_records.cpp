#include "kz_db.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
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
									 TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure, u64 styleMask,
									 bool includeStyleCounts, u64 personalSteamId, bool mixedStyles)
{
	auto *connection = KZDatabaseService::GetDatabaseConnection();
	const std::string map = connection->Escape(mapName.Get());
	const std::string course = connection->Escape(courseName.Get());
	const i32 recordModeID = KZ::mode::GetRecordModeID(modeID, styleMask);
	const CUtlString mode = KZ::mode::GetModeInfoFromDatabaseID(modeID).shortModeName;
	const i32 nopreRecordModeID = mode == "CKZ" ? KZ::mode::GetNopreRecordModeID(modeID) : -1;

	u64 allowedMask = 0, nopreBit = 0;
	if (mixedStyles || includeStyleCounts)
	{
		const auto &styles = g_pKZStyleManager->GetStyleInfos();
		FOR_EACH_VEC(styles, i)
		{
			const auto &style = styles[i];
			if (style.databaseID < 0 || style.databaseID >= 32 || !style.shortName)
			{
				continue;
			}
			const u64 bit = 1ull << style.databaseID;
			if (KZ::style::CanUseStyleInMode(style.shortName, mode.Get()))
			{
				allowedMask |= bit;
			}
			if (!V_stricmp(style.shortName, KZ::style::NopreName))
			{
				nopreBit = bit;
			}
		}
	}

	Transaction txn;
	char query[4096];
	for (i32 pro = 0; pro <= 1; ++pro)
	{
		if (mixedStyles)
		{
			V_snprintf(query, sizeof(query), sql_get_course_styles_top, map.c_str(), course.c_str(), allowedMask, modeID, nopreBit,
					   nopreRecordModeID, nopreBit, pro, count, offset);
		}
		else
		{
			V_snprintf(query, sizeof(query), pro ? sql_getcoursetoppro : sql_getcoursetop, map.c_str(), course.c_str(), recordModeID,
					   styleMask, count, offset);
		}
		txn.queries.push_back(query);
	}
	if (includeStyleCounts)
	{
		V_snprintf(query, sizeof(query), sql_get_course_styles, map.c_str(), course.c_str(), allowedMask, modeID, nopreBit, nopreRecordModeID,
				   nopreBit);
		txn.queries.push_back(query);
	}
	if (personalSteamId)
	{
		for (i32 pro = 0; pro <= 1; ++pro)
		{
			if (mixedStyles)
			{
				V_snprintf(query, sizeof(query), sql_get_course_styles_personal, map.c_str(), course.c_str(), allowedMask, modeID, nopreBit,
						   nopreRecordModeID, nopreBit, pro, personalSteamId);
			}
			else
			{
				V_snprintf(query, sizeof(query), sql_get_course_personal, map.c_str(), course.c_str(), recordModeID, styleMask, pro, personalSteamId);
			}
			txn.queries.push_back(query);
		}
	}
	connection->ExecuteTransaction(txn, onSuccess, onFailure);
}
