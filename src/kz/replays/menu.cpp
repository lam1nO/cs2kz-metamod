// Меню реплея: словарь пунктов, их семантика и тексты карточки panorama-меню
// (hud/layout/rpmenu.cpp) + cs2menus-меню выбора реплея по нику (OpenReplaySearchMenu).
// Старое cs2menus-!rpmenu удалено 09.09; сама команда !rpmenu лишь напоминает, что меню
// открывается само при просмотре реплей-бота.
#include "kz/kz.h"
#include "kz/language/kz_language.h"
#include "menu.h"
#include "commands.h"
#include "kz_replaysystem.h" // GetTime/GetEndTime/GetPaused — строка состояния карточки
#include "data.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include <cmath>

#include "tier0/memdbgon.h"

// Меню-движок cs2menus (определён в cs2kz.cpp); может быть nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

namespace
{
	// Шаг перемотки (сек) регулируемой строки: A — назад, D — вперёд. ±30 сек остаётся
	// командой !rpgoto +30.
	constexpr float RPMENU_SEEK_STEP_10 = 10.0f;

	// Пресеты скорости: A/D переключают по списку, а не прибавляют шаг — на замедлении
	// осмысленны доли (0.25 ощутимо медленнее 0.5), а на ускорении — кратности.
	constexpr float RPMENU_SPEEDS[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
	constexpr int RPMENU_SPEEDS_COUNT = (int)(sizeof(RPMENU_SPEEDS) / sizeof(RPMENU_SPEEDS[0]));

	// Ближайший пресет к текущей скорости: она могла быть задана !rpspeed произвольным
	// числом, и A/D обязаны продолжить с ближайшей ступени, а не прыгнуть в начало списка.
	int NearestSpeedIndex(float speed)
	{
		int best = 0;
		for (int i = 1; i < RPMENU_SPEEDS_COUNT; i++)
		{
			if (fabsf(RPMENU_SPEEDS[i] - speed) < fabsf(RPMENU_SPEEDS[best] - speed))
			{
				best = i;
			}
		}
		return best;
	}
} // namespace

// === Семантика пунктов ======================================================================

void KZ::replaysystem::menu::ApplyReplayMenuInput(KZPlayer *player, ReplayMenuLine line, ReplayMenuInput input)
{
	if (!player)
	{
		return;
	}
	using namespace KZ::replaysystem;
	const bool select = input == ReplayMenuInput::Select;
	const int dir = input == ReplayMenuInput::Inc ? 1 : -1;

	switch (line)
	{
		case ReplayMenuLine::Pause:
			if (select)
			{
				commands::ToggleReplayPause(player);
			}
			break;
		case ReplayMenuLine::Step:
			// Шаг на тик без чата на каждый шаг (автоповтор на удержании); E — шаг вперёд.
			commands::StepReplay(player, select ? 1 : dir, false);
			break;
		case ReplayMenuLine::Seek:
			if (!select)
			{
				char seek[16];
				V_snprintf(seek, sizeof(seek), "%+d", dir * (int)RPMENU_SEEK_STEP_10);
				commands::JumpToReplayTime(player, seek);
			}
			break;
		case ReplayMenuLine::Restart:
			if (select)
			{
				// «С начала»: снять паузу (иначе «заново» не начнётся) и перемотать на 0.
				if (data::IsReplayPlaying())
				{
					data::GetCurrentReplay()->replayPaused = false;
				}
				commands::JumpToReplayTime(player, "0");
			}
			break;
		case ReplayMenuLine::Speed:
			if (select)
			{
				commands::SetReplaySpeed(player, 1.0f, false);
			}
			else
			{
				int idx = NearestSpeedIndex(commands::GetReplaySpeed()) + dir;
				idx = idx < 0 ? 0 : (idx >= RPMENU_SPEEDS_COUNT ? RPMENU_SPEEDS_COUNT - 1 : idx);
				commands::SetReplaySpeed(player, RPMENU_SPEEDS[idx], false);
			}
			break;
		case ReplayMenuLine::End:
			if (select)
			{
				commands::StopReplay(player);
			}
			break;
		default:
			break;
	}
}

std::string KZ::replaysystem::menu::GetReplayMenuLineText(KZPlayer *player, ReplayMenuLine line)
{
	using namespace KZ::replaysystem;
	const char *lang = player->languageService->GetLanguage();
	switch (line)
	{
		case ReplayMenuLine::Pause:
		{
			bool paused = data::IsReplayPlaying() && data::GetCurrentReplay()->replayPaused;
			return KZLanguageService::PrepareMessageWithLang(lang, paused ? "Replay Panel - Resume" : "Replay Panel - Pause");
		}
		case ReplayMenuLine::Step:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Step");
		case ReplayMenuLine::Seek:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Seek", (int)RPMENU_SEEK_STEP_10);
		case ReplayMenuLine::Restart:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Restart");
		case ReplayMenuLine::Speed:
		{
			char speedText[16];
			commands::FormatReplaySpeed(commands::GetReplaySpeed(), speedText, sizeof(speedText));
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Speed", speedText);
		}
		case ReplayMenuLine::End:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - End");
		default:
			return "";
	}
}

std::string KZ::replaysystem::menu::GetReplayMenuTitleText(KZPlayer *player)
{
	using namespace KZ::replaysystem;
	const char *lang = player->languageService->GetLanguage();
	const char *name = "";
	if (data::IsReplayPlaying() && data::GetCurrentReplay()->header.has_player())
	{
		name = data::GetCurrentReplay()->header.player().name().c_str();
	}
	return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Title", name);
}

std::string KZ::replaysystem::menu::GetReplayMenuStatusText(KZPlayer *player)
{
	using namespace KZ::replaysystem;
	const char *lang = player->languageService->GetLanguage();
	char speedText[16];
	commands::FormatReplaySpeed(commands::GetReplaySpeed(), speedText, sizeof(speedText));
	// Время — до десятых: строка живая (каждый тик), сотые мельтешат и не читаются.
	char time[32], end[32];
	utils::FormatTime(GetTime(), time, sizeof(time), false);
	utils::FormatTime(GetEndTime(), end, sizeof(end), false);
	return KZLanguageService::PrepareMessageWithLang(lang, GetPaused() ? "Replay Panel - Status Paused" : "Replay Panel - Status", speedText, time,
													 end);
}

std::string KZ::replaysystem::menu::GetReplayMenuHintText(KZPlayer *player, ReplayMenuLine line)
{
	const char *lang = player->languageService->GetLanguage();
	switch (line)
	{
		case ReplayMenuLine::Pause:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Hint Pause");
		case ReplayMenuLine::Step:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Hint Step");
		case ReplayMenuLine::Seek:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Hint Seek");
		case ReplayMenuLine::Restart:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Hint Restart");
		case ReplayMenuLine::Speed:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Hint Speed");
		case ReplayMenuLine::End:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Hint End");
		default:
			return "";
	}
}

// Выбор реплея из списка совпавших по нику (паттерн kz_spec_menu.cpp).
static_function void OnReplaySearchMenuSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!p)
	{
		return;
	}
	// info — UUID реплея строкой. Ревалидации «цель ещё существует» здесь нет и не нужно:
	// LoadReplay сам разбирает UUID, ищет файл локально и при промахе уходит в докачку
	// с центрального хранилища — то есть закрывает и случай «файл уехал, пока меню висело».
	const char *info = g_pMenus->GetItemInfo(menu, item);
	if (!info || !info[0])
	{
		return;
	}
	KZ::replaysystem::commands::LoadReplay(p, info);
}

