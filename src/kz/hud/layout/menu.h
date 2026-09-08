#pragma once
#include "kz/kz.h"

// Меню настроек panorama-худа и крестика (Task 11, реестр — задача 2): урезанный перенос
// апстримного src/kz/option/menu/{kz_menu,model,tables}.cpp. Pref-registry (KZOptNode/KZOptItem,
// KZ::menu::Add*, kz/option/menu/model.h) у нас ОБЩИЙ с апстримом (Task 1) — состав меню
// (категории/пункты) регистрируется отдельно, hud/prefs/hud_prefs.cpp, а этот файл только
// обходит KZ::menu::GetTree() и рендерит. prefs_transfer (экспорт/импорт настроек) апстрима
// по-прежнему не переносим — не требуется этой задачей.
//
// Разметка — тот же чужой аддон 3469155349, что и mhud.vxml_c (KZ_MHUD_LAYOUT в layout.h):
// путь по образцу задачи 4/10.
#define KZ_MENU_LAYOUT "panorama/layout/custom_game/cs2kz/menu.vxml_c"

// Фиксированные ёмкости слотов разметки menu.vxml (тот же контракт, что у апстримного
// kz_menu.h): категории слева, пункты в средней колонке, свотчи попапа цвета. Наше дерево
// заметно меньше апстримного (7 категорий, максимум 10 пунктов в самой длинной — Timer), но
// константы держим как в апстриме, чтобы разметка совпадала без сюрпризов на расширении.
#define KZ_MENU_CATS   20
#define KZ_MENU_ITEMS  20
#define KZ_MENU_SWATCH 40
// Строк попапа списка (Choice) — li0..li31 в разметке; наш самый длинный getChoices (тип худа)
// даёт всего 3 строки, но ёмкость держим как в разметке на случай будущих рантайм-списков.
#define KZ_MENU_LIST 32
