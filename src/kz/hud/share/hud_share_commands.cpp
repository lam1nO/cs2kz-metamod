// Чат-команды обмена настройками худа: !hudshare (выдать код), !hudget <код> (применить),
// !hudtake (забрать худ наблюдаемого), !hudundo (откатить последнее применение). Регистрация — тем же путём, что соседние команды
// худа (SCMD, utils/simplecmds.h): консольное имя kz_*, чат-триггер ! получается сам.
// Ядро (снимок/валидация/применение/предохранители) — hud_share.cpp; здесь только транспорт.
//
// Общего SCMD_COOLDOWN (0.2 с, utils/simplecmds.cpp) для !hudshare НЕ достаточно: это ЗАПИСЬ в
// общую MySQL флота, и бинд давал бы ~5 INSERT/с. Поэтому у выдачи кода свой кулдаун
// (KZ::hudshare::TakeShareCooldown — он живёт в ядре вместе с остальным per-slot состоянием), а
// сверху — квота живых кодов на владельца в самой базе (queries/hudshares.h). Чтению (!hudget)
// и откату (!hudundo) хватает общего: !hudundo базу не трогает вовсе, !hudget только читает.
#include "kz/hud/share/hud_share.h"
#include "kz/db/kz_db.h"
#include "kz/language/kz_language.h"
#include "kz/option/kz_option.h"
#include "utils/logging.h"
#include "utils/simplecmds.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

#include "tier1/strtools.h"

#include "tier0/memdbgon.h"

SCMD(kz_hudshare, SCFL_HUD | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	const u64 steamID = player->GetSteamId64();
	if (!steamID || !player->GetClient())
	{
		// Владелец снимка обязателен (журнал и будущая модерация) — без Steam-auth его нет.
		KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_store_failed reason=not_authenticated slot=%i\n", player->GetPlayerSlot().Get());
		player->languageService->PrintChat(true, false, "HUD Share - Not Ready");
		return MRES_SUPERCEDE;
	}

	std::string snapshot;
	if (!KZ::hudshare::Capture(player, snapshot))
	{
		// Единственная причина отказа снимка — незагруженные префы (fail-closed, см. Capture):
		// иначе мы выдали бы игроку код на набор дефолтов вместо его худа.
		KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_store_failed reason=capture_failed steam_id=%llu\n", steamID);
		player->languageService->PrintChat(true, false, "HUD Share - Not Ready");
		return MRES_SUPERCEDE;
	}

	// Кулдаун берём ПОСЛЕ дешёвых проверок и перед единственной записью: отказ «настройки не
	// загрузились» не должен съедать игроку следующие пять секунд.
	const f32 cooldown = KZ::hudshare::TakeShareCooldown(player);
	if (cooldown > 0.0f)
	{
		player->languageService->PrintChat(true, false, "HUD Share - Store Cooldown", cooldown);
		return MRES_SUPERCEDE;
	}

	char code[KZ::hudshare::CODE_LENGTH + 1];
	KZ::hudshare::GenerateCode(code, sizeof(code));

	// Строку кода копируем в лямбду по значению: она живёт на стеке этого вызова, а ответ БД
	// приходит асинхронно. Игрока резолвим по userID в колбэке (за round-trip он может уйти) —
	// тот же паттерн, что в setup_client.cpp/load_savedrun.cpp.
	const CPlayerUserId userID = player->GetClient()->GetUserID();
	const std::string codeCopy = code;
	const i32 keyCount = (i32)KZ::hudshare::GetKeys().size();

	KZDatabaseService::StoreHudShare(
		steamID, code, KZ::hudshare::SNAPSHOT_VERSION, snapshot,
		[userID, codeCopy, steamID, keyCount](std::vector<ISQLQuery *> queries)
		{
			KZ_LOG_INFO(LogChannel::Option, "[cyb] hud_share_stored steam_id=%llu code=%s keys=%i schema_version=%i\n", steamID, codeCopy.c_str(),
						keyCount, (i32)KZ::hudshare::SNAPSHOT_VERSION);
			KZPlayer *pl = g_pKZPlayerManager->ToPlayer(userID);
			if (!pl || !pl->IsAuthenticated())
			{
				return;
			}
			pl->languageService->PrintChat(true, false, "HUD Share - Code Created", codeCopy.c_str());
		},
		[userID, codeCopy, steamID](std::string error, int failIndex)
		{
			// Отказ, а не молчание: самая вероятная причина — коллизия кода (PRIMARY KEY), и
			// лечится она повтором команды, потому что повтор генерирует НОВЫЙ случайный код
			// (GenerateCode) — та же коллизия не воспроизводится. Upsert тут был бы хуже:
			// затёр бы чужой снимок под его же кодом.
			KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_store_failed reason=db_insert steam_id=%llu code=%s error=%s\n", steamID,
						codeCopy.c_str(), error.c_str());
			KZPlayer *pl = g_pKZPlayerManager->ToPlayer(userID);
			if (!pl || !pl->IsAuthenticated())
			{
				return;
			}
			// Кулдаун отпускаем: выдача не состоялась, и повторить надо сразу, а не через 5 с.
			KZ::hudshare::ReleaseShareCooldown(pl);
			pl->languageService->PrintChat(true, false, "HUD Share - Store Failed");
		});

	return MRES_SUPERCEDE;
}

