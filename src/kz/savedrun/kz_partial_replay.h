/*
 * kz_partial_replay.h — реплей для бэкапа незавершённого рана (SavedRuns).
 *
 * Выход игрока посреди рана сохраняет ран в SavedRuns (kz_savedrun.*), и до этого модуля
 * запись рана при этом терялась: восстановленный ран финишировал без реплея. Теперь:
 *  1. На дисконнекте живой рекордер рана забирается целиком (кадры, события, в том числе
 *     паузы, прыжки, команды) и пишется КУСКОМ в kzreplays/partial/<ключ>.replay — обычная
 *     сериализация реплея (Recorder::WriteToMemory) за 44-байтным префиксом «магия + id куска».
 *     id куска кладётся в снапшот SavedRuns (поле "rp"): кусок принадлежит ровно той строке,
 *     которая на него ссылается, и чужой/устаревший кусок никогда не приклеится к рану.
 *  2. При восстановлении (ApplySnapshot) сразу заводится рекордер, а кусок асинхронно вставляется
 *     в его начало: живые тики сдвигаются так, чтобы кадры шли подряд, на стыке — пара
 *     TIMER_PAUSE→TIMER_RESUME (обычный плейбек склеивает, `!replay … full` показывает разрыв)
 *     и бит RpFlags::splice на последнем кадре куска (разрез AWR читает стык как телепорт).
 *  3. Диски серверов разные, MySQL общая: кусок уезжает в api (`POST /replays/v1/partial`,
 *     одна перезаписываемая строка на ключ SavedRuns) через outbox; на восстановлении без
 *     локального файла он докачивается (`GET /replays/v1/partial` → публичный URL S3).
 *     Инвалидация SavedRuns (финиш, !r, !stop, noclip, prac-политика) удаляет и кусок —
 *     локальный файл и строку api (`DELETE`, по id куска).
 * TTL — как у SavedRuns: 30 дней (локально — PurgeExpiredLocal на загрузке карты, в api —
 * чистка при аплоаде).
 */
#pragma once

#include "common.h"

#include <string>

class KZPlayer;

namespace KZPartialReplay
{
	// Ключ куска = ключ хранения SavedRuns (steam, карта, курс, режим, стили).
	struct Key
	{
		u64 steamId64 {};
		std::string map;
		i32 course {};      // cyber-номер курса (KZ::course::GetCyberCourseNumber)
		std::string mode;   // шорт-нейм режима форка — как колонка SavedRuns.Mode
		std::string styles; // KZSavedRunService::BuildStylesString
	};

	// Ключ для игрока на курсе с cyber-номером courseNumber. false — ключа нет (игрок не
	// аутентифицирован, имя карты недоступно).
	bool MakeKey(KZPlayer *player, i32 courseNumber, Key &out);

	// Дисконнект с бегущим (или замороженным в prac) раном: забирает живой рекордер рана и
	// асинхронно пишет кусок. outPartialId — id куска для снапшота SavedRuns; пусто — куска не
	// будет (рекордера нет). Зовётся ДО TimerStop (тот убил бы рекордер).
	void SaveOnDisconnect(KZPlayer *player, const Key &key, std::string &outPartialId);

	// Ран только что восстановлен из SavedRuns (таймер поднят, игрок на паузе): заводит рекордер
	// и вставляет в его начало кусок partialId (локальный файл, иначе api). Пустой partialId —
	// no-op (строка SavedRuns старой сборки или ран без рекордера). restoredTime — время рана из
	// снапшота (подпись паузы стыка).
	void ResumeAfterRestore(KZPlayer *player, const Key &key, const std::string &partialId, f64 restoredTime);

	// Сохранённый ран инвалидирован: удалить локальный кусок ключа и строку api (только по
	// известному id — удаление «вслепую» по ключу могло бы снести свежий кусок, выгруженный
	// другим сервером позже).
	void Invalidate(const Key &key, const std::string &partialId, const char *reason);

	// TTL локальных кусков — раз на загрузку карты (рядом с PurgeExpiredSavedRuns).
	void PurgeExpiredLocal();
} // namespace KZPartialReplay
