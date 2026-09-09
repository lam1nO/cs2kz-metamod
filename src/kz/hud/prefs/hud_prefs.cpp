// Регистрация меню настроек panorama-худа и крестика в общем реестре (kz/option/menu/model.h,
// Task 1). Раньше (Task 11) состав меню жил статическими таблицами прямо в layout/menu.cpp —
// задача 2 переносит РОВНО тот же состав (категории, пункты, ключи префов, диапазоны,
// дефолты — ничего не меняя) сюда, а рендер (layout/menu.cpp) обходит KZ::menu::GetTree().
#include "kz/hud/kz_hud.h"
#include "kz/hud/layout/layout.h"
#include "kz/hud/share/hud_share.h"
#include "kz/option/menu/model.h"
#include "kz/option/kz_option.h"
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
	// Дефолт 2 (Underscore) синхронизирован с текущими настройками игрока (задача hud-defaults).
	return player->optionService->GetPreferenceInt("mhudKeysIdle", 2);
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
// колбэком (AddActionToggle сервисов в misc_prefs/local_prefs) он оставил бы кэш сервиса
// рассинхронизированным — ровно тот дефект, из-за которого сломались тумблеры до этого
// фикс-раунда. Обводка исключением не является: её кэш — layoutPrefs, а ActivateMenuItem
// зовёт RefreshLayoutPrefs сразу после onActivate кнопки. В General своей кнопки «сбросить
// страницу» нет: там лежит общий Reset All (ниже), а HudType не сбрасывается вовсе — у его
// Choice-пункта нет префа, и ResetNode такие пункты пропускает.
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

// === Сброс ВСЕХ настроек худа (апстримный "Reset All" на странице General) ====================
// У апстрима такая кнопка есть (origin/master hud_prefs.cpp, General) — при порте потерялась.
// Живёт в General, а не на родительской категории «Худ»: у категории с подкатегориями своих
// пунктов не бывает вовсе (ActiveMenuNode, layout/menu.cpp — родитель возвращает NULL, пока не
// выбрана подкатегория), так что пункт на ней был бы недостижим для игрока.
// Сбрасывает страницы всех пяти элементов + прицел (s_resettableNodes) и саму General.
// HudType при этом НЕ сбрасывается: его Choice-пункт зарегистрирован без prefKey, а ResetNode
// пункты без префа пропускает — иначе «сбросить оформление» могло бы выключить игроку худ.
static_global KZOptNode *s_hudGeneralNode {};

static_function void ResetAllOnActivate(KZPlayer *player, i64 tag)
{
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(s_resettableNodes); i++)
	{
		KZ::menu::ResetNode(player, s_resettableNodes[i]);
	}
	KZ::menu::ResetNode(player, s_hudGeneralNode);
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
	// Дефолт 0 (выкл) синхронизирован с текущими настройками игрока (задача hud-defaults) —
	// тот же ответ отдаёт GetElementOutlinePref (layout/prefs.cpp) через fallback на hudOutline.
	KZ::menu::SetItemPref(node, def.outlineKey, KZOptStorage::Bool, 0);
	// Прозрачность хранится Int (0-100), а не Float, как размер/позиция того же элемента —
	// AddSize по умолчанию заводит Float (px-поле), поэтому storage переопределяем ЯВНО:
	// реальный потребитель (layout/prefs.cpp:GetLayoutPrefs) читает opacityKey через
	// GetPreferenceInt, разойтись с ним значило бы читать иное значение, чем видит худ.
	KZ::menu::AddSize(node, "HUD - Menu Label Opacity", def.opacityKey, 100, 0, 100);
	KZ::menu::SetItemUnit(node, "%");
	KZ::menu::SetItemPref(node, def.opacityKey, KZOptStorage::Int, 100);
}

// === Обмен худом: четыре действия одной страницей =============================================
// Ядро — hud/share/hud_share.cpp, транспорт кода — hud/share/hud_share_commands.cpp; здесь
// только пункты меню поверх тех же функций, что и чат-команды. Ни один из четырёх пунктов не
// хранит настройки (Button без префа), поэтому в белый список обмена они не попадают
// (AddEntry пропускает storage == None, option/pref_registry.cpp) и отпечаток состава не двигают.
static_function void ShareCodeOnActivate(KZPlayer *player, i64 tag)
{
	KZ::hudshare::IssueShareCode(player);
}

