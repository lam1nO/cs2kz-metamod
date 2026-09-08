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

#define LAYOUT_SIZE_MIN 8
#define LAYOUT_SIZE_MAX 100

// Дефолт синхронизирован с текущими настройками игрока (задача hud-defaults): было
// stratum2-bold-monodigit, стало lato-bold (слаг panorama_tables.cpp, "Lato Bold*").
#define LAYOUT_DEFAULT_FONT "lato-bold"
// Разметка живёт в чужом аддоне 3469155349; путь совпадает с апстримным.
#define KZ_MHUD_LAYOUT "panorama/layout/custom_game/cs2kz/mhud.vxml_c"
