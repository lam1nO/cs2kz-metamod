// Меню !rpmenu — управление воспроизведением реплея (cs2menus, паттерн kz_option_menu).
#include "kz/kz.h"
#include "kz/language/kz_language.h"
#include "menu.h"
#include "commands.h"
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
	// Пауза — первый пункт меню: подпись живая, пересобирается в OnRpMenuSelect.
	constexpr int RPMENU_ITEM_PAUSE = 0;

	// Шаг перемотки (сек) для регулируемой строки; знак delta задаёт направление
	// (A/AdjustDec = назад, D/AdjustInc = вперёд).
	// Вторая строка перемотки (30 сек) убрана, когда появилась строка скорости: бюджет
	// видимых строк меню — 5 (см. CYBER.md, «Бюджет !rpmenu»), шестая уводит меню в
	// пагинацию и прячет «Завершить» за «далее». ±30 сек остаётся тройным ► по этой
	// строке и командой !rpgoto +30.
	constexpr float RPMENU_SEEK_STEP_10 = 10.0f;

	// Строка скорости: индекс фиксирован, подпись живая (перерисовывается после A/D).
	constexpr int RPMENU_ITEM_SPEED = 3;

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

	std::string SpeedItemText(KZPlayer *player)
	{
		using namespace KZ::replaysystem;
		char speedText[16];
		commands::FormatReplaySpeed(commands::GetReplaySpeed(), speedText, sizeof(speedText));
		const char *lang = player->languageService->GetLanguage();
		return KZLanguageService::PrepareMessageWithLang(lang, "Replay Menu - Speed", speedText);
	}

	// Строка паузы делает два дела: E — пауза/продолжить, A/D — шаг на один тик записи.
	// Отдельной строки под шаг нет намеренно — бюджет меню 5 строк (CYBER.md), а жест
	// «встал на стопкадр и листаю» естественно живёт на той же строке, что и стопкадр.
	std::string PauseItemText(KZPlayer *player)
	{
		using namespace KZ::replaysystem;
		bool paused = data::IsReplayPlaying() && data::GetCurrentReplay()->replayPaused;
		const char *lang = player->languageService->GetLanguage();
		return KZLanguageService::PrepareMessageWithLang(lang, paused ? "Replay Menu - Resume Step" : "Replay Menu - Pause Step");
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

	using namespace KZ::replaysystem;

	// Регулируемых строк несколько (перемотка, скорость, шаг) — что именно крутят,
	// говорит info строки, а не её индекс.
	const char *key = g_pMenus->GetItemInfo(menu, item);
	if (key && KZ_STREQ(key, "speed"))
	{
		int idx = NearestSpeedIndex(commands::GetReplaySpeed());
		idx += (delta > 0.0f) ? 1 : -1;
		idx = idx < 0 ? 0 : (idx >= RPMENU_SPEEDS_COUNT ? RPMENU_SPEEDS_COUNT - 1 : idx);
		// announce=false: значение видно в самой строке, дублировать его в чат незачем.
		commands::SetReplaySpeed(p, RPMENU_SPEEDS[idx], false);
		g_pMenus->SetItemText(menu, RPMENU_ITEM_SPEED, SpeedItemText(p).c_str());
		return;
	}
	if (key && KZ_STREQ(key, "pause"))
	{
		// A/D по строке паузы — шаг на тик. announce=false: строку в чат на каждый шаг
		// печатать нельзя, cs2menus повторяет adjust на удержании клавиши.
		commands::StepReplay(p, delta > 0.0f ? 1 : -1, false);
		// Шаг сам ставит реплей на паузу — подпись строки обязана это отразить.
		g_pMenus->SetItemText(menu, RPMENU_ITEM_PAUSE, PauseItemText(p).c_str());
		return;
	}

	char seek[16];
	V_snprintf(seek, sizeof(seek), "%+d", (int)delta);
	commands::JumpToReplayTime(p, seek);
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
	// Регулируемые строки (info "seek" и "speed") реагируют на A/D в OnRpMenuAdjust;
	// выбор E по ним ничего не делает — сюда попадём, но действий нет.

	// Живые подписи — по фактическому состоянию: и пауза, и скорость могли смениться
	// мимо меню (!rppause, !rpspeed, рестарт), а обновить их можно только отсюда и из
	// OnRpMenuAdjust — своего тика у меню нет.
	g_pMenus->SetItemText(menu, RPMENU_ITEM_PAUSE, PauseItemText(p).c_str());
	g_pMenus->SetItemText(menu, RPMENU_ITEM_SPEED, SpeedItemText(p).c_str());
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
	// Строка регулируемая: E (выбор) — пауза/продолжить, A/D — шаг на тик записи.
	g_pMenus->AddAdjustableItem(m, PauseItemText(player).c_str(), "pause", 1.0f, -1.0f, 1.0f);
	std::string restart = KZLanguageService::PrepareMessageWithLang(lang, "Replay Menu - Restart");
	g_pMenus->AddItem(m, restart.c_str(), "restart", false);

	// Перемотка — одной регулируемой строкой: A (◄) — назад, D (►) — вперёд.
	// Стрелки рисует движок; текст показывает фиксированный шаг.
	std::string seek10 = SeekItemText(player, (int)RPMENU_SEEK_STEP_10);
	g_pMenus->AddAdjustableItem(m, seek10.c_str(), "seek", RPMENU_SEEK_STEP_10, -RPMENU_SEEK_STEP_10, RPMENU_SEEK_STEP_10);

	// Скорость воспроизведения: A (◄) — медленнее, D (►) — быстрее, по пресетам.
	// Индекс строки обязан совпадать с RPMENU_ITEM_SPEED — по нему обновляется подпись.
	std::string speed = SpeedItemText(player);
	g_pMenus->AddAdjustableItem(m, speed.c_str(), "speed", 1.0f, -1.0f, 1.0f);

	g_pMenus->SetAdjustCallback(m, &OnRpMenuAdjust);

	// «Завершить» — останавливает реплей и закрывает меню (обрабатывается в OnRpMenuSelect).
	std::string endLabel = KZLanguageService::PrepareMessageWithLang(lang, "Replay Menu - End");
	g_pMenus->AddItem(m, endLabel.c_str(), "end", false);

	// Не закрываем при выборе — меню держится, пока игрок сам не закроет (0/ESC).
	g_pMenus->SetCloseOnSelect(m, false);

	s_rpMenu[slot] = m;
	g_pMenus->DisplayMenu(m, slot, 0);
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

	// Один хэндл на слот — пересоздаём при повторном вызове (паттерн kz_option_menu/!spec).
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
