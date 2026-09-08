// Регистрация меню настроек panorama-худа и крестика в общем реестре (kz/option/menu/model.h,
// Task 1). Раньше (Task 11) состав меню жил статическими таблицами прямо в layout/menu.cpp —
// задача 2 переносит РОВНО тот же состав (категории, пункты, ключи префов, диапазоны,
// дефолты — ничего не меняя) сюда, а рендер (layout/menu.cpp) обходит KZ::menu::GetTree().
#include "kz/hud/kz_hud.h"
#include "kz/hud/layout/layout.h"
#include "kz/option/menu/model.h"

#include "tier0/memdbgon.h"

// === Тип худа: цикл Standard -> Panorama -> Off -> Standard =================================
// Не булев тумблер (три состояния) и не сырой преф — hudType читается/пишется через
// GetHudType()/SetHudType() (kz_hud.cpp), которые сами тащат миграцию легаси-значений
// (mhudMaster, удалённый particle-тип 1). Моделируем как Choice: список из трёх пунктов в
// ТОМ ЖЕ порядке, что и старый цикл NextHudType, а клик по пункту (layout/menu.cpp) перебирает
// список по кругу — попап списка (как у апстримного Choice) сюда не переносим, его и не было.

static_function void GetHudTypeChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
{
	out.push_back({"Standard", KZHUDService::HUD_TYPE_STANDARD});
	out.push_back({"Panorama", KZHUDService::HUD_TYPE_PANORAMA});
	out.push_back({"Off", KZHUDService::HUD_TYPE_OFF});
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
// как CompareType в misc_prefs.cpp.

static_function void GetKeysIdleChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
{
	out.push_back({"Show", 0});
	out.push_back({"Hide", 1});
	out.push_back({"Underscore", 2});
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
	KZ::menu::AddToggle(node, "HUD - Menu Label Outline", def.outlineKey, true);
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

	KZOptNode *speed = KZ::menu::AddCategory("HUD - Menu Cat Speed");
	AddHudElementItems(speed, LayoutElement::Speed);
	KZ::menu::AddColor(speed, "HUD - Menu Label Color", "mhudSpeedColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(speed, "HUD - Menu Label CjColor", "mhudSpeedCjColor", MHUD_DEF_CJ_COLOR);

	KZOptNode *prespeed = KZ::menu::AddCategory("HUD - Menu Cat Prespeed");
	AddHudElementItems(prespeed, LayoutElement::Prespeed);
	KZ::menu::AddColor(prespeed, "HUD - Menu Label Color", "mhudPrespeedColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(prespeed, "HUD - Menu Label PerfColor", "mhudPrespeedPerfColor", MHUD_DEF_PERF_COLOR);
	KZ::menu::AddColor(prespeed, "HUD - Menu Label JumpbugColor", "mhudPrespeedJumpbugColor", MHUD_DEF_JUMPBUG_COLOR);

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

	KZOptNode *checkpoint = KZ::menu::AddCategory("HUD - Menu Cat Checkpoint");
	AddHudElementItems(checkpoint, LayoutElement::Checkpoint);
	KZ::menu::AddColor(checkpoint, "HUD - Menu Label Color", "mhudCheckpointColor", MHUD_DEF_BASE_COLOR);

	KZOptNode *crosshair = KZ::menu::AddCategory("HUD - Menu Cat Crosshair");
	KZ::menu::AddToggle(crosshair, "HUD - Menu Label Enabled", "mhudCrosshair", false);
	KZ::menu::AddSize(crosshair, "HUD - Menu Label Scale", "mhudCrosshairScale", 100, 0, 500);
	KZ::menu::SetItemUnit(crosshair, "%");
	KZ::menu::SetItemPref(crosshair, "mhudCrosshairScale", KZOptStorage::Int, 100);
}
