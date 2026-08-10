#ifndef KZ_REPLAYMENU_H
#define KZ_REPLAYMENU_H

class KZPlayer;

namespace KZ::replaysystem::menu
{
	// Меню управления реплеем (!rpmenu); тихий no-op, если cs2menus не загружен.
	void OpenReplayControlsMenu(KZPlayer *player);

	// Открыто ли у слота ИМЕННО !rpmenu (а не любое другое cs2menus-меню). Сравнение по
	// ХЭНДЛУ созданного нами меню — заголовок для этого не годится: он переводится и
	// правится. false, если cs2menus нет, меню закрыто или открыто чужое.
	// Зовётся из худа только когда меню реально открыто (GetActiveMenu берёт мьютекс cs2menus).
	bool IsReplayControlsMenuOpen(int slot);
} // namespace KZ::replaysystem::menu

#endif // KZ_REPLAYMENU_H
