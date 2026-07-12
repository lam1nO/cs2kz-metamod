#pragma once
#include "../kz.h"

namespace KZ::spec
{
	// Меню выбора цели !spec при неоднозначной подстроке (cs2menus, одноразовый выбор).
	// candidates — первые count совпадений; total — полное число (для подсказки
	// «уточни подстроку»). Без cs2menus — фолбэк: спек первого кандидата.
	void OpenSpectateMenu(KZPlayer *player, KZPlayer **candidates, i32 count, i32 total);
} // namespace KZ::spec
