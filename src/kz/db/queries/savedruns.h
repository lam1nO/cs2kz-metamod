// Сейвы незавершённых ранов (транш 3 UX-pack). Ключ хранения:
// (SteamID64, MapName, Course, Mode, Styles); поиск при заходе — без Course.
constexpr char mysql_savedruns_create[] = R"(
	CREATE TABLE IF NOT EXISTS SavedRuns (
		SteamID64 BIGINT UNSIGNED NOT NULL,
		MapName VARCHAR(128) NOT NULL,
		Course SMALLINT NOT NULL,
		Mode VARCHAR(16) NOT NULL,
		Styles VARCHAR(64) NOT NULL DEFAULT '',
		RunTime DOUBLE NOT NULL,
		TpCount INT UNSIGNED NOT NULL DEFAULT 0,
		Snapshot MEDIUMTEXT NOT NULL,
		UpdatedAt TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
		PRIMARY KEY (SteamID64, MapName, Course, Mode, Styles),
		INDEX idx_savedruns_lookup (SteamID64, MapName, Mode, Styles, UpdatedAt)
	)
)";

constexpr char sqlite_savedruns_create[] = R"(
	CREATE TABLE IF NOT EXISTS SavedRuns (
		SteamID64 INTEGER NOT NULL,
		MapName TEXT NOT NULL,
		Course INTEGER NOT NULL,
		Mode TEXT NOT NULL,
		Styles TEXT NOT NULL DEFAULT '',
		RunTime REAL NOT NULL,
		TpCount INTEGER NOT NULL DEFAULT 0,
		Snapshot TEXT NOT NULL,
		UpdatedAt INTEGER NOT NULL DEFAULT (strftime('%s','now')),
		PRIMARY KEY (SteamID64, MapName, Course, Mode, Styles)
	)
)";

// Upsert снапшота незавершённого рана (Task 2). Порядок параметров V_snprintf:
// SteamID64, MapName, Course, Mode, Styles, RunTime, TpCount, Snapshot — все строки уже
// экранированы вызывающей стороной через GetDatabaseConnection()->Escape().
constexpr char mysql_savedruns_upsert[] = R"(
	INSERT INTO SavedRuns (SteamID64, MapName, Course, Mode, Styles, RunTime, TpCount, Snapshot)
	VALUES (%llu, '%s', %d, '%s', '%s', %f, %u, '%s')
	ON DUPLICATE KEY UPDATE RunTime=VALUES(RunTime), TpCount=VALUES(TpCount),
		Snapshot=VALUES(Snapshot), UpdatedAt=CURRENT_TIMESTAMP
)";

constexpr char sqlite_savedruns_upsert[] = R"(
	INSERT INTO SavedRuns (SteamID64, MapName, Course, Mode, Styles, RunTime, TpCount, Snapshot, UpdatedAt)
	VALUES (%llu, '%s', %d, '%s', '%s', %f, %u, '%s', strftime('%%s','now'))
	ON CONFLICT (SteamID64, MapName, Course, Mode, Styles) DO UPDATE SET
		RunTime=excluded.RunTime, TpCount=excluded.TpCount,
		Snapshot=excluded.Snapshot, UpdatedAt=excluded.UpdatedAt
)";
