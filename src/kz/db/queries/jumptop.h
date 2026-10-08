// =====[ JUMPTOP ]=====
//
// Топ по прыжкам (как !jumptop в GOKZ), но в своей таблице, а не в апстримной Jumpstats: той
// не хватает полей для разбора прыжка на АХК (карта, сервер, реплей, полная статистика), а
// апстрим однажды может начать писать в неё сам. Живёт в общей MySQL флота — топ и рекорд
// сервера одни на весь флот.
//
// Пишется ТОЛЬКО новый личный рекорд (по дистанции и отдельно по блоку), история PB не
// удаляется: строки старых PB нужны для разбора («как рос результат»), а топ выбирает лучшую
// строку игрока сам. Removed — мягкое снятие админом (kz_jumptop_remove), строка остаётся для
// разбора. Числа — DOUBLE как есть (без масштабирования GOKZ ×10000).
//
// Mode — ID из таблицы Modes (KZModeManager::ModePluginInfo::databaseID), JumpType — enum
// JumpType форка (0 LJ, 1 BH, 2 MBH, 3 WJ, 4 LAJ, 5 LAH, 6 JB).

constexpr char mysql_jumptop_create[] = R"(
    CREATE TABLE IF NOT EXISTS Jumptop (
        ID INTEGER UNSIGNED NOT NULL AUTO_INCREMENT,
        SteamID64 BIGINT UNSIGNED NOT NULL,
        Mode TINYINT UNSIGNED NOT NULL,
        JumpType TINYINT UNSIGNED NOT NULL,
        IsBlockJump TINYINT UNSIGNED NOT NULL,
        Block SMALLINT UNSIGNED NOT NULL,
        Distance DOUBLE NOT NULL,
        Strafes SMALLINT UNSIGNED NOT NULL,
        Sync DOUBLE NOT NULL,
        Pre DOUBLE NOT NULL,
        Max DOUBLE NOT NULL,
        Airtime DOUBLE NOT NULL,
        Height DOUBLE NOT NULL,
        Offset DOUBLE NOT NULL,
        MapName VARCHAR(255) NOT NULL,
        ServerID VARCHAR(32) NOT NULL,
        ReplayUUID CHAR(36) NOT NULL,
        Details MEDIUMTEXT NOT NULL,
        Removed TINYINT UNSIGNED NOT NULL DEFAULT 0,
        Created TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
        CONSTRAINT PK_Jumptop PRIMARY KEY (ID),
        INDEX IDX_Jumptop_Top (Mode, JumpType, IsBlockJump, Removed),
        INDEX IDX_Jumptop_Player (SteamID64))
)";

constexpr char sqlite_jumptop_create[] = R"(
    CREATE TABLE IF NOT EXISTS Jumptop (
        ID INTEGER NOT NULL,
        SteamID64 INTEGER NOT NULL,
        Mode INTEGER NOT NULL,
        JumpType INTEGER NOT NULL,
        IsBlockJump INTEGER NOT NULL,
        Block INTEGER NOT NULL,
        Distance REAL NOT NULL,
        Strafes INTEGER NOT NULL,
        Sync REAL NOT NULL,
        Pre REAL NOT NULL,
        Max REAL NOT NULL,
        Airtime REAL NOT NULL,
        Height REAL NOT NULL,
        Offset REAL NOT NULL,
        MapName TEXT NOT NULL,
        ServerID TEXT NOT NULL,
        ReplayUUID TEXT NOT NULL,
        Details TEXT NOT NULL,
        Removed INTEGER NOT NULL DEFAULT 0,
        Created TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
        CONSTRAINT PK_Jumptop PRIMARY KEY (ID))
)";

// Реплей прыжка (~7 с: 5 до приземления, 2 после) — сериализованный файл реплея в base64.
// В базе, а не на диске инстанса: kzreplays/ живёт в контейнере и пропадает при пересоздании,
// а центральное хранилище реплеев (api /replays/v1) знает только раны. Ключ — UUID реплея,
// на него ссылаются строки Jumptop (одна-две строки на прыжок: дистанция и блок).
constexpr char mysql_jumptop_replays_create[] = R"(
    CREATE TABLE IF NOT EXISTS JumptopReplays (
        ReplayUUID CHAR(36) NOT NULL,
        Data MEDIUMTEXT NOT NULL,
        Created TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
        CONSTRAINT PK_JumptopReplays PRIMARY KEY (ReplayUUID))
)";

constexpr char sqlite_jumptop_replays_create[] = R"(
    CREATE TABLE IF NOT EXISTS JumptopReplays (
        ReplayUUID TEXT NOT NULL,
        Data TEXT NOT NULL,
        Created TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
        CONSTRAINT PK_JumptopReplays PRIMARY KEY (ReplayUUID))
)";

// Порядок параметров: SteamID64, Mode, JumpType, IsBlockJump, Block, Distance, Strafes, Sync, Pre,
// Max, Airtime, Height, Offset, MapName, ServerID, ReplayUUID, Details (строки экранированы).
constexpr char sql_jumptop_insert[] = R"(
    INSERT INTO Jumptop (SteamID64, Mode, JumpType, IsBlockJump, Block, Distance, Strafes, Sync, Pre, Max, Airtime, Height, Offset,
        MapName, ServerID, ReplayUUID, Details)
        VALUES (%llu, %d, %d, %d, %d, %.6f, %d, %.6f, %.6f, %.6f, %.6f, %.6f, %.6f, '%s', '%s', '%s', '%s')
)";

