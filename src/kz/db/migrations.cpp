#include "kz_db.h"
#include "kz/option/kz_option.h"
#include "kz/db/cyb_db_status.h" // [cyb]

#include <regex>
#include "checksum_crc.h"

#include "queries/migrations.h"
#include "queries/courses.h"
#include "queries/jumpstats.h"
#include "queries/maps.h"
#include "queries/modes.h"
#include "queries/players.h"
#include "queries/bans.h"
#include "queries/styles.h"
#include "queries/startpos.h"
#include "queries/times.h"
#include "queries/savedruns.h"
#include "queries/hudshares.h"

#include "vendor/sql_mm/src/public/sql_mm.h"
#include "vendor/sql_mm/src/public/sqlite_mm.h"
#include "vendor/sql_mm/src/public/mysql_mm.h"

using namespace KZ::Database;

static_global bool localDBConnected = false;

std::string trimString(const char *str)
{
	static_persist const std::regex pattern("\\s+");

	return std::regex_replace(str, pattern, " ");
}

// Warning: Do NOT change the content or the order of these queries! Do appends only.
// clang-format off
static_global const std::string mysqlMigrations[] = 
{
	trimString(mysql_players_create),
	trimString(mysql_modes_create),
	trimString(mysql_styles_create),
	trimString(mysql_maps_create),
	trimString(mysql_mapcourses_create),
	trimString(mysql_times_create),
	trimString(mysql_jumpstats_create),
	trimString(mysql_startpos_create),
	trimString(mysql_times_alter_id_column),
	trimString(mysql_bans_create),
	trimString(mysql_savedruns_create),
	trimString(mysql_hudshares_create),
};

static_global const std::string sqliteMigrations[] = 
{
	trimString(sqlite_players_create),
	trimString(sqlite_modes_create),
	trimString(sqlite_styles_create),
	trimString(sqlite_maps_create),
	trimString(sqlite_mapcourses_create),
	trimString(sqlite_times_create),
	trimString(sqlite_jumpstats_create),
	trimString(sqlite_startpos_create),
	trimString(sqlite_times_alter_id_column_1),
	trimString(sqlite_times_alter_id_column_2),
	trimString(sqlite_times_alter_id_column_3),
	trimString(sqlite_times_alter_id_column_4),
	trimString(sqlite_bans_create),
	trimString(sqlite_savedruns_create),
	trimString(sqlite_hudshares_create),
};

// clang-format on

void KZDatabaseService::RunMigrations()
{
	Transaction txn;
	switch (KZDatabaseService::GetDatabaseType())
	{
		case DatabaseType::MySQL:
		{
			txn.queries.push_back(mysql_migrations_create);
			break;
		}
		case DatabaseType::SQLite:
		{
			txn.queries.push_back(sqlite_migrations_create);
			break;
		}
	}
	txn.queries.push_back(sql_migrations_fetchall);

	GetDatabaseConnection()->ExecuteTransaction(txn, KZDatabaseService::CheckMigrations, OnGenericTxnFailure);
}

