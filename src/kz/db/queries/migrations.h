// =====[ MIGRATIONS ]=====

constexpr char mysql_migrations_create[] = R"(
    CREATE TABLE IF NOT EXISTS Migrations ( 
        ID INTEGER UNSIGNED NOT NULL AUTO_INCREMENT, 
        CRC32 INTEGER UNSIGNED NOT NULL UNIQUE, 
        Created TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
        CONSTRAINT PK_Migrations PRIMARY KEY (ID))
)";

constexpr char sqlite_migrations_create[] = R"(
    CREATE TABLE IF NOT EXISTS Migrations ( 
        ID INTEGER NOT NULL, 
        CRC32 INTEGER NOT NULL UNIQUE, 
        Created TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
        CONSTRAINT PK_Migrations PRIMARY KEY (ID))
)";

constexpr char sql_migrations_fetchall[] = R"(
    SELECT ID, CRC32 
        FROM Migrations
        ORDER BY ID
)";

// Догоняющая миграция ИДЕМПОТЕНТНА по CRC32 (колонка UNIQUE). Спасаем от гонки старта: у нас
// 12 инстансов на ОДНОЙ общей MySQL, и на промоуте они перезапускаются пачкой. Голый INSERT в
// этом случае у проигравшего падает на UNIQUE, а обработчик отказа рвёт соединение
// (migrations.cpp: databaseConnection->Destroy()) — инстанс до рестарта живёт БЕЗ базы, то есть
// не пишет ни префы, ни РЕКОРДЫ, и игрок об этом не узнаёт. Идемпотентная вставка защищает и
// все будущие миграции, а не только последнюю.
// Требование к массивам миграций от этого не меняется: текст каждой миграции обязан быть
// УНИКАЛЬНЫМ (append-only), иначе одинаковый CRC даст меньше строк, чем длина массива, и
// хвост будет догоняться на каждом старте.
constexpr char mysql_migrations_insert[] = R"(
    INSERT IGNORE INTO Migrations (CRC32, Created)
        VALUES ('%lu', CURRENT_TIMESTAMP)
)";

constexpr char sqlite_migrations_insert[] = R"(
    INSERT OR IGNORE INTO Migrations (CRC32, Created)
        VALUES ('%lu', CURRENT_TIMESTAMP)
)";
