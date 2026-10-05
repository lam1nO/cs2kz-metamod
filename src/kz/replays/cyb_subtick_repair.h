#pragma once
// cybershoke: ремонт сабтиков записи, сделанной сборкой с багом сброса чанка (до фикса в
// Recorder::PushData). Чистая функция без зависимостей движка — гоняется хост-тестом
// tests/subtick_repair_test.cpp.
//
// Что сломано в такой записи. Первый сброс чанка рекордера случался, когда кадров было
// FLUSH_INTERVAL_TICKS, а счётчиков сабтиков — на один меньше: в чанк уходил байт из-за конца
// вектора счётчиков (мусор), а настоящий счётчик сброшенного кадра — первым в следующий чанк.
// Ходов (moves) мусорному счётчику не досталось. При записи файла UnpackSubtickData раздавал ходы
// по счётчикам подряд, поэтому:
//   * элементов сабтиков на 1 больше, чем кадров; лишний (мусорный) стоит на индексе
//     garbageIdx = <первый кадр рекордера> + FLUSH_INTERVAL_TICKS - 1;
//   * мусорный элемент «съел» g = его счётчик ходов из общего потока; у себя он сохранил только
//     первые min(g, MAX) (остальные писались мимо массива и затёрлись соседями) — эти g-MAX ходов
//     потеряны;
//   * у каждого элемента после него счётчик верный (сдвинут на один индекс), а ходы взяты из
//     потока со сдвигом g.
//
// Ремонт: выкинуть мусорный элемент и раздать ходы заново, восстановив поток
// [сохранённые ходы мусорного] + [g-MAX потерянных] + [ходы всех элементов после него].
// Кадрам, чьи ходы задевают потерянный кусок или выходят за конец потока, ставим 0 ходов —
// это лишь «сабтиков не было», а не выдуманные нажатия.
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace KZ::replaysystem::subtickrepair
{
	// Element: тип с полями numSubtickMoves (целое) и subtickMoves[maxMoves].
	// Возвращает false (и не трогает subs), если запись не похожа на ожидаемую поломку.
	template<typename Element, uint32_t maxMoves>
	bool RepairExtraSubtick(std::vector<Element> &subs, size_t numTicks, size_t garbageIdx)
	{
		// Мусорный элемент не может быть последним: за ним обязан стоять настоящий счётчик
		// сброшенного кадра.
		if (subs.size() != numTicks + 1 || garbageIdx + 1 >= subs.size())
		{
			return false;
		}
		using Move = typename std::remove_reference<decltype(subs[0].subtickMoves[0])>::type;

		const uint32_t g = (uint32_t)subs[garbageIdx].numSubtickMoves;
		const uint32_t kept = g < maxMoves ? g : maxMoves;

		// Поток ходов начиная с мусорного элемента. lost[k] — ход k неизвестен.
		std::vector<Move> stream;
		std::vector<bool> lost;
		for (uint32_t j = 0; j < kept; j++)
		{
			stream.push_back(subs[garbageIdx].subtickMoves[j]);
			lost.push_back(false);
		}
		for (uint32_t j = kept; j < g; j++)
		{
			stream.push_back(Move {});
			lost.push_back(true);
		}
		for (size_t i = garbageIdx + 1; i < subs.size(); i++)
		{
			const uint32_t c = (uint32_t)subs[i].numSubtickMoves;
			const uint32_t n = c < maxMoves ? c : maxMoves;
			for (uint32_t j = 0; j < n; j++)
			{
				stream.push_back(subs[i].subtickMoves[j]);
				lost.push_back(false);
			}
		}

		size_t offset = 0;
		for (size_t i = garbageIdx + 1; i < subs.size(); i++)
		{
			const uint32_t c = (uint32_t)subs[i].numSubtickMoves;
			const uint32_t n = c < maxMoves ? c : maxMoves;
			bool ok = offset + n <= stream.size();
			for (uint32_t j = 0; ok && j < n; j++)
			{
				ok = !lost[offset + j];
			}
			Element &dst = subs[i];
			if (ok)
			{
				for (uint32_t j = 0; j < n; j++)
				{
					dst.subtickMoves[j] = stream[offset + j];
				}
				dst.numSubtickMoves = n;
			}
			else
			{
				dst.numSubtickMoves = 0;
			}
			offset += n;
		}
		subs.erase(subs.begin() + (std::ptrdiff_t)garbageIdx);
		return true;
	}
} // namespace KZ::replaysystem::subtickrepair
