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

// Меню реплея спектатора (layout/rpmenu.cpp): дефолты префов rpmenuX/Y/Size/Step/Font —
// пункт «Меню реплея» в настройках (hud/prefs/hud_prefs.cpp). X/Y — проценты от центра,
// size — пиксели, step — шаг строк в процентах.
//
// ВНИМАНИЕ: с переездом меню на СВОЮ страницу (KZ_RPMENU_LAYOUT) рендер эти префы больше НЕ
// читает — вся геометрия, шрифты и фон карточки заданы её vcss (карточка фиксированного
// размера, спека 2026-09-11-replay-player-panorama). Ключи, пункт настроек и запись в
// GetOwnLayoutPrefs оставлены намеренно: у игроков уже лежат сохранённые значения, а ключи
// входят в белый список обмена худом — их удаление отдельное решение, не часть переезда.
// Масштаб карточки меню реплея (преф rpmenuScale, пункт «Меню реплея» в настройках). Это
// ЕДИНСТВЕННЫЙ преф меню реплея, который новая страница действительно читает.
// Ступени, а не любое число: сервер умеет только SetHasClass, поэтому каждый масштаб — свой
// готовый набор правил в rpmenu-scale.css (генератор — tools/build_rpmenu_scale.py в аддоне).
// Значение префа снапится к ближайшей ступени (SnapReplayMenuScale).
#define RPMENU_DEF_SCALE 100
#define RPMENU_SCALE_MIN 75
#define RPMENU_SCALE_MAX 130
extern const i32 RPMENU_SCALE_STEPS[];
extern const i32 RPMENU_SCALE_STEP_COUNT;
i32 SnapReplayMenuScale(i32 percent);

#define RPMENU_DEF_X    -34
#define RPMENU_DEF_Y    -8
#define RPMENU_DEF_SIZE 20
#define RPMENU_DEF_STEP 2
#define RPMENU_DEF_FONT "stratum2-mono"
#define RPMENU_STEP_MIN 1
#define RPMENU_STEP_MAX 12
#define RPMENU_DEF_BACKGROUND 30 // непрозрачность подложки, %

// Шрифты меню реплея — таблица выбора в пункте настроек (реализация в layout/prefs.cpp);
// преф rpmenuFont, не попавший в неё, читается как RPMENU_DEF_FONT.
// Ограничение «только моноширинные» осталось от старой карточки на разметке ХУДА: там строки
// добивались NBSP до одной длины в кодовых точках, и это точно работает только в моно. Своя
// страница берёт шрифты из своего vcss и rpmenuFont не применяет вовсе, так что ни расширять,
// ни урезать таблицу сейчас незачем — см. комментарий у дефолтов выше.
extern const char *const RPMENU_MONO_FONTS[];
extern const i32 RPMENU_MONO_FONT_COUNT;
const char *ResolveReplayMenuFontSlug(const char *slug);

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
