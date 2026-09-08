// Регистрация меню настроек panorama-худа и крестика в общем реестре (kz/option/menu/model.h,
// Task 1). Раньше (Task 11) состав меню жил статическими таблицами прямо в layout/menu.cpp —
// задача 2 переносит РОВНО тот же состав (категории, пункты, ключи префов, диапазоны,
// дефолты — ничего не меняя) сюда, а рендер (layout/menu.cpp) обходит KZ::menu::GetTree().
#include "kz/hud/kz_hud.h"
#include "kz/hud/layout/layout.h"
#include "kz/option/menu/model.h"
#include "kz/language/kz_language.h"

#include "tier0/memdbgon.h"

// === Тип худа: Standard / Panorama / Off ====================================================
// Не булев тумблер (три состояния) и не сырой преф — hudType читается/пишется через
// GetHudType()/SetHudType() (kz_hud.cpp), которые сами тащат миграцию легаси-значений
// (mhudMaster, удалённый particle-тип 1). Моделируем как Choice: список из трёх пунктов в
// ТОМ ЖЕ порядке, что и старый цикл NextHudType; сам цикл убран — с задачи 3 клик открывает
// попап списка (layout/menu.cpp), как у любого другого Choice, а не перебирает по кругу.
// Значения переведены через ключи (Task 15, тот же паттерн, что kBeamTypeKeys в misc_prefs.cpp) —
// до этого висели литералом (см. отчёт задачи 9, раздел "осталось непереведённым").

static_function void GetHudTypeChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
{
	const char *lang = player->languageService->GetLanguage();
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - HudType Standard"), KZHUDService::HUD_TYPE_STANDARD});
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - HudType Panorama"), KZHUDService::HUD_TYPE_PANORAMA});
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - HudType Off"), KZHUDService::HUD_TYPE_OFF});
}

static_function i64 GetHudTypeCurrent(KZPlayer *player, i64 tag)
{
	return (i64)player->hudService->GetHudType();
}

static_function void OnHudTypePick(KZPlayer *player, i64 tag, i64 id)
{
	player->hudService->SetHudType((i32)id);
}

// === mhudKeysIdle: чем показывать ненажатую клавишу (Task 5, транш "клавиши") ================
// Сырой int-преф (0 show / 1 hide / 2 underscore, см. LayoutElement::Keys/mhud.cpp) — в
// отличие от hudType это НЕ вычисляемое состояние сервиса, поэтому читаем/пишем сам преф,
// как CompareType в misc_prefs.cpp. Значения переведены (Task 15) — см. комментарий у
// GetHudTypeChoices выше.

static_function void GetKeysIdleChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
{
	const char *lang = player->languageService->GetLanguage();
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - Idle Show"), 0});
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - Idle Hide"), 1});
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - Idle Underscore"), 2});
}

static_function i64 GetKeysIdleCurrent(KZPlayer *player, i64 tag)
{
	return player->optionService->GetPreferenceInt("mhudKeysIdle", 0);
}

static_function void OnKeysIdlePick(KZPlayer *player, i64 tag, i64 id)
{
	if (id < 0 || id > 2)
	{
		return;
	}
	player->optionService->SetPreferenceInt("mhudKeysIdle", id);
}

// === Обводка элемента: тумблер через колбэки, а не голый преф ================================
// Худ применяет МИГРИРОВАННОЕ значение (KZHUDService::GetElementOutlinePref, layout/prefs.cpp:
// поэлементный mhud*Outline, а при его отсутствии — старый общий hudOutline). Голый
// AddToggle(def.outlineKey, true) читал бы преф второй раз и с другим дефолтом: игроку с
// hudOutline=false меню показывало "Вкл" при выключенной обводке, а первый клик был видимым
// no-op (писал false поверх уже действующего false). tag — индекс элемента в LAYOUT_ELEMENTS.
static_function i64 OutlineGetCurrent(KZPlayer *player, i64 tag)
{
	return player->hudService->GetElementOutlinePref((LayoutElement)tag) ? 1 : 0;
}

static_function void OutlineOnActivate(KZPlayer *player, i64 tag)
{
	// Инвертируем то же эффективное значение, которое показано в меню; запись поэлементного
	// ключа заодно завершает миграцию для этого элемента (дальше probe видит сохранённый ключ).
	const bool next = !player->hudService->GetElementOutlinePref((LayoutElement)tag);
	player->optionService->SetPreferenceBool(LAYOUT_ELEMENTS[tag].outlineKey, next);
}

