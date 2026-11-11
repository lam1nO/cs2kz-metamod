#include "../src/kz/hud/layout/menu_work_queue.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

int main()
{
	MenuWorkQueue<4> q;
	long long now = 0;
	auto clock = [&] { return now; };
	std::vector<std::string> got;
	for (int i = 0; i < 100; ++i)
	{
		q.Push(0, "a" + std::to_string(i));
		q.Push(1, "b" + std::to_string(i));
	}
	auto apply = [&](int slot, const std::string &op) { got.push_back(std::to_string(slot) + op); };
	auto noFill = [](int) { return false; };
	// Весь сервер, а не 128 операций на каждого игрока.
	assert(q.Run(apply, noFill, clock) == 128);
	assert(got.size() == 128);
	for (int i = 0; i < 64; ++i)
	{
		assert(got[2 * i] == "0a" + std::to_string(i));
		assert(got[2 * i + 1] == "1b" + std::to_string(i));
	}
	got.clear();
	auto slowApply = [&](int slot, const std::string &op)
	{
		apply(slot, op);
		now += 250;
	};
	assert(q.Run(slowApply, noFill, clock) == 4);
	assert(got.size() == 4);
	// Закрытие, recycle и повторное использование слота выбрасывают его старые операции.
	q.Clear(0);
	q.Clear(1);
	q.Push(0, "fresh");
	got.clear();
	assert(q.Run(apply, noFill, clock) == 1);
	assert(got == std::vector<std::string> {"0fresh"});
	assert(q.Run(apply, noFill, clock) == 0);
	// PB/WR делит тот же бюджет; tile не обгоняет batch своего слота.
	q.Push(0, "grid");
	int fills = 0;
	auto fill = [&](int slot)
	{
		if (slot != 0 || fills >= 200)
		{
			return false;
		}
		assert(!got.empty() && got.back() == "0grid");
		++fills;
		return true;
	};
	assert(q.Run(apply, fill, clock) == 128);
	assert(fills == 127);
	// Пустые слоты не расходуют бюджет и не вызывают бесконечного обхода.
	q.Clear(0);
	now = 0;
	fills = 0;
	auto slowFill = [&](int slot)
	{
		if (slot != 3)
		{
			return false;
		}
		++fills;
		now += 600;
		return true;
	};
	assert(q.Run(apply, slowFill, clock) == 2);
	assert(fills == 2); // Одна неделимая операция вправе пересечь deadline.
	// Recycle прямо из Apply не инвалидирует текущую строку и не исполняет хвост.
	q.Push(0, "recycle");
	q.Push(0, "obsolete");
	int applied = 0;
	auto recycleApply = [&](int slot, const std::string &op)
	{
		assert(op == "recycle");
		q.Clear(slot);
		assert(op == "recycle");
		++applied;
	};
	assert(q.Run(recycleApply, noFill, clock) == 1);
	assert(applied == 1);

	std::cout << "menu_work_queue: budgets, fairness, FIFO, cancellation, fill PASS\n";
}
