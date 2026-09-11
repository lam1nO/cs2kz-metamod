#pragma once
// [cyb] НАШ файл, апстрима здесь нет. Машинно-читаемое состояние локальной БД плагина для
// внешней проверки: инвариант kz_db_ready (infra/ansible/roles/fleet_invariants в монорепо)
// опрашивает по rcon команду kz_db_status и по её ответу отличает «плагин работает с базой»
// от «плагин живёт без базы». Снаружи эти два состояния не различимы ничем: сервер здоров,
// игроки играют, а раны и префы не сохраняются — так и прошёл инцидент 09-10.09.2026.
#include "common.h" // u32

namespace KZ::cyb::db
{
	// Вызывается из KZDatabaseService::CheckMigrations ровно один раз за проверку миграций.
	// applied — строк в таблице Migrations, known — сколько миграций знает эта сборка.
	void NoteMigrationCheck(u32 applied, u32 known);
} // namespace KZ::cyb::db