SCMD(kz_hudget, SCFL_HUD | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	if (args->ArgC() < 2)
	{
		player->languageService->PrintChat(true, false, "HUD Share - Usage Get");
		return MRES_SUPERCEDE;
	}
	if (!player->GetClient())
	{
		// userID нужен колбэку БД для безопасного резолва игрока после async round-trip.
		player->languageService->PrintChat(true, false, "HUD Share - Not Ready");
		return MRES_SUPERCEDE;
	}

	char code[KZ::hudshare::CODE_LENGTH + 1];
	if (!KZ::hudshare::NormalizeCode(args->Arg(1), code, sizeof(code)))
	{
		KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_get_failed reason=code_invalid steam_id=%llu\n", player->GetSteamId64(false));
		player->languageService->PrintChat(true, false, "HUD Share - Code Invalid");
		return MRES_SUPERCEDE;
	}

	const CPlayerUserId userID = player->GetClient()->GetUserID();
	const std::string codeCopy = code;
	const u64 steamID = player->GetSteamId64(false);

	KZDatabaseService::FetchHudShare(
		code,
		[userID, codeCopy, steamID](std::vector<ISQLQuery *> queries)
		{
			KZPlayer *pl = g_pKZPlayerManager->ToPlayer(userID);
			if (!pl || !pl->IsAuthenticated())
			{
				return;
			}
			ISQLResult *result = queries.empty() ? NULL : queries[0]->GetResultSet();
			if (!result || !result->FetchRow())
			{
				KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_get_failed reason=code_not_found steam_id=%llu code=%s\n", steamID,
							codeCopy.c_str());
				pl->languageService->PrintChat(true, false, "HUD Share - Code Not Found");
				return;
			}
			const char *snapshot = result->GetString(0);
			const u64 owner = (u64)result->GetInt64(1);
			// Детализация лога — код и владелец снимка: по ней разбирается жалоба «мне дали
			// код, а худ не такой».
			char detail[64];
			V_snprintf(detail, sizeof(detail), "code=%s owner=%llu", codeCopy.c_str(), owner);
			KZ::hudshare::Apply(pl, snapshot, KZ::hudshare::Source::ShareCode, detail);
		},
		[userID, codeCopy, steamID](std::string error, int failIndex)
		{
			KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_get_failed reason=db_fetch steam_id=%llu code=%s error=%s\n", steamID, codeCopy.c_str(),
						error.c_str());
			KZPlayer *pl = g_pKZPlayerManager->ToPlayer(userID);
			if (!pl || !pl->IsAuthenticated())
			{
				return;
			}
			pl->languageService->PrintChat(true, false, "HUD Share - Not Ready");
		});

	return MRES_SUPERCEDE;
}

// Забрать худ наблюдаемого. Транспорта у команды нет вовсе (ни БД, ни кода): снимок снимается
// с цели спектейта в этом же процессе, поэтому и кулдауна своего ей не нужно — хватает общего
// SCMD_COOLDOWN. Вся логика и все отказы — в KZ::hudshare::TakeFromSpectated (ядро), потому что
// у неё два вызывающих: эта команда и пункт меню (hud/prefs/hud_prefs.cpp).
SCMD(kz_hudtake, SCFL_HUD | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	KZ::hudshare::TakeFromSpectated(player);
	return MRES_SUPERCEDE;
}

SCMD(kz_hudundo, SCFL_HUD | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	if (!KZ::hudshare::HasUndo(player))
	{
		player->languageService->PrintChat(true, false, "HUD Share - Undo Empty");
		return MRES_SUPERCEDE;
	}
	// Отчёт игроку и лог печатает сам Apply — здесь добавлять нечего.
	KZ::hudshare::ApplyUndo(player);
	return MRES_SUPERCEDE;
}
