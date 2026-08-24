/*
 * cyb_replay_upload.h — аплоад PB/WR-реплеев в центральное хранилище
 * Cyber-платформы (api POST /replays/v1/upload).
 *
 * Вызывается из RunSubmission (submission.cpp) в момент, когда одновременно
 * известны: результат локальной БД (новый личный рекорд + локальный ранг,
 * save_time.cpp/queries/save_time.h) и готовый в памяти буфер сериализованного
 * реплея (ReplayFileWriter::QueueWrite → RunSubmission::OnReplayReady).
 *
 * Fail-open для рана: любая ошибка (нет конфига, сеть, 4xx/5xx) не влияет на
 * ход рана, локальный/глобальный сабмит или объявления игрокам. Но не
 * fire-and-forget: сама отправка идёт через KZOutboxService::SendReplay с
 * write-ahead метаданными на диске — недоставленный аплоад дошлёт ретраер.
 */
#pragma once

#include "kz/timer/submission.h"
#include "kz/outbox/kz_outbox.h"

namespace CybReplayUpload
{
	// Валидация рана + сборка метаданных центрального аплоада (для write-ahead
	// outbox и самой отправки). false — грузить нечего/нельзя: cybEmitUrl не
	// сконфигурирован, режим не маппится на короткое имя api (кастомный режим),
	// у рана есть стили, буфер реплея пуст или превышает лимит api (32 МБ),
	// map/course не проходят валидацию api. Причины отказа — в логе.
	bool BuildMeta(const RunSubmission &sub, bool isServerRecord, bool unconfirmedPb, KZOutboxService::ReplayMeta &out);

	// Онлайн-путь (локальная БД подтвердила новый PB): перезаписывает write-ahead
	// мету подтверждённым вариантом и шлёт type=pb (+type=wr при isServerRecord)
	// через общую с ретраером функцию отправки.
	void MaybeUpload(const RunSubmission &sub, bool isServerRecord);
} // namespace CybReplayUpload
