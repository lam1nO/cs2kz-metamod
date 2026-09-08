// Регистрация категории Misc в реестре настроек (kz/option/menu/model.h). Все восемь префов
// из брифа (mode/styles/pistol/beam/beamOffset/preferredLanguage/showTips/fov) читаются кодом,
// но ни в !options (kz_option_menu.cpp), ни в panorama-меню пункта для них не было — см.
// docs/design/2026-09-08-hud-options-diff.md §2 ("Misc целиком").
//
// Отклонения от буквального текста брифа (см. task-7-brief.md), обе — по прямому требованию
// родительской задачи "регистрируй правильным типом по смыслу, не тумблером ради простоты":
//  - beam НЕ тумблер: desiredBeamType — трёхзначный преф (None/Ground/Feet, BeamType),
//    регистрируем Choice, как и апстримный аналог desiredBeamType.
//  - beamOffset НЕ Size: это Vector-преф (x,y,z одним значением), регистрируем AddVector.
//
// safeguard: старый единый int-преф "safeguard" (Disabled/Nub/Pro) БОЛЬШЕ НЕ ПИШЕТСЯ ни одной
// командой — !sg/!pro разошлись на два независимых булевых префа sgReset/sgTeleport (см.
// комментарий над KZTimerService::GetSafeguardTeleport, kz_timer.cpp), "safeguard" читается
// только как фолбэк для игроков, ни разу не трогавших новые команды. Регистрировать "safeguard"
// как единый Choice — значит завести пункт, который в лучшем случае no-op (у игрока уже есть
// sgReset/sgTeleport) и в худшем вводит в заблуждение (не покрывает независимость двух
// защит). Регистрируем два реальных живых префа: sgReset и sgTeleport.
//
// preferredCompareType — пункта меню действительно нет нигде (проверено task-7-brief.md
// шаг 1), добавляем как Choice(5): None/SPB/GPB/SR/WR (см. KZTimerService::CompareType).
//
// Вызов регистрации (KZMiscMenu_Register) в общий Init-порядок пока никто не включает —
// потребитель дерева (KZMenuService) ещё не построен, см. тот же комментарий в
// jumpstats_prefs.cpp.
#include "kz/option/kz_option.h"
#include "kz/option/menu/model.h"
#include "kz/language/kz_language.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
#include "kz/pistol/kz_pistol.h"
#include "kz/beam/kz_beam.h"
#include "kz/tip/kz_tip.h"
#include "kz/fov/kz_fov.h"
#include "kz/timer/kz_timer.h"
#include "kz/checkpoint/kz_checkpoint.h"

#include "tier0/memdbgon.h"

// modeInfos определён без static в kz_mode_manager.cpp (внешняя линковка, в отличие от
// styleInfos там же — тот static_global, поэтому под styles заведён KZStyleManager::GetStyleInfos,
// см. kz_style.h/kz_style_manager.cpp). Раз символ уже внешний, отдельного геттера не заводим.
extern CUtlVector<KZModeManager::ModePluginInfo> modeInfos;

namespace
{
	// ---------------------------------------------------------------- Mode (Choice) ----
	const char *CurrentModeName(KZPlayer *player)
	{
		return player->optionService->GetPreferenceStr("preferredMode", KZOptionService::GetOptionStr("defaultMode", KZ_DEFAULT_MODE));
	}

	void ModeGetChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
	{
		const char *current = CurrentModeName(player);
		i64 id = 0;
		FOR_EACH_VEC(modeInfos, i)
		{
			if (modeInfos[i].id < 0)
			{
				continue; // числится в БД, но плагин не загружен — выбрать нельзя
			}
			out.push_back({std::string(modeInfos[i].longModeName.Get()), id, nullptr, !V_stricmp(modeInfos[i].longModeName, current)});
			id++;
		}
	}

	i64 ModeGetCurrent(KZPlayer *player, i64 tag)
	{
		const char *current = CurrentModeName(player);
		i64 id = 0;
		FOR_EACH_VEC(modeInfos, i)
		{
			if (modeInfos[i].id < 0)
			{
				continue;
			}
			if (!V_stricmp(modeInfos[i].longModeName, current))
			{
				return id;
			}
			id++;
		}
		return -1;
	}

	void ModeOnPick(KZPlayer *player, i64 tag, i64 id)
	{
		i64 idx = 0;
		FOR_EACH_VEC(modeInfos, i)
		{
			if (modeInfos[i].id < 0)
			{
				continue;
			}
			if (idx == id)
			{
				g_pKZModeManager->SwitchToMode(player, modeInfos[i].longModeName.Get());
				return;
			}
			idx++;
		}
	}

