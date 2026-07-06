#pragma once
#include "../kz.h"

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

	// Восстановление уже выполнено/не нужно в этой сессии на этой карте.
	bool restoreAttempted {};
	// Fetch сейва уже запущен (Task 4: старт на первом живом спауне).
	bool fetchStarted {};
	// Снапшот, полученный из БД до спауна (Task 4).
	std::string pendingSnapshot {};
	i32 pendingCourse {};
	f64 pendingRunTime {};
	u32 pendingTpCount {};
	bool hasPending {};
	void Reset()
	{
		restoreAttempted = false;
		fetchStarted = false;
		pendingSnapshot.clear();
		hasPending = false;
	}
};
