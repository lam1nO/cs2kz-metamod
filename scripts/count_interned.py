#!/usr/bin/env python3
"""count_interned.py — бюджет интернирования сущности окна !options / редактора !hud.

custom_hud_layout интернирует id панелей, имена классов и имена dialog-переменных в ТРИ таблицы,
каждая не больше HUD_LAYOUT_MAX_INTERNED_STRINGS = 1024 (sdk/entity/ccscustomhudlayout.h). Скрипт
собирает строки, которые плагин МОЖЕТ отправить на сущность меню (cyber/options.xml):
  - литералы id/классов/переменных в вызовах SetMenu*/SetLayout*/ApplyLayoutElementTo из
    layout/menu.cpp и layout/editor.cpp;
  - слоты вида SLOT_ID(fn, "tab%i") × ёмкость разметки (menu.h);
  - id реплики худа x_* (таблицы KEY_PANELS/KEY_GLYPHS/PBWR_* из layout/mhud.cpp + EDITOR_REPLICA);
  - семейства значимых классов худа (x--N/y--N, font-size--N, opacity--N, pal-fg/pal-bg/grad/gbg,
    font-family--*, key-size--N, key-glow-N) — худший случай за жизнь сущности.
Печатает число по каждой таблице и сумму; порог — 1024 НА ТАБЛИЦУ. Запуск из корня kz-hud:
  python3 scripts/count_interned.py
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
LAYOUT = os.path.join(ROOT, "src", "kz", "hud", "layout")
LIMIT = 1024


def read(name):
    return open(os.path.join(LAYOUT, name), encoding="utf-8").read()


menu_h = read("menu.h")
cap = {m.group(1): int(m.group(2)) for m in re.finditer(r"#define (KZ_\w+)\s+(\d+)", menu_h)}
menu, editor, mhud = read("menu.cpp"), read("editor.cpp"), read("mhud.cpp")
src = menu + editor

panels, classes, vars_ = set(), set(), set()

# Литералы в вызовах: первый аргумент после layout — id, дальше класс или переменная.
for m in re.finditer(r"SetMenu(Bool)?Class\(layout,\s*\"([\w]+)\",\s*\"([\w-]+)\"", src):
    panels.add(m.group(2)); classes.add(m.group(3))
for m in re.finditer(r"SetMenuSwapClass\(layout,\s*\"([\w]+)\"", src):
    panels.add(m.group(1))
for m in re.finditer(r"SetMenuVar\(layout,\s*\"([\w]+)\",\s*\"([\w]+)\"", src):
    panels.add(m.group(1)); vars_.add(m.group(2))
for m in re.finditer(r"SetMenu(?:Bool)?Class\(layout,\s*[^,]+,\s*\"([\w-]+)\"", src):
    classes.add(m.group(1))
for m in re.finditer(r"\"(t-[a-z]+)\"", src):
    classes.add(m.group(1))
# Подписи панели свойств {"p_pos", ...}.
for m in re.finditer(r"\{\"(p_\w+)\",\s*\"HUD Editor", editor):
    vars_.add(m.group(1))

# Слоты разметки.
slot_caps = {
    "tab%i": cap["KZ_MENU_TABS"], "t%i": cap["KZ_MENU_TABS"],
    "sec%i": cap["KZ_MENU_ROWS"], "s%i": cap["KZ_MENU_ROWS"], "row%i": cap["KZ_MENU_ROWS"],
    "lbl%i": cap["KZ_MENU_ROWS"], "l%i": cap["KZ_MENU_ROWS"], "sub%i": cap["KZ_MENU_ROWS"], "d%i": cap["KZ_MENU_ROWS"],
    "tg%i": cap["KZ_MENU_ROWS"], "st%i_val": cap["KZ_MENU_ROWS"], "v%i": cap["KZ_MENU_ROWS"],
    "bt%i": cap["KZ_MENU_ROWS"], "b%i": cap["KZ_MENU_ROWS"], "cl%i": cap["KZ_MENU_ROWS"],
    "fn%i": cap["KZ_MENU_ROWS"], "f%i": cap["KZ_MENU_ROWS"],
    "cp%i": cap["KZ_MENU_PRESETS"], "ch%i": cap["KZ_MENU_HUES"], "cv%i": cap["KZ_MENU_LUMS"],
    "li%i": cap["KZ_MENU_LIST_ROWS"], "lf%i": cap["KZ_MENU_FAMILIES"],
    "el%i": cap["KZ_EDITOR_LIST_ROWS"], "et%i": cap["KZ_EDITOR_LIST_ROWS"], "en%i": cap["KZ_EDITOR_LIST_ROWS"],
}
var_slots = {"t%i", "s%i", "l%i", "d%i", "v%i", "b%i", "f%i", "en%i"}
for m in re.finditer(r"SLOT_ID\(\w+,\s*\"([^\"]+)\"\)", src):
    fmt = m.group(1)
    n = slot_caps.get(fmt)
    if n is None:
        sys.exit(f"unknown slot {fmt}: add its capacity to count_interned.py")
    target = vars_ if fmt in var_slots else panels
    if fmt == "li%i":  # li%i — и id строки, и имя её переменной
        vars_.update(f"li{i}" for i in range(n))
    target.update(fmt.replace("%i", str(i)) for i in range(n))
rows, segs = cap["KZ_MENU_ROWS"], cap["KZ_MENU_SEGS"]
panels.update(f"sg{i}_{k}" for i in range(rows) for k in range(segs))
vars_.update(f"g{i}_{k}" for i in range(rows) for k in range(segs))

# Реплика худа: id mhud.cpp с префиксом x_ и корни e_*/x_* из EDITOR_REPLICA.
def array(name):
    m = re.search(name + r"\[\]?[^=]*=\s*\{([^}]*)\}", mhud)
    return re.findall(r"\"([\w]+)\"", m.group(1)) if m else []
for name in ("KEY_PANELS", "KEY_GLYPHS", "PBWR_CELL_IDS", "PBWR_TIME_IDS", "PBWR_CAP_IDS"):
    ids = array(name)
    if not ids:
        sys.exit(f"{name} not found in mhud.cpp")
    panels.update("x_" + i for i in ids)
panels.update("x_" + i for i in ("mhud_keys", "mhud_delta", "mhud_pos", "mhud_ang", "pw_nub", "pw_pro", "mhud_progress_pct",
                                  "mhud_progress_fill"))
vars_.update(array("PBWR_VARS") + ["delta", "pos", "ang", "progress_cap"])
for m in re.finditer(r"\{\"(e_\w+)\",\s*\"(x_\w+)\",\s*\"(\w+)\",\s*\"(tag_\w+)\"", editor):
    panels.update([m.group(1), m.group(2)]); vars_.update([m.group(3), m.group(4)])
# Доп. строки цветов панели свойств (EDITOR_XROW_IDS/EDITOR_XC_IDS/EDITOR_XROW_VARS в editor.cpp).
for name, target in (("EDITOR_XROW_IDS", panels), ("EDITOR_XC_IDS", panels), ("EDITOR_XROW_VARS", vars_),
                     ("EDITOR_TROW_IDS", panels), ("EDITOR_TG_IDS", panels), ("EDITOR_TROW_VARS", vars_),
                     ("EDITOR_SG_IDS", panels), ("EDITOR_SG_VARS", vars_), ("EDITOR_STD_ROW_IDS", panels)):
    m = re.search(name + r"\[[^=]*=\s*\{([^}]*)\}", editor)
    if not m:
        sys.exit(f"{name} not found in editor.cpp")
    target.update(re.findall(r"\"([\w]+)\"", m.group(1)))
# w-p--38 — заливка «Прогресса» на реплике (плейсхолдер 38%, один класс на всю жизнь сущности).
classes.update(["w-p--38"])
classes.update(["hidden", "outline", "pressed", "d-ahead", "d-behind", "hide-idle", "keys-underscore", "keys-noborder",
                "keys-noglow", "keys-nofill", "keys-letters", "keys-square"])

static_classes = len(classes)
# Семейства классов (худший случай за жизнь сущности).
families = {
    "x--N/y--N (редактор: -50..50)": 2 * 101,
    "font-size--Npx (8..100)": 93,
    "opacity--Npct (0..100)": 101,
    "font-family--* (таблица шрифтов)": len(re.findall(r"\"font-family--", read("panorama_tables.cpp"))),
    "pal-fg-N + grad-N (цвет текста)": 160 + 40,
    "pal-bg-N + gbg-N (свотчи)": 160 + 40,
    "key-size--N (8..100)": 93,
    "key-glow-N (сплошные)": 160,
}

print(f"panel ids:      {len(panels):5d} / {LIMIT}")
print(f"dialog vars:    {len(vars_):5d} / {LIMIT}")
print(f"classes static: {static_classes:5d}")
total_classes = static_classes + sum(families.values())
for k, v in families.items():
    print(f"  + {k}: {v}")
print(f"classes worst:  {total_classes:5d} / {LIMIT}  (реально за сеанс — единицы из каждого семейства)")
print(f"TOTAL (static literals + slots): {len(panels) + len(vars_) + static_classes}")
worst = max(len(panels), len(vars_), total_classes)
print("OK" if worst <= LIMIT else
      f"WARN: худший случай таблицы классов {total_classes} > {LIMIT} — страхует пересоздание сущности "
      f"на открытии (KZ_MENU_INTERN_RECYCLE={cap.get('KZ_MENU_INTERN_RECYCLE')})")
