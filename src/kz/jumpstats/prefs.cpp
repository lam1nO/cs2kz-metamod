#include "kz_jumpstats.h"
#include "kz/option/kz_option.h"
#include "kz/language/kz_language.h"

#include "utils/simplecmds.h"
#include "utils/utils.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

// Menu engine (defined in cs2kz.cpp); may be nullptr if the plugin isn't loaded.
extern ICS2Menus *g_pMenus;

bool KZJumpstatsService::GetDistTierFromString(const char *tierString, DistanceTier &outTier)
{
	if (!tierString || !V_stricmp("", tierString))
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Tier Hint");
		return false;
	}
	if (utils::IsNumeric(tierString))
	{
		DistanceTier tierValue = static_cast<DistanceTier>(V_StringToInt32(tierString, -1));
		if (tierValue < DistanceTier_None || tierValue >= DISTANCETIER_COUNT)
		{
			this->player->languageService->PrintChat(true, false, "Jumpstats Option - Tier Hint");
			return false;
		}
		outTier = tierValue;
		return true;
	}
	if (KZ_STREQI("None", tierString))
	{
		outTier = DistanceTier_None;
		return true;
	}
	if (KZ_STREQI("Meh", tierString))
	{
		outTier = DistanceTier_Meh;
		return true;
	}
	if (KZ_STREQI("Impressive", tierString))
	{
		outTier = DistanceTier_Impressive;
		return true;
	}
	if (KZ_STREQI("Perfect", tierString))
	{
		outTier = DistanceTier_Perfect;
		return true;
	}
	if (KZ_STREQI("Godlike", tierString))
	{
		outTier = DistanceTier_Godlike;
		return true;
	}
	if (KZ_STREQI("Ownage", tierString))
	{
		outTier = DistanceTier_Ownage;
		return true;
	}
	if (KZ_STREQI("Wrecker", tierString))
	{
		outTier = DistanceTier_Wrecker;
		return true;
	}
	return false;
}

void KZJumpstatsService::SetBroadcastMinTier(const char *tierString)
{
	DistanceTier tier;
	bool success = GetDistTierFromString(tierString, tier);
	if (!success)
	{
		return;
	}

	if (tier == this->player->optionService->GetPreferenceInt("jsBroadcastMinTier", DistanceTier_Godlike))
	{
		return;
	}

	this->player->optionService->SetPreferenceInt("jsBroadcastMinTier", tier);
	if (tier == 0)
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Broadcast Tier - Disabled");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Broadcast Tier - Response", tierString);
	}
}

void KZJumpstatsService::SetBroadcastMinTierConsole(const char *tierString)
{
	DistanceTier tier;
	bool success = GetDistTierFromString(tierString, tier);
	if (!success)
	{
		return;
	}

	if (tier == this->player->optionService->GetPreferenceInt("jsBroadcastMinTierConsole", DistanceTier_Godlike))
	{
		return;
	}

	this->player->optionService->SetPreferenceInt("jsBroadcastMinTierConsole", tier);
	if (tier == 0)
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Console Broadcast Tier - Disabled");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Console Broadcast Tier - Response", tierString);
	}
}

void KZJumpstatsService::SetBroadcastSoundMinTier(const char *tierString)
{
	DistanceTier tier;
	bool success = GetDistTierFromString(tierString, tier);
	if (!success)
	{
		return;
	}

	if (tier == this->player->optionService->GetPreferenceInt("jsBroadcastSoundMinTier", DistanceTier_Godlike))
	{
		return;
	}

	this->player->optionService->SetPreferenceInt("jsBroadcastSoundMinTier", tier);
	if (tier == 0)
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Sound Broadcast Tier - Disabled");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Sound Broadcast Tier - Response", tierString);
	}
}

void KZJumpstatsService::SetMinTier(const char *tierString)
{
	DistanceTier tier;
	bool success = GetDistTierFromString(tierString, tier);
	if (!success)
	{
		return;
	}

	if (tier == this->player->optionService->GetPreferenceInt("jsMinTier", DistanceTier_Impressive))
	{
		return;
	}

	this->player->optionService->SetPreferenceInt("jsMinTier", tier);
	if (tier == 0)
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Tier - Disabled");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Tier - Response", tierString);
	}
}

void KZJumpstatsService::SetMinTierConsole(const char *tierString)
{
	DistanceTier tier;
	bool success = GetDistTierFromString(tierString, tier);
	if (!success)
	{
		return;
	}

	if (tier == this->player->optionService->GetPreferenceInt("jsMinTierConsole", DistanceTier_Impressive))
	{
		return;
	}

	this->player->optionService->SetPreferenceInt("jsMinTierConsole", tier);
	if (tier == 0)
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Console Tier - Disabled");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Console Tier - Response", tierString);
	}
}

