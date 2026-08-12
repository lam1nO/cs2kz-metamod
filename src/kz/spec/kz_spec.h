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
	// Наблюдает ли this->player за target ОТ ПЕРВОГО ЛИЦА. Отличие от IsSpectating —
	// режим камеры: particle-MHUD спектатора имеет смысл только in-eye (позицию оверлея
	// клиент считает от глаз наблюдаемого, в chase/roaming он повис бы в мире).
	bool IsSpectatingInEye(KZPlayer *target);
	bool SpectatePlayer(const char *playerName);
	bool SpectatePlayer(KZPlayer *target);
	bool CanSpectate();

	// Максимум пунктов в меню выбора цели спека (см. kz_spec_menu.cpp).
	static constexpr i32 KZ_SPEC_MENU_MAX_ITEMS = 8;

	// Кандидаты на спек по подстроке ника (case-insensitive). Точные совпадения
	// приоритетны: при наличии хотя бы одного точного возвращаются только точные.
	// Кандидаты — подключённые не-спектаторы, исключая самого игрока и невидимок
	// (невидимку нельзя ни увидеть в меню, ни выбрать целью по имени).
	// Заполняет candidates (не более maxCandidates), возвращает ПОЛНОЕ число совпадений.
	i32 CollectSpectateCandidates(const char *query, KZPlayer **candidates, i32 maxCandidates);

	// Список зрителей this->player глазами viewer: невидимые для viewer зрители пропускаются.
	void GetSpectatorList(CUtlVector<CUtlString> &spectatorList, KZPlayer *viewer);
	KZPlayer *GetSpectatedPlayer();
	KZPlayer *GetNextSpectator(KZPlayer *current);
};
