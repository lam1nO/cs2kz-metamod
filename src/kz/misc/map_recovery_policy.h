#pragma once
#include <cstdint>

namespace KZ::misc::customchangemap
{
// Workshop-карта вне пула тоже допустима: !mcustom на whitelist не должен
// откатываться на стартовую карту. Восстанавливаем только официальный fallback.
constexpr bool ShouldRecoverStartMap(bool enabled, bool requestActive, std::uint64_t currentWorkshopID, double now,
                                    double graceUntil, double retryAt)
{
 return enabled && !requestActive && currentWorkshopID == 0 && now >= graceUntil && now >= retryAt;
}
}
