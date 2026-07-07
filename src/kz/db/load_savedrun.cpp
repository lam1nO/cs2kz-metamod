#include <string>

#include "kz_db.h"
#include "queries/savedruns.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

using namespace KZ::Database;

// SavedRuns (Task 4): fetch по ключу поиска (без Course — не знаем его заранее). Fire-and-forget
// в смысле состояния игрока: DB-слой не резолвит KZPlayer* сам (может исчезнуть за время
// async round-trip) — это ответственность вызывающей стороны в onSuccess/onFailure (см.
// setup_client.cpp: userID-паттерн). Здесь только строим и исполняем запрос.
void KZDatabaseService::FetchSavedRun(u64 steamID64, CUtlString mapName, CUtlString mode, CUtlString styles,
									  TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure)
{
	if (!KZDatabaseService::IsReady())
	{
		onFailure("Database not ready.", 0);
		return;
	}

	ISQLConnection *db = KZDatabaseService::GetDatabaseConnection();
	std::string cleanMapName = db->Escape(mapName.Get());
	std::string cleanMode = db->Escape(mode.Get());
	std::string cleanStyles = db->Escape(styles.Get());

	char query[2048];
	V_snprintf(query, sizeof(query), sql_savedruns_fetch, steamID64, cleanMapName.c_str(), cleanMode.c_str(), cleanStyles.c_str());

	Transaction txn;
	txn.queries.push_back(query);

	db->ExecuteTransaction(txn, onSuccess, onFailure);
}
