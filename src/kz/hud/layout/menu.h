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
// kz_menu.h): категории слева, пункты в средней колонке, свотчи попапа цвета. Состав дерева
// на сегодня — 14 категорий, самая длинная (Keys) занимает 19 пунктов из 20; константы держим
// как в апстриме, чтобы разметка совпадала без сюрпризов на расширении. Пункты сверх KZ_MENU_ITEMS
// молча не показываются, поэтому RenderMenuItems пишет об усечении в лог (panorama_menu_items_truncated).
#define KZ_MENU_CATS   20
#define KZ_MENU_ITEMS  20
#define KZ_MENU_SWATCH 40
// Строк попапа списка (Choice) — li0..li31 в разметке; самые длинные getChoices рантайм-овые
// (Language 13 строк; Mode/Styles/Pistol — по числу загруженных плагинов), ёмкость держим как
// в разметке. Страницы попапа листаются (MenuPopupPageStep), так что список длиннее 32 не режется.
// Список шрифтов (68 начертаний) листается не по 32, а по СЕМЕЙСТВАМ: 15 страниц, самая длинная
// (Stratum2) — 29 строк, то есть в эту же ёмкость (layout/menu.cpp, GetListPopupSlice).
#define KZ_MENU_LIST 32

// Окно проверки инварианта «нет захвата ввода при закрытом меню» (CheckMenuCaptureInvariant,
// layout/menu.cpp), секунды. Проверка живёт в игровом такте — окно нужно, чтобы не читать схему
// на каждого игрока каждый тик; секунда с запасом укладывается в «игрок не успел заметить».
#define KZ_MENU_CAPTURE_CHECK_INTERVAL 1.0