bool KZ::replaysystem::menu::OpenReplaySearchMenu(KZPlayer *player, const std::vector<SearchHit> &hits)
{
	if (g_pMenus == nullptr || !player || hits.empty())
	{
		return false;
	}

	int slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return false;
	}

	// Один хэндл на слот — пересоздаём при повторном вызове (тот же паттерн, что в !spec).
	static MenuHandle s_searchMenu[MAXPLAYERS + 1] = {};
	if (s_searchMenu[slot] != kInvalidMenuHandle)
	{
		g_pMenus->DestroyMenu(s_searchMenu[slot]);
		s_searchMenu[slot] = kInvalidMenuHandle;
	}

	const char *lang = player->languageService->GetLanguage();
	std::string title = KZLanguageService::PrepareMessageWithLang(lang, "Replay Search Menu - Title");
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), &OnReplaySearchMenuSelect);
	if (m == kInvalidMenuHandle)
	{
		return false;
	}

	for (const SearchHit &hit : hits)
	{
		// Порядок пунктов — как пришёл от api (по PB по возрастанию, лучший первым);
		// своей сортировки здесь нет намеренно, чтобы список совпадал с сайтом.
		char timeText[32];
		utils::FormatTime((f64)hit.pbTimeMs / 1000.0, timeText, sizeof(timeText));
		std::string label = KZLanguageService::PrepareMessageWithLang(lang, "Replay Search Menu - Entry", hit.nickname.c_str(), timeText);
		g_pMenus->AddItem(m, label.c_str(), hit.replayUuid.c_str(), false);
	}

	// Одноразовый выбор — меню закрывается по клику (как !spec, в отличие от !rpmenu).
	g_pMenus->SetCloseOnSelect(m, true);

	s_searchMenu[slot] = m;
	g_pMenus->DisplayMenu(m, slot, 0);
	return true;
}

// Команды открытия больше нет: panorama-меню реплея само открыто, пока игрок наблюдает бота
// (hud/layout/rpmenu.cpp). Команда оставлена, чтобы привычный !rpmenu не отвечал молчанием.
SCMD(kz_rpmenu, SCFL_REPLAY | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	player->languageService->PrintChat(true, false, "Replay Panel - Auto");
	return MRES_SUPERCEDE;
}
