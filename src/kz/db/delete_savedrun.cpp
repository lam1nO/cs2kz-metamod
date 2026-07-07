#include <string>
#include <vector>

#include "kz_db.h"
#include "queries/savedruns.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

using namespace KZ::Database;

// SavedRuns (Task 5): удалить сейв по полному ключу хранения — явный сброс/финиш/оверврайт
// (см. KZSavedRunService::InvalidateCurrent). Fire-and-forget: ошибка удаления не должна блокировать
// !stop/finish/noclip/teleport-to-start — максимум переживём один устаревший сейв до следующей
// попытки инвалидации (upsert следующего рана всё равно перезапишет строку по тому же ключу).
void KZDatabaseService::DeleteSavedRun(u64 steamID64, CUtlString mapName, i32 course, CUtlString mode, CUtlString styles)
{
	if (!KZDatabaseService::IsReady())
	{
		return;
	}

	ISQLConnection *db = KZDatabaseService::GetDatabaseConnection();
	std::string cleanMapName = db->Escape(mapName.Get());
	std::string cleanMode = db->Escape(mode.Get());
	std::string cleanStyles = db->Escape(styles.Get());

	char query[1024];
	V_snprintf(query, sizeof(query), sql_savedruns_delete, steamID64, cleanMapName.c_str(), course, cleanMode.c_str(), cleanStyles.c_str());

	Transaction txn;
	txn.queries.push_back(query);

	db->ExecuteTransaction(txn, OnGenericTxnSuccess, OnGenericTxnFailure);
}

// SavedRuns (Task 5): TTL-чистка сейвов старше 30 дней. Вызывается раз на загрузку карты (см.
// KZDatabaseServiceEventListener_Timer::OnMapSetup в kz_timer.cpp, рядом с SetupLocalCourses —
// общий "per-map init" колбэк, который стреляет один раз после успешного KZDatabaseService::SetupMap()).
// Запрос без параметров - экранировать нечего, в транзакцию идёт как есть (не через V_snprintf).
void KZDatabaseService::PurgeExpiredSavedRuns()
{
	if (!KZDatabaseService::IsReady())
	{
		return;
	}

	const char *purgeQuery = (KZDatabaseService::GetDatabaseType() == KZ::Database::DatabaseType::MySQL) ? mysql_savedruns_purge
																										   : sqlite_savedruns_purge;

	Transaction txn;
	txn.queries.push_back(purgeQuery);

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, OnGenericTxnSuccess, OnGenericTxnFailure);
}
