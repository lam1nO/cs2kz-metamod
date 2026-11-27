#pragma once
#include <cstdint>
#include <string_view>

namespace KZ::misc::customchangemap
{
// Workshop-карта вне пула тоже допустима: !mcustom на whitelist не должен
// откатываться на стартовую карту. Восстанавливаем только официальный fallback.
constexpr bool ShouldRecoverStartMap(bool enabled, bool requestActive, std::uint64_t currentWorkshopID, double now,
                                    double graceUntil, double retryAt)
{
 return enabled && !requestActive && currentWorkshopID == 0 && now >= graceUntil && now >= retryAt;
}
// Нулевой ID бывает и у официального уровня, и пока движок ещё загружает карту.
constexpr bool MapRequestSuperseded(std::uint64_t previousID, std::string_view previousName,
                                    std::uint64_t currentID, std::string_view currentName)
{
 return currentID ? currentID != previousID : !currentName.empty() && currentName != previousName;
}
}
