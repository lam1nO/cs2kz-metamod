// Регистрация категории Jumpstats в реестре настроек (kz/option/menu/model.h). Дефолты и
// сами префы уже существуют и читаются игровым кодом (см. src/kz/jumpstats/prefs.cpp) — этот
// файл только выставляет их пунктами меню поверх тех же ключей, включая два префа, которых
// не было в старом кнопочном !js-меню (jsFailstatsConsole, jsBroadcastMinTierConsole — см.
// docs/design/2026-09-08-hud-options-diff.md §2/§4: 10 пунктов при 12 существующих префах).
//
// !js (kz_jumpstats.h::CreateJumpstatsMenu, cs2menus) не трогаем — он остаётся рабочим до
// задачи переноса на реестр (Task 10 транша), поэтому фразы тиров ниже дублируют
// s_tierNameKeys из prefs.cpp (static-таблица того файла, внутренняя, трогать незачем).
//
// Вызов регистрации (KZJumpstatsMenu_Register) в общий Init-порядок пока никто не включает —
// потребитель дерева (KZMenuService, см. комментарий в model.h) ещё не построен, это отдельная
// параллельная задача. Как будет готов — один вызов из cs2kz.cpp рядом с другими ::Init().
#include "kz/jumpstats/kz_jumpstats.h"
#include "kz/option/kz_option.h"
#include "kz/option/menu/model.h"
#include "kz/language/kz_language.h"

#include "tier0/memdbgon.h"

namespace
{
	// Порядок — как в DistanceTier (kz_jumpstats.h). Локализованных тиров нет вообще (см.
	// "Jumpstats Option - Tier Hint") — ru-фраза хранит то же английское слово, форк-конвенция.
	const char *kTierNameKeys[DISTANCETIER_COUNT] = {
		"Jumpstats - Tier Name None",    "Jumpstats - Tier Name Meh",    "Jumpstats - Tier Name Impressive", "Jumpstats - Tier Name Perfect",
		"Jumpstats - Tier Name Godlike", "Jumpstats - Tier Name Ownage", "Jumpstats - Tier Name Wrecker",
	};

	// Какой из шести тир-префов стоит за пунктом Choice — приходит через tag, чтобы одна
	// тройка колбэков обслуживала все шесть (иначе пришлось бы городить 6 пар функций).
	enum class JSTierChoice : i64
	{
		MinTier,
		MinTierConsole,
		SoundMinTier,
		BroadcastMinTier,
		BroadcastMinTierConsole,
		BroadcastSoundMinTier,
	};

	struct JSTierChoiceDef
	{
		const char *prefKey;
		i32 def;
	};

	// НАШИ дефолты трансляции — Godlike(4). Апстримный Ownage(5) НЕ переносим: значения уже
	// лежат в БД игроков (см. docs/design/2026-09-08-hud-options-diff.md §4, task-6-brief.md).
	const JSTierChoiceDef kTierChoiceDefs[] = {
		{"jsMinTier", DistanceTier_Impressive},
		{"jsMinTierConsole", DistanceTier_Impressive},
		{"jsSoundMinTier", DistanceTier_Impressive},
		{"jsBroadcastMinTier", DistanceTier_Godlike},
		{"jsBroadcastMinTierConsole", DistanceTier_Godlike},
		{"jsBroadcastSoundMinTier", DistanceTier_Godlike},
	};

	void JSTierGetChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
	{
		const JSTierChoiceDef &def = kTierChoiceDefs[(i32)tag];
		i64 current = player->optionService->GetPreferenceInt(def.prefKey, def.def);
		const char *lang = player->languageService->GetLanguage();
		for (i32 t = 0; t < DISTANCETIER_COUNT; t++)
		{
			out.push_back({KZLanguageService::PrepareMessageWithLang(lang, kTierNameKeys[t]), t, nullptr, t == current});
		}
	}

	i64 JSTierGetCurrent(KZPlayer *player, i64 tag)
	{
		const JSTierChoiceDef &def = kTierChoiceDefs[(i32)tag];
		return player->optionService->GetPreferenceInt(def.prefKey, def.def);
	}

