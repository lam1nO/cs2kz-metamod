/*
 * cyb_replay_common.h — общие хелперы для аплоада (cyb_replay_upload.cpp) и
 * докачки (cyb_replay_download.cpp) центральных PB/WR-реплеев Cyber-платформы.
 *
 * Вынесено сюда как ТРЕТИЙ потребитель мэппинга режима (см. концерн отчёта
 * Task 4 т4b: "если появится третий потребитель — вынести в общий хелпер").
 * cyb_emitter.cpp (kz/timer/) НЕ трогаем — это другой домен (ingest событий
 * kz.run_finished), а не реплеи; его копия MapMode остаётся как есть, чтобы
 * не расширять список изменяемых файлов задачи без нужды.
 */
#pragma once

#include <string>

namespace CybReplayCommon
{
	// Маппинг короткого имени режима cs2kz (например "CKZ"/"ckz") → короткое имя
	// api ("ckz"/"vnl"/"kzt"). Пустая строка — режим не поддерживается центральным
	// хранилищем (кастомные режимы сверх этих трёх туда не выгружаются и оттуда
	// не резолвятся).
	const char *MapMode(const std::string &shortName);

	// api валидирует map как [a-z0-9_-]{1,128} (apps/api/src/modules/replays/replays.controller.ts).
	bool IsValidMapName(const std::string &name);
} // namespace CybReplayCommon
