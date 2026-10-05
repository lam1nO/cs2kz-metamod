// Host-тест ремонта сабтиков записи со старым багом сброса чанка
// (src/kz/replays/cyb_subtick_repair.h). В сборку плагина не входит, гоняется руками:
//   g++ -std=c++17 -O1 -Wall -Wextra tests/subtick_repair_test.cpp -o /tmp/subtick_repair_test && /tmp/subtick_repair_test
//
// Тест воспроизводит старую цепочку целиком: рекордер с прежним порядком сброса (чанк уходит
// между кадром и его сабтиками, в чанк попадает байт из-за конца вектора счётчиков), склейку
// чанков и UnpackSubtickData старой сборки — и проверяет, что ремонт возвращает исходные сабтики.
#include "../src/kz/replays/cyb_subtick_repair.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>

static constexpr uint32_t MAXM = 4; // MAX_SUBTICK_MOVES в миниатюре

struct Move
{
	float when;
	uint32_t button;
	bool operator==(const Move &o) const
	{
		return when == o.when && button == o.button;
	}
};

struct Sub
{
	uint32_t numSubtickMoves;
	Move subtickMoves[MAXM];
};

using KZ::replaysystem::subtickrepair::RepairExtraSubtick;

// Что видел рекордер: по кадру — его ходы.
static std::vector<std::vector<Move>> Truth(size_t ticks, uint32_t seed)
{
	std::mt19937 rng(seed);
	std::vector<std::vector<Move>> t(ticks);
	uint32_t serial = 1;
	for (auto &v : t)
	{
		const uint32_t n = rng() % 3 == 0 ? rng() % (MAXM + 1) : 0;
		for (uint32_t j = 0; j < n; j++)
		{
			v.push_back(Move {(float)serial, serial});
			serial++;
		}
	}
	return t;
}

// Старый рекордер + старый UnpackSubtickData. flush — FLUSH_INTERVAL_TICKS в миниатюре,
// prefix — кадры вставленного (склеенного) куска до живых, garbage — мусорный байт.
static std::vector<Sub> OldWrite(const std::vector<std::vector<Move>> &truth, size_t flush, size_t prefix, uint8_t garbage)
{
	std::vector<uint8_t> fileCounts;
	std::vector<Move> fileMoves;
	// Склеенный кусок — готовый чанк, у него всё ровно.
	for (size_t i = 0; i < prefix; i++)
	{
		fileCounts.push_back((uint8_t)truth[i].size());
		fileMoves.insert(fileMoves.end(), truth[i].begin(), truth[i].end());
	}
	size_t ticks = 0; // живые кадры в памяти
	std::vector<uint8_t> counts;
	std::vector<Move> moves;
	for (size_t i = prefix; i < truth.size(); i++)
	{
		ticks++;
		if (ticks >= flush)
		{
			// Сброс между кадром и его сабтиками: в чанк — ticks счётчиков, последний из-за конца.
			std::vector<uint8_t> chunk(counts);
			if (chunk.size() < ticks)
			{
				chunk.push_back(garbage);
			}
			fileCounts.insert(fileCounts.end(), chunk.begin(), chunk.end());
			fileMoves.insert(fileMoves.end(), moves.begin(), moves.end());
			ticks = 0;
			counts.clear();
			moves.clear();
		}
		counts.push_back((uint8_t)truth[i].size());
		moves.insert(moves.end(), truth[i].begin(), truth[i].end());
	}
	fileCounts.insert(fileCounts.end(), counts.begin(), counts.end());
	fileMoves.insert(fileMoves.end(), moves.begin(), moves.end());

	// Старый UnpackSubtickData: ходы подряд по счётчикам, без ограничения на MAXM. Запись мимо
	// массива ложится в соседние элементы (как в памяти вектора), чтение за концом ходов — мусор.
	std::vector<Sub> out(fileCounts.size() + 8);
	uint32_t *flat = reinterpret_cast<uint32_t *>(out.data());
	const size_t words = sizeof(Sub) / sizeof(uint32_t);
	size_t offset = 0;
	for (size_t i = 0; i < fileCounts.size(); i++)
	{
		flat[i * words] = fileCounts[i];
		for (uint32_t j = 0; j < fileCounts[i]; j++)
		{
			const Move m = offset < fileMoves.size() ? fileMoves[offset] : Move {-1.0f, 0xDEAD};
			offset++;
			memcpy(&flat[i * words + 1 + j * 2], &m, sizeof(Move));
		}
	}
	out.resize(fileCounts.size());
	// Сжатый файл хранит счётчик как есть (u32), читатель плейбека потом режет до MAXM.
	return out;
}