// Личные рекорды игрока по всем режимам/типам — для кэша на заходе. Лучшая строка выбирается в
// коде (истории PB у игрока немного), SQL общий для MySQL и SQLite.
constexpr char sql_jumptop_getpbs[] = R"(
    SELECT Mode, JumpType, IsBlockJump, Block, Distance
        FROM Jumptop
        WHERE SteamID64 = %llu AND Removed = 0
)";

// Активный бан — тот же критерий, что у апстримного топа (Bans.ExpiresAt NULL = навсегда).
#define JUMPTOP_NOT_BANNED_SQL \
	"NOT EXISTS (SELECT 1 FROM Bans b WHERE b.SteamID64 = j.SteamID64 AND (b.ExpiresAt IS NULL OR b.ExpiresAt > CURRENT_TIMESTAMP))"

// Место прыжка в топе: сколько ДРУГИХ игроков имеют строку лучше (дистанционный топ — по
// дистанции; блочный — по блоку, затем по дистанции), плюс общее число игроков в топе.
// Ровно равная дистанция — первенство у более ранней строки (меньший ID), иначе #1 получили бы оба.
// Параметры: Mode, JumpType, IsBlockJump, Block, Block, Distance, Distance, ID, SteamID64, Mode, JumpType, IsBlockJump.
constexpr char sql_jumptop_rank[] = R"(
    SELECT
        (SELECT COUNT(DISTINCT j.SteamID64) FROM Jumptop j
            WHERE j.Mode = %d AND j.JumpType = %d AND j.IsBlockJump = %d AND j.Removed = 0
                AND (j.Block > %d OR (j.Block = %d AND (j.Distance > %.6f OR (j.Distance = %.6f AND j.ID < %u))))
                AND j.SteamID64 <> %llu AND )" JUMPTOP_NOT_BANNED_SQL R"(),
        (SELECT COUNT(DISTINCT j.SteamID64) FROM Jumptop j
            WHERE j.Mode = %d AND j.JumpType = %d AND j.IsBlockJump = %d AND j.Removed = 0 AND )" JUMPTOP_NOT_BANNED_SQL R"()
)";

// Топ: лучшая строка каждого игрока (окно по игроку), без снятых и забаненных. MySQL 8 и
// SQLite >= 3.25 умеют ROW_NUMBER. Для дистанционного топа Block у всех строк 0, поэтому
// одна сортировка годится обоим видам.
// Параметры: Mode, JumpType, IsBlockJump, Limit.
constexpr char sql_jumptop_gettop[] = R"(
    SELECT t.ID, t.SteamID64, COALESCE(p.Alias, ''), t.Block, t.Distance, t.Strafes, t.Sync, t.Pre, t.Max, t.Airtime, t.ReplayUUID,
        t.MapName
        FROM (
            SELECT j.*, ROW_NUMBER() OVER (PARTITION BY j.SteamID64 ORDER BY j.Block DESC, j.Distance DESC, j.ID ASC) AS rn
                FROM Jumptop j
                WHERE j.Mode = %d AND j.JumpType = %d AND j.IsBlockJump = %d AND j.Removed = 0 AND )" JUMPTOP_NOT_BANNED_SQL R"(
        ) t
        LEFT JOIN Players p ON p.SteamID64 = t.SteamID64
        WHERE t.rn = 1
        ORDER BY t.Block DESC, t.Distance DESC, t.ID ASC
        LIMIT %d
)";

// Одна строка целиком — для !jumpinfo (разбор на АХК). Параметр: ID.
constexpr char sql_jumptop_getjump[] = R"(
    SELECT j.ID, j.SteamID64, COALESCE(p.Alias, ''), j.Mode, j.JumpType, j.IsBlockJump, j.Block, j.Distance, j.Strafes, j.Sync, j.Pre,
        j.Max, j.Airtime, j.Height, j.Offset, j.MapName, j.ServerID, j.ReplayUUID, j.Details, j.Removed, j.Created
        FROM Jumptop j
        LEFT JOIN Players p ON p.SteamID64 = j.SteamID64
        WHERE j.ID = %d
)";

// Личные рекорды для !jspb: лучшая строка игрока в каждом (тип, вид) для одного режима.
// Параметры: SteamID64, Mode.
constexpr char sql_jumptop_getplayerbests[] = R"(
    SELECT t.ID, t.JumpType, t.IsBlockJump, t.Block, t.Distance, t.Strafes, t.Sync, t.Pre, t.Max, t.Airtime, t.MapName
        FROM (
            SELECT j.*, ROW_NUMBER() OVER (PARTITION BY j.JumpType, j.IsBlockJump ORDER BY j.Block DESC, j.Distance DESC, j.ID ASC) AS rn
                FROM Jumptop j
                WHERE j.SteamID64 = %llu AND j.Mode = %d AND j.Removed = 0
        ) t
        WHERE t.rn = 1
        ORDER BY t.JumpType ASC, t.IsBlockJump ASC
)";

// Снятие/возврат прыжка админом. Параметры: Removed (0/1), ID.
constexpr char sql_jumptop_setremoved[] = R"(
    UPDATE Jumptop SET Removed = %d WHERE ID = %d
)";

// Реплей: вставка (повтор — тот же UUID — безвреден) и чтение.
constexpr char mysql_jumptop_replay_insert[] = R"(
    INSERT IGNORE INTO JumptopReplays (ReplayUUID, Data) VALUES ('%s', '%s')
)";

constexpr char sqlite_jumptop_replay_insert[] = R"(
    INSERT OR IGNORE INTO JumptopReplays (ReplayUUID, Data) VALUES ('%s', '%s')
)";

constexpr char sql_jumptop_replay_fetch[] = R"(
    SELECT Data FROM JumptopReplays WHERE ReplayUUID = '%s'
)";
