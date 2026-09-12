#include "kz_db.h"
#include "vendor/sql_mm/src/public/sql_mm.h"

#include "queries/players.h"

void KZDatabaseService::FindPlayerByAlias(CUtlString playerName, TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure)
{
	if (!KZDatabaseService::IsReady())
	{
		return;
	}

	Transaction txn;
	char query[2048];

	// Get player's steamID through their alias.
	std::string cleanedPlayerName = KZDatabaseService::GetDatabaseConnection()->Escape(playerName.Get());
	V_snprintf(query, sizeof(query), sql_players_searchbyalias, cleanedPlayerName.c_str(), cleanedPlayerName.c_str());
	txn.queries.push_back(query);

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}

// Ник по SteamID64. Нужен `!awr` (src/kz/replays/cyb_awr_info.cpp): платформа отдаёт держателя
// AWR одним числом, а показать игроку надо имя. Таблица Players у флота ОДНА (общая MySQL), то
// есть держатель найдётся, даже если он сейчас на другом сервере сети; на sqlite-инстансе —
// только если он играл здесь. Пустой result set = имени не знаем, это не ошибка: вызывающий
// показывает SteamID64.
void KZDatabaseService::FindAliasBySteamID64(u64 steamID64, TransactionSuccessCallbackFunc onSuccess, TransactionFailureCallbackFunc onFailure)
{
	if (!KZDatabaseService::IsReady())
	{
		return;
	}

	Transaction txn;
	char query[512];

	// Параметр числовой (u64 из тела ответа api), экранировать нечего.
	V_snprintf(query, sizeof(query), sql_players_getalias, steamID64);
	txn.queries.push_back(query);

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
}
