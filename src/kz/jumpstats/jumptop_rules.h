// Чистые правила джамптопа (без зависимостей от SDK — для host-теста tests/jumptop_rules_test.cpp).
// Числа JumpType и пороги сверяются со значениями форка static_assert'ами в jumptop.cpp.
#pragma once

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

	// LJ, BH, MBH, WJ, LAJ, LAH, JB.
	inline bool IsTrackedJumpType(int32_t jumpType)
	{
		return jumpType >= typeLongJump && jumpType <= typeJumpbug;
	}

	// Пороги GOKZ (gokz-localdb/db/save_js.sp): дистанция 186..500 (LAJ от 50), без понижения
	// (offset >= -0.03125); ладдер-джамп — исключение: приземление у него ниже отрыва с лестницы.
	inline bool PassesDistanceRules(int32_t jumpType, float distance, float offset)
	{
		if (!IsTrackedJumpType(jumpType) || distance > maxDistance)
		{
			return false;
		}
		if (distance < (jumpType == typeLadderJump ? minLadderDistance : minDistance))
		{
			return false;
		}
		if (jumpType != typeLadderJump && offset < -offsetEpsilon)
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
