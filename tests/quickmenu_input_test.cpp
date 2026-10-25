#include "../src/kz/hud/layout/quickmenu_input.h"
#include <cassert>
#include <cstdio>

using KZ::quickmenu::InputLatch;

int main()
{
	constexpr std::uint64_t use = 1, close = 2, next = 4, unrelated = 8;
	constexpr std::uint64_t nav = use | close | next;
	InputLatch input;

	// Несколько usercmd до одного server tick: короткое нажатие не теряется.
	input.Observe(use, nav, false);
	input.Observe(0, nav, false);
	assert(input.Take() == use);
	assert(input.Take() == 0);

	// E, удержанная при переходе из меню в инструмент, не ставит Paint сама.
	input.Observe(use, nav, false);
	input.Transition();
	input.Observe(use, nav, false);
	assert(input.Take() == 0);
	input.Observe(0, nav, false);
	input.Observe(use, nav, false);
	assert(input.Take() == use);

	// Чужое меню забрало ввод: накопленные события отброшены; F, закрывшая
	// радио-меню, не открывает quickmenu тем же нажатием на следующем тике.
	input.Observe(next, nav, false);
	input.Observe(close, nav, true);
	assert(input.Take() == 0);
	input.Observe(close, nav, false);
	assert(input.Take() == 0);
	input.Observe(0, nav, false);
	input.Observe(close, nav, false);
	assert(input.Take() == close);

	// Во время чужого захвата отслеживаются и отпускания: возврат после
	// отпущенной F позволяет следующему новому нажатию открыть панель.
	input.Observe(0, nav, true);
	input.Observe(close | unrelated, nav, false);
	assert(input.Take() == close);
	input.Observe(close | unrelated, nav, false);
	assert(input.Take() == 0);
	std::puts("quickmenu input transitions: ok");
}