void KZDatabaseService::CheckMigrations(std::vector<ISQLQuery *> queries)
{
	ISQLResult *result = queries[1]->GetResultSet();

	u32 current = result->GetRowCount();
	u32 max = 0;
	switch (KZDatabaseService::GetDatabaseType())
	{
		case DatabaseType::MySQL:
		{
			max = sizeof(mysqlMigrations) / sizeof(mysqlMigrations[0]);
			break;
		}
		case DatabaseType::SQLite:
		{
			max = sizeof(sqliteMigrations) / sizeof(sqliteMigrations[0]);
			break;
		}
	}
	// [cyb] Снимок для rcon-команды kz_db_status (инвариант kz_db_ready).
	KZ::cyb::db::NoteMigrationCheck(current, max);

	if (current > max)
	{
		// [cyb] ТЕРПИМОСТЬ К «МИГРАЦИЙ В БАЗЕ БОЛЬШЕ, ЧЕМ ЗНАЕТ КОД». Правка НАША, не апстрима:
		// у апстрима здесь ранний return, то есть инстанс остаётся БЕЗ базы до перезапуска на
		// другой сборке. У нас на ОДНОЙ MySQL живёт весь флот, поэтому одна канареечная сборка
		// с новой миграцией выключала базу всем остальным: инцидент 09-10.09.2026 — ~6 часов
		// без сохранения ранов и без префов на всём флоте, лечилось пином профиля.
		//
		// Почему принять лишние строки безопасно ИМЕННО для аддитивных миграций: массив миграций
		// append-only (см. предупреждение выше), поэтому первые `max` строк таблицы — это ровно
		// те миграции, которые знает эта сборка, и они ниже сверяются по CRC как и раньше, строка
		// в строку. Лишний хвост — это CREATE TABLE новых таблиц и ADD COLUMN новых колонок:
		// таблиц, которых старый код не знает, он не касается, а лишние колонки он не перечисляет
		// ни в SELECT, ни в INSERT (все запросы форка — со списком колонок).
		//
		// Чего терпимость НЕ ловит (осознанно, проверить это здесь нечем — текстов чужих миграций
		// у нас нет, есть только их CRC):
		//   * новая миграция СМЕНИЛА ТИП/сузила колонку, которую читает старый код (например
		//     INT -> BIGINT или TEXT -> VARCHAR(64)): чтение молча усечётся или запись упадёт;
		//   * новая миграция добавила NOT NULL-колонку без DEFAULT: INSERT'ы старого кода начнут
		//     отказывать (шумно, но только в рантайме);
		//   * новая миграция переименовала/удалила колонку: запросы старого кода сломаются;
		//   * новая миграция навесила UNIQUE или индекс на СУЩЕСТВУЮЩУЮ таблицу: старый код пишет
		//     то, что раньше было законно, и получает отказ вставки на дубле;
		//   * даже ЧИСТО аддитивный случай: старый код пишет строки без нового поля, и оно
		//     заполняется DEFAULT'ом. Отказа нет ни у кого, но новый код позже прочитает такие
		//     строки как «значение ноль», а не как «значения не было» — тихая порча смысла.
		// Все три случая — НЕаддитивные миграции; выкатывать такие на общую базу можно только
		// синхронно со всем флотом. Признак ситуации виден снаружи: строка ниже + инвариант
		// kz_db_ready (см. docs/runbooks/fleet-invariants.md в монорепо).
		KZ_LOG_WARN(LogChannel::DB,
					"[cyb] db_migrations_ahead reason=plugin_older_than_db applied=%u known=%u ahead=%u tolerated=1\n", current, max,
					current - max);
		// Дальше работаем с ИЗВЕСТНЫМ ПРЕФИКСОМ: сверяем его по CRC (цикл ниже) и считаем базу
		// полностью настроенной (current == max → onSuccess), догонять нечего.
		current = max;
	}

	for (u32 i = 0; i < current; i++)
	{
		result->FetchRow();
		u32 currentCRC = result->GetInt64(1);
		std::string migrationQuery;

		switch (KZDatabaseService::GetDatabaseType())
		{
			case DatabaseType::MySQL:
			{
				migrationQuery = mysqlMigrations[i];
				break;
			}
			case DatabaseType::SQLite:
			{
				migrationQuery = sqliteMigrations[i];
				break;
			}
		}

		u32 crc = CRC32_ProcessSingleBuffer(migrationQuery.c_str(), migrationQuery.length());
		KZ_LOG_DEBUG(LogChannel::DB, "crc = %u, currentCRC = %u\n", crc, currentCRC);
		if (currentCRC != crc)
		{
			KZ_LOG_WARN(LogChannel::DB, "Fatal error: Migration query %s with CRC %u does not match the database's %u!\n", migrationQuery.c_str(),
						crc, currentCRC);
			KZ_LOG_WARN(LogChannel::DB, "Database migration failed. LocalDB will not be available.");
			databaseConnection->Destroy();
			databaseConnection = nullptr;
			return;
		}
	}

	auto onSuccess = []()
	{
		KZ_LOG_INFO(LogChannel::DB, "Database migration successful.\n");
		localDBConnected = true;
		KZDatabaseService::SetupMap();
		CALL_FORWARD(eventListeners, OnDatabaseSetup);
	};

	auto onFailure = []()
	{
		KZ_LOG_WARN(LogChannel::DB, "Database migration failed. LocalDB will not be available.\n");
		databaseConnection->Destroy();
		databaseConnection = nullptr;
	};

	// If there's no migration needed, the database is already fully setup.
	if (current == max)
	{
		onSuccess();
		return;
	}

	Transaction txn;
	char query[2048];
	for (u32 i = current; i < max; i++)
	{
		switch (KZDatabaseService::GetDatabaseType())
		{
			case DatabaseType::MySQL:
			{
				txn.queries.push_back(mysqlMigrations[i]);
				// Вставка идемпотентна (INSERT IGNORE) — от гонки одновременного старта
				// инстансов на общей базе, см. queries/migrations.h.
				V_snprintf(query, sizeof(query), mysql_migrations_insert,
						   CRC32_ProcessSingleBuffer(mysqlMigrations[i].c_str(), (int)mysqlMigrations[i].length()));
				break;
			}
			case DatabaseType::SQLite:
			{
				txn.queries.push_back(sqliteMigrations[i]);
				V_snprintf(query, sizeof(query), sqlite_migrations_insert,
						   CRC32_ProcessSingleBuffer(sqliteMigrations[i].c_str(), (int)sqliteMigrations[i].length()));
				break;
			}
		}
		txn.queries.push_back(query);
	}

	GetDatabaseConnection()->ExecuteTransaction(
		txn, [onSuccess](std::vector<ISQLQuery *> queries) { onSuccess(); }, [onFailure](std::string error, int failIndex) { onFailure(); });
}

bool KZDatabaseService::IsReady()
{
	return localDBConnected;
}
