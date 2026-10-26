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
	// Инструмент гасит только E/R/F. Любая комбинация движения, прыжка,
	// приседания, ходьбы и чужих действий проходит без изменения.
	constexpr std::uint64_t tool = use | reload | close;
	constexpr std::uint64_t movement = (1ull << 16) | (1ull << 17) | (1ull << 18) | (1ull << 19) | (1ull << 20) | (1ull << 21) | (1ull << 22);
	InputLatch guard;
	guard.ObserveCommand(1, movement | use | reload, tool, false);
	assert(guard.Take() == (use | reload));
	const auto activeMask = guard.SuppressionMask(tool, true);
	assert(activeMask == tool);
	assert((activeMask & movement) == 0);
	assert(((movement | use | reload) & ~activeMask) == movement);

	// F закрывает помощник при удержанных E/R: хвост этих кнопок не
	// становится +use/+reload в игре после закрытия.
	guard.ObserveCommand(2, movement | use | reload | close, tool, false);
	assert(guard.Take() == close);
	guard.SuppressionMask(tool, true);
	guard.Transition(tool);
	assert(guard.SuppressionMask(tool, false) == tool);
	guard.ObserveCommand(2, 0, tool, false); // обнулённый backup не снимает guard
	assert(guard.SuppressionMask(tool, false) == tool);
	guard.ObserveCommand(3, movement | use | reload, tool, false);
	assert(guard.SuppressionMask(tool, false) == tool); // F-release остаётся погашенным
	assert(guard.Take() == 0);
	guard.ObserveCommand(4, movement, tool, false); // настоящие отпускания
	assert(guard.SuppressionMask(tool, false) == (use | reload));
	guard.ObserveCommand(5, movement | use, tool, false);
	assert(guard.Take() == use);
	assert(guard.SuppressionMask(tool, false) == 0); // новая E снова игровая

	// Guard не разрастается на движение, даже если все кнопки удержаны.
	for (std::uint64_t combination = 0; combination < 128; combination++)
	{
		InputLatch sample;
		const auto rawMovement = combination << 16;
		sample.ObserveCommand(1, rawMovement | tool, tool, false);
		sample.SuppressionMask(tool, true);
		sample.Transition(tool);
		const auto closedMask = sample.SuppressionMask(tool, false);
		assert((closedMask & rawMovement) == 0);
		assert(((rawMovement | tool) & ~closedMask) == rawMovement);
	}

	// Close() закрытой quickmenu вызывается чужими меню. Он не захватывает
	// их R/F: Transition без собственных controlled keys не заводит guard.
	InputLatch foreign;
	foreign.ObserveCommand(1, reload | close | movement, tool, true);
	foreign.Transition();
	assert(foreign.SuppressionMask(tool, false) == 0);
	assert(foreign.Take() == 0);
	// ЛКМ/ПКМ остаются обычной игре: они не являются действиями инструмента,
	// не попадают в его suppression/exit guard и не теряются на переходах.
	constexpr std::uint64_t attacks = (1ull << 32) | (1ull << 33);
	InputLatch mouse;
	mouse.ObserveCommand(1, movement | attacks, tool, false);
	assert(mouse.Take() == 0);
	auto mask = mouse.SuppressionMask(tool, true);
	assert(((movement | attacks) & ~mask) == (movement | attacks));
	mouse.ObserveCommand(2, movement | attacks | close, tool, false);
	assert(mouse.Take() == close);
	mouse.SuppressionMask(tool, true);
	mouse.Transition(tool);
	mouse.ObserveCommand(2, 0, tool, false); // backup не снимает guard F
	assert(mouse.SuppressionMask(tool, false) == close);
	mouse.ObserveCommand(3, movement | attacks, tool, false);
	assert(mouse.SuppressionMask(tool, false) == close); // release F погашен
	assert(mouse.Take() == 0);
	mouse.ObserveCommand(4, movement, tool, false);
	assert(mouse.SuppressionMask(tool, false) == 0); // release мыши не принадлежит UI
	mouse.ObserveCommand(5, movement | attacks, tool, false);
	assert(mouse.SuppressionMask(tool, false) == 0);
	assert(mouse.Take() == 0);
	// Короткое E не открывает меню; hold срабатывает ровно один раз.
	KZ::quickmenu::HoldGesture hold;
	assert(!hold.Update(true, 10, .7));
	assert(hold.Active());
	assert(!hold.Update(true, 10.5, .7));
	assert(!hold.Update(false, 10.6, .7));
	assert(!hold.Active());
	assert(!hold.Update(true, 11, .7));
	assert(hold.Update(true, 11.71, .7));
	assert(!hold.Active());
	assert(!hold.Update(true, 20, .7));
	// Переход/чужой UI требует реального отпускания.
	hold.Cancel(true);
	assert(!hold.Update(true, 21, .7));
	assert(!hold.Update(false, 22, .7));
	assert(!hold.Update(true, 23, .7));
	assert(hold.Update(true, 23.71, .7));
	// Реальный release внутри пачки usercmd сбрасывает таймер и blocked,
	// даже если до Tick уже пришёл новый press.
	hold.Cancel(false);
	assert(!hold.Update(true, 30, .7));
	hold.Cancel(false); // fresh raw release в OnUsercmds
	assert(!hold.Update(true, 30.6, .7));
	assert(!hold.Update(true, 30.8, .7));
	assert(hold.Update(true, 31.31, .7));
	hold.Cancel(false); // release после срабатывания
	assert(!hold.Update(true, 32, .7));
	assert(hold.Update(true, 32.71, .7));
	// Сброс времени карты не завершает старое удержание.
	hold.Cancel(false);
	assert(!hold.Update(true, 50, .7));
	assert(!hold.Update(true, 1, .7));
	assert(hold.Update(true, 1.71, .7));
	std::puts("quickmenu input transitions, duplicates, movement, attack and exit guard: ok");
}
