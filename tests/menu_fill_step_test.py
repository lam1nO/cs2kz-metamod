#!/usr/bin/env python3
"""Host regression for the actual native tile-apply body, with an engine test double.
No SDK/CS2 required. --source can point at a pre-fix gsmenu_fill.cpp to prove RED.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1] / 'src/kz/hud/layout/gsmenu_fill.cpp')
args = parser.parse_args()
source = args.source.read_text()
name = 'ApplyNext' if 'bool ApplyNext(' in source else 'Apply'
start = source.index(('bool ' if name == 'ApplyNext' else 'void ') + name + '(')
brace = source.index('{', start)
depth = 1
end = brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
apply_body = source[start:end]
fixture = r'''
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <iostream>
using i32 = int;
#define V_snprintf std::snprintf
#define KZ_LOG_WARN(...) ((void)0)
struct KZPlayer {};
enum Status { k_eHudPanelClassStatus_HasClass, k_eHudPanelClassStatus_DoesNotHaveClass };
struct TileOut {
    bool set = false, done = false, iswr = false, nowr = false;
    std::string pb, gap, wr, wrn;
};
struct SlotFill {
    std::vector<std::string> tiles;
    bool active = false, queued = false;
    size_t nextTile = 0;
    std::vector<TileOut> applied;
};
SlotFill g_fill[1];
struct WrCache {};
struct CCSCustomHudLayout {
    int writes = 0;
    std::string lastPanel;
    bool SetHasClass(const char *panel, const char *, Status) { ++writes; lastPanel = panel; return true; }
    bool SetDialogVariableString(const char *panel, const char *, const char *) { ++writes; lastPanel = panel; return true; }
    CCSCustomHudLayout *GetGlobalLayoutState() { return this; }
    void MarkFullChanged() {}
} layout;
bool recycle = false;
namespace KZ::gsmenu {
    CCSCustomHudLayout *ExtLayoutOf(KZPlayer *) {
        if (recycle) g_fill[0] = SlotFill{};
        return &layout;
    }
}
TileOut Compute(const SlotFill &, const WrCache &, const std::string &) {
    TileOut t;
    t.set = t.done = t.iswr = t.nowr = true;
    t.pb = t.gap = t.wr = t.wrn = "data";
    return t;
}
'''
main = r'''
int main() {
    KZPlayer player;
    WrCache wr;
    auto &f = g_fill[0];
    f.tiles.assign(400, "kz_test");
    f.active = f.queued = true;
    CALL;
    // Старый Apply записывал 2800 значений за один неделимый вызов.
    assert(layout.writes <= 7);
    assert(layout.writes == 7 && layout.lastPanel == "gt0");
    for (int i = 1; i < 400; ++i) CALL;
    assert(layout.writes == 2800 && !f.queued && f.nextTile == 400);
    CALL;
    assert(layout.writes == 2800);
    // Та же сетка: диф-кэш сохраняется, повторных сетевых записей нет.
    f.nextTile = 0; f.queued = true;
    for (int i = 0; i < 400; ++i) CALL;
    assert(layout.writes == 2800);
    // Пустое имя очищает прежнюю плитку, включая PB/WR и классы.
    f.tiles[0].clear(); f.nextTile = 0; f.queued = true;
    CALL;
    assert(layout.writes == 2807);
    // Recycle из ExtLayoutOf сбрасывает fill до чтения элементов вектора.
    recycle = true;
    CALL;
    assert(layout.writes == 2807 && !f.queued && !f.active);
    std::cout << "menu_fill_step: 400 tiles, diff, empty name, recycle PASS\n";
}
'''.replace('CALL', name + '(&player, 0, wr)')
with tempfile.TemporaryDirectory(prefix='kz-fill-test-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    binary = Path(tmp) / 'test'
    cpp.write_text(fixture + apply_body + main)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
