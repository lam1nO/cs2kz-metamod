#include <string>
#include <vector>

#include "kz_db.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
#include "kz/timer/kz_timer.h"
#include "queries/savedruns.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

using namespace KZ::Database;

// SavedRuns (Task 2): upsert снапшота незавершённого рана. Собирает map/course/mode/styles из
// состояния игрока (по образцу save_time.cpp / submission.cpp), экранирует все строковые аргументы
// и стреляет транзакцией fire-and-forget — ошибка сейва не должна блокировать дисконнект/спаун игрока.
void KZDatabaseService::SaveRun(KZPlayer *player, f64 runTime, u32 tpCount, const std::string &snapshot)
{
	if (!KZDatabaseService::IsReady())
	{
		return;
	}

	const KZCourseDescriptor *course = player->timerService->GetCourse();
	if (!course)
	{
		// Нет активного курса - сохранять нечего.
		return;
	}

	bool mapNameOk = false;
	CUtlString mapName = g_pKZUtils->GetCurrentMapName(&mapNameOk);
	if (!mapNameOk || mapName.IsEmpty())
	{
		KZ_LOG_WARN(LogChannel::DB, "[SavedRuns] Cannot save run for %s: current map name unavailable.\n", player->GetName());
		return;
	}

	i32 courseNumber = KZ::course::GetCyberCourseNumber(course);

	KZModeManager::ModePluginInfo modeInfo = KZ::mode::GetModeInfo(player->modeService);

	CUtlString styles;
	FOR_EACH_VEC(player->styleServices, i)
	{
		if (i > 0)
		{
			styles.Append(",");
		}
		styles.Append(player->styleServices[i]->GetStyleShortName());
	}

	ISQLConnection *db = KZDatabaseService::GetDatabaseConnection();
	std::string cleanMapName = db->Escape(mapName.Get());
	std::string cleanMode = db->Escape(modeInfo.shortModeName.Get());
	std::string cleanStyles = db->Escape(styles.Get());
	std::string cleanSnapshot = db->Escape(snapshot.c_str());

	const char *upsertQuery = (KZDatabaseService::GetDatabaseType() == KZ::Database::DatabaseType::MySQL) ? mysql_savedruns_upsert
																											: sqlite_savedruns_upsert;

	// Снапшот (MEDIUMTEXT/TEXT) может быть заметно больше фиксированных 2КБ, которые обычно хватают
	// другим запросам в этом слое - буфер под запрос считаем динамически, чтобы не обрезать JSON.
	size_t bufferSize = cleanMapName.size() + cleanMode.size() + cleanStyles.size() + cleanSnapshot.size() + 512;
	std::vector<char> query(bufferSize);
	V_snprintf(query.data(), (int)query.size(), upsertQuery, player->GetSteamId64(), cleanMapName.c_str(), courseNumber, cleanMode.c_str(),
			   cleanStyles.c_str(), runTime, tpCount, cleanSnapshot.c_str());

	Transaction txn;
	txn.queries.push_back(query.data());

	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, OnGenericTxnSuccess, OnGenericTxnFailure);
}