// === Сброс страницы (KZ::menu::ResetNode) ====================================================
// Апстрим держит такую кнопку на каждой странице элемента (origin/master hud_prefs.cpp:256) —
// при порте она потерялась, и ResetNode остался без единого вызова. Возвращаем её ТОЛЬКО на
// страницы, где все пункты — обычные префы: ResetNode пишет преф напрямую, и на пункте с
// колбэком (AddActionToggle сервисов в misc/local/jumpstats) он оставил бы кэш сервиса
// рассинхронизированным — ровно тот дефект, из-за которого сломались тумблеры до этого
// фикс-раунда. Обводка исключением не является: её кэш — layoutPrefs, а ActivateMenuItem
// зовёт RefreshLayoutPrefs сразу после onActivate кнопки. В General сбрасывать нечего
// (единственный пункт — HudType, у него нет своего префа), кнопки там нет.
static_global KZOptNode *s_resettableNodes[(i32)LayoutElement::Count + 1] {};

static_function void ResetPageOnActivate(KZPlayer *player, i64 tag)
{
	if (tag < 0 || tag >= (i64)KZ_ARRAYSIZE(s_resettableNodes))
	{
		return;
	}
	KZ::menu::ResetNode(player, s_resettableNodes[tag]);
}

// Регистрирует кнопку сброса в конце страницы и запоминает узел под её тегом.
static_function void AddResetButton(KZOptNode *node, i32 slot)
{
	s_resettableNodes[slot] = node;
	KZ::menu::AddButton(node, "HUD - Menu Label Reset", &ResetPageOnActivate, slot);
}

// === Пять полей элемента, общих для Timer/Speed/Prespeed/Keys/Checkpoint — ключи из
// LAYOUT_ELEMENTS (entity.cpp, Task 4), один источник правды, как и раньше в menu.cpp. =======
static_function void AddHudElementItems(KZOptNode *node, LayoutElement e)
{
	const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)e];
	KZ::menu::AddToggle(node, "HUD - Menu Label Enabled", def.enabledKey, true);
	KZ::menu::AddPosition(node, "HUD - Menu Label Position", def.xKey, def.yKey, def.xDefault, def.yDefault);
	KZ::menu::AddSize(node, "HUD - Menu Label Size", def.sizeKey, def.sizeDefault, LAYOUT_SIZE_MIN, LAYOUT_SIZE_MAX);
	KZ::menu::AddFont(node, "HUD - Menu Label Font", def.fontKey, LAYOUT_DEFAULT_FONT);
	// Обводка — теперь поэлементный тумблер (def.outlineKey, задача 4), а не один общий
	// пункт на всё меню: старый "Outline" в General убран, чтобы не осталось двух источников.
	// Через колбэки — из-за миграции с hudOutline, см. OutlineGetCurrent выше.
	KZ::menu::AddActionToggle(node, "HUD - Menu Label Outline", &OutlineGetCurrent, &OutlineOnActivate, (i64)e);
	KZ::menu::SetItemPref(node, def.outlineKey, KZOptStorage::Bool, 1);
	// Прозрачность хранится Int (0-100), а не Float, как размер/позиция того же элемента —
	// AddSize по умолчанию заводит Float (px-поле), поэтому storage переопределяем ЯВНО:
	// реальный потребитель (layout/prefs.cpp:GetLayoutPrefs) читает opacityKey через
	// GetPreferenceInt, разойтись с ним значило бы читать иное значение, чем видит худ.
	KZ::menu::AddSize(node, "HUD - Menu Label Opacity", def.opacityKey, 100, 0, 100);
	KZ::menu::SetItemUnit(node, "%");
	KZ::menu::SetItemPref(node, def.opacityKey, KZOptStorage::Int, 100);
}

