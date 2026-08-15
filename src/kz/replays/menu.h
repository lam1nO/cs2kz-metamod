#ifndef KZ_REPLAYMENU_H
#define KZ_REPLAYMENU_H

#include "sdk/datatypes.h" // u64 в SearchHit

#include <string>
#include <vector>

class KZPlayer;

namespace KZ::replaysystem::menu
{
	// Один кандидат для меню выбора реплея по нику (ответ
	// GET /v1/kz/maps/:map/replays/search, схема kzReplaySearchResultSchema).
	struct SearchHit
	{
		std::string nickname;
		std::string replayUuid;
		// PB держателя в миллисекундах — им же отсортирован ответ api (лучший первым);
		// здесь только показываем, повторно не сортируем.
		u64 pbTimeMs {};
	};

	// Меню выбора реплея, когда подстрока ника совпала с несколькими держателями PB
	// (паттерн !spec: одноразовый выбор, закрытие по клику). Возвращает false, если меню
	// показать нечем или некому (cs2menus не загружен, пустой список, плохой слот) — тогда
	// вызывающий сам решает, что делать, и грузит первого кандидата.
	bool OpenReplaySearchMenu(KZPlayer *player, const std::vector<SearchHit> &hits);

	// Меню управления реплеем (!rpmenu); тихий no-op, если cs2menus не загружен.
	void OpenReplayControlsMenu(KZPlayer *player);

	// Открыто ли у слота ИМЕННО !rpmenu (а не любое другое cs2menus-меню). Сравнение по
	// ХЭНДЛУ созданного нами меню — заголовок для этого не годится: он переводится и
	// правится. false, если cs2menus нет, меню закрыто или открыто чужое.
	// Зовётся из худа только когда меню реально открыто (GetActiveMenu берёт мьютекс cs2menus).
	bool IsReplayControlsMenuOpen(int slot);
} // namespace KZ::replaysystem::menu

#endif // KZ_REPLAYMENU_H