	void JSTierOnPick(KZPlayer *player, i64 tag, i64 id)
	{
		const JSTierChoiceDef &def = kTierChoiceDefs[(i32)tag];
		player->optionService->SetPreferenceInt(def.prefKey, id);
	}
} // namespace

// Регистрирует категорию Jumpstats. Пока не вызывается ниоткуда — см. комментарий в шапке.
void KZJumpstatsMenu_Register()
{
	KZOptNode *cat = KZ::menu::AddCategory("Options - Menu Cat Jumpstats");

	KZ::menu::AddToggle(cat, "Jumpstats - Menu Label Reporting", "jsReporting", true);
	KZ::menu::AddToggle(cat, "Jumpstats - Menu Label Always", "jsAlways", false);

	KZ::menu::AddChoice(cat, "Jumpstats - Menu Label MinTier", &JSTierGetChoices, &JSTierGetCurrent, &JSTierOnPick, (i64)JSTierChoice::MinTier);
	KZ::menu::SetItemPref(cat, "jsMinTier", KZOptStorage::Int, DistanceTier_Impressive);

	KZ::menu::AddChoice(cat, "Jumpstats - Menu Label MinTierConsole", &JSTierGetChoices, &JSTierGetCurrent, &JSTierOnPick,
						(i64)JSTierChoice::MinTierConsole);
	KZ::menu::SetItemPref(cat, "jsMinTierConsole", KZOptStorage::Int, DistanceTier_Impressive);

	KZ::menu::AddChoice(cat, "Jumpstats - Menu Label SoundMinTier", &JSTierGetChoices, &JSTierGetCurrent, &JSTierOnPick,
						(i64)JSTierChoice::SoundMinTier);
	KZ::menu::SetItemPref(cat, "jsSoundMinTier", KZOptStorage::Int, DistanceTier_Impressive);

	// Хранится как float 0.0-2.0 (см. kz_jsvolume, клэмп 0..2), в меню — проценты 0-200.
	KZ::menu::AddSize(cat, "Jumpstats - Menu Label Volume", "jsVolume", 75, 0, 200);
	KZ::menu::SetItemUnit(cat, "%");
	KZ::menu::SetItemScale(cat, 100);

	KZ::menu::AddChoice(cat, "Jumpstats - Menu Label BroadcastMinTier", &JSTierGetChoices, &JSTierGetCurrent, &JSTierOnPick,
						(i64)JSTierChoice::BroadcastMinTier);
	KZ::menu::SetItemPref(cat, "jsBroadcastMinTier", KZOptStorage::Int, DistanceTier_Godlike);

	// Не было в старом !js-меню (s_jsMenuItems, prefs.cpp) — см. шапку файла.
	KZ::menu::AddChoice(cat, "Jumpstats - Menu Label BroadcastMinTierConsole", &JSTierGetChoices, &JSTierGetCurrent, &JSTierOnPick,
						(i64)JSTierChoice::BroadcastMinTierConsole);
	KZ::menu::SetItemPref(cat, "jsBroadcastMinTierConsole", KZOptStorage::Int, DistanceTier_Godlike);

	KZ::menu::AddChoice(cat, "Jumpstats - Menu Label BroadcastSoundTier", &JSTierGetChoices, &JSTierGetCurrent, &JSTierOnPick,
						(i64)JSTierChoice::BroadcastSoundMinTier);
	KZ::menu::SetItemPref(cat, "jsBroadcastSoundMinTier", KZOptStorage::Int, DistanceTier_Godlike);

	KZ::menu::AddToggle(cat, "Jumpstats - Menu Label Failstats", "jsFailstats", true);
	// Не было в старом !js-меню — см. шапку файла.
	KZ::menu::AddToggle(cat, "Jumpstats - Menu Label FailstatsConsole", "jsFailstatsConsole", true);
	KZ::menu::AddToggle(cat, "Jumpstats - Menu Label ExtendedChatStats", "jsExtendedChatStats", false);
}
