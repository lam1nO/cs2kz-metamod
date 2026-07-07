/*
 * cyb_replay_upload.h — fire-and-forget аплоад PB/WR-реплеев в центральное
 * хранилище Cyber-платформы (api POST /replays/v1/upload).
 *
 * Вызывается из RunSubmission (submission.cpp) в момент, когда одновременно
 * известны: результат локальной БД (новый личный рекорд + локальный ранг,
 * save_time.cpp/queries/save_time.h) и готовый в памяти буфер сериализованного
 * реплея (ReplayFileWriter::QueueWrite → RunSubmission::OnReplayReady).
 *
 * Fail-open: любая ошибка (нет конфига, сеть, 4xx/5xx) только логируется —
 * не влияет на ход рана, локальный/глобальный сабмит или объявления игрокам.
 */
#pragma once

#include "kz/timer/submission.h"

namespace CybReplayUpload
{
	// Пытается загрузить реплей текущего рана как новый personal best игрока
	// (type=pb). Если isServerRecord — дополнительно шлёт второй, независимый
	// POST с type=wr (локальный ранг игрока после этого рана равен 1).
	//
	// Ничего не делает (тихо), если: cybEmitUrl не сконфигурирован, режим не
	// маппится на короткое имя api (кастомный режим), у рана есть стили,
	// буфер реплея пуст, превышает лимит api (32 МБ), либо map/course не
	// проходят валидацию api.
	void MaybeUpload(const RunSubmission &sub, bool isServerRecord);
} // namespace CybReplayUpload
