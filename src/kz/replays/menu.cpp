// Меню реплея: словарь пунктов, их семантика и данные карточки нашей panorama-страницы
// (hud/layout/rpmenu.cpp) + cs2menus-меню выбора реплея по нику (OpenReplaySearchMenu).
// Старое cs2menus-!rpmenu удалено 09.09; меню открывается само при просмотре реплей-бота, а
// команда !rpmenu прячет/возвращает его.
#include "kz/kz.h"
#include "kz/language/kz_language.h"
#include "kz/hud/kz_hud.h" // !rpmenu — тумблер скрытия меню реплея
#include "menu.h"
#include "commands.h"
#include "kz_replaysystem.h" // GetPaused — строка состояния карточки
#include "data.h"
#include "cyb_replay_download.h" // Kind — вид запроса, которым запущен плейбек (бейдж шапки)
#include "playback.h" // эффективная шкала тиков (без пауз) для строки состояния
#include "utils/utils.h"
#include "utils/simplecmds.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include <cmath>

#include "tier0/memdbgon.h"

// Меню-движок cs2menus (определён в cs2kz.cpp); может быть nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

namespace
{
	// Шаги перемотки (сек) регулируемых строк: A — назад, D — вперёд. Их ДВА, отдельными
	// пунктами меню, а не один: клавиатура даёт строке ровно одну пару A/D, и второй шаг иначе
	// был бы доступен только мышью, которой у меню нет. Точный — подвести к месту, быстрый —
	// перескочить кусок. ±30 сек остаётся командой !rpgoto +30.
	constexpr float RPMENU_SEEK_STEP_FINE = 3.0f;
	constexpr float RPMENU_SEEK_STEP_FAST = 15.0f;

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
			// Шаг на тик без чата на каждый шаг (автоповтор на удержании); E не назначен
			// (решение пользователя 10.09 — короче легенда).
			if (!select)
			{
				commands::StepReplay(player, dir, false);
			}
			break;
		case ReplayMenuLine::Seek:
		case ReplayMenuLine::SeekFast:
			if (!select)
			{
				const int step = (int)(line == ReplayMenuLine::SeekFast ? RPMENU_SEEK_STEP_FAST : RPMENU_SEEK_STEP_FINE);
				char seek[16];
				V_snprintf(seek, sizeof(seek), "%+d", dir * step);
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
			// Только A/D по пресетам; сброс на 1x по E снят (решение пользователя 10.09) — 1x и так
			// пресет в списке.
			if (!select)
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
		// Значений в подписях НЕТ: шаг и скорость написаны на чипах справа от строки, и дубль
		// («ПЕРЕМОТКА 10С» рядом с чипом «−10s») только удлинял подпись (спека §4, макеты).
		case ReplayMenuLine::Seek:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Seek");
		case ReplayMenuLine::SeekFast:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Seek Fast");
		case ReplayMenuLine::Restart:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Restart");
		case ReplayMenuLine::Speed:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Speed");
		case ReplayMenuLine::End:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - End");
		default:
			return "";
	}
}

std::string KZ::replaysystem::menu::GetReplayMenuAuthorName()
{
	using namespace KZ::replaysystem;
	// Голый ник, без обрамления «РЕПЛЕЙ · …»: это слот `nick` шапки карточки, и что перед нами
	// плеер реплея, теперь говорит сама карточка. Обрезку длинного ника делает vcss (ellipsis).
	if (data::IsReplayPlaying() && data::GetCurrentReplay()->header.has_player())
	{
		return data::GetCurrentReplay()->header.player().name();
	}
	return "";
}

std::string KZ::replaysystem::menu::GetReplayMenuMetaText()
{
	using namespace KZ::replaysystem;
	if (!data::IsReplayPlaying())
	{
		return "";
	}
	const auto &header = data::GetCurrentReplay()->header;
	std::string meta;
	const auto append = [&meta](const std::string &part)
	{
		if (part.empty())
		{
			return;
		}
		if (!meta.empty())
		{
			meta += " \xC2\xB7 "; // «·» U+00B7 в UTF-8 — разделитель из спеки
		}
		meta += part;
	};
	if (header.has_map())
	{
		append(header.map().name());
	}
	// Курс и режим есть только у реплея рана: у джамп-реплея свой набор полей (jump()), и
	// смешивать их в одну строку нечем — спека для него описывает другой подстрочник.
	if (header.has_run())
	{
		append(header.run().course_name());
		append(header.run().mode().short_name());
	}
	return meta;
}

// Позиция и длительность (ReplayMenuStatus.position/total) — В ОДНОЙ шкале:
//  - реплей рана (header.run() с временем): таймер рана бота (KZ::replaysystem::GetTime — тот же
//    источник, что у худа: без предстартовых секунд записи и без записанных пауз; позиция плейбека
//    по тикам расходилась с худом — замечание пользователя 09.09) / итоговое время из заголовка.
//    После финиша/стопа startTime обнулён и GetTime() = 0, а худ показывает итоговое время — берём
//    replay->endTime (он живёт до перемотки назад, где ResetReplayState его обнуляет), а не
//    GetEndTime(): тот гасится через 3 с после стоп-тика.
//  - иначе (джамп-реплей, у него TIMER_START нет и GetTime() всегда 0): эффективная позиция и
//    длительность записи без пауз (playback.h).
bool KZ::replaysystem::menu::IsReplayMenuRunReplay()
{
	using namespace KZ::replaysystem;
	const auto *replay = data::GetCurrentReplay();
	return data::IsReplayPlaying() && replay->header.has_run() && replay->header.run().time() > 0.0f;
}

