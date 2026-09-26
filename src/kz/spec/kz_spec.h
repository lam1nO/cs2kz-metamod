#pragma once
#include "../kz.h"

struct KZCourseDescriptor;

// Чьи данные показывают информационные команды (!pb/!wr/!replay pb|wr|…) по умолчанию, когда
// курс/режим/игрок не заданы аргументами: у наблюдающего — НАБЛЮДАЕМОГО (живого игрока или
// реплей-бота), иначе самого игрока. Раньше эти команды у спектатора брали его собственный
// последний курс/режим, и «!pb» за чужим раном показывал не то.
struct KZInfoSubject
{
	// Чьи режим и стили брать; никогда не NULL. У реплей-бота они живые: события записи
	// переключают режим/стили бота (replays/events.cpp).
	KZPlayer *player {};
	bool spectated {}; // player — наблюдаемый, а не сам вызывающий
	bool replayBot {}; // player — реплей-бот
	// Курс субъекта; NULL — курса нет (вызывающий берёт первый курс карты, как раньше).
	// У бота — курс идущего рана записи (таймер бота курса не знает).
	const KZCourseDescriptor *course {};
	// Чей PB: у живого — сам игрок, у бота — владелец записи из шапки реплея. 0 — владельца
	// у записи нет (ручной/джамп-реплей): тогда вызывающий, как раньше.
	u64 steamId64 {};
	CUtlString name;
};

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
	// Кандидаты — подключённые не-спектаторы, исключая самого игрока и невидимок
	// (невидимку нельзя ни увидеть в меню, ни выбрать целью по имени).
	// Заполняет candidates (не более maxCandidates), возвращает ПОЛНОЕ число совпадений.
	i32 CollectSpectateCandidates(const char *query, KZPlayer **candidates, i32 maxCandidates);

	// Список зрителей this->player глазами viewer: невидимые для viewer зрители пропускаются.
	void GetSpectatorList(CUtlVector<CUtlString> &spectatorList, KZPlayer *viewer);
	KZPlayer *GetSpectatedPlayer();
	// См. KZInfoSubject выше.
	KZInfoSubject GetInfoSubject();
	KZPlayer *GetNextSpectator(KZPlayer *current);
};
