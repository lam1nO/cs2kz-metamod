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

	// Шаги перемотки (сек) для регулируемых строк; знак delta задаёт направление
	// (A/AdjustDec = назад, D/AdjustInc = вперёд).
	constexpr float RPMENU_SEEK_STEP_10 = 10.0f;
	constexpr float RPMENU_SEEK_STEP_30 = 30.0f;

	std::string PauseItemText(KZPlayer *player)
	{
		using namespace KZ::replaysystem;
		bool paused = data::IsReplayPlaying() && data::GetCurrentReplay()->replayPaused;
		const char *lang = player->languageService->GetLanguage();
		return KZLanguageService::PrepareMessageWithLang(lang, paused ? "Replay Menu - Resume" : "Replay Menu - Pause");
	}

	// Текст регулируемой строки перемотки: «Перемотка: N сек». Стрелки ◄ ► рисует
	// сам движок cs2menus для adjustable-строк; значение (шаг) фиксировано.
	std::string SeekItemText(KZPlayer *player, int step)
	{
		const char *lang = player->languageService->GetLanguage();
		return KZLanguageService::PrepareMessageWithLang(lang, "Replay Menu - Seek", step);
	}

	// Хэндл нашего !rpmenu на слот — один на слот, пересоздаётся при повторном открытии
	// (паттерн kz_option_menu). Живёт в области файла, а не внутри функции: по нему худ
	// отличает !rpmenu от любого другого cs2menus-меню (см. IsReplayControlsMenuOpen).
	// Сравнение по хэндлу корректно и на устаревшем значении: cs2menus раздаёт хэндлы
	// монотонным счётчиком (`m_nextHandle++`, menu_manager.cpp) и НЕ переиспользует
	// освобождённые id — чужое меню не может получить наш номер, а закрытое/уничтоженное
	// наше просто перестаёт быть активным. Оговорка: счётчик живёт в объекте менеджера, и
	// `meta unload/load` cs2menus начинает нумерацию заново, а этот массив её переживает.
	// Практически это перекрыто тем, что после выгрузки cs2menus висячим становится сам
	// `g_pMenus` (общая проблема форка, не этого места).
	MenuHandle s_rpMenu[MAXPLAYERS + 1] = {};
} // namespace

// A/D по регулируемой строке перемотки: знак delta задаёт направление (D = +шаг
// вперёд, A = −шаг назад). Переиспользуем seek-логику JumpToReplayTime, собрав
// относительный сдвиг вида "+10"/"-30". min/max движку нужны для клампа значения,
// но здесь значение не хранится (шаг фиксирован) — они не используются.
static_function void OnRpMenuAdjust(MenuHandle menu, int slot, int item, f32 delta, f32 minValue, f32 maxValue)
{
	KZPlayer *p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!p)
	{
		return;
	}
	char seek[16];
	V_snprintf(seek, sizeof(seek), "%+d", (int)delta);
	KZ::replaysystem::commands::JumpToReplayTime(p, seek);
}

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
	else if (KZ_STREQ(key, "end"))
	{
		// «Завершить»: остановить плейбек, убрать бота, закрыть меню.
		commands::StopReplay(p);
		g_pMenus->CancelMenu(slot);
		return;
	}
	// Регулируемые строки перемотки (info "seek") реагируют на A/D в OnRpMenuAdjust;
	// выбор E по ним ничего не делает — сюда попадём, но действий нет.

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

	// Перемотка — одной регулируемой строкой на шаг: A (◄) — назад, D (►) — вперёд.
	// Стрелки рисует движок; текст показывает фиксированный шаг. Вторая строка — шаг 30.
	std::string seek10 = SeekItemText(player, (int)RPMENU_SEEK_STEP_10);
	g_pMenus->AddAdjustableItem(m, seek10.c_str(), "seek", RPMENU_SEEK_STEP_10, -RPMENU_SEEK_STEP_10, RPMENU_SEEK_STEP_10);
	std::string seek30 = SeekItemText(player, (int)RPMENU_SEEK_STEP_30);
	g_pMenus->AddAdjustableItem(m, seek30.c_str(), "seek", RPMENU_SEEK_STEP_30, -RPMENU_SEEK_STEP_30, RPMENU_SEEK_STEP_30);
	g_pMenus->SetAdjustCallback(m, &OnRpMenuAdjust);

	// «Завершить» — останавливает реплей и закрывает меню (обрабатывается в OnRpMenuSelect).
	std::string endLabel = KZLanguageService::PrepareMessageWithLang(lang, "Replay Menu - End");
	g_pMenus->AddItem(m, endLabel.c_str(), "end", false);

	// Не закрываем при выборе — меню держится, пока игрок сам не закроет (0/ESC).
	g_pMenus->SetCloseOnSelect(m, false);

	s_rpMenu[slot] = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

bool KZ::replaysystem::menu::IsReplayControlsMenuOpen(int slot)
{
	if (g_pMenus == nullptr || slot < 0 || slot > MAXPLAYERS)
	{
		return false;
	}
	MenuHandle mine = s_rpMenu[slot];
	if (mine == kInvalidMenuHandle)
	{
		return false;
	}
	return g_pMenus->GetActiveMenu(slot) == mine;
}

SCMD(kz_rpmenu, SCFL_REPLAY | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	KZ::replaysystem::menu::OpenReplayControlsMenu(player);
	return MRES_SUPERCEDE;
}
