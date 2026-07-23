#pragma once
#include "../kz.h"

class KZGotoService : public KZBaseService
{
	using KZBaseService::KZBaseService;

private:
	// Собрать доступные цели: не сам игрок и не спектаторы. query пустой/nullptr → все;
	// иначе точные совпадения имени приоритетнее подстрочных (паттерн kz_spec).
	i32 CollectGotoCandidates(const char *query, KZPlayer **candidates, i32 maxCandidates);

public:
	virtual void Reset() override;
	static void Init();

	// Телепорт к конкретному игроку (чеки таймера/спектатора внутри). true = телепортнуло.
	bool GotoPlayer(KZPlayer *targetPlayer);
	// Резолв по подстроке ника: пусто → меню всех доступных игроков,
	// неоднозначная подстрока → меню кандидатов (как у !spec).
	bool GotoPlayer(const char *playerNamePart);
};
