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
// size — пиксели, step — шаг строк в процентах. Значения подбирает пользователь на канарейке.
// Дефолты = настройки пользователя с канарейки 09.09 (rpmenuX/Size/Step/Font/Background из его префов).
#define RPMENU_DEF_X    -34
#define RPMENU_DEF_Y    -8
#define RPMENU_DEF_SIZE 20
#define RPMENU_DEF_STEP 2
#define RPMENU_DEF_FONT "stratum2-mono"
#define RPMENU_STEP_MIN 1
#define RPMENU_STEP_MAX 12
#define RPMENU_DEF_BACKGROUND 30 // непрозрачность подложки, %

// Шрифты меню реплея — ТОЛЬКО моноширинные (решение 09.09 после канарейки cyb.169: в
// пропорциональном шрифте строки «в разнобой» и скачут при смене выбора). Левый край карточки
// строится добивкой NBSP до одной длины в кодовых точках, что точно только в моно; метрик глифов
// у сервера нет (файлов шрифтов на ноде нет), так что выровнять пропорциональный шрифт нечем.
// Таблица — в layout/prefs.cpp; преф rpmenuFont, не попавший в неё, читается как RPMENU_DEF_FONT.
extern const char *const RPMENU_MONO_FONTS[];
extern const i32 RPMENU_MONO_FONT_COUNT;
const char *ResolveReplayMenuFontSlug(const char *slug);

// Дефолт синхронизирован с текущими настройками игрока (задача hud-defaults): было
// stratum2-bold-monodigit, стало lato-bold (слаг panorama_tables.cpp, "Lato Bold*").
#define LAYOUT_DEFAULT_FONT "lato-bold"
// Разметка живёт в чужом аддоне 3469155349; путь совпадает с апстримным.
#define KZ_MHUD_LAYOUT "panorama/layout/custom_game/cs2kz/mhud.vxml_c"

// Отказ SetHasClass/SetDialogVariableString по упору в HUD_LAYOUT_MAX_INTERNED_STRINGS —
// общий лог для entity.cpp и rpmenu.cpp (реализация в entity.cpp).
void LogHudInternFailure(KZPlayer *player, const char *panelId, const char *className);
