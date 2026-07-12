#ifndef KZ_REPLAYMENU_H
#define KZ_REPLAYMENU_H

class KZPlayer;

namespace KZ::replaysystem::menu
{
	// Меню управления реплеем (!rpmenu); тихий no-op, если cs2menus не загружен.
	void OpenReplayControlsMenu(KZPlayer *player);
} // namespace KZ::replaysystem::menu

#endif // KZ_REPLAYMENU_H