	// -------------------------------------------------------------- Styles (Choice) ----
	// Многовыборный список (KZChoice::selected на каждую строку) — стили складываются в стек,
	// а не выбираются по одному, поэтому getCurrent здесь не имеет смысла (getChoices сам
	// метит selected) и SetItemPref не зовём: экспортировать/импортировать одно значение
	// стека через тот же механизм, что и одиночный преф, некорректно.
	void StylesGetChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
	{
		i64 id = 0;
		const auto &infos = g_pKZStyleManager->GetStyleInfos();
		FOR_EACH_VEC(infos, i)
		{
			if (infos[i].id < 0)
			{
				continue;
			}
			bool active = false;
			FOR_EACH_VEC(player->styleServices, s)
			{
				if (!V_stricmp(player->styleServices[s]->GetStyleName(), infos[i].longName))
				{
					active = true;
					break;
				}
			}
			out.push_back({std::string(infos[i].longName), id, nullptr, active});
			id++;
		}
	}

	i64 StylesGetCurrent(KZPlayer *player, i64 tag)
	{
		return -1; // нет одного «текущего» значения — см. комментарий у StylesGetChoices
	}

	void StylesOnPick(KZPlayer *player, i64 tag, i64 id)
	{
		i64 idx = 0;
		const auto &infos = g_pKZStyleManager->GetStyleInfos();
		FOR_EACH_VEC(infos, i)
		{
			if (infos[i].id < 0)
			{
				continue;
			}
			if (idx == id)
			{
				g_pKZStyleManager->ToggleStyle(player, infos[i].longName);
				return;
			}
			idx++;
		}
	}

	// -------------------------------------------------------------- Pistol (Choice) ----
	void PistolGetChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
	{
		i16 current = KZPistolService::GetPistolIndexByName(player->optionService->GetPreferenceStr("preferredPistol", "weapon_usp_silencer"));
		for (i16 i = 0; i < (i16)KZPistolService::pistols.size(); i++)
		{
			out.push_back({std::string(KZPistolService::pistols[i].name), i, nullptr, i == current});
		}
	}

	i64 PistolGetCurrent(KZPlayer *player, i64 tag)
	{
		return KZPistolService::GetPistolIndexByName(player->optionService->GetPreferenceStr("preferredPistol", "weapon_usp_silencer"));
	}

	void PistolOnPick(KZPlayer *player, i64 tag, i64 id)
	{
		if (id < 0 || id >= (i64)KZPistolService::pistols.size())
		{
			return;
		}
		player->pistolService->preferredPistol = (i16)id;
		player->pistolService->UpdatePistol();
		player->optionService->SetPreferenceStr("preferredPistol", KZPistolService::pistols[id].className);
	}

	// ---------------------------------------------------------------- Beam (Choice) -----
	const char *kBeamTypeKeys[KZBeamService::BEAM_COUNT] = {"Misc - Beam Type None", "Misc - Beam Type Ground", "Misc - Beam Type Feet"};

	void BeamGetChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
	{
		u8 current = player->beamService->desiredBeamType;
		const char *lang = player->languageService->GetLanguage();
		for (u8 t = 0; t < KZBeamService::BEAM_COUNT; t++)
		{
			out.push_back({KZLanguageService::PrepareMessageWithLang(lang, kBeamTypeKeys[t]), t, nullptr, t == current});
		}
	}

	i64 BeamGetCurrent(KZPlayer *player, i64 tag)
	{
		return player->beamService->desiredBeamType;
	}

	void BeamOnPick(KZPlayer *player, i64 tag, i64 id)
	{
		player->beamService->SetBeamType((u8)id);
		player->optionService->SetPreferenceInt("desiredBeamType", player->beamService->desiredBeamType);
	}

	// beamOffset рисуется из кэша KZBeamService::playerBeamOffset (kz_beam.h), который иначе
	// обновляется только в OnPlayerPreferencesLoaded (kz_beam.cpp:22-23) — на коннекте. Generic
	// Vector-пункт пишет прямо в преф через prefKey/storage, кэш не трогая: без досинка правка
	// в меню осела бы в БД, но луч не сдвинулся бы до реконнекта — тот же класс бага, что решён
	// в Task 8 для HidePlayers/HideWeapon/TimerStopSound через колбэк на запись. У Vector нет
	// AddActionToggle-аналога, но есть onEdit (открытие/закрытие попапа) — используем его как тот
	// же по смыслу колбэк: на закрытии (begin=false) перечитываем свежезаписанный преф в кэш.
	void BeamOffsetOnEdit(KZPlayer *player, i64 tag, bool begin)
	{
		if (begin)
		{
			return;
		}
		player->beamService->playerBeamOffset = player->optionService->GetPreferenceVector("beamOffset", KZBeamService::defaultOffset);
	}

