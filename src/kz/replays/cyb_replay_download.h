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

#include <functional>
#include <vector>

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

	// Тот же резолв и та же докачка, но БЕЗ плейбека: байты файла уезжают колбэку
	// (`!lead`, src/kz/lead). Ни SetPendingAwr, ни LoadReplay здесь не зовутся — реплей-бот
	// не спавнится, состояние глобального плейбека не трогается.
	//
	// Колбэк зовётся на ГЛАВНОМ потоке (колбэк Steam HTTP). Игрок к этому моменту мог уйти —
	// поэтому в него приходит CPlayerUserId, а не указатель: получатель обязан сам сделать
	// ToPlayer и проверить результат. ПУСТОЙ буфер означает отказ (нет записи, сеть, мусор в
	// ответе): фразу в чат выбирает вызывающая фича, сам RequestFile в чат НЕ пишет.
	void RequestFile(KZPlayer *player, Kind kind, u64 targetSteamId64, std::function<void(CPlayerUserId, std::vector<char>)> onReady);

	// Ожидание AWR-режима для следующего LoadReplay: резолв асинхронный, а путь загрузки
	// общий (кэш downloads/ и докачка оба зовут commands::LoadReplay, и туда нечем донести
	// вид записи). Привязано к UUID: `!replay <uuid>` с диска идёт в LoadReplay напрямую,
	// мимо резолва, и без привязки съел бы чужое ожидание. Take возвращает true только на
	// тот же uuid и в ЛЮБОМ случае гасит состояние — протухнуть ожиданию негде.
	void SetPendingAwr(const char *uuid, u64 awrMs);
	bool TakePendingAwr(const char *uuid, u64 &awrMs);
	// Снять ожидание, не потребляя (error-пути докачки: файла не будет, LoadReplay не позовут).
	void ClearPendingAwr();

	// !replay <uuid>: докачка конкретного реплея по UUID (api GET
	// /replays/v1/by-uuid). Вызывается из LoadReplay, когда файла нет локально —
	// глобал-сервис у нас отключён, KZGlobalService::RequestReplay мёртвый путь.
	void RequestAndPlayByUuid(KZPlayer *player, const char *uuid);
} // namespace CybReplayDownload