void KZJumpstatsService::ToggleExtendedChatStats()
{
	this->player->optionService->SetPreferenceBool("jsExtendedChatStats",
												   !this->player->optionService->GetPreferenceBool("jsExtendedChatStats", false));
	if (this->player->optionService->GetPreferenceBool("jsExtendedChatStats", false))
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Extended Chat Stats - Enable");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Extended Chat Stats - Disable");
	}
}

void KZJumpstatsService::SetJumpstatsVolume(f32 volume)
{
	this->player->optionService->SetPreferenceFloat("jsVolume", volume);
	this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Volume - Response", volume);
}

void KZJumpstatsService::ToggleJumpstatsReporting()
{
	this->player->optionService->SetPreferenceBool("jsReporting", !this->player->optionService->GetPreferenceBool("jsReporting", true));
	if (this->player->optionService->GetPreferenceBool("jsReporting", true))
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Reporting - Enable");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Reporting - Disable");
	}
}

void KZJumpstatsService::SetSoundMinTier(const char *tierString)
{
	DistanceTier tier;
	bool success = GetDistTierFromString(tierString, tier);
	if (!success)
	{
		return;
	}

	if (tier == this->player->optionService->GetPreferenceInt("jsSoundMinTier", DistanceTier_Impressive))
	{
		return;
	}

	this->player->optionService->SetPreferenceInt("jsSoundMinTier", tier);
	if (tier == 0)
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Sound Tier - Disabled");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Minimum Sound Tier - Response", tierString);
	}
}

void KZJumpstatsService::ToggleJSAlways()
{
	this->player->optionService->SetPreferenceBool("jsAlways", !this->player->optionService->GetPreferenceBool("jsAlways", false));
	if (this->player->optionService->GetPreferenceBool("jsAlways", false))
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Always - Enable");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Always - Disable");
	}
}

void KZJumpstatsService::ToggleFailstatsReporting()
{
	this->player->optionService->SetPreferenceBool("jsFailstats", !this->player->optionService->GetPreferenceBool("jsFailstats", true));
	if (this->player->optionService->GetPreferenceBool("jsFailstats", true))
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Failstats Chat Reporting - Enable");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Failstats Chat Reporting - Disable");
	}
}

void KZJumpstatsService::ToggleFailstatsConsoleReporting()
{
	this->player->optionService->SetPreferenceBool("jsFailstatsConsole", !this->player->optionService->GetPreferenceBool("jsFailstatsConsole", true));
	if (this->player->optionService->GetPreferenceBool("jsFailstatsConsole", true))
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Failstats Console Reporting - Enable");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Jumpstats Option - Failstats Console Reporting - Disable");
	}
}

SCMD(kz_jstier, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->SetMinTier(args->Arg(1));
	return MRES_SUPERCEDE;
}

SCMD(kz_jstierconsole, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->SetMinTierConsole(args->Arg(1));
	return MRES_SUPERCEDE;
}

SCMD(kz_jssound, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->SetSoundMinTier(args->Arg(1));
	return MRES_SUPERCEDE;
}

SCMD(kz_jsbroadcast, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->SetBroadcastMinTier(args->Arg(1));
	return MRES_SUPERCEDE;
}

SCMD(kz_jsbroadcastconsole, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->SetBroadcastMinTierConsole(args->Arg(1));
	return MRES_SUPERCEDE;
}

SCMD(kz_jsbroadcastsound, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->SetBroadcastSoundMinTier(args->Arg(1));
	return MRES_SUPERCEDE;
}

SCMD(kz_jsalways, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->ToggleJSAlways();
	return MRES_SUPERCEDE;
}

SCMD(kz_jsfailstats, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->ToggleFailstatsReporting();
	return MRES_SUPERCEDE;
}

SCMD(kz_jsfailstatsconsole, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->ToggleFailstatsConsoleReporting();
	return MRES_SUPERCEDE;
}

SCMD(kz_jsextend, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->ToggleExtendedChatStats();
	return MRES_SUPERCEDE;
}

SCMD(kz_jsvolume, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (args->ArgC() < 2)
	{
		player->languageService->PrintChat(true, false, "Jumpstats Option - Jumpstats Volume - Current",
										   player->optionService->GetPreferenceFloat("jsVolume", 0.75f));
		return MRES_SUPERCEDE;
	}
	f32 volume = Clamp(static_cast<f32>(V_atof(args->Arg(1))), 0.0f, 2.0f);

	player->jumpstatsService->SetJumpstatsVolume(volume);
	return MRES_SUPERCEDE;
}