	// -------------------------------------------------------------- Language (Choice) ---
	// Список курируем, как MENU_FONTS в hud/layout/menu.cpp курирует шрифты: реальных
	// переводов у форка не 32 (весь translations/config.txt — таблица автоопределения по
	// cl_language, не список готовых переводов), а по факту около 13 — тот же набор языков,
	// что несёт большинство файлов translations/*.phrases.txt (см., например, фразу "Offset"
	// в cs2kz-jumpstats.phrases.txt). Имена — автонимы, поэтому НЕ через PrepareMessageWithLang
	// (имя языка не должно переводиться на текущий язык интерфейса).
	struct LangChoiceDef
	{
		const char *code; // ключ preferredLanguage / suffix в .phrases.txt
		const char *autonym;
	};

	const LangChoiceDef kLanguages[] = {
		{"en", "English"}, {"ru", "Русский"}, {"de", "Deutsch"},    {"es", "Español"}, {"it", "Italiano"}, {"pl", "Polski"},   {"tr", "Türkçe"},
		{"ko", "한국어"},  {"chi", "中文"},   {"ua", "Українська"}, {"sv", "Svenska"}, {"fi", "Suomi"},    {"lv", "Latviešu"},
	};

	void LanguageGetChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
	{
		const char *current = player->languageService->GetLanguage();
		for (i64 i = 0; i < (i64)KZ_ARRAYSIZE(kLanguages); i++)
		{
			out.push_back({std::string(kLanguages[i].autonym), i, nullptr, !V_stricmp(kLanguages[i].code, current)});
		}
	}

	i64 LanguageGetCurrent(KZPlayer *player, i64 tag)
	{
		const char *current = player->languageService->GetLanguage();
		for (i64 i = 0; i < (i64)KZ_ARRAYSIZE(kLanguages); i++)
		{
			if (!V_stricmp(kLanguages[i].code, current))
			{
				return i;
			}
		}
		return -1;
	}

	// Повторяет SCMD(kz_language) (kz_language.cpp) — там же взят порядок вызовов.
	void LanguageOnPick(KZPlayer *player, i64 tag, i64 id)
	{
		if (id < 0 || id >= (i64)KZ_ARRAYSIZE(kLanguages))
		{
			return;
		}
		const char *language = kLanguages[id].code;
		bool shouldReconnect = !(player->checkpointService->GetCheckpointCount() || player->timerService->GetTimerRunning());
		KZLanguageService::UpdateLanguage(player->GetSteamId64(false), language, KZLanguageService::LanguageInfo::CacheLevel::CACHE_OVERRIDE, true);
		player->optionService->SetPreferenceStr("preferredLanguage", language);
		if (!shouldReconnect)
		{
			player->languageService->PrintChat(false, false, "Language Change - Manual Menu Change Required");
		}
	}

	// ------------------------------------------------------------ Show tips (toggle) ----
	// showTips персистентен (правка ревью задачи 7: до неё Reset() жёстко ставил true, теперь
	// читает/пишет optionService, см. kz_tip.cpp) — колбэки, а не голый AddToggle, потому что
	// запись обязана попасть и в кэш-член KZTipService, и в преф разом (ToggleTips() делает оба).
	i64 ShowTipsGetCurrent(KZPlayer *player, i64 tag)
	{
		return player->tipService->GetShowTips() ? 1 : 0;
	}

	void ShowTipsOnActivate(KZPlayer *player, i64 tag)
	{
		player->tipService->ToggleTips();
	}

	// --------------------------------------------------------- Safeguard (toggles) ------
	i64 SgResetGetCurrent(KZPlayer *player, i64 tag)
	{
		return player->timerService->GetSafeguardReset() ? 1 : 0;
	}

	void SgResetOnActivate(KZPlayer *player, i64 tag)
	{
		player->timerService->ToggleSafeguard();
	}

	i64 SgTeleportGetCurrent(KZPlayer *player, i64 tag)
	{
		return player->timerService->GetSafeguardTeleport() ? 1 : 0;
	}

	void SgTeleportOnActivate(KZPlayer *player, i64 tag)
	{
		player->timerService->ToggleProSafeguard();
	}

	// -------------------------------------------------------- Compare type (Choice) -----
	// Акронимы (None/SPB/GPB/SR/WR) — те же, что "| SPB {diff_time}" и т.п. во всех языках
	// translations/cs2kz-timer.phrases.txt: не переводятся, литералы без PrepareMessageWithLang.
	const char *kCompareTypeNames[KZTimerService::CompareType::COMPARETYPE_COUNT] = {"None", "SPB", "GPB", "SR", "WR"};

