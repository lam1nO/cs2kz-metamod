#pragma once
#include "kz/hud/kz_hud.h"

// Дефолты элементов panorama-худа: X/Y — ПРОЦЕНТЫ от центра, size — пиксели.
// Не путать с particle-MHUD, где офсеты в его собственных юнитах (префы там отдельные).
#define LAYOUT_DEF_TIMER_X    0
#define LAYOUT_DEF_TIMER_Y    35
#define LAYOUT_DEF_TIMER_SIZE 24

#define LAYOUT_DEF_SPEED_X    0
#define LAYOUT_DEF_SPEED_Y    8
#define LAYOUT_DEF_SPEED_SIZE 34

#define LAYOUT_DEF_PRESPEED_X    0
#define LAYOUT_DEF_PRESPEED_Y    12
#define LAYOUT_DEF_PRESPEED_SIZE 22

#define LAYOUT_DEF_KEYS_X    0
#define LAYOUT_DEF_KEYS_Y    20
// 30, а не 20 — дефолт синхронизирован с текущими настройками игрока (задача hud-defaults).
#define LAYOUT_DEF_KEYS_SIZE 30

#define LAYOUT_DEF_CHECKPOINT_X    0
#define LAYOUT_DEF_CHECKPOINT_Y    30
#define LAYOUT_DEF_CHECKPOINT_SIZE 20

// «Прогресс: N%» по маршруту `!lead` — «в поле таймера, но настраивается отдельно» (решение
// пользователя): X как у таймера, Y на шесть процентов ниже, кегль мельче. Элемент по
// умолчанию ВЫКЛЮЧЕН (LayoutElementDef::enabledDefault), поэтому этой раскладки никто не
// увидит, пока сам не включит пункт в меню.
#define LAYOUT_DEF_LEADPROGRESS_X    LAYOUT_DEF_TIMER_X
#define LAYOUT_DEF_LEADPROGRESS_Y    (LAYOUT_DEF_TIMER_Y + 6)
#define LAYOUT_DEF_LEADPROGRESS_SIZE 18

#define LAYOUT_SIZE_MIN 8
#define LAYOUT_SIZE_MAX 100

// Настройки меню реплея — страница «Меню реплея» в `!options` (hud/prefs/hud_prefs.cpp), ключи
// читает layout/prefs.cpp:RefreshLayoutPrefs. Их РОВНО ДВЕ, и обе — списки, а не числа: сервер
// умеет над panorama-страницей только SetHasClass, поэтому каждое значение обязано иметь свой
// готовый набор правил в vcss аддона. Числовой ползунок здесь означал бы, что большинство
// значений ничего не меняют — игрок читает это как «настройка сломана».
//
// Прежние ключи (rpmenuX/Y/Size/Step/Font/Outline/Background) удалены 17.09.2026: они остались
// от карточки на разметке ХУДА и после переезда на свою страницу не влияли ни на что. Пункты
// вводили в заблуждение (замечание владельца), а сохранённые у игроков значения теперь просто
// не читаются — миграции не нужно. Состав белого списка обмена худом от этого меняется, и
// старые снимки применяются с предупреждением о другом составе: это штатное поведение обмена
// (отпечаток считается из реестра, см. hud/share/hud_share.cpp).

// Масштаб карточки: ступени процентов, класс .rp-scale--N на панели replay_card.
// Наборы правил — rpmenu-scale.css (генератор tools/build_rpmenu_scale.py в аддоне).
#define RPMENU_DEF_SCALE 100
extern const i32 RPMENU_SCALE_STEPS[];
extern const i32 RPMENU_SCALE_STEP_COUNT;
i32 SnapReplayMenuScale(i32 percent);

// Позиция карточки: ЯКОРЯ, а не координаты. Класс .rp-pos--<slug> на той же панели; правила —
// в rpmenu.css (их шесть, руками, генератор не нужен). Дефолт — левый край по центру высоты,
// как в спеке (docs/design/2026-09-11-replay-player-panorama, §1.0).
#define RPMENU_DEF_ANCHOR 1 // индекс "left-middle" в RPMENU_ANCHORS
extern const char *const RPMENU_ANCHORS[];
extern const i32 RPMENU_ANCHOR_COUNT;
// Ключи фраз для пунктов выбора, в том же порядке, что RPMENU_ANCHORS.
extern const char *const RPMENU_ANCHOR_PHRASES[];
i32 ClampReplayMenuAnchor(i32 index);

// Дефолт синхронизирован с текущими настройками игрока (задача hud-defaults): было
// stratum2-bold-monodigit, стало lato-bold (слаг panorama_tables.cpp, "Lato Bold*").
#define LAYOUT_DEFAULT_FONT "lato-bold"
// Разметка живёт в нашем аддоне GYMSTRIKE-KZ (см. KZ_WORKSHOP_ADDON_ID); путь намеренно
// совпадает с апстримным — аддон собран из тех же исходников, и при откате на чужой
// айтем код менять не придётся.
#define KZ_MHUD_LAYOUT "panorama/layout/custom_game/cs2kz/mhud.vxml_c"
// Своя страница меню реплея спектатора (layout/rpmenu.cpp): карточка плеера целиком — шапка,
// время, шкала перемотки с отметками, семь пунктов и подсказка. Живёт ТОЛЬКО в нашем аддоне
// (custom_game/cyber/…), апстримного аналога у неё нет; исходники разметки — gymstrike-kz/content/panorama
// (rpmenu.xml + rpmenu.css + rpmenu-positions.css + rpmenu-scale.css), спека
// 2026-09-11-replay-player-panorama.
#define KZ_RPMENU_LAYOUT "panorama/layout/custom_game/cyber/rpmenu.vxml_c"

// Отказ SetHasClass/SetDialogVariableString по упору в HUD_LAYOUT_MAX_INTERNED_STRINGS —
// общий лог для entity.cpp и rpmenu.cpp (реализация в entity.cpp).
void LogHudInternFailure(KZPlayer *player, const char *panelId, const char *className);
