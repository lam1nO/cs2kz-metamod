// Чистые правила джамптопа (без зависимостей от SDK — для host-теста tests/jumptop_rules_test.cpp).
// Числа JumpType и пороги сверяются со значениями форка static_assert'ами в jumptop.cpp.
#pragma once

#include <cmath>
#include <cstdint>

namespace KZ::jumptop::rules
{
	constexpr int32_t typeLongJump = 0;
	constexpr int32_t typeLadderJump = 4;
	constexpr int32_t typeJumpbug = 6;
	constexpr float minDistance = 186.0f;    // JS_MIN_BLOCK_DISTANCE
	constexpr float minLadderDistance = 50.0f; // JS_MIN_LAJ_BLOCK_DISTANCE
	constexpr float maxDistance = 500.0f;
	constexpr float offsetEpsilon = 0.03125f; // JS_EPSILON
	constexpr float maxLadderDrop = 2.0f;     // JS_MAX_LADDERJUMP_OFFSET

	// LJ, BH, MBH, WJ, LAJ, LAH, JB.
	inline bool IsTrackedJumpType(int32_t jumpType)
	{
		return jumpType >= typeLongJump && jumpType <= typeJumpbug;
	}

	// Пороги GOKZ (gokz-localdb/db/save_js.sp): дистанция 186..500 (LAJ от 50), без понижения
	// (offset >= -0.03125). LAJ: GOKZ требует приземления на высоте верха лестницы (TraceLadderOffset);
	// у нас отрыв меряется от точки схода с лестницы, поэтому допуск вниз 2 юнита — как в jumpstats.
	inline bool PassesDistanceRules(int32_t jumpType, float distance, float offset)
	{
		// NaN не проходит ни одно сравнение ниже — без этой проверки он попал бы в топ.
		if (!std::isfinite(distance) || !std::isfinite(offset))
		{
			return false;
		}
		if (!IsTrackedJumpType(jumpType) || distance > maxDistance)
		{
			return false;
		}
		if (distance < (jumpType == typeLadderJump ? minLadderDistance : minDistance))
		{
			return false;
		}
		if (offset < (jumpType == typeLadderJump ? -maxLadderDrop : -offsetEpsilon))
		{
			return false;
		}
		return true;
	}

	// Лучше ли (block, distance) прошлого рекорда. Блочный топ — по блоку, затем по дистанции.
	inline bool IsBetter(bool isBlock, int32_t block, double distance, int32_t oldBlock, double oldDistance)
	{
		if (isBlock)
		{
			return block > oldBlock || (block == oldBlock && distance > oldDistance);
		}
		return distance > oldDistance;
	}
} // namespace KZ::jumptop::rules