	void CompareTypeGetChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
	{
		i64 current = player->optionService->GetPreferenceInt("preferredCompareType", KZTimerService::CompareType::COMPARE_GPB);
		for (i64 t = 0; t < (i64)KZTimerService::CompareType::COMPARETYPE_COUNT; t++)
		{
			out.push_back({std::string(kCompareTypeNames[t]), t, nullptr, t == current});
		}
	}

	i64 CompareTypeGetCurrent(KZPlayer *player, i64 tag)
	{
		return player->optionService->GetPreferenceInt("preferredCompareType", KZTimerService::CompareType::COMPARE_GPB);
	}

	void CompareTypeOnPick(KZPlayer *player, i64 tag, i64 id)
	{
		static const char *kTypeStrings[KZTimerService::CompareType::COMPARETYPE_COUNT] = {"none", "spb", "gpb", "sr", "wr"};
		if (id < 0 || id >= (i64)KZTimerService::CompareType::COMPARETYPE_COUNT)
		{
			return;
		}
		player->timerService->SetCompareTarget(kTypeStrings[id]);
	}
} // namespace

// Регистрирует категорию Misc. Пока не вызывается ниоткуда — см. комментарий в шапке файла.
void KZMiscMenu_Register()
{
	KZOptNode *cat = KZ::menu::AddCategory("Options - Menu Cat Misc");

	KZ::menu::AddChoice(cat, "Options - Menu Label Mode", &ModeGetChoices, &ModeGetCurrent, &ModeOnPick);
	KZ::menu::SetItemPref(cat, "preferredMode", KZOptStorage::Str, 0, KZ_DEFAULT_MODE);

	KZ::menu::AddChoice(cat, "Options - Menu Label Styles", &StylesGetChoices, &StylesGetCurrent, &StylesOnPick);

	KZ::menu::AddChoice(cat, "Options - Menu Label Pistol", &PistolGetChoices, &PistolGetCurrent, &PistolOnPick);
	KZ::menu::SetItemPref(cat, "preferredPistol", KZOptStorage::Str, 0, "weapon_usp_silencer");

	KZ::menu::AddChoice(cat, "Options - Menu Label Beam", &BeamGetChoices, &BeamGetCurrent, &BeamOnPick);
	KZ::menu::SetItemPref(cat, "desiredBeamType", KZOptStorage::Int, KZBeamService::BEAM_NONE);

	// Границы — наши (код не проверяет offset вообще, см. kz_beamoffset); диапазон с запасом
	// вокруг дефолта (0, 0, 1.75). onEdit — досинк кэша луча на закрытии попапа, см. выше.
	KZ::menu::AddVector(cat, "Options - Menu Label BeamOffset", "beamOffset", KZBeamService::defaultOffset, -64, 64, 0, &BeamOffsetOnEdit);

	KZ::menu::AddChoice(cat, "Options - Menu Label Language", &LanguageGetChoices, &LanguageGetCurrent, &LanguageOnPick);
	KZ::menu::SetItemPref(cat, "preferredLanguage", KZOptStorage::Str);

	KZ::menu::AddActionToggle(cat, "Options - Menu Label ShowTips", &ShowTipsGetCurrent, &ShowTipsOnActivate);
	KZ::menu::SetItemPref(cat, "showTips", KZOptStorage::Bool, 1);

	KZ::menu::AddSize(cat, "Options - Menu Label FOV", "fov", (i32)KZFOVService::GetDefaultFOV(), (i32)KZFOVService::GetMinFOV(),
					  (i32)KZFOVService::GetMaxFOV());
	KZ::menu::SetItemUnit(cat, "");

	// safeguard: два независимых префа, не единый Choice — см. комментарий в шапке файла.
	KZ::menu::AddActionToggle(cat, "Options - Menu Label SafeguardReset", &SgResetGetCurrent, &SgResetOnActivate);
	KZ::menu::SetItemPref(cat, "sgReset", KZOptStorage::Int, 0);
	KZ::menu::AddActionToggle(cat, "Options - Menu Label SafeguardTeleport", &SgTeleportGetCurrent, &SgTeleportOnActivate);
	KZ::menu::SetItemPref(cat, "sgTeleport", KZOptStorage::Int, 0);

	KZ::menu::AddChoice(cat, "Options - Menu Label CompareType", &CompareTypeGetChoices, &CompareTypeGetCurrent, &CompareTypeOnPick);
	KZ::menu::SetItemPref(cat, "preferredCompareType", KZOptStorage::Int, KZTimerService::CompareType::COMPARE_GPB);
}
