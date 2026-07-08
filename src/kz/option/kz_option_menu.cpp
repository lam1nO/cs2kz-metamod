// Интерактивное меню !options (cs2menus) — замена RadioPanel-группы menu_option (radio2),
// которая не умеет самопереоткрываться (у нативного RadioPanel-хака CS2 есть только 2
// самопереоткрывающихся слота — radio/radio1, третьего "radio2" не существует).
#include "kz/option/kz_option.h"
#include "kz/language/kz_language.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "kz/quiet/kz_quiet.h"
#include "kz/jumpstats/kz_jumpstats.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include "tier0/memdbgon.h"

// Меню-движок cs2menus (определён в kz_hud.cpp); может быть nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

static void ToggleHidePlayersOpt(KZPlayer *player)
{
	player->quietService->ToggleHide();
}

static void ToggleHideWeaponOpt(KZPlayer *player)
{
	player->quietService->ToggleHideWeapon();
}

static void ToggleHideLegsOpt(KZPlayer *player)
{
	player->ToggleHideLegs();
}

static void ToggleJsDisplayOpt(KZPlayer *player)
{
	player->jumpstatsService->ToggleJumpstatsReporting();
}

static void ToggleJsAlwaysOpt(KZPlayer *player)
{
	player->jumpstatsService->ToggleJSAlways();
}

struct OptionMenuToggle
{
	const char *labelKey;
	const char *prefKey;
	bool defaultValue;
	void (*toggle)(KZPlayer *player);
};

// Таблица per-пункт тумблеров меню !options.
static const OptionMenuToggle s_optionToggles[] = {
	{"Options - Menu Label HidePlayers", "hideOtherPlayers", false, &ToggleHidePlayersOpt},
	{"Options - Menu Label HideWeapon",  "hideWeapon",       false, &ToggleHideWeaponOpt },
	{"Options - Menu Label HideLegs",    "hideLegs",         true,  &ToggleHideLegsOpt   },
	{"Options - Menu Label JsDisplay",   "jsReporting",      true,  &ToggleJsDisplayOpt  },
	{"Options - Menu Label JsAlways",    "jsAlways",         false, &ToggleJsAlwaysOpt   },
};

// Колбэк выбора пункта меню !options.
static_function void OnOptionsMenuSelect(MenuHandle menu, int slot, int item)
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

	// Действия без состояния — не обновляют текст пункта, просто выполняются.
	if (KZ_STREQ(key, "action:ssp"))
	{
		p->checkpointService->SetStartPosition();
		return;
	}
	if (KZ_STREQ(key, "action:csp"))
	{
		p->checkpointService->ClearStartPosition();
		return;
	}

	const char *lang = p->languageService->GetLanguage();
	for (const auto &t : s_optionToggles)
	{
		if (KZ_STREQ(key, t.prefKey))
		{
			t.toggle(p);
			bool nowOn = p->optionService->GetPreferenceBool(t.prefKey, t.defaultValue);
			std::string elemLabel = KZLanguageService::PrepareMessageWithLang(lang, t.labelKey);
			const char *statePhrase = nowOn ? "HUD - Menu On" : "HUD - Menu Off";
			std::string stateStr = KZLanguageService::PrepareMessageWithLang(lang, statePhrase);
			char newText[128];
			V_snprintf(newText, sizeof(newText), "%s: %s", elemLabel.c_str(), stateStr.c_str());
			g_pMenus->SetItemText(menu, item, newText);
			return;
		}
	}
}

static void OpenOptionsMenu(KZPlayer *player)
{
	if (g_pMenus == nullptr)
	{
		return;
	}

	int slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return;
	}

	// Один хэндл на слот — пересоздаём при повторном вызове.
	static MenuHandle s_optionsMenu[MAXPLAYERS + 1] = {};
	if (s_optionsMenu[slot] != kInvalidMenuHandle)
	{
		g_pMenus->DestroyMenu(s_optionsMenu[slot]);
		s_optionsMenu[slot] = kInvalidMenuHandle;
	}

	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, "Options", &OnOptionsMenuSelect);
	if (m == kInvalidMenuHandle)
	{
		return;
	}

	const char *lang = player->languageService->GetLanguage();

	for (const auto &t : s_optionToggles)
	{
		bool on = player->optionService->GetPreferenceBool(t.prefKey, t.defaultValue);
		std::string elemLabel = KZLanguageService::PrepareMessageWithLang(lang, t.labelKey);
		const char *statePhrase = on ? "HUD - Menu On" : "HUD - Menu Off";
		std::string stateStr = KZLanguageService::PrepareMessageWithLang(lang, statePhrase);
		char text[128];
		V_snprintf(text, sizeof(text), "%s: %s", elemLabel.c_str(), stateStr.c_str());
		g_pMenus->AddItem(m, text, t.prefKey, false);
	}

	std::string sspLabel = KZLanguageService::PrepareMessageWithLang(lang, "Options - Menu Label SetStartPos");
	g_pMenus->AddItem(m, sspLabel.c_str(), "action:ssp", false);
	std::string cspLabel = KZLanguageService::PrepareMessageWithLang(lang, "Options - Menu Label ClearStartPos");
	g_pMenus->AddItem(m, cspLabel.c_str(), "action:csp", false);

	// Не закрываем при выборе — меню держится, пока игрок сам не закроет (0/ESC).
	g_pMenus->SetCloseOnSelect(m, false);

	s_optionsMenu[slot] = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

SCMD(kz_options, SCFL_PLAYER | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	OpenOptionsMenu(player);
	return MRES_SUPERCEDE;
}