SCMD(kz_jumpstats, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->ToggleJumpstatsReporting();
	return MRES_SUPERCEDE;
}

// === Interactive kz_js menu (cs2menus) ===============================================

// Phrase keys for tier names. No localized tier names existed before this menu (tiers
// appear as plain English words in every language, see "Jumpstats Option - Tier Hint"),
// so these are new keys; en/ru currently share the English word by fork convention.
static const char *s_tierNameKeys[DISTANCETIER_COUNT] = {
	"Jumpstats - Tier Name None",       "Jumpstats - Tier Name Meh",     "Jumpstats - Tier Name Impressive",
	"Jumpstats - Tier Name Perfect",    "Jumpstats - Tier Name Godlike", "Jumpstats - Tier Name Ownage",
	"Jumpstats - Tier Name Wrecker",
};

// Volume presets cycled through by the jsVolume menu item.
static constexpr f32 s_volumePresets[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};

enum class JSMenuItemKind : u8
{
	Toggle,    // bool preference: on/off
	TierCycle, // int preference: 0..DISTANCETIER_COUNT-1, cycles
	Volume,    // float preference: cycles through s_volumePresets
};

struct JSMenuItem
{
	const char *label;   // phrase key for the item label
	const char *prefKey; // preference name in optionService, also the menu item's info tag
	JSMenuItemKind kind;
	i64 defaultInt;   // default for Toggle (0/1) and TierCycle
	f32 defaultFloat; // default for Volume
};

// kz_js menu items, in display order.
static const JSMenuItem s_jsMenuItems[] = {
	{"Jumpstats - Menu Label Reporting",         "jsReporting",             JSMenuItemKind::Toggle,    1,                       0.0f },
	{"Jumpstats - Menu Label Always",            "jsAlways",                JSMenuItemKind::Toggle,    0,                       0.0f },
	{"Jumpstats - Menu Label MinTier",           "jsMinTier",               JSMenuItemKind::TierCycle, DistanceTier_Impressive, 0.0f },
	{"Jumpstats - Menu Label SoundMinTier",      "jsSoundMinTier",          JSMenuItemKind::TierCycle, DistanceTier_Impressive, 0.0f },
	{"Jumpstats - Menu Label Volume",            "jsVolume",                JSMenuItemKind::Volume,    0,                       0.75f},
	{"Jumpstats - Menu Label BroadcastMinTier",  "jsBroadcastMinTier",      JSMenuItemKind::TierCycle, DistanceTier_Godlike,    0.0f },
	{"Jumpstats - Menu Label BroadcastSoundTier","jsBroadcastSoundMinTier", JSMenuItemKind::TierCycle, DistanceTier_Godlike,    0.0f },
	{"Jumpstats - Menu Label Failstats",         "jsFailstats",             JSMenuItemKind::Toggle,    1,                       0.0f },
	{"Jumpstats - Menu Label ExtendedChatStats", "jsExtendedChatStats",     JSMenuItemKind::Toggle,    0,                       0.0f },
	{"Jumpstats - Menu Label MinTierConsole",    "jsMinTierConsole",        JSMenuItemKind::TierCycle, DistanceTier_Impressive, 0.0f },
};

// "<label>: <value>" text for one menu item, from the current preference.
static_function std::string JSMenuItemText(KZPlayer *p, const JSMenuItem &it, const char *lang)
{
	std::string label = KZLanguageService::PrepareMessageWithLang(lang, it.label);
	char text[128];
	switch (it.kind)
	{
		case JSMenuItemKind::Toggle:
		{
			bool on = p->optionService->GetPreferenceBool(it.prefKey, it.defaultInt != 0);
			std::string state = KZLanguageService::PrepareMessageWithLang(lang, on ? "HUD - Menu On" : "HUD - Menu Off");
			// ВКЛ зелёным, ВЫКЛ красным (чат-цвет-байт 0x04/0x07 → cs2menus ColorizeChat в <font color>).
			const char *stateColor = on ? "\x04" : "\x07";
			V_snprintf(text, sizeof(text), "%s: %s%s", label.c_str(), stateColor, state.c_str());
			break;
		}
		case JSMenuItemKind::TierCycle:
		{
			i64 tier = Clamp(p->optionService->GetPreferenceInt(it.prefKey, it.defaultInt), (i64)0, (i64)(DISTANCETIER_COUNT - 1));
			std::string tierName = KZLanguageService::PrepareMessageWithLang(lang, s_tierNameKeys[tier]);
			V_snprintf(text, sizeof(text), "%s: %s", label.c_str(), tierName.c_str());
			break;
		}
		case JSMenuItemKind::Volume:
		{
			f32 vol = static_cast<f32>(p->optionService->GetPreferenceFloat(it.prefKey, it.defaultFloat));
			V_snprintf(text, sizeof(text), "%s: %.0f%%", label.c_str(), vol * 100.0f);
			break;
		}
	}
	return std::string(text);
}

