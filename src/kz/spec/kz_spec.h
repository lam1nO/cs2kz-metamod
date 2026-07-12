#pragma once
#include "../kz.h"

class KZSpecService : public KZBaseService
{
	using KZBaseService::KZBaseService;

private:
	bool savedPosition;
	Vector savedOrigin;
	QAngle savedAngles;
	bool savedOnLadder;

public:
	virtual void Reset() override;
	static void Init();
	bool HasSavedPosition();
	void SavePosition();
	void LoadPosition();
	void ResetSavedPosition();

	bool IsSpectating(KZPlayer *target);
	bool SpectatePlayer(const char *playerName);
	bool SpectatePlayer(KZPlayer *target);
	bool CanSpectate();

	// Максимум пунктов в меню выбора цели спека (см. kz_spec_menu.cpp).
	static constexpr i32 KZ_SPEC_MENU_MAX_ITEMS = 8;

	// Кандидаты на спек по подстроке ника (case-insensitive). Точные совпадения
	// приоритетны: при наличии хотя бы одного точного возвращаются только точные.
	// Кандидаты — подключённые не-спектаторы, исключая самого игрока.
	// Заполняет candidates (не более maxCandidates), возвращает ПОЛНОЕ число совпадений.
	i32 CollectSpectateCandidates(const char *query, KZPlayer **candidates, i32 maxCandidates);

	void GetSpectatorList(CUtlVector<CUtlString> &spectatorList);
	KZPlayer *GetSpectatedPlayer();
	KZPlayer *GetNextSpectator(KZPlayer *current);
};
