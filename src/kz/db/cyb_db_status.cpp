// [cyb] НАШ файл, апстрима здесь нет. См. cyb_db_status.h.
#include "cyb_db_status.h"
#include "kz/db/kz_db.h"
#include "utils/utils.h"

#include "tier0/memdbgon.h"

// Состояние глобальное, а не per-player, СОЗНАТЕЛЬНО: это состояние процесса (одна БД на
// инстанс), пишется один раз при настройке БД и читается только из rcon-команды. В хуках
// движения не участвует.
static_global bool s_migrationCheckRan = false;
static_global u32 s_appliedMigrations = 0;
static_global u32 s_knownMigrations = 0;

void KZ::cyb::db::NoteMigrationCheck(u32 applied, u32 known)
{
	s_migrationCheckRan = true;
	s_appliedMigrations = applied;
	s_knownMigrations = known;
}

CON_COMMAND_F(kz_db_status, "Print the local database state of the plugin (machine readable, used by the kz_db_ready invariant).", FCVAR_NONE)
{
	// Игрокам не отдаём, как и прочим настоящим ConCommand форка (канон — kz_invisible.cpp).
	if (utils::GetController(context.GetPlayerSlot()))
	{
		return;
	}
	const char *driver = "none";
	switch (KZDatabaseService::GetDatabaseType())
	{
		case KZ::Database::DatabaseType::MySQL:
			driver = "mysql";
			break;
		case KZ::Database::DatabaseType::SQLite:
			driver = "sqlite";
			break;
		default:
			break;
	}
	// ready — единственное, что решает инвариант; остальное для разбора ПОЧЕМУ.
	// checked=0 значит, что до сверки миграций дело не дошло вовсе (нет конфига БД или не
	// удалось подключиться), и applied/known тогда нули, а не «миграций ноль».
	// ahead>0 — на базе уже побывала сборка новее этой (см. db_migrations_ahead).
	u32 ahead = s_appliedMigrations > s_knownMigrations ? s_appliedMigrations - s_knownMigrations : 0;
	Msg("[cyb] db_status ready=%d checked=%d driver=%s applied=%u known=%u ahead=%u\n", KZDatabaseService::IsReady() ? 1 : 0,
		s_migrationCheckRan ? 1 : 0, driver, s_appliedMigrations, s_knownMigrations, ahead);
}
