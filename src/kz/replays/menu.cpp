// Меню !rpmenu — управление воспроизведением реплея (cs2menus, паттерн kz_option_menu).
#include "kz/kz.h"
#include "kz/language/kz_language.h"
#include "menu.h"
#include "commands.h"
#include "data.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include "tier0/memdbgon.h"

// Меню-движок cs2menus (определён в cs2kz.cpp); может быть nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

namespace
{
	// Пауза — первый пункт меню: подпись живая, пересобирается в OnRpMenuSelect.
	constexpr int RPMENU_ITEM_PAUSE = 0;

	std::string PauseItemText(KZPlayer *player)
	{
		using namespace KZ::replaysystem;
		bool paused = data::IsReplayPlaying() && data::GetCurrentReplay()->replayPaused;
		const char *lang = player->languageService->GetLanguage();
		return KZLanguageService::PrepareMessageWithLang(lang, paused ? "Replay Menu - Resume" : "Replay Menu - Pause");
	}
} // namespace

static_function void OnRpMenuSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!p)
	{
		return;
	}
	const char *key = g_pMenus->GetItemInfo(menu, item);
	if (!key || !key[0])
	{
		return;
	}

	using namespace KZ::replaysystem;

	if (KZ_STREQ(key, "pause"))
	{
		commands::ToggleReplayPause(p);
	}
	else if (KZ_STREQ(key, "restart"))
	{
		// «С начала»: снять паузу (иначе «заново» не начнётся) и перемотать на 0.
		if (data::IsReplayPlaying())
		{
			data::GetCurrentReplay()->replayPaused = false;
		}
		commands::JumpToReplayTime(p, "0");
	}
	else
	{
		// Ключ пункта — готовый относительный сдвиг ("+10"/"-30"); кламп внутри JumpToReplayTime.
		commands::JumpToReplayTime(p, key);
	}

	// Подпись паузы — по фактическому состоянию (могла смениться и рестартом, и !rppause мимо меню).
	g_pMenus->SetItemText(menu, RPMENU_ITEM_PAUSE, PauseItemText(p).c_str());
}

void KZ::replaysystem::menu::OpenReplayControlsMenu(KZPlayer *player)
{
	if (g_pMenus == nullptr || !player)
	{
		return;
	}

	int slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return;
	}

	// Один хэндл на слот — пересоздаём при повторном вызове (паттерн kz_option_menu).
	static MenuHandle s_rpMenu[MAXPLAYERS + 1] = {};
	if (s_rpMenu[slot] != kInvalidMenuHandle)
	{
		g_pMenus->DestroyMenu(s_rpMenu[slot]);
		s_rpMenu[slot] = kInvalidMenuHandle;
	}

	const char *lang = player->languageService->GetLanguage();
	std::string title = KZLanguageService::PrepareMessageWithLang(lang, "Replay Menu - Title");
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), &OnRpMenuSelect);
	if (m == kInvalidMenuHandle)
	{
		return;
	}

	// Порядок фиксирован: пауза обязана быть пунктом RPMENU_ITEM_PAUSE.
	g_pMenus->AddItem(m, PauseItemText(player).c_str(), "pause", false);
	std::string restart = KZLanguageService::PrepareMessageWithLang(lang, "Replay Menu - Restart");
	g_pMenus->AddItem(m, restart.c_str(), "restart", false);

	static const struct
	{
		const char *labelKey;
		const char *seek;
	} seekItems[] = {
		{"Replay Menu - Back10", "-10"},
		{"Replay Menu - Fwd10",  "+10"},
		{"Replay Menu - Back30", "-30"},
		{"Replay Menu - Fwd30",  "+30"},
	};
	for (const auto &s : seekItems)
	{
		std::string label = KZLanguageService::PrepareMessageWithLang(lang, s.labelKey);
		g_pMenus->AddItem(m, label.c_str(), s.seek, false);
	}

	// Не закрываем при выборе — меню держится, пока игрок сам не закроет (0/ESC).
	g_pMenus->SetCloseOnSelect(m, false);

	s_rpMenu[slot] = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

SCMD(kz_rpmenu, SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	KZ::replaysystem::menu::OpenReplayControlsMenu(player);
	return MRES_SUPERCEDE;
}
