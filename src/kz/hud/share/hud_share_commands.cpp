// Чат-команды обмена настройками худа: !hudshare (выдать код), !hudget <код> (применить),
// !hudtake (забрать худ наблюдаемого), !hudundo (откатить последнее применение),
// !hudexport (выгрузить настройки текстом в консоль). Регистрация — тем же путём, что соседние
// команды худа (SCMD, utils/simplecmds.h): консольное имя kz_*, чат-триггер ! получается сам.
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

#include <time.h>

#include "tier1/strtools.h"

#include "tier0/memdbgon.h"

// Выдача кода: тело команды вынесено в функцию, потому что вызывающих два — !hudshare и пункт
// меню «Поделиться худом» (hud/prefs/hud_prefs.cpp). Живёт здесь, а не в ядре: это транспорт
// (INSERT в общую MySQL флота), и ядро обмена о базе не знает.
void KZ::hudshare::IssueShareCode(KZPlayer *player)
{
	if (!player)
	{
		return;
	}
	const u64 steamID = player->GetSteamId64();
	if (!steamID || !player->GetClient())
	{
		// Владелец снимка обязателен (журнал и будущая модерация) — без Steam-auth его нет.
		KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_store_failed reason=not_authenticated slot=%i\n", player->GetPlayerSlot().Get());
		player->languageService->PrintChat(true, false, "HUD Share - Not Ready");
		return;
	}

	std::string snapshot;
	if (!KZ::hudshare::Capture(player, snapshot))
	{
		// Единственная причина отказа снимка — незагруженные префы (fail-closed, см. Capture):
		// иначе мы выдали бы игроку код на набор дефолтов вместо его худа.
		KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_store_failed reason=capture_failed steam_id=%llu\n", steamID);
		player->languageService->PrintChat(true, false, "HUD Share - Not Ready");
		return;
	}

	// Кулдаун берём ПОСЛЕ дешёвых проверок и перед единственной записью: отказ «настройки не
	// загрузились» не должен съедать игроку следующие пять секунд.
	const f32 cooldown = KZ::hudshare::TakeShareCooldown(player);
	if (cooldown > 0.0f)
	{
		player->languageService->PrintChat(true, false, "HUD Share - Store Cooldown", cooldown);
		return;
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
}

SCMD(kz_hudshare, SCFL_HUD | SCFL_PREFERENCE)
{
	KZ::hudshare::IssueShareCode(g_pKZPlayerManager->ToPlayer(controller));
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

// === Выгрузка настроек текстом (второй путь обмена, спека §4: «оба пути») ====================
// Строки `setinfo kzp_<ключ> <значение>;` — ФОРМАТ АПСТРИМА один в один
// (origin/master:src/kz/option/prefs_transfer.cpp, ExportPrefs + KZ_PREF_CVAR_PREFIX), потому
// что читает их не наш сервер, а глобальные cs2kz-серверы: у них есть userinfo-импорт, у нас
// его нет (наш обмен — короткий код через общую MySQL). Отсюда два следствия:
//   - команда ОДНОСТОРОННЯЯ: выгрузка, вставлять этот блок обратно нам некуда (у себя тот же
//     худ переносится кодом !hudshare/!hudget);
//   - строка штампа (kzp__stamp) печатается ПЕРВОЙ и убирать её не надо. Факты апстрима: путь
//     «вставил ДО захода на сервер» читает ReadImport, и он без штампа выходит сразу — то есть
//     не применяет НИЧЕГО; путь «вставил уже в игре» обслуживает DrainImport, и он применяет
//     блок и без штампа (`stamped ? stamp <= last : fromConnect`). Со штампом работают ОБА
//     пути, и блок применяется один раз, а не при каждом коннекте.
// Печать в КОНСОЛЬ, а не в чат: 64 строки в чате — это спам на весь сервер и обрезка по длине.
// Тело вынесено в функцию по той же причине, что и выдача кода: вызывающих два — !hudexport и
// пункт меню «Выгрузить настройки в консоль» (hud/prefs/hud_prefs.cpp).
void KZ::hudshare::ExportToConsole(KZPlayer *player)
{
	if (!player)
	{
		return;
	}
	const std::vector<KZ::prefs::Entry> &keys = KZ::hudshare::GetKeys();
	if (keys.empty())
	{
		// Тот же отказ, что у остальных путей обмена: пустой список выглядел бы как «выгружено 0».
		KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_export_failed reason=registry_empty steam_id=%llu\n", player->GetSteamId64(false));
		player->languageService->PrintChat(true, false, "HUD Share - Not Ready");
		return;
	}
	if (!player->optionService || !player->optionService->IsLoaded())
	{
		// Fail-closed, как в Capture: иначе выгрузили бы игроку набор дефолтов под видом его худа.
		KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_export_failed reason=prefs_not_loaded steam_id=%llu\n", player->GetSteamId64(false));
		player->languageService->PrintChat(true, false, "HUD Share - Not Ready");
		return;
	}

	player->languageService->PrintConsole(false, false, "HUD Share - Export Header");
	player->languageService->PrintConsole(false, false, "HUD Share - Export Workshop Note");
	player->languageService->PrintConsole(false, false, "HUD Share - Export Length Note");
	player->languageService->PrintConsole(false, false, "HUD Share - Export Stamp Note");
	player->PrintConsole(false, false, "setinfo kzp__stamp %lli;", (long long)time(NULL));

	i32 count = 0;
	char value[256];
	for (const KZ::prefs::Entry &entry : keys)
	{
		if (!KZ::prefs::ReadValue(player, entry, value, sizeof(value)))
		{
			// Ни значения, ни дефолта — в блоке ключа не будет, у той стороны останется своё.
			KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_export_skipped reason=no_value key=%s steam_id=%llu\n", entry.key,
						player->GetSteamId64(false));
			continue;
		}
		if (!value[0])
		{
			// Пустая строка (шрифт, сохранённый как ""): `setinfo kzp_x ;` на той стороне — мусор.
			KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_export_skipped reason=empty key=%s steam_id=%llu\n", entry.key, player->GetSteamId64(false));
			continue;
		}
		if (entry.item && entry.item->type == KZOptItemType::Color)
		{
			// Одно представление цвета на выход: у игрока, ни разу не применявшего чужой худ,
			// в префах лежит ЗНАКОВОЕ (белый = -1, см. option/pref_registry.cpp), и вставлять
			// такое на чужой сервер значило бы зависеть от того, маскирует ли он при чтении.
			int64 packed = 0;
			if (V_StringToValue<int64>(value, packed))
			{
				V_snprintf(value, sizeof(value), "%llu", (unsigned long long)(u32)packed);
			}
		}
		// Строковые значения (шрифты) — в кавычках, и это БАЙТ В БАЙТ апстримная строка, хотя в
		// его ExportPrefs формат без кавычек: кавычки апстрим ставит раньше, внутри своего
		// ReadValue (origin/master:src/kz/option/pref_registry.cpp, ветка Str: `"\"%s\""`).
		// Наш ReadValue их не ставит сознательно (значение уезжает полем снимка, а не строкой
		// консольной команды — см. шапку pref_registry.h), поэтому здесь они добавляются явно.
		if (entry.storage == KZOptStorage::Str)
		{
			player->PrintConsole(false, false, "setinfo kzp_%s \"%s\";", entry.key, value);
		}
		else
		{
			player->PrintConsole(false, false, "setinfo kzp_%s %s;", entry.key, value);
		}
		count++;
	}
	player->languageService->PrintConsole(false, false, "HUD Share - Export Footer");
	// Предупреждения повторяются ПОСЛЕ блока намеренно: до блока их не увидит игрок с
	// прокрученной консолью — а это ровно тот, кто потом скажет «вставил, ничего не
	// применилось». Оба ограничения (фильтр setinfo на workshop-картах и ~511 символов за
	// вставку) для нас не теоретические: все наши карты workshop.
	player->languageService->PrintConsole(false, false, "HUD Share - Export Footer Reminder");
	player->languageService->PrintChat(true, false, "HUD Share - Exported", count);
	KZ_LOG_INFO(LogChannel::Option, "[cyb] hud_exported steam_id=%llu keys=%i of=%i\n", player->GetSteamId64(false), count, (i32)keys.size());
}

SCMD(kz_hudexport, SCFL_HUD | SCFL_PREFERENCE)
{
	KZ::hudshare::ExportToConsole(g_pKZPlayerManager->ToPlayer(controller));
	return MRES_SUPERCEDE;
}
