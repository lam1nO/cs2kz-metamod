#include "../src/kz/hud/layout/quickmenu_input.h"
#include <cassert>
#include <cstdio>

using KZ::quickmenu::InputLatch;

int main()
{
	constexpr std::uint64_t use = 1, close = 2, next = 4, unrelated = 8;
	constexpr std::uint64_t nav = use | close | next;
	InputLatch input;
	std::int32_t command = 0;

	// Несколько usercmd до одного server tick: короткое нажатие не теряется.
	input.ObserveCommand(++command, use, nav, false);
	input.ObserveCommand(++command, 0, nav, false);
	assert(input.Take() == use);
	assert(input.Take() == 0);

	// E, удержанная при переходе из меню в инструмент, не ставит Paint сама.
	input.ObserveCommand(++command, use, nav, false);
	input.Transition();
	input.ObserveCommand(++command, use, nav, false);
	assert(input.Take() == 0);
	input.ObserveCommand(++command, 0, nav, false);
	input.ObserveCommand(++command, use, nav, false);
	assert(input.Take() == use);

	// Чужое меню забрало ввод: накопленные события отброшены; F, закрывшая
	// радио-меню, не открывает quickmenu тем же нажатием на следующем тике.
	input.ObserveCommand(++command, next, nav, false);
	input.ObserveCommand(++command, close, nav, true);
	assert(input.Take() == 0);
	input.ObserveCommand(++command, close, nav, false);
	assert(input.Take() == 0);
	input.ObserveCommand(++command, 0, nav, false);
	input.ObserveCommand(++command, close, nav, false);
	assert(input.Take() == close);

	// Во время чужого захвата отслеживаются и отпускания: возврат после
	// отпущенной F позволяет следующему новому нажатию открыть панель.
	input.ObserveCommand(++command, 0, nav, true);
	input.ObserveCommand(++command, close | unrelated, nav, false);
	assert(input.Take() == close);
	input.ObserveCommand(++command, close | unrelated, nav, false);
	assert(input.Take() == 0);
	// R уже вернула Paint -> Drawer. Повторный backup того же cmd обнулён
	// фильтром; он не означает отпускание R и не должен закрыть Drawer.
	constexpr std::uint64_t reload = 16;
	constexpr std::uint64_t allNav = nav | reload;
	InputLatch dedupe;
	dedupe.ObserveCommand(100, reload, allNav, false);
	assert(dedupe.Take() == reload);
	dedupe.Transition();
	assert(dedupe.lastCommand == 100);
	dedupe.ObserveCommand(100, 0, allNav, false); // тот же cmd после маскирования
	dedupe.ObserveCommand(101, reload, allNav, false);
	assert(dedupe.Take() == 0);
	dedupe.ObserveCommand(99, 0, allNav, false); // старый backup с отпусканием
	dedupe.ObserveCommand(102, reload, allNav, false);
	assert(dedupe.Take() == 0);

	// Только настоящее отпускание в НОВОЙ команде разрешает новый edge.
	dedupe.ObserveCommand(103, 0, allNav, false);
	dedupe.ObserveCommand(104, reload, allNav, false);
	assert(dedupe.Take() == reload);
	dedupe.Transition();
	dedupe.ObserveCommand(103, close, allNav, false); // старое F не закрывает UI
	assert(dedupe.Take() == 0);
	dedupe.ObserveCommand(105, close, allNav, false);
	assert(dedupe.Take() == close);
	dedupe.Transition();
	dedupe.ObserveCommand(105, 0, allNav, false); // обнулённый дубликат F
	dedupe.ObserveCommand(106, close, allNav, false);
	assert(dedupe.Take() == 0);

	// Даже если чужая UI получила только backup-команду, старые pending
	// события ей не передаются и после возврата не исполняются нашим меню.
	dedupe.ObserveCommand(107, next, allNav, false);
	dedupe.ObserveCommand(106, 0, allNav, true);
	assert(dedupe.Take() == 0);
	assert(dedupe.lastCommand == 107);
	assert(dedupe.held == next);
	dedupe = InputLatch(); // новый слот/карта принимает новую нумерацию
	dedupe.ObserveCommand(1, use, allNav, false);
	assert(dedupe.Take() == use);
	std::puts("quickmenu input transitions and duplicate commands: ok");
}