static_function void ShareTakeOnActivate(KZPlayer *player, i64 tag)
{
	// Динамического гейта у пунктов нет: enabledBy умеет только БУЛЕВ преф (IsMenuItemEnabled,
	// layout/menu.cpp), а «наблюдаю ли я сейчас за кем-то» — состояние кадра, не настройка, и
	// серым пункт стал бы только до следующей перерисовки меню. Поэтому пункт кликабелен всегда,
	// а отказ печатает TakeFromSpectated — с причиной, а не молчанием.
	KZ::hudshare::TakeFromSpectated(player);
}

static_function void ShareExportOnActivate(KZPlayer *player, i64 tag)
{
	// Второй, равноправный путь переноса (спека §4 «оба пути»): код !hudshare работает только у
	// нас, а текстовый блок — на глобальных cs2kz-серверах. Печать идёт в КОНСОЛЬ, поэтому
	// подпись пункта об этом говорит: иначе клик выглядел бы как пункт, который ничего не делает.
	KZ::hudshare::ExportToConsole(player);
}

static_function void ShareUndoOnActivate(KZPlayer *player, i64 tag)
{
	// Пустой слот — отдельная фраза, та же, что у !hudundo: «ничего не произошло» без строки
	// в чате игрок читает как сломанный пункт.
	if (!KZ::hudshare::HasUndo(player))
	{
		player->languageService->PrintChat(true, false, "HUD Share - Undo Empty");
		return;
	}
	KZ::hudshare::ApplyUndo(player);
}

