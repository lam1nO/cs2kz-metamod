/*
 * kz_outbox.h — дисковый outbox завершённых ранов (csgo/cyb_outbox/).
 *
 * Зачем: все три стока завершённого рана — локальная MySQL (save_time.cpp),
 * платформенный ingest (cyb_emitter.cpp) и центральный реплей
 * (cyb_replay_upload.cpp) — были fire-and-forget: при обрыве сети до
 * control-хоста раны молча терялись (инцидент 18-24.08.2026, 50+ ранов
 * восстановлено руками).
 *
 * Схема write-ahead: в момент завершения рана на диск пишутся
 *   <runUUID>.event.json  — готовое тело POST /ingest/v1/events (id = runUUID —
 *                           ключ идемпотентности на платформе);
 *   <runUUID>.sql         — отрендеренный INSERT в Times в IGNORE-семантике
 *                           (Times.ID — PRIMARY KEY, повтор вставки безвреден);
 *   <runUUID>.replay.meta — метаданные центрального аплоада PB/WR-реплея
 *                           (сам файл реплея и так живёт в kzreplays/).
 * ДО первой попытки отправки. Подтверждение стока (2xx / успех транзакции)
 * удаляет свой файл — в штатном режиме файлы живут доли секунды.
 *
 * Ретраер: таймер раз в 60 с сканирует очередь и дошлёт файлы старше 30 с
 * (чтобы не гоняться с онлайн-путём) теми же функциями отправки, что и
 * онлайн-путь. Ответ 4xx → cyb_outbox/dead/ с логом; TTL — cybOutboxTtlHours
 * (дефолт 168 ч). Всё асинхронно: HTTP через Steam async (utils/http),
 * чтение реплея через AsyncFileIO — в игровом такте ничего не делается.
 */
#pragma once

#include "common.h"

#include <string>
#include <vector>

class KZOutboxService
{
public:
	// Лимит тела запроса реплея на стороне api (см. план T4b, Global Constraints).
	static constexpr size_t maxReplayBytes = 32u * 1024u * 1024u; // 32 МБ

	// Метаданные центрального реплей-аплоада; сериализуются в <runUUID>.replay.meta
	// одной строкой JSON.
	struct ReplayMeta
	{
		std::string runUuid;    // UUID рана; он же имя файла реплея и replayUuid для api
		u64 steamId64 {};
		std::string map;        // имя карты (валидировано под api: [a-z0-9_-]{1,128})
		i32 course {};          // номер курса по конвенции cyber (0 = main, N = bonus N)
		std::string mode;       // api-режим: ckz/vnl/kzt
		bool isServerRecord {}; // ран — локальный рекорд сервера (ранг 1) → дополнительно шлём type=wr
		// Локальная БД не подтвердила, что ран — новый PB (лежала в момент финиша).
		// Ретраер перед аплоадом сверяет timeMs с платформенным PB игрока и грузит
		// только не худшее время; WR-вариант в этой ветке не грузим (ранг без БД не
		// определить — честно грузим только pb).
		bool unconfirmedPb {};
		u64 timeMs {};          // время рана в мс — для сверки с платформенным PB
		std::string replayPath; // путь к файлу реплея относительно csgo/ (kzreplays/<uuid>.replay)
	};

	// Создаёт каталоги очереди и запускает таймер ретраера. Зовётся один раз при загрузке плагина.
	static void Init();

	// --- Write-ahead: зовутся в момент завершения рана, ДО первой попытки отправки ---

	// Готовое тело POST /ingest/v1/events (строит cyb_emitter, id события = runUuid).
	static void EnqueueEvent(const std::string &runUuid, const std::string &body);

	// Рендерит INSERT в Times по тому же шаблону, что save_time.cpp (sql_times_insert),
	// но в IGNORE-семантике. Только вставка — без PB/rank-запросов (те нужны только худу в моменте).
	static void EnqueueTimeInsert(const std::string &runUuid, u64 steamID64, u32 courseID, i32 modeID, f64 time, u64 teleports, u64 styleIDs,
								  const std::string &metadata);

	// Пишет (или перезаписывает — переход unconfirmed → confirmed) метаданные реплей-аплоада.
	static void EnqueueReplayMeta(const ReplayMeta &meta);

	// --- Подтверждение стока: удаляет write-ahead файл (no-op, если файла уже нет) ---

	static void AckEvent(const std::string &runUuid, const char *reason);
	static void AckSql(const std::string &runUuid, const char *reason);
	static void AckReplay(const std::string &runUuid, const char *reason);

	// Ран оказался не PB (или иная причина не грузить) — write-ahead реплея снимается как drop.
	static void DropReplay(const std::string &runUuid, const char *reason);

	// Поздний ответ глобального API переименовал ран localUUID → apiUUID (см.
	// RunSubmission::DoLateAPIResponse): переносим write-ahead меты вместе с файлом реплея.
	static void RenameReplayMeta(const std::string &oldUuid, const std::string &newUuid);

	// --- Отправка: ОБЩИЕ функции для онлайн-пути и ретраера (не копировать логику отправки!) ---

	// POST события в ingest; 2xx → AckEvent, 4xx → dead/, иначе файл остаётся до следующего тика.
	static void SendEvent(const std::string &runUuid, const std::string &body);

	// Аплоад реплея: POST type=pb, при meta.isServerRecord — следом type=wr; полный успех → AckReplay.
	static void SendReplay(const ReplayMeta &meta, const std::vector<char> &buffer);

private:
	// Выполнение отложенного INSERT (только ретраер: онлайн-путь вставляет через
	// KZDatabaseService::SaveTime — там та же вставка идёт одной транзакцией с
	// PB/rank-запросами для худа). Duplicate-key считается успехом.
	static void SendTimeInsert(const std::string &runUuid, const std::string &query);

	static f64 Tick();
	static void ProcessQueue();
	static void RetryEventFile(const std::string &name, const std::string &runUuid);
	static void RetrySqlFile(const std::string &name, const std::string &runUuid);
	static void RetryReplayFile(const std::string &name, const std::string &runUuid);
	static void RetryReplayRead(const ReplayMeta &meta);
};
