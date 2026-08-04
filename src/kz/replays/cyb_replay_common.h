/*
 * cyb_replay_common.h — общий маппинг «шорт-нейм режима cs2kz → api-режим
 * платформы» и валидация имени карты. Потребители маппинга: реплеи (аплоад
 * cyb_replay_upload.cpp, докачка cyb_replay_download.cpp), платформенный
 * PB/WR-кэш худа (kz_timer.cpp), профиль — звания и мост GG1 (kz_profile.cpp).
 * У cyb_emitter.cpp (ingest событий kz.run_finished) — собственная локальная
 * копия MapMode, оставлена как есть (другой домен).
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
