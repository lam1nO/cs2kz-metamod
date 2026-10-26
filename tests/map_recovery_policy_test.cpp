#include "../src/kz/misc/map_recovery_policy.h"
#include <cassert>
using KZ::misc::customchangemap::ShouldRecoverStartMap;
int main()
{
 // Стартовая отсрочка, активный !mcustom и late load не должны запускать смену.
 assert(!ShouldRecoverStartMap(true, false, 0, 44, 45, 0));
 assert(!ShouldRecoverStartMap(true, true, 0, 300, 45, 0));
 assert(!ShouldRecoverStartMap(false, false, 0, 300, 45, 0));
 // После исчерпания заявки Dust2 не остаётся навсегда: пауза и новая попытка.
 assert(!ShouldRecoverStartMap(true, false, 0, 284, 45, 285));
 assert(ShouldRecoverStartMap(true, false, 0, 285, 45, 285));
 assert(ShouldRecoverStartMap(true, false, 0, 1800, 45, 285));
 // Любая Workshop-карта, включая whitelist !mcustom вне пула, остаётся на месте.
 assert(!ShouldRecoverStartMap(true, false, 3456225822ULL, 1800, 45, 285));
}
