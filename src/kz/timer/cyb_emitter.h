/*
 * cyb_emitter.h — fire-and-forget POST rana v ingest Cyber-platformy.
 *
 * Vyzyvaetsya iz RunSubmission::RunSubmission() srazu posle togo,
 * kak vse polya snapshota zapolneny. Fail-open: lyubaya oshibka HTTP
 * logируetsya, no ne vliyaet na ход rana ili local/global submit.
 */
#pragma once

#include "kz/timer/submission.h"

namespace CybEmitter
{
	// Отправляет событие kz.run_finished в наш ingest.
	// Вызов неблокирующий (HTTP::Request::Send использует Steam async HTTP).
	void Emit(const RunSubmission &sub);
} // namespace CybEmitter