// Проверка: каждый кадр либо совпадает с правдой, либо пуст (ходы потеряны). Возвращает
// число пустых кадров, у которых в правде ходы были.
static size_t Check(const std::vector<Sub> &subs, const std::vector<std::vector<Move>> &truth)
{
	assert(subs.size() == truth.size());
	size_t dropped = 0;
	for (size_t i = 0; i < truth.size(); i++)
	{
		if (subs[i].numSubtickMoves == 0 && !truth[i].empty())
		{
			dropped++;
			continue;
		}
		assert(subs[i].numSubtickMoves == truth[i].size());
		for (uint32_t j = 0; j < subs[i].numSubtickMoves; j++)
		{
			assert(subs[i].subtickMoves[j] == truth[i][j]);
		}
	}
	return dropped;
}

static void Case(size_t ticks, size_t flush, size_t prefix, uint8_t garbage, uint32_t seed, bool expectLoss)
{
	auto truth = Truth(ticks, seed);
	auto subs = OldWrite(truth, flush, prefix, garbage);
	assert(subs.size() == ticks + 1);
	const size_t garbageIdx = prefix + flush - 1;
	assert(subs[garbageIdx].numSubtickMoves == garbage);
	bool ok = (RepairExtraSubtick<Sub, MAXM>(subs, ticks, garbageIdx));
	assert(ok);
	const size_t dropped = Check(subs, truth);
	if (!expectLoss)
	{
		assert(dropped == 0);
	}
	else
	{
		// Потеряно garbage-MAXM ходов: пустых кадров не больше этого числа.
		assert(dropped <= (size_t)(garbage - MAXM));
	}
	printf("ok ticks=%zu flush=%zu prefix=%zu garbage=%u dropped=%zu\n", ticks, flush, prefix, garbage, dropped);
}

int main()
{
	// Мусорный байт 0 — ходы не съедены, достаточно выкинуть элемент.
	Case(500, 100, 0, 0, 1, false);
	// Мусор в пределах MAXM — съеденные ходы сохранились в самом мусорном элементе.
	Case(500, 100, 0, 3, 2, false);
	Case(500, 100, 0, MAXM, 3, false);
	// Мусор больше MAXM (на живом файле был 238 при 36) — часть ходов потеряна, остальное ровно.
	Case(500, 100, 0, 40, 4, true);
	Case(5000, 1000, 0, 238, 5, true);
	// Несколько сбросов — лишний элемент всё равно один, на первом сбросе.
	Case(1000, 100, 0, 17, 6, true);
	// Склеенный кусок впереди: мусор на prefix + flush - 1.
	Case(800, 100, 250, 9, 7, true);
	Case(800, 100, 250, 2, 8, false);

	// Отказы: размеры уже ровные; мусорный индекс последний или за концом.
	{
		std::vector<Sub> s(10);
		assert(!(RepairExtraSubtick<Sub, MAXM>(s, 10, 3)));
		std::vector<Sub> t(11);
		assert(!(RepairExtraSubtick<Sub, MAXM>(t, 10, 10)));
		assert(!(RepairExtraSubtick<Sub, MAXM>(t, 10, 50)));
		assert(t.size() == 11);
	}
	printf("subtick_repair_test: all ok\n");
	return 0;
}