// Chat fallback for when the menu engine isn't loaded: current value of every item.
static_function void PrintJumpstatsMenuSummary(KZPlayer *p)
{
	const char *lang = p->languageService->GetLanguage();
	for (const auto &it : s_jsMenuItems)
	{
		p->languageService->PrintChat(true, false, "Jumpstats - Menu Summary Line", JSMenuItemText(p, it, lang).c_str());
	}
}

// Menu item select callback (pattern: OnHUDMenuSelect, particles.cpp).
static_function void OnJSMenuSelect(MenuHandle menu, int slot, int item)
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
	for (const auto &it : s_jsMenuItems)
	{
		if (!KZ_STREQ(key, it.prefKey))
		{
			continue;
		}
		switch (it.kind)
		{
			case JSMenuItemKind::Toggle:
			{
				bool next = !p->optionService->GetPreferenceBool(it.prefKey, it.defaultInt != 0);
				p->optionService->SetPreferenceBool(it.prefKey, next);
				break;
			}
			case JSMenuItemKind::TierCycle:
			{
				i64 next = (p->optionService->GetPreferenceInt(it.prefKey, it.defaultInt) + 1) % DISTANCETIER_COUNT;
				p->optionService->SetPreferenceInt(it.prefKey, next);
				break;
			}
			case JSMenuItemKind::Volume:
			{
				f32 cur = static_cast<f32>(p->optionService->GetPreferenceFloat(it.prefKey, it.defaultFloat));
				f32 next = s_volumePresets[0]; // wrap back to the first preset if already at/above the last
				for (f32 v : s_volumePresets)
				{
					if (v > cur + 0.001f)
					{
						next = v;
						break;
					}
				}
				p->optionService->SetPreferenceFloat(it.prefKey, next);
				break;
			}
		}
		std::string text = JSMenuItemText(p, it, p->languageService->GetLanguage());
		g_pMenus->SetItemText(menu, item, text.c_str());
		return;
	}
}

// Один хэндл JS-меню на слот — пересоздаётся при каждом построении. Вход теперь один (!js):
// подменю старого cs2menus-меню !options убрано вместе с ним, настройки джампстатов живут в
// panorama-реестре.
static_global MenuHandle s_jsMenu[MAXPLAYERS + 1] = {};

u32 KZJumpstatsService::CreateJumpstatsMenu()
{
	if (g_pMenus == nullptr)
	{
		return kInvalidMenuHandle;
	}
	int slot = this->player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return kInvalidMenuHandle;
	}
	if (s_jsMenu[slot] != kInvalidMenuHandle)
	{
		g_pMenus->DestroyMenu(s_jsMenu[slot]);
		s_jsMenu[slot] = kInvalidMenuHandle;
	}
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, "Jumpstats", &OnJSMenuSelect);
	if (m == kInvalidMenuHandle)
	{
		return kInvalidMenuHandle;
	}
	const char *lang = this->player->languageService->GetLanguage();
	for (const auto &it : s_jsMenuItems)
	{
		std::string text = JSMenuItemText(this->player, it, lang);
		g_pMenus->AddItem(m, text.c_str(), it.prefKey, false);
	}
	g_pMenus->SetCloseOnSelect(m, false); // item text updates live
	s_jsMenu[slot] = m;
	return m;
}

void KZJumpstatsService::OpenJumpstatsMenu()
{
	if (g_pMenus == nullptr)
	{
		PrintJumpstatsMenuSummary(this->player);
		return;
	}
	MenuHandle m = (MenuHandle)this->CreateJumpstatsMenu();
	if (m == kInvalidMenuHandle)
	{
		PrintJumpstatsMenuSummary(this->player);
		return;
	}
	g_pMenus->DisplayMenu(m, this->player->GetPlayerSlot().Get(), 0);
}

SCMD(kz_js, SCFL_JUMPSTATS | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->jumpstatsService->OpenJumpstatsMenu();
	return MRES_SUPERCEDE;
}
