// Меню выбора цели !spec при неоднозначной подстроке (cs2menus, паттерн !rpmenu).
#include "kz_spec_menu.h"
#include "kz_spec.h"
#include "kz/invisible/kz_invisible.h"
#include "kz/language/kz_language.h"
#include "utils/utils.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include "tier0/memdbgon.h"

// Меню-движок cs2menus (определён в cs2kz.cpp); может быть nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

static_function void OnSpecMenuSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!p)
	{
		return;
	}
	const char *info = g_pMenus->GetItemInfo(menu, item);
	if (!info || !info[0])
	{
		return;
	}
	// info — userID кандидата строкой; ревалидация: цель могла выйти, уйти в спек
	// или стать невидимкой (RCON), пока меню висело.
	KZPlayer *target = g_pKZPlayerManager->ToPlayer(CPlayerUserId(V_StringToInt32(info, -1)));
	if (!target || !target->GetController() || target->GetController()->GetTeam() == CS_TEAM_SPECTATOR
		|| KZInvisibleService::ShouldHideFrom(target, p))
	{
		p->languageService->PrintChat(true, false, "Spectate Failure (Player Not Found)");
		return;
	}
	p->specService->SpectatePlayer(target);
}

void KZ::spec::OpenSpectateMenu(KZPlayer *player, KZPlayer **candidates, i32 count, i32 total)
{
	if (!player || count <= 0)
	{
		return;
	}
	// Без движка меню — фолбэк: первый совпавший (поведение не хуже апстримного).
	if (g_pMenus == nullptr)
	{
		player->specService->SpectatePlayer(candidates[0]);
		return;
	}

	int slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return;
	}

	// Один хэндл на слот — пересоздаём при повторном вызове (паттерн kz_option_menu).
	static MenuHandle s_specMenu[MAXPLAYERS + 1] = {};
	if (s_specMenu[slot] != kInvalidMenuHandle)
	{
		g_pMenus->DestroyMenu(s_specMenu[slot]);
		s_specMenu[slot] = kInvalidMenuHandle;
	}

	const char *lang = player->languageService->GetLanguage();
	std::string title = KZLanguageService::PrepareMessageWithLang(lang, "Spec Menu - Title");
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), &OnSpecMenuSelect);
	if (m == kInvalidMenuHandle)
	{
		return;
	}

	for (i32 i = 0; i < count; i++)
	{
		char info[16];
		V_snprintf(info, sizeof(info), "%d", candidates[i]->GetClient()->GetUserID().Get());
		g_pMenus->AddItem(m, candidates[i]->GetName(), info, false);
	}

	// Одноразовый выбор — меню закрывается по клику (в отличие от !rpmenu).
	g_pMenus->SetCloseOnSelect(m, true);

	s_specMenu[slot] = m;
	g_pMenus->DisplayMenu(m, slot, 0);

	if (total > count)
	{
		player->languageService->PrintChat(true, false, "Spec - Too Many Matches", total, count);
	}
}
