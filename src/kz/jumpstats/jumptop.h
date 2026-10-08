/*
 * jumptop.h — топ по прыжкам (!jumptop), как в GOKZ: личные рекорды и рекорд сервера по
 * LJ/BH/MBH/WJ/LAJ/LAH/JB в каждом режиме, отдельно по дистанции и по блоку.
 *
 * После каждого завершённого прыжка: годен ли он в топ (см. IsEligible), бьёт ли личный рекорд
 * (кэш PB игрока грузится на заходе). Новый PB пишется в общую MySQL флота (таблица Jumptop)
 * вместе с полной статистикой прыжка (JSON по стрейфам — для разбора на АХК), а реплей прыжка
 * (5 с до приземления + 2 с после, полный RpJumpStats с вызовами AirAccelerate) — в
 * JumptopReplays. Место в топе считает база; #1 объявляется всему серверу.
 *
 * Команды: !jumptop (!jstop, !jt) — меню топа, выбор строки = статистика + реплей ботом;
 * !jspb [ник] — свои/чужие PB; !jumpinfo <id> — полная статистика строки в консоль;
 * серверная консоль: kz_jumptop_remove / kz_jumptop_restore <id> — снять/вернуть прыжок.
 */
#pragma once

#include "common.h"
#include "kz/jumpstats/kz_jumpstats.h"
#include "utils/uuid.h"
#include "jumptop_rules.h"

#include <vector>

class KZPlayer;

namespace KZ::jumptop
{
	// Сколько строк в топе (как JS_TOP_RECORD_COUNT в GOKZ).
	constexpr i32 topCount = 20;

	void Init();

	// Зовётся из KZJumpstatsService::EndJump после записи прыжка в реплей (вне prac).
	void OnJumpFinish(KZPlayer *player, Jump *jump);

	// Готовый буфер реплея прыжка, помеченного джамптопом (KZRecordingService). Главный поток.
	void StoreReplay(const UUID_t &uuid, const std::vector<char> &buffer);

	using rules::IsBetter;
	using rules::IsTrackedJumpType;
	using rules::PassesDistanceRules;
} // namespace KZ::jumptop