void KZHUDService::InitMenuPrefs()
{
	KZOptNode *general = KZ::menu::AddCategory("HUD - Menu Cat General");
	KZ::menu::AddChoice(general, "HUD - Menu Label HudType", GetHudTypeChoices, GetHudTypeCurrent, OnHudTypePick);
	// Общий пункт "Outline" убран (задача 4): обводка стала поэлементной
	// (LAYOUT_ELEMENTS[*].outlineKey у каждого элемента свой, пункт — в AddHudElementItems),
	// два источника одной и той же настройки — прямой путь к рассинхрону.

	KZOptNode *timer = KZ::menu::AddCategory("HUD - Menu Cat Timer");
	AddHudElementItems(timer, LayoutElement::Timer);
	KZ::menu::AddColor(timer, "HUD - Menu Label ProColor", "mhudTimerProColor", MHUD_DEF_TIMER_PRO_COLOR);
	KZ::menu::AddColor(timer, "HUD - Menu Label TpColor", "mhudTimerTpColor", MHUD_DEF_TIMER_TP_COLOR);
	KZ::menu::AddColor(timer, "HUD - Menu Label PausedColor", "mhudTimerPausedColor", MHUD_DEF_TIMER_PAUSED_COLOR);
	KZ::menu::AddColor(timer, "HUD - Menu Label StoppedColor", "mhudTimerStoppedColor", MHUD_DEF_TIMER_STOPPED_COLOR);
	AddResetButton(timer, (i32)LayoutElement::Timer);

	KZOptNode *speed = KZ::menu::AddCategory("HUD - Menu Cat Speed");
	AddHudElementItems(speed, LayoutElement::Speed);
	KZ::menu::AddColor(speed, "HUD - Menu Label Color", "mhudSpeedColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(speed, "HUD - Menu Label CjColor", "mhudSpeedCjColor", MHUD_DEF_CJ_COLOR);
	AddResetButton(speed, (i32)LayoutElement::Speed);

	KZOptNode *prespeed = KZ::menu::AddCategory("HUD - Menu Cat Prespeed");
	AddHudElementItems(prespeed, LayoutElement::Prespeed);
	KZ::menu::AddColor(prespeed, "HUD - Menu Label Color", "mhudPrespeedColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(prespeed, "HUD - Menu Label PerfColor", "mhudPrespeedPerfColor", MHUD_DEF_PERF_COLOR);
	KZ::menu::AddColor(prespeed, "HUD - Menu Label JumpbugColor", "mhudPrespeedJumpbugColor", MHUD_DEF_JUMPBUG_COLOR);
	AddResetButton(prespeed, (i32)LayoutElement::Prespeed);

	KZOptNode *keys = KZ::menu::AddCategory("HUD - Menu Cat Keys");
	AddHudElementItems(keys, LayoutElement::Keys);
	KZ::menu::AddColor(keys, "HUD - Menu Label Color", "mhudKeysColor", MHUD_DEF_BASE_COLOR);
	// hudKeysOverlap читался кодом (layout/prefs.cpp) ещё до этой задачи, но пункта в меню у
	// него не было — одна из шести находок транша "клавиши" (без пункта/команды у игрока).
	// Дефолт true — тот же, что уже читает GetPreferenceBool на этом ключе.
	KZ::menu::AddToggle(keys, "HUD - Menu Label Overlap", "hudKeysOverlap", true);
	KZ::menu::AddColor(keys, "HUD - Menu Label OverlapColor", "mhudKeysOverlapColor", MHUD_DEF_KEYS_OVERLAP_COLOR);
	KZ::menu::SetItemEnabledBy(keys, "hudKeysOverlap");
	// Осевой режим: тонировать только конфликтующую пару клавиш вместо всего контейнера.
	KZ::menu::AddToggle(keys, "HUD - Menu Label OverlapAxisOnly", "mhudKeysOverlapAxis", false);
	KZ::menu::SetItemEnabledBy(keys, "hudKeysOverlap");
	KZ::menu::AddColor(keys, "HUD - Menu Label PressedColor", "mhudKeysPressedColor", MHUD_DEF_KEYS_PRESSED_COLOR);
	KZ::menu::SetItemSolidOnly(keys); // key-glow-N (keys.css) — только сплошные, градиента там нет
	KZ::menu::AddColor(keys, "HUD - Menu Label OverlapGlowColor", "mhudKeysOverlapGlowColor", MHUD_DEF_KEYS_OVERLAP_GLOW_COLOR);
	KZ::menu::SetItemSolidOnly(keys);
	KZ::menu::SetItemEnabledBy(keys, "hudKeysOverlap");
	KZ::menu::AddToggle(keys, "HUD - Menu Label Letters", "mhudKeysLetters", false);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Square", "mhudKeysSquare", false);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Border", "mhudKeysBorder", true);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Glow", "mhudKeysGlow", true);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Fill", "mhudKeysFill", true);
	KZ::menu::AddChoice(keys, "HUD - Menu Label Idle", &GetKeysIdleChoices, &GetKeysIdleCurrent, &OnKeysIdlePick);
	KZ::menu::SetItemPref(keys, "mhudKeysIdle", KZOptStorage::Int, 0);
	AddResetButton(keys, (i32)LayoutElement::Keys);

	KZOptNode *checkpoint = KZ::menu::AddCategory("HUD - Menu Cat Checkpoint");
	AddHudElementItems(checkpoint, LayoutElement::Checkpoint);
	KZ::menu::AddColor(checkpoint, "HUD - Menu Label Color", "mhudCheckpointColor", MHUD_DEF_BASE_COLOR);
	AddResetButton(checkpoint, (i32)LayoutElement::Checkpoint);

	KZOptNode *crosshair = KZ::menu::AddCategory("HUD - Menu Cat Crosshair");
	KZ::menu::AddToggle(crosshair, "HUD - Menu Label Enabled", "mhudCrosshair", false);
	KZ::menu::AddSize(crosshair, "HUD - Menu Label Scale", "mhudCrosshairScale", 100, 0, 500);
	KZ::menu::SetItemUnit(crosshair, "%");
	KZ::menu::SetItemPref(crosshair, "mhudCrosshairScale", KZOptStorage::Int, 100);
	AddResetButton(crosshair, (i32)LayoutElement::Count);
}
