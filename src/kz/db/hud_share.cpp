#include <string>
#include <vector>

#include "kz_db.h"
#include "queries/hudshares.h"
#include "utils/logging.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

using namespace KZ::Database;

// Обмен настройками худа: доступ к таблице HudShares через ТОТ ЖЕ слой, что остальные запросы
// (второго способа добраться до базы в форке не заводим). Резолв KZPlayer* по userID — забота
// вызывающей стороны в onSuccess (тот же паттерн, что FetchSavedRun/setup_client.cpp): за время
// async round-trip игрок может уйти.
//
// ПРИОРИТЕТ ОТКАЗОВ. В этом файле три запроса, и они НЕ равнозначны:
//   - вставка снимка (StoreHudShare) — ОБЯЗАТЕЛЬНАЯ: без неё игрок не получает код;
//   - чтение снимка (FetchHudShare) — ОБЯЗАТЕЛЬНОЕ: без него !hudget не работает;
//   - вытеснение лишних кодов владельца и TTL-чистка — НЕОБЯЗАТЕЛЬНАЯ УБОРКА.
// Поэтому уборка живёт СВОИМИ транзакциями с собственной обработкой отказа и не может утащить
// за собой главное действие. Раньше вытеснение ехало в той же транзакции, что вставка, — то
// есть ошибка в необязательном DELETE отменяла бы обязательный INSERT, и игрок не получал бы
// код из-за уборки. Отказ уборки = строка error с машинно-читаемым reason и всё: лишние строки
// подберёт следующая попытка (уборка владельца — на следующей выдаче кода, TTL — на следующей
// загрузке карты).

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

	Transaction txn;
	txn.queries.push_back(query.data());

	// Единственный запрос транзакции — INSERT (не upsert): коллизия кода обязана быть отказом,
	// иначе она затирала бы чужой снимок под его же кодом. Отказ уходит вызывающей стороне,
	// а та печатает игроку «попробуй ещё раз»; повтор команды генерирует НОВЫЙ случайный код
	// (KZ::hudshare::GenerateCode), то есть та же коллизия не воспроизводится.
	//
	// Вытеснение лишних кодов владельца — ОТДЕЛЬНОЙ транзакцией и только после успешной
	// вставки: новый код обязан войти в число сохраняемых, а его собственный отказ не должен
	// отменить уже состоявшуюся выдачу (см. «приоритет отказов» в шапке файла).
	db->ExecuteTransaction(
		txn,
		[ownerSteamID64, onSuccess](std::vector<ISQLQuery *> queries)
		{
			onSuccess(queries);
			KZDatabaseService::PruneHudSharesForOwner(ownerSteamID64);
		},
		onFailure);
}

// Квота живых кодов на владельца (обоснование и сами запросы — queries/hudshares.h).
// Fire-and-forget и НЕОБЯЗАТЕЛЬНАЯ: отказ ничего не отменяет и ничем не грозит, кроме лишних
// строк до следующей выдачи кода этим же игроком.
void KZDatabaseService::PruneHudSharesForOwner(u64 ownerSteamID64)
{
	if (!KZDatabaseService::IsReady())
	{
		return;
	}

	const char *pruneQuery =
		(KZDatabaseService::GetDatabaseType() == KZ::Database::DatabaseType::MySQL) ? mysql_hudshares_prune_owner : sqlite_hudshares_prune_owner;
	char prune[1024];
	V_snprintf(prune, sizeof(prune), pruneQuery, ownerSteamID64, ownerSteamID64, hudshares_max_per_owner);

	Transaction txn;
	txn.queries.push_back(prune);

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(
		txn, [](std::vector<ISQLQuery *> queries) {}, [ownerSteamID64](std::string error, int failIndex)
		{ KZ_LOG_ERROR(LogChannel::DB, "[cyb] hud_share_prune_failed reason=db_delete owner=%llu error=%s\n", ownerSteamID64, error.c_str()); });
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

	// Только SELECT. Чистку сюда не подмешиваем ни в каком виде: чтение снимка обязано работать
	// независимо от того, удалась ли уборка истёкших (её отказ означает лишь, что в таблице
	// полежат просроченные строки — код из такой строки просто применится, это не отказ).
	Transaction txn;
	txn.queries.push_back(query);

	db->ExecuteTransaction(txn, onSuccess, onFailure);
}

// TTL-чистка снимков старше 30 дней. Зовётся раз на загрузку карты, рядом с
// PurgeExpiredSavedRuns (kz/timer/kz_timer.cpp, OnMapSetup) — то есть ни !hudshare, ни !hudget
// от неё не зависят вовсе. Fire-and-forget и НЕОБЯЗАТЕЛЬНАЯ; отказ логируем error-ом с
// машинно-читаемым reason, а не общим OnGenericTxnFailure: у неё нет второго шанса до
// следующей карты, и молча копящаяся таблица — это то, что мы обязаны увидеть в Loki.
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

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(
		txn, [](std::vector<ISQLQuery *> queries) {}, [](std::string error, int failIndex)
		{ KZ_LOG_ERROR(LogChannel::DB, "[cyb] hud_share_purge_failed reason=db_delete error=%s\n", error.c_str()); });
}
