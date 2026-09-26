#pragma once
#include "kz/kz.h"

// Меню настроек panorama-худа и крестика (Task 11, реестр — задача 2): урезанный перенос
// апстримного src/kz/option/menu/{kz_menu,model,tables}.cpp. Pref-registry (KZOptNode/KZOptItem,
// KZ::menu::Add*, kz/option/menu/model.h) у нас ОБЩИЙ с апстримом (Task 1) — состав меню
// (категории/пункты) регистрируется отдельно, hud/prefs/hud_prefs.cpp, а этот файл только
// обходит KZ::menu::GetTree() и рендерит. prefs_transfer (экспорт/импорт настроек) апстрима
// по-прежнему не переносим — не требуется этой задачей.
//
// Разметка — свой лист аддона GYMSTRIKE-KZ (спека 2026-09-26-hud-editor-options §3.2): окно
// !options, редактор !hud и общие попапы в ОДНОЙ разметке, ветки переключаются классом hidden
// на opt_root/edit_root. Старый cs2kz/menu.xml остаётся в аддоне до промоута плагина на флот
// (на него ссылаются старые версии плагина), этой версией не используется.
#define KZ_MENU_LAYOUT "panorama/layout/custom_game/cyber/options.vxml_c"

// Ёмкости разметки cyber/options.xml (tools/gen_options_xml.py аддона) — менять только вместе с ней.
#define KZ_MENU_TABS     6  // tab0..tab5 — категории верхнего уровня
#define KZ_MENU_ROWS     16 // sec{i}/row{i} — плоский список «секция + пункты» вкладки, страница
#define KZ_MENU_SEGS     4  // sg{i}_0..3 — Choice с <= 4 вариантами рисуется сегментами
#define KZ_MENU_PRESETS  32 // cp0..cp31 — пресеты попапа цвета
#define KZ_MENU_HUES     10 // ch0..ch9 — оттенки
#define KZ_MENU_LUMS     8  // cv0..cv7 — яркости выбранного оттенка
#define KZ_MENU_LIST_ROWS 8 // li0..li7 — строки попапа списка
#define KZ_MENU_FAMILIES 3  // lf0..lf2 — семейства шрифтов Stratum2/Noto/Arial
#define KZ_EDITOR_ITEMS  10 // el{i}/et{i} — элементы худа в порядке LayoutElement
#define KZ_EDITOR_GRID_COLS 64 // g{c}_{r} — сетка кликов редактора
#define KZ_EDITOR_GRID_ROWS 36
// Интерн-таблицы сущности (HUD_LAYOUT_MAX_INTERNED_STRINGS = 1024 на каждую из трёх: id
// панелей, классы, имена переменных) растут за жизнь сущности и не освобождаются. На открытии
// окна/редактора сущность с таблицей больше порога пересоздаётся (layout/menu.cpp,
// RecycleMenuLayoutIfFull) — закрытая сущность ничего не держит, пересоздание бесплатно.
#define KZ_MENU_INTERN_RECYCLE 900

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
