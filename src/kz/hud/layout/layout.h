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

// Поля редактора `!hud` (PB/WR, showpos, курс, тип рана) — ВРЕМЕННЫЕ дефолты до решения
// владельца (чеклист H6). Борд 1 даёт их в процентах ЭКРАНА от левого верхнего угла
// (PB/WR 1/88, showpos 1/2, курс 50/2, тип рана 47/93), а у нас X/Y — проценты от ЦЕНТРА, и
// элемент центрируется на своей точке. Прямой перевод p-50 увёл бы левые элементы
// наполовину за край экрана, поэтому они подтянуты внутрь примерно на полширины элемента.
#define LAYOUT_DEF_PBWR_X    (-40)
#define LAYOUT_DEF_PBWR_Y    38
#define LAYOUT_DEF_PBWR_SIZE 17

#define LAYOUT_DEF_SHOWPOS_X    (-42)
#define LAYOUT_DEF_SHOWPOS_Y    (-46)
#define LAYOUT_DEF_SHOWPOS_SIZE 13

#define LAYOUT_DEF_COURSE_X    0
#define LAYOUT_DEF_COURSE_Y    (-46)
#define LAYOUT_DEF_COURSE_SIZE 13

#define LAYOUT_DEF_RUNTYPE_X    (-3)
#define LAYOUT_DEF_RUNTYPE_Y    43
#define LAYOUT_DEF_RUNTYPE_SIZE 15

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

// Позиция карточки: ПРОЦЕНТЫ ОТ ЦЕНТРА экрана по обеим осям, как у элементов худа, и теми же
// классами .x--[neg]Npct / .y--[neg]Npct из cs2kz/positions.css (файл уже едет в аддоне ради
// mhud — своего набора классов заводить не пришлось). Пункт — обычный Position со степперами
// ±1/±5, ровно как у соседей (решение владельца 17.09.2026: якоря-список заменены на шаги).
//
// Дефолт по X считается так: центр карточки = 960 + (-36 % × 1920) = 268.8, то есть левый край
// при масштабе 100 % (ширина 360) — 89 px. Спека просит 48, но держать ЛЕВЫЙ край на месте при
// центрирующем позиционировании нельзя: на 130 % карточка шириной 468 уехала бы за левый край
// экрана. -36 % держит её на экране на всех пяти масштабах (75…130 %), а точную посадку игрок
// доводит теми же ±1/±5.
#define RPMENU_DEF_POS_X (-36)
#define RPMENU_DEF_POS_Y 0

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
