/*
 * cyb_replay_download.h — `!replay pb`/`!replay wr`: резолв и докачка
 * центрального PB/WR-реплея Cyber-платформы (api GET /replays/v1/resolve),
 * затем запуск существующего плейбека (kz/replays/commands.cpp).
 *
 * Ключ резолва: map = текущая карта, course = cyber-номер ТЕКУЩЕГО курса
 * игрока (нет курса → 0, см. KZ::course::GetCyberCourseNumber), mode =
 * короткое api-имя ТЕКУЩЕГО режима игрока. Именно ключ по построению — это
 * mode-гейт спеки: kzt-реплей из vnl недоступен, т.к. запрос с mode=vnl
 * никогда не найдёт запись, загруженную под mode=kzt.
 *
 * WR = overall (nub) серверный рекорд.
 *
 * PRO (`WRPro`/`PBPro`, они же `!replay wrpro`/`pbpro`/`gpbpro`): pro-раны
 * отдельным файлом в центральное хранилище НЕ выгружаются — реплей пишется
 * только на overall/NUB-рекорд и на локальный рекорд инстанса
 * (cyb_replay_upload.cpp::MaybeUpload). Поэтому pro-резолв на стороне api
 * отдаёт файл ТОЛЬКО когда лучший overall-ран игрока сам по себе без
 * телепортов, то есть NUB-PB и PRO-PB — один и тот же ран и именно он
 * представлен файлом. Иначе честный 404: показать чужой ран с ТП под видом
 * pro-рекорда хуже, чем сказать «нет».
 *
 * Fail-open в сторону пользователя: любая ошибка (нет конфига, сеть, 404,
 * неожиданный ответ) — только сообщение в чат + лог, ран/сервер не страдают.
 */
#pragma once

#include "common.h"
#include "sdk/datatypes.h"

class KZPlayer;

namespace CybReplayDownload
{
	enum class Kind
	{
		PB,
		WR,
		PBPro,
		WRPro,
		// AWR (absolute world record) — лучший ран ПОСЛЕ вырезки телепорт-петель.
		// steamId64 не нужен: это рекорд сети, как WR.
		AWR,
	};

	// targetSteamId64 используется только для Kind::PB и Kind::PBPro (0 — не задан
	// явно, вызывающий код обязан подставить player->GetSteamId64() для "своего" PB
	// заранее — см. commands.cpp). Для Kind::WR/WRPro параметр игнорируется:
	// резолв рекорда сети не фильтрует по игроку.
	void RequestAndPlay(KZPlayer *player, Kind kind, u64 targetSteamId64);

	// Ожидание AWR-режима для следующего LoadReplay: резолв асинхронный, а путь загрузки
	// общий (кэш downloads/ и докачка оба зовут commands::LoadReplay, и туда нечем донести
	// вид записи). Потребляется ОДИН раз: любой другой путь загрузки (`!replay <uuid>`,
	// sr/spb…) зовёт TakePendingAwr тем же кодом и получает false.
	void SetPendingAwr(bool on, u64 awrMs);
	bool TakePendingAwr(u64 &awrMs);

	// !replay <uuid>: докачка конкретного реплея по UUID (api GET
	// /replays/v1/by-uuid). Вызывается из LoadReplay, когда файла нет локально —
	// глобал-сервис у нас отключён, KZGlobalService::RequestReplay мёртвый путь.
	void RequestAndPlayByUuid(KZPlayer *player, const char *uuid);
} // namespace CybReplayDownload
