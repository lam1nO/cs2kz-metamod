#include "../src/kz/replays/progress_seek.h"
#include <array>
#include <cassert>
#include <limits>
#include <cstdio>

int main()
{
	using KZ::replaysystem::progress::FindTick;
	using KZ::replaysystem::progress::ShouldCapture;
	assert(ShouldCapture(true, true, false));
	assert(ShouldCapture(true, false, true)); // practice without running timer/clock
	assert(ShouldCapture(true, true, true));
	assert(!ShouldCapture(true, false, false));
	assert(!ShouldCapture(false, true, false));
	assert(!ShouldCapture(false, false, true));
	// Ближайший кадр выбирается по позиции, без привязки ко времени игрока.
	std::array<float, 7> distances {0, 10000, 900, 25, 100, 0, 0};
	auto distance = [&](uint32_t tick) { return distances[tick]; };
	assert(FindTick(7, 1, 4, distance) == 3); // кадры вне окна рана не подходят
	distances[3] = -1;                        // пауза, prac или вырез AWR
	assert(FindTick(7, 1, 4, distance) == 4);
	distances[4] = std::numeric_limits<float>::quiet_NaN();
	assert(FindTick(7, 1, 4, distance) == 2);
	assert(!FindTick(7, 1, 4, distance, 20)); // слишком далеко: оставляем начало
	distances[2] = 100;
	distances[3] = 100;
	assert(FindTick(7, 1, 4, distance) == 2); // одинаковое расстояние
	assert(!FindTick(0, 0, 0, distance));
	assert(!FindTick(7, 4, 1, distance));
	assert(!FindTick(7, 1, 7, distance));
	assert(!FindTick(7, 1, 4, distance, -1));
	assert(FindTick(7, 5, 5, distance) == 5); // окно из одного кадра
	std::puts("progress_seek: all tests passed");
}
