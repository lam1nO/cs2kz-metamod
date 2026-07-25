#pragma once
#include "../kz.h"
#include <string>

// Персистентный незавершённый ран: сейв при дисконнекте, восстановление на спауне.
// Спека: docs/superpowers/specs/2026-07-06-kz-ux-pack-design.md (C).
class KZSavedRunService : public KZBaseService
{
	using KZBaseService::KZBaseService;

public:
	// Task 3: сохранение при дисконнекте (вызывать ДО TimerStop).
	void SaveOnDisconnect();
	// Task 4: попытка восстановления (вызывается из OnPlayerSpawn).
	void TryRestoreOnSpawn();
	// Task 5: удалить сейв текущего (map, course, mode, styles) игрока.
	void InvalidateCurrent(const char *reason);
	// Task 5: чистка сейвов старше 30 дней (раз на загрузку карты).
	static void PurgeExpired();

	// Task 2: сериализует текущее состояние таймера/чекпоинтов игрока в JSON-снапшот (формат v=1,
	// см. docs/superpowers/sdd/t3-task-2-brief.md). Не мутирует состояние игрока.
	std::string SerializeSnapshot();

	// Styles-подпись игрока для ключа SavedRuns (короткие имена стилей через запятую, в порядке
	// styleServices). Единая точка построения: upsert (save_savedrun.cpp), fetch + сверка после
	// round-trip (TryRestoreOnSpawn), инвалидация (Task 5).
	static CUtlString BuildStylesString(KZPlayer *player);

	// Task 4: парсит, валидирует и ПРИМЕНЯЕТ снапшот из БД к состоянию игрока (timerService +
	// checkpointService), затем телепортирует на точку заморозки (опциональное поле pos — Task 7,
	// prac) или на последний чекпоинт, если pos нет, и ставит паузу. course/tpCount — соседние
	// колонки строки SavedRuns (не часть JSON, см. save_savedrun.cpp), snapshot — сам JSON
	// (Task 2 формат, pos — Task 7).
	// Возвращает false без побочных эффектов, если снапшот повреждён/неизвестной версии/не
	// проходит базовую санность, ИЛИ курс с таким cyber-номером не найден на текущей карте
	// (карта обновилась), ИЛИ чекпоинтов нет и pos тоже нет (pro-ран без prac-заморозки) — в этом
	// случае чат-сообщение не выводится.
	bool ApplySnapshot(i32 course, u32 tpCount, const std::string &snapshot);

	// Восстановление уже выполнено/не нужно в этой сессии на этой карте (сбрасывается при
	// KZPlayer::Reset(), т.е. на дисконнект/смену карты для обычных клиентов).
	bool restoreAttempted {};
	// Fetch сейва уже запущен (Task 4: старт на первом живом спауне из TryRestoreOnSpawn).
	// Применение снапшота происходит прямо в колбэке этого fetch'а (не ждём следующий спаун —
	// он может не случиться до конца карты); повторные спауны, пока fetch летит, no-op.
	bool fetchStarted {};
	void Reset()
	{
		restoreAttempted = false;
		fetchStarted = false;
	}
};
