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
 * WR = overall (nub) серверный рекорд. Pro-раны отдельно в центральное
 * хранилище НЕ выгружаются (см. cyb_replay_upload.cpp::MaybeUpload — второй
 * POST с type=wr шлётся только для style-less рана; `!replay wr` резолвит
 * ровно эти записи и никогда не найдёт pro-ран).
 *
 * Fail-open в сторону пользователя: любая ошибка (нет конфига, сеть, 404,
 * неожиданный ответ) — только сообщение в чат + лог, ран/сервер не страдают.
 */
#pragma once

#include "sdk/datatypes.h"

class KZPlayer;

namespace CybReplayDownload
{
	enum class Kind
	{
		PB,
		WR,
	};

	// targetSteamId64 используется только для Kind::PB (0 — не задан явно,
	// вызывающий код обязан подставить player->GetSteamId64() для "своего" PB
	// заранее — см. commands.cpp). Для Kind::WR параметр игнорируется:
	// резолв WR не фильтрует по игроку.
	void RequestAndPlay(KZPlayer *player, Kind kind, u64 targetSteamId64);
} // namespace CybReplayDownload
