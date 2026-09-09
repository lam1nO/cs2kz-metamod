#include <string>
#include <vector>

#include "kz_db.h"
#include "queries/hudshares.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

using namespace KZ::Database;

// Обмен настройками худа: доступ к таблице HudShares через ТОТ ЖЕ слой, что остальные запросы
// (второго способа добраться до базы в форке не заводим). Резолв KZPlayer* по userID — забота
// вызывающей стороны в onSuccess (тот же паттерн, что FetchSavedRun/setup_client.cpp): за время
// async round-trip игрок может уйти.

void KZDatabaseService::StoreHudShare(u64 ownerSteamID64, const char *code, i32 schemaVersion, const std::string &snapshot,
									  TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure)
{
	if (!KZDatabaseService::IsReady())
	{
		onFailure("Database not ready.", 0);
		return;
	}

	ISQLConnection *db = KZDatabaseService::GetDatabaseConnection();
	std::string cleanCode = db->Escape(code);
	std::string cleanSnapshot = db->Escape(snapshot.c_str());

	// Снимок (TEXT) заметно больше фиксированных 2 КБ, которыми обходятся другие запросы слоя —
	// буфер считаем динамически, как в save_savedrun.cpp, чтобы не обрезать его молча.
	size_t bufferSize = cleanCode.size() + cleanSnapshot.size() + 512;
	std::vector<char> query(bufferSize);
	V_snprintf(query.data(), (int)query.size(), sql_hudshares_insert, cleanCode.c_str(), ownerSteamID64, (u32)schemaVersion, cleanSnapshot.c_str());

	// Квота на владельца — в ТОЙ ЖЕ транзакции и ПОСЛЕ вставки: либо код появился и лишние
	// вытеснены, либо не произошло ни того ни другого. Обоснование квоты — queries/hudshares.h.
	const char *pruneQuery =
		(KZDatabaseService::GetDatabaseType() == KZ::Database::DatabaseType::MySQL) ? mysql_hudshares_prune_owner : sqlite_hudshares_prune_owner;
	char prune[1024];
	V_snprintf(prune, sizeof(prune), pruneQuery, ownerSteamID64, ownerSteamID64, hudshares_max_per_owner);

	Transaction txn;
	txn.queries.push_back(query.data());
	txn.queries.push_back(prune);

	db->ExecuteTransaction(txn, onSuccess, onFailure);
}

void KZDatabaseService::FetchHudShare(const char *code, TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure)
{
	if (!KZDatabaseService::IsReady())
	{
		onFailure("Database not ready.", 0);
		return;
	}

	ISQLConnection *db = KZDatabaseService::GetDatabaseConnection();
	std::string cleanCode = db->Escape(code);

	char query[512];
	V_snprintf(query, sizeof(query), sql_hudshares_fetch, cleanCode.c_str());

	Transaction txn;
	txn.queries.push_back(query);

	db->ExecuteTransaction(txn, onSuccess, onFailure);
}

// TTL-чистка снимков старше 30 дней. Зовётся раз на загрузку карты, рядом с
// PurgeExpiredSavedRuns (kz/timer/kz_timer.cpp, OnMapSetup). Fire-and-forget.
void KZDatabaseService::PurgeExpiredHudShares()
{
	if (!KZDatabaseService::IsReady())
	{
		return;
	}

	const char *purgeQuery =
		(KZDatabaseService::GetDatabaseType() == KZ::Database::DatabaseType::MySQL) ? mysql_hudshares_purge : sqlite_hudshares_purge;

	Transaction txn;
	txn.queries.push_back(purgeQuery);

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, OnGenericTxnSuccess, OnGenericTxnFailure);
}
