// Снимки настроек худа под короткий код обмена (спека 2026-09-09-hud-share-design.md §4).
// Живут в той же общей MySQL флота, что и Players.Preferences (10.77.0.1/cs2kz), поэтому код,
// выданный на одном инстансе, применяется на любом другом без транспорта.
//
// Код хранится УЖЕ НОРМАЛИЗОВАННЫМ (верхний регистр, алфавит без похожих символов — см.
// KZ::hudshare::NormalizeCode): регистронезависимость обеспечивает код плагина, а не коллация
// столбца — у SQLite её нет вовсе, а у MySQL она зависит от настроек сервера.
//
// Владелец (OwnerSteamID64) обязателен: он нужен журналу и будущей модерации «плохих» кодов.
// Одноразовым код не делаем (одним кодом делятся с несколькими), TTL — 30 дней, как у SavedRuns.
constexpr char mysql_hudshares_create[] = R"(
	CREATE TABLE IF NOT EXISTS HudShares (
		Code VARCHAR(16) NOT NULL,
		OwnerSteamID64 BIGINT UNSIGNED NOT NULL,
		SchemaVersion INTEGER UNSIGNED NOT NULL,
		Snapshot TEXT NOT NULL,
		Created TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
		PRIMARY KEY (Code),
		INDEX idx_hudshares_owner (OwnerSteamID64, Created),
		INDEX idx_hudshares_created (Created)
	)
)";

constexpr char sqlite_hudshares_create[] = R"(
	CREATE TABLE IF NOT EXISTS HudShares (
		Code TEXT NOT NULL,
		OwnerSteamID64 INTEGER NOT NULL,
		SchemaVersion INTEGER NOT NULL,
		Snapshot TEXT NOT NULL,
		Created INTEGER NOT NULL DEFAULT (strftime('%s','now')),
		PRIMARY KEY (Code)
	)
)";

// Вставка нового снимка. ИМЕННО INSERT, а не upsert: код совпал бы с ЧУЖИМ — upsert затёр бы
// чужой снимок под его же кодом. Коллизия обязана быть отказом, игрок повторит команду.
// Порядок параметров: Code, OwnerSteamID64, SchemaVersion, Snapshot — строки экранированы
// вызывающей стороной через GetDatabaseConnection()->Escape().
constexpr char sql_hudshares_insert[] = R"(
	INSERT INTO HudShares (Code, OwnerSteamID64, SchemaVersion, Snapshot)
	VALUES ('%s', %llu, %u, '%s')
)";

// Чтение по коду. Пустой result set = кода нет или он истёк (истёкшие удаляет чистка ниже).
constexpr char sql_hudshares_fetch[] = R"(
	SELECT Snapshot, OwnerSteamID64, SchemaVersion FROM HudShares
	WHERE Code = '%s' LIMIT 1
)";

// TTL-чистка (30 дней), раз на загрузку карты, fire-and-forget. Без параметров — строка идёт
// в транзакцию как есть, не через V_snprintf (см. hud_share_db.cpp).
constexpr char mysql_hudshares_purge[] = R"(
	DELETE FROM HudShares WHERE Created < NOW() - INTERVAL 30 DAY
)";

constexpr char sqlite_hudshares_purge[] = R"(
	DELETE FROM HudShares WHERE Created < strftime('%s','now') - 2592000
)";
