#include "../src/kz/hud/layout/layout_fallback.h"
#include <cassert>
#include <string>
int main()
{
    auto onlyLegacy = [](const char *p) { return std::string(p) == "legacy"; };
    auto both = [](const char *) { return true; };
    auto neither = [](const char *) { return false; };
    assert(SelectHudLayout("shared", "legacy", onlyLegacy) == "legacy");
    assert(SelectHudLayout("shared", "legacy", both) == "shared");
    assert(SelectHudLayout("shared", "legacy", neither).empty());
    // Старые двухаргументные вызовы не меняются и не требуют нового ресурсного контракта.
    assert(SelectHudLayout("legacy", "", neither) == "legacy");
}
