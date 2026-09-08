// Команда `!options`/`!o` и общий страж cs2menus-меню на старте забега.
//
// Старое построение !options на cs2menus (корень с подменю чекпоинты/видимость/звуки/
// сообщения/paint, таблицы пунктов и их apply-функции) удалено: команда переехала на
// panorama-реестр настроек ещё в задаче 10, а с ним построение стало недостижимым.
// Настройки худа и локальные ветки живут в реестре (kz/option/menu, kz/option/prefs).
// Меню !zones/!rpmenu/!prac/!goto/джампстатов/реплеев остаются на cs2menus.
//
// InitOptionsMenu()/её таймер-слушатель РАБОЧИЕ: слушатель гасит ЛЮБОЕ открытое
// cs2menus-меню на старте забега (!zones/!rpmenu/!prac/!goto/джампстаты/реплеи) —
// снимать его нельзя.
#include "kz/option/kz_option.h"
#include "kz/timer/kz_timer.h"
#include "kz/hud/kz_hud.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include "tier0/memdbgon.h"

// Меню-движок cs2menus (определён в cs2kz.cpp); может быть nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

// Старт забега закрывает любое открытое cs2menus-меню игрока: NavSelect на серверах —
// E (см. gameops core.cfg), и оставленное открытым незакрывающееся меню превращало
// каждый E (кнопки/двери карты) в тычок по пункту меню посреди рана.
static_global class KZTimerServiceEventListener_OptionsMenu : public KZTimerServiceEventListener
{
	virtual void OnTimerStartPost(KZPlayer *player, u32 courseGUID) override
	{
		if (g_pMenus == nullptr || !player)
		{
			return;
		}
		int slot = player->GetPlayerSlot().Get();
		if (slot >= 0 && slot <= MAXPLAYERS && g_pMenus->HasMenu(slot))
		{
			g_pMenus->CancelMenu(slot);
		}
	}
} s_optionsMenuTimerListener;

void KZ::option::InitOptionsMenu()
{
	KZTimerService::RegisterEventListener(&s_optionsMenuTimerListener);
}

// Корень реестра настроек (panorama) — тот же вход, тот же захват ввода, что у `kz_hudmenu`
// ниже (layout/menu.cpp), но БЕЗ ключа категории: OpenLayoutMenu(NULL) ставит menuCategory=-1 —
// список всех категорий (HUD + Misc + Jumpstats + наши локальные ветки, все четыре Register()
// включены в Init-порядок, Task 15), ни одна не выбрана. `kz_hudmenu` ниже, напротив, передаёт
// ключ HUD-категории и садится сразу на неё (R8) — с этой правки !options и !hudmenu
// физически разные экраны, а не совпадают на индексе 0.
// Если panorama-сущность не создалась (аддон не доехал), OpenLayoutMenu сам сообщает об
// этом игроку в чат ("MHUD - Unavailable") — тишины на команду не остаётся.
SCMD(kz_options, SCFL_PLAYER | SCFL_PREFERENCE | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	if (player->hudService->IsLayoutMenuOpen())
	{
		player->hudService->CloseLayoutMenu();
	}
	else
	{
		player->hudService->OpenLayoutMenu();
	}
	return MRES_SUPERCEDE;
}

// Короткий алиас !o. Без SCFL_HELP: в чат-списке !help остаётся одна строка !options
// (алиас упомянут в её описании), иначе список дублируется.
SCMD_LINK(kz_o, kz_options);