static_function f64 ReplayMenuTotalTime()
{
	using namespace KZ::replaysystem;
	if (!data::IsReplayPlaying())
	{
		return 0.0;
	}
	if (KZ::replaysystem::menu::IsReplayMenuRunReplay())
	{
		const auto *replay = data::GetCurrentReplay();
		// AWR: знаменатель — время БЕЗ вырезанных петель, иначе числитель (позиция в
		// эффективной шкале, где петли пропущены) никогда не дошёл бы до знаменателя.
		if (replay->awrMode)
		{
			return (f64)replay->awrMs / 1000.0;
		}
		return (f64)replay->header.run().time();
	}
	const u32 effectiveCount = playback::EffectiveTickCount();
	return effectiveCount > 0 ? (f64)(effectiveCount - 1) * ENGINE_FIXED_TICK_INTERVAL : 0.0;
}

static_function f64 ReplayMenuPositionTime()
{
	using namespace KZ::replaysystem;
	if (!data::IsReplayPlaying())
	{
		return 0.0;
	}
	const auto *replay = data::GetCurrentReplay();
	if (KZ::replaysystem::menu::IsReplayMenuRunReplay())
	{
		return replay->endTime > 0.0f ? (f64)replay->endTime : (f64)GetTime();
	}
	return (f64)playback::RawTickToEffective(replay->currentTick) * ENGINE_FIXED_TICK_INTERVAL;
}

void KZ::replaysystem::menu::GetReplayMenuStatus(ReplayMenuStatus &out)
{
	using namespace KZ::replaysystem;
	out.position = ReplayMenuPositionTime();
	out.total = ReplayMenuTotalTime();
	// «Пауза» — и записанная пауза рана (GetPaused), и пауза плейбека зрителем (replayPaused):
	// в обоих случаях время стоит.
	out.paused = data::IsReplayPlaying() && (GetPaused() || data::GetCurrentReplay()->replayPaused);
	// Карточка — ФИКСИРОВАННОЙ ширины формат «N.NN», а не чатовый FormatReplaySpeed («1», «0.25»):
	// поле скорости и пилюля состояния переписываются на ходу, и плавающее число знаков
	// дёргало бы соседние элементы строки. В чате формат прежний.
	V_snprintf(out.speed, sizeof(out.speed), "%.2f", commands::GetReplaySpeed());
}

int KZ::replaysystem::menu::GetReplayMenuSeekStepSeconds(bool fast)
{
	return (int)(fast ? RPMENU_SEEK_STEP_FAST : RPMENU_SEEK_STEP_FINE);
}

bool KZ::replaysystem::menu::IsReplayMenuAwr()
{
	using namespace KZ::replaysystem;
	return data::IsReplayPlaying() && data::GetCurrentReplay()->awrMode;
}

void KZ::replaysystem::menu::GetReplayMenuBadge(ReplayMenuBadge &out)
{
	using namespace KZ::replaysystem;
	out.text.clear();
	out.cls = "other";
	if (!data::IsReplayPlaying())
	{
		return;
	}
	const auto *replay = data::GetCurrentReplay();
	// AWR проверяем ПЕРВЫМ и по самому плейбеку, а не по виду запроса: в AWR-режим реплей
	// уходит только через резолв awr, но режим мог и отвалиться (разрез не удался) — тогда
	// играет обычная запись, и обещать «AWR» нельзя.
	if (replay->awrMode)
	{
		out.text = "AWR";
		out.cls = "wr"; // золотой, как метка AWR в чате
		return;
	}
	// Метки не бывает у записи, запущенной мимо резолва: типа реплея в файле нет
	// (data-availability.md §3). Бейдж в этом случае скрыт — ширина шапки от него не зависит.
	if (replay->badgeKind < 0)
	{
		return;
	}
	switch ((CybReplayDownload::Kind)replay->badgeKind)
	{
		case CybReplayDownload::Kind::PB:
			out.text = "PB";
			out.cls = "pb";
			break;
		case CybReplayDownload::Kind::WR:
			out.text = "WR";
			out.cls = "wr";
			break;
		// Приоритет при совпадении «рекорд + pro» — за рекордом (решение в data-availability.md
		// §3): цвет берём рекордный, а PRO дописываем текстом.
		case CybReplayDownload::Kind::PBPro:
			out.text = "PB PRO";
			out.cls = "pb";
			break;
		case CybReplayDownload::Kind::WRPro:
			out.text = "WR PRO";
			out.cls = "wr";
			break;
		default:
			break;
	}
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
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Hint Seek", (int)RPMENU_SEEK_STEP_FINE);
		case ReplayMenuLine::SeekFast:
			return KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - Hint Seek", (int)RPMENU_SEEK_STEP_FAST);
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

// panorama-меню реплея само открыто, пока игрок наблюдает бота (hud/layout/rpmenu.cpp);
// !rpmenu прячет его и возвращает обратно (по умолчанию показано) — за реплеем меню иногда
// закрывает обзор. Само закрытие/открытие делает следующий тик UpdateReplayMenu.
SCMD(kz_rpmenu, SCFL_REPLAY | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	const bool hidden = player->hudService->ToggleReplayMenuHidden();
	player->languageService->PrintChat(true, false, hidden ? "Replay Panel - Hidden" : "Replay Panel - Shown");
	return MRES_SUPERCEDE;
}
