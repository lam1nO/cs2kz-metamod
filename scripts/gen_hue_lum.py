#!/usr/bin/env python3
"""gen_hue_lum.py — таблицы попапа цвета окна !options/редактора !hud (color_popup в cyber/options.xml).

Попап даёт 32 пресета (cp0..cp31) и сетку 10 оттенков (ch0..ch9) × 8 яркостей (cv0..cv7). Сервер
произвольного цвета в CSS поставить не может — только класс pal-bg-N/pal-fg-N из palette.css
аддона. Поэтому каждая клетка попапа — ИНДЕКС ПАЛИТРЫ, ближайший (по RGB) к целевому цвету дизайна.
Скрипт читает palette.css, считает ближайшие индексы и переписывает блок между маркерами
`// GEN:color_popup` … `// /GEN:color_popup` в src/kz/hud/layout/panorama_tables.cpp.

Запуск (из корня kz-hud):
  python3 scripts/gen_hue_lum.py [путь/к/palette.css]
По умолчанию palette.css берётся из монорепо ~/cyber/cyber-hud (аддон GYMSTRIKE-KZ).
Повторный запуск идемпотентен.
"""
import colorsys
import os
import re
import sys

DEFAULT_PALETTE = os.path.expanduser(
    "~/cyber/cyber-hud/game-addons/gymstrike-kz/content/panorama/styles/custom_game/cs2kz/palette.css")
TARGET = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "kz", "hud", "layout", "panorama_tables.cpp")

# Пресеты (порядок = cp0..cp31): 8 нейтральных, 16 насыщенных (токены дизайна: accent #00AAFF,
# ahead #4CC38A, behind #FF5C5C, warn #FFB02E и др.), 8 пастельных.
PRESETS = [
    "FFFFFF", "E8EEF2", "CFD8DE", "9AA7B0", "5E6B75", "2A3640", "121A21", "000000",
    "00AAFF", "4CC38A", "FF5C5C", "FFB02E", "FFD84D", "7CFF4C", "00FFD0", "4C8DFF",
    "8A5CFF", "D05CFF", "FF4CA8", "FF7A3D", "FF3030", "30FF60", "30A0FF", "FFFF40",
    "FFB3B3", "FFE0A0", "C8FFB0", "B0F0FF", "B8C8FF", "E0B8FF", "FFC0E0", "F0F0C0",
]
HUES = 10       # ch0..ch9: 0°, 36°, … 324°
LUMS = [0.88, 0.78, 0.68, 0.58, 0.48, 0.38, 0.28, 0.18]  # cv0..cv7: от светлого к тёмному (HSL L)


def load_palette(path):
    css = open(path, encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"\.pal-bg-(\d+)\s*\{\s*background-color:\s*#([0-9a-fA-F]{6})", css):
        out[int(m.group(1))] = tuple(int(m.group(2)[i:i + 2], 16) for i in (0, 2, 4))
    if not out:
        sys.exit(f"no .pal-bg-N in {path}")
    return [out[i] for i in sorted(out)]


def nearest(pal, rgb):
    return min(range(len(pal)), key=lambda i: sum((a - b) ** 2 for a, b in zip(pal[i], rgb)))


def main():
    pal = load_palette(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PALETTE)
    presets = [nearest(pal, tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))) for h in PRESETS]
    grid = []
    for h in range(HUES):
        row = []
        for l in LUMS:
            r, g, b = colorsys.hls_to_rgb(h / HUES, l, 1.0)
            row.append(nearest(pal, (round(r * 255), round(g * 255), round(b * 255))))
        grid.append(row)

    lines = ["// GEN:color_popup — scripts/gen_hue_lum.py, руками не править"]
    lines.append(f"static_global const i32 PRESET_PALETTE[{len(presets)}] = {{{', '.join(map(str, presets))}}};")
    lines.append(f"static_global const i32 HUE_LUM_PALETTE[{HUES}][{len(LUMS)}] =")
    lines.append("{")
    for row in grid:
        lines.append("\t{" + ", ".join(f"{v:3d}" for v in row) + "},")
    lines.append("};")
    lines.append("// /GEN:color_popup")
    block = "\n".join(lines)

    src = open(TARGET, encoding="utf-8").read()
    new, n = re.subn(r"// GEN:color_popup.*?// /GEN:color_popup", lambda _: block, src, flags=re.S)
    if n != 1:
        sys.exit("marker // GEN:color_popup not found exactly once in panorama_tables.cpp")
    if new != src:
        open(TARGET, "w", encoding="utf-8").write(new)
    print(f"ok: {len(pal)} palette entries, {len(presets)} presets ({len(set(presets))} distinct), "
          f"{HUES}x{len(LUMS)} grid ({len({v for r in grid for v in r})} distinct)")


if __name__ == "__main__":
    main()