void KZHUDService::InitMenuPrefs()
{
	// Дерево, а не плоский список (задача «дерево категорий»): все семь страниц худа —
	// ПОДКАТЕГОРИИ одного узла «Худ», как у апстрима (origin/master hud_prefs.cpp:135-162,
	// AddCategory("Menu - HUD") + AddSub на каждый элемент). Состав пунктов не меняется —
	// меняется только группировка; слева они рисуются теми же кнопками cat%i с отступом
	// (класс indent), см. layout/menu.cpp:BuildMenuLeft.
	KZOptNode *hud = KZ::menu::AddCategory("HUD - Menu Cat Hud");

	KZOptNode *general = KZ::menu::AddSub(hud, "HUD - Menu Cat General");
	s_hudGeneralNode = general;
	KZ::menu::AddChoice(general, "HUD - Menu Label HudType", GetHudTypeChoices, GetHudTypeCurrent, OnHudTypePick);
	// Общий пункт "Outline" убран (задача 4): обводка стала поэлементной
	// (LAYOUT_ELEMENTS[*].outlineKey у каждого элемента свой, пункт — в AddHudElementItems),
	// два источника одной и той же настройки — прямой путь к рассинхрону.
	//
	// Пункта под апстримный showPanel здесь НЕТ, и это решение, а не пропуск: его тумблер
	// KZHUDService::TogglePanel() не вызывается ниоткуда (git grep — только объявление и
	// определение), панорама преф вообще не читает (layout/mhud.cpp:367), а роль «показывать
	// худ или нет» с задачи 12 у типа Off — то есть у пункта HudType строкой выше. Пункт на
	// showPanel звал бы мёртвый метод, а пункт «Панель» поверх HudType дал бы ДВА источника
	// одного состояния — тот же рассинхрон, из-за которого убран общий Outline (строкой выше).
	//
	// mhudMimicSpec — мимикрия под настройки наблюдаемого (layout/prefs.cpp:GetLayoutPrefs).
	// Дефолт false = тот же, что читает RefreshLayoutPrefs (layout/prefs.cpp:129).
	KZ::menu::AddToggle(general, "HUD - Menu Label MimicSpec", "mhudMimicSpec", false);
	KZ::menu::SetItemSubtext(general, "HUD - Menu Label MimicSpec Sub");
	// compactPanel — СТАНДАРТНЫЙ (HTML) худ, не MHUD: две строки вместо полной панели
	// (KZHUDService::IsCompactPanel, kz_hud.cpp). Преф жив и до этой задачи переключался только
	// командой `!panel compact` — пункта в меню у него не было. Голый AddToggle, а не
	// AddActionToggle: IsCompactPanel() кэша не держит, читает преф заново каждый раз.
	// Дефолт false = дефолт GetPreferenceBool("compactPanel") без второго аргумента.
	KZ::menu::AddToggle(general, "HUD - Menu Label CompactPanel", "compactPanel", false);
	KZ::menu::SetItemSubtext(general, "HUD - Menu Label CompactPanel Sub");
	// Сброс всех страниц худа разом — единственный пункт General со своим действием, поэтому
	// стоит последним (см. ResetAllOnActivate выше).
	KZ::menu::AddButton(general, "HUD - Menu Label ResetAll", &ResetAllOnActivate);

	KZOptNode *timer = KZ::menu::AddSub(hud, "HUD - Menu Cat Timer");
	AddHudElementItems(timer, LayoutElement::Timer);
	// hudTimerDetail — сотые доли и часы в таймере (layout/prefs.cpp:108, timerDetailed).
	// Преф читался кодом, а тронуть его игрок не мог вообще: ни пункта, ни команды. Ключ НАШ
	// (апстримный называется mhudTimerDetailed) — не переименовываем, иначе у всех игроков
	// настройка сбросилась бы на дефолт. Дефолт true = тот же, что читает RefreshLayoutPrefs.
	KZ::menu::AddToggle(timer, "HUD - Menu Label TimerDetail", "hudTimerDetail", true);
	KZ::menu::AddColor(timer, "HUD - Menu Label ProColor", "mhudTimerProColor", MHUD_DEF_TIMER_PRO_COLOR);
	KZ::menu::AddColor(timer, "HUD - Menu Label TpColor", "mhudTimerTpColor", MHUD_DEF_TIMER_TP_COLOR);
	KZ::menu::AddColor(timer, "HUD - Menu Label PausedColor", "mhudTimerPausedColor", MHUD_DEF_TIMER_PAUSED_COLOR);
	KZ::menu::AddColor(timer, "HUD - Menu Label StoppedColor", "mhudTimerStoppedColor", MHUD_DEF_TIMER_STOPPED_COLOR);
	AddResetButton(timer, (i32)LayoutElement::Timer);

	KZOptNode *speed = KZ::menu::AddSub(hud, "HUD - Menu Cat Speed");
	AddHudElementItems(speed, LayoutElement::Speed);
	// mhudSpeedPrecise — "%.2f" вместо "%.0f" (layout/prefs.cpp:121, применяется в
	// layout/mhud.cpp:UpdateSpeedElement). Преф читался, доступа у игрока не было.
	KZ::menu::AddToggle(speed, "HUD - Menu Label Decimal", "mhudSpeedPrecise", false);
	KZ::menu::AddColor(speed, "HUD - Menu Label Color", "mhudSpeedColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(speed, "HUD - Menu Label CjColor", "mhudSpeedCjColor", MHUD_DEF_CJ_COLOR);
	AddResetButton(speed, (i32)LayoutElement::Speed);

	KZOptNode *prespeed = KZ::menu::AddSub(hud, "HUD - Menu Cat Prespeed");
	AddHudElementItems(prespeed, LayoutElement::Prespeed);
	// Три префа престрейфа, портированные соседней задачей (layout/prefs.cpp:124-126,
	// применяются в layout/mhud.cpp:UpdatePrespeedElement) — дефолты те же, что там читаются.
	KZ::menu::AddToggle(prespeed, "HUD - Menu Label Decimal", "mhudPrespeedPrecise", false);
	KZ::menu::AddToggle(prespeed, "HUD - Menu Label PrespeedBrackets", "mhudPrespeedBrackets", false);
	KZ::menu::AddToggle(prespeed, "HUD - Menu Label PrespeedHideWalkOff", "mhudPrespeedHideWalkOff", false);
	KZ::menu::SetItemSubtext(prespeed, "HUD - Menu Label PrespeedHideWalkOff Sub");
	KZ::menu::AddColor(prespeed, "HUD - Menu Label Color", "mhudPrespeedColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(prespeed, "HUD - Menu Label PerfColor", "mhudPrespeedPerfColor", MHUD_DEF_PERF_COLOR);
	KZ::menu::AddColor(prespeed, "HUD - Menu Label JumpbugColor", "mhudPrespeedJumpbugColor", MHUD_DEF_JUMPBUG_COLOR);
	AddResetButton(prespeed, (i32)LayoutElement::Prespeed);

	KZOptNode *keys = KZ::menu::AddSub(hud, "HUD - Menu Cat Keys");
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
	// Дефолты пяти тумблеров ниже синхронизированы с текущими настройками игрока (задача
	// hud-defaults) — те же значения читает layout/prefs.cpp:RefreshLayoutPrefs.
	KZ::menu::AddToggle(keys, "HUD - Menu Label Letters", "mhudKeysLetters", true);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Square", "mhudKeysSquare", true);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Border", "mhudKeysBorder", false);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Glow", "mhudKeysGlow", false);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Fill", "mhudKeysFill", false);
	KZ::menu::AddChoice(keys, "HUD - Menu Label Idle", &GetKeysIdleChoices, &GetKeysIdleCurrent, &OnKeysIdlePick);
	KZ::menu::SetItemPref(keys, "mhudKeysIdle", KZOptStorage::Int, 2);
	AddResetButton(keys, (i32)LayoutElement::Keys);

	KZOptNode *checkpoint = KZ::menu::AddSub(hud, "HUD - Menu Cat Checkpoint");
	AddHudElementItems(checkpoint, LayoutElement::Checkpoint);
	KZ::menu::AddColor(checkpoint, "HUD - Menu Label Color", "mhudCheckpointColor", MHUD_DEF_BASE_COLOR);
	AddResetButton(checkpoint, (i32)LayoutElement::Checkpoint);

	KZOptNode *crosshair = KZ::menu::AddSub(hud, "HUD - Menu Cat Crosshair");
	// Дефолт true синхронизирован с текущими настройками игрока (задача hud-defaults).
	KZ::menu::AddToggle(crosshair, "HUD - Menu Label Enabled", "mhudCrosshair", true);
	KZ::menu::AddSize(crosshair, "HUD - Menu Label Scale", "mhudCrosshairScale", 100, 0, 500);
	KZ::menu::SetItemUnit(crosshair, "%");
	KZ::menu::SetItemPref(crosshair, "mhudCrosshairScale", KZOptStorage::Int, 100);
	AddResetButton(crosshair, (i32)LayoutElement::Count);

	// Обмен худом — СВОЯ подкатегория, а не пункты в General. Довод: в General лежит «сбросить
	// все настройки худа», и действия обмена рядом с ней читались бы как часть сброса, а
	// главное — General это страница СВОЙСТВ худа (тип, мимикрия, компактная панель), тогда как
	// обмен это ДЕЙСТВИЯ над всем худом целиком. Отдельная страница ещё и обязательна по месту:
	// пункт «забрать худ наблюдаемого» ищут в спектейте, а меню в спектейте показывает ровно
	// выбранную подкатегорию — своя страница находится по названию, не перебором General.
	// Своей кнопки «сбросить страницу» у неё нет: сбрасывать нечего, префов на странице ноль.
	KZOptNode *share = KZ::menu::AddSub(hud, "HUD Share - Menu Cat Share");
	KZ::menu::AddButton(share, "HUD Share - Menu Label ShareCode", &ShareCodeOnActivate);
	KZ::menu::SetItemSubtext(share, "HUD Share - Menu Label ShareCode Sub");
	KZ::menu::AddButton(share, "HUD Share - Menu Label Take", &ShareTakeOnActivate);
	KZ::menu::SetItemSubtext(share, "HUD Share - Menu Label Take Sub");
	KZ::menu::AddButton(share, "HUD Share - Menu Label Export", &ShareExportOnActivate);
	KZ::menu::SetItemSubtext(share, "HUD Share - Menu Label Export Sub");
	KZ::menu::AddButton(share, "HUD Share - Menu Label Undo", &ShareUndoOnActivate);
	KZ::menu::SetItemSubtext(share, "HUD Share - Menu Label Undo Sub");
}
