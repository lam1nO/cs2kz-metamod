#include "cs2kz.h"
#include "kz/kz.h"
#include "kz_replay.h"
#include "playback.h"
#include "data.h"
#include "bot.h"
#include "item.h"
#include "events.h"
#include "sdk/usercmd.h"
#include <vector>
#include <cmath>

namespace KZ::replaysystem::playback
{
	// Последний itemDef, чей GiveNamedItem вернул null — гард от тикового цикла
	// раздевания бота. Сбрасывается на старте реплея и при успешной выдаче.
	static_global i32 g_lastFailedGiveItemDef = -1;

	// Паузные сегменты в ИНДЕКСАХ тиков плейбека (не serverTick). Выводятся из пар
	// событий TIMER_PAUSE→TIMER_RESUME один раз на старте/навигации, чтобы в тик-цикле
	// проверка была O(1) по курсору без аллокаций. Единственный активный реплей — один
	// глобальный бот, поэтому состояние статическое (как g_lastFailedGiveItemDef).
	struct PauseSegment
	{
		u32 startTick; // индекс первого тика паузы
		u32 endTick;   // индекс кадра возобновления (полуинтервал [startTick, endTick))
	};

	static_global std::vector<PauseSegment> g_pauseSegments;
	// Курсор следующего непройденного сегмента (плейбек монотонен вперёд).
	static_global size_t g_nextPauseSegment = 0;

	// Первый индекс тика с tickData[idx].serverTick >= serverTick. serverTick в записи
	// монотонно не убывает (движковый tickcount), поэтому бинарный поиск корректен.
	// Может вернуть tickCount, если такого тика нет.
	static_function u32 TickIndexForServerTick(const data::ReplayPlayback *replay, u32 serverTick)
	{
		u32 lo = 0, hi = replay->tickCount;
		while (lo < hi)
		{
			u32 mid = lo + (hi - lo) / 2;
			if (replay->tickData[mid].serverTick < serverTick)
			{
				lo = mid + 1;
			}
			else
			{
				hi = mid;
			}
		}
		return lo;
	}

	void ClearPauseSegments()
	{
		g_pauseSegments.clear();
		g_nextPauseSegment = 0;
	}

	void BuildPauseSegments()
	{
		ClearPauseSegments();

		auto replay = data::GetCurrentReplay();
		if (!replay->events || replay->numEvents == 0 || !replay->tickData || replay->tickCount == 0)
		{
			return;
		}

		bool inPause = false;
		u32 pauseStartServerTick = 0;
		for (u32 i = 0; i < replay->numEvents; i++)
		{
			const RpEvent *e = &replay->events[i];
			if (e->type != RPEVENT_TIMER_EVENT)
			{
				continue;
			}
			switch (e->data.timer.type)
			{
				case RpEvent::RpEventData::TimerEvent::TIMER_PAUSE:
					// Паузы не вкладываются (CanPause запрещает паузу в паузе) — берём первую.
					if (!inPause)
					{
						inPause = true;
						pauseStartServerTick = e->serverTick;
					}
					break;
				case RpEvent::RpEventData::TimerEvent::TIMER_RESUME:
					if (inPause)
					{
						inPause = false;
						u32 startIdx = TickIndexForServerTick(replay, pauseStartServerTick);
						u32 endIdx = TickIndexForServerTick(replay, e->serverTick);
						// Кадр возобновления обязан существовать; сегмент — хотя бы 1 тик.
						if (endIdx < replay->tickCount && startIdx < endIdx)
						{
							g_pauseSegments.push_back({startIdx, endIdx});
						}
					}
					break;
				case RpEvent::RpEventData::TimerEvent::TIMER_START:
				case RpEvent::RpEventData::TimerEvent::TIMER_END:
				case RpEvent::RpEventData::TimerEvent::TIMER_STOP:
					// Границы рана рвут незакрытую паузу — не тащим её через ран.
					inPause = false;
					break;
				default:
					break;
			}
		}
	}

	// Пропуск паузных сегментов при монотонном продвижении вперёд. O(1) амортизированно
	// по курсору g_nextPauseSegment, без аллокаций — безопасно для тик-пути.
	static_function u32 AdvancePastPauses(u32 tick)
	{
		while (g_nextPauseSegment < g_pauseSegments.size())
		{
			const PauseSegment &seg = g_pauseSegments[g_nextPauseSegment];
			if (tick < seg.startTick)
			{
				break; // до ближайшей паузы ещё не дошли
			}
			if (tick < seg.endTick)
			{
				// Вошли внутрь паузы — прыжок на кадр возобновления.
				tick = seg.endTick;
			}
			// tick >= seg.endTick: сегмент пройден — к следующему (на случай смежных).
			g_nextPauseSegment++;
		}
		return tick;
	}

	u32 SnapSeekTargetOutOfPause(u32 tick)
	{
		for (const PauseSegment &seg : g_pauseSegments)
		{
			if (tick >= seg.startTick && tick < seg.endTick)
			{
				// Интерьер паузы — застывшие кадры (в skip-режиме не показываются);
				// приземляем сик на кадр возобновления.
				return seg.endTick;
			}
			if (tick < seg.startTick)
			{
				break; // сегменты упорядочены — дальше начала только позже
			}
		}
		return tick;
	}

	u32 EffectiveTickCount()
	{
		auto replay = data::GetCurrentReplay();
		u32 skipped = 0;
		for (const PauseSegment &seg : g_pauseSegments)
		{
			skipped += seg.endTick - seg.startTick;
		}
		return replay->tickCount > skipped ? replay->tickCount - skipped : 0;
	}

	u32 RawTickToEffective(u32 rawTick)
	{
		u32 skipped = 0;
		for (const PauseSegment &seg : g_pauseSegments)
		{
			if (rawTick < seg.startTick)
			{
				break;
			}
			// Внутри сегмента — считаем как его начало (интерьер паузы не показывается).
			skipped += (rawTick < seg.endTick ? rawTick : seg.endTick) - seg.startTick;
		}
		return rawTick - skipped;
	}

	u32 EffectiveTickToRaw(u32 effectiveTick)
	{
		// Сегменты упорядочены и не пересекаются: каждый, чьё начало уже позади, сдвигает сырой
		// индекс на свою длину. Результат никогда не попадает внутрь паузы.
		u32 raw = effectiveTick;
		for (const PauseSegment &seg : g_pauseSegments)
		{
			if (raw < seg.startTick)
			{
				break;
			}
			raw += seg.endTick - seg.startTick;
		}
		return raw;
	}

	void ResetPauseCursor(u32 tick)
	{
		g_nextPauseSegment = 0;
		while (g_nextPauseSegment < g_pauseSegments.size() && g_pauseSegments[g_nextPauseSegment].endTick <= tick)
		{
			g_nextPauseSegment++;
		}
	}

	// Порог «между кадрами был телепорт, а не движение», юниты. Обычная скорость в KZ —
	// сотни u/s, то есть единицы юнитов за тик; сотня юнитов за кадр бывает только на
	// телепорте/чекпоинте. Интерполировать через такой разрыв нельзя — бот проехал бы
	// сквозь карту, поэтому на нём сэмпл вырождается в «взять кадр как есть».
	static constexpr f32 KZ_RP_INTERP_MAX_DIST = 128.0f;

	// Линейная интерполяция угла по кратчайшей дуге: yaw записан в (-180, 180],
	// без нормализации переход через ±180 развернул бы бота на полный оборот.
	static_function f32 LerpAngle(f32 a, f32 b, f32 f)
	{
		f32 diff = fmodf(b - a + 540.0f, 360.0f) - 180.0f;
		return a + diff * f;
	}

	// Состояние кадра, которое имеет смысл интерполировать между записанными тиками.
	// Остальное (кнопки, присед, оружие, флаги) берётся с ближайшего кадра целиком:
	// это дискретные величины, у них промежуточного значения не существует.
	struct SampledState
	{
		Vector origin;
		Vector velocity;
		QAngle angles;
	};

	// Сэмпл внутри записанного кадра: frac=0 — начало тика (pre), frac=1 — конец (post).
	// Именно pre→post, а не pre[i]→pre[i+1]: запись может иметь пропуски тиков, и тогда
	// pre следующего кадра — уже другой момент времени, а pre/post одного кадра всегда
	// пара «начало/конец одной симуляции».
	static_function SampledState SampleFrame(const data::ReplayPlayback *replay, u32 tick, f32 frac)
	{
		const TickData *td = &replay->tickData[tick];
		if (frac <= 0.0f)
		{
			return {td->pre.origin, td->pre.velocity, td->pre.angles};
		}
		if (frac >= 1.0f)
		{
			return {td->post.origin, td->post.velocity, td->post.angles};
		}
		if ((td->post.origin - td->pre.origin).Length() > KZ_RP_INTERP_MAX_DIST)
		{
			// Телепорт внутри кадра — показываем начало, скачок произойдёт на границе кадра.
			return {td->pre.origin, td->pre.velocity, td->pre.angles};
		}
		SampledState out;
		out.origin = td->pre.origin + (td->post.origin - td->pre.origin) * frac;
		out.velocity = td->pre.velocity + (td->post.velocity - td->pre.velocity) * frac;
		out.angles = QAngle(LerpAngle(td->pre.angles.x, td->post.angles.x, frac), LerpAngle(td->pre.angles.y, td->post.angles.y, frac),
							LerpAngle(td->pre.angles.z, td->post.angles.z, frac));
		return out;
	}

	// Куда плейхед приедет к концу серверного тика, в виде (кадр, доля).
	// Ровная граница представляется как (кадр-1, 1.0), а не (кадр, 0.0): при скорости 1
	// это даёт ровно post текущего кадра — то же, что делал код до множителя скорости.
	static_function void EndOfTickPlayhead(const data::ReplayPlayback *replay, u32 *tick, f32 *frac)
	{
		if (replay->tickCount == 0)
		{
			// Данных нет — индексировать нечего. Вызывающие это и так гарантируют, но
			// результат уходит прямо в индекс массива, поэтому гард стоит на месте.
			*tick = 0;
			*frac = 0.0f;
			return;
		}
		f32 pos = replay->tickFraction + replay->playbackSpeed;
		u32 steps = (u32)pos;
		f32 leftover = pos - (f32)steps;
		if (leftover <= 0.0f && steps > 0)
		{
			steps--;
			leftover = 1.0f;
		}
		u32 target = replay->currentTick + steps;
		if (target >= replay->tickCount)
		{
			target = replay->tickCount - 1;
			leftover = 1.0f;
		}
		*tick = target;
		*frac = leftover;
	}

	bool GetDisplayedFrameVelocity(KZPlayer *player, Vector &out)
	{
		if (!player || !bot::IsValidBot(player->GetController()))
		{
			return false;
		}
		auto replay = data::GetCurrentReplay();
		if (!replay->playingReplay || replay->currentTick >= replay->tickCount)
		{
			return false;
		}
		u32 tick;
		f32 frac;
		EndOfTickPlayhead(replay, &tick, &frac);
		out = SampleFrame(replay, tick, frac).velocity;
		return true;
	}

	void OnPhysicsSimulate(KZPlayer *player)
	{
		if (!player || !bot::IsValidBot(player->GetController()))
		{
			return;
		}

		auto replay = data::GetCurrentReplay();
		if (!replay->playingReplay)
		{
			return;
		}

		TickData *tickData = &replay->tickData[replay->currentTick];
		auto pawn = player->GetPlayerPawn();

		// Начало тика — сэмпл на текущей позиции плейхеда. При скорости 1 tickFraction
		// всегда 0, то есть это ровно pre текущего кадра, как было раньше.
		SampledState pre = SampleFrame(replay, replay->currentTick, replay->tickFraction);

		// Setting the origin via teleport will break client interp, set the values directly
		pawn->m_CBodyComponent()->m_pSceneNode()->m_vecAbsOrigin(pre.origin);
		pawn->m_angEyeAngles(pre.angles);
		pawn->m_vecAbsVelocity(pre.velocity);

		auto moveServices = player->GetMoveServices();
		moveServices->m_LegacyJump().m_flJumpPressedTime =
			g_pKZUtils->GetServerGlobals()->curtime + tickData->pre.jumpPressedTime - tickData->gameTime;

		moveServices->m_flDuckSpeed = tickData->pre.duckSpeed;
		moveServices->m_flDuckAmount = tickData->pre.duckAmount;
		moveServices->m_flLastDuckTime = g_pKZUtils->GetServerGlobals()->curtime + tickData->pre.lastDuckTime - tickData->gameTime;
		moveServices->m_bDucking = tickData->pre.replayFlags.ducking;
		moveServices->m_bDucked = tickData->pre.replayFlags.ducked;
		moveServices->m_bDesiresDuck = tickData->pre.replayFlags.desiresDuck;

		// Apply modern jump data with tick offset
		moveServices->m_ModernJump().m_nLastActualJumpPressTick =
			g_pKZUtils->GetServerGlobals()->tickcount + tickData->modernJump.lastActualJumpPressTick - tickData->serverTick;
		moveServices->m_ModernJump().m_flLastActualJumpPressFrac = tickData->modernJump.lastActualJumpPressFrac;
		moveServices->m_ModernJump().m_nLastUsableJumpPressTick =
			g_pKZUtils->GetServerGlobals()->tickcount + tickData->modernJump.lastUsableJumpPressTick - tickData->serverTick;
		moveServices->m_ModernJump().m_flLastUsableJumpPressFrac = tickData->modernJump.lastUsableJumpPressFrac;
		moveServices->m_ModernJump().m_nLastLandedTick =
			g_pKZUtils->GetServerGlobals()->tickcount + tickData->modernJump.lastLandedTick - tickData->serverTick;
		moveServices->m_ModernJump().m_flLastLandedFrac = tickData->modernJump.lastLandedFrac;
		moveServices->m_ModernJump().m_flLastLandedVelocityX = tickData->modernJump.lastLandedVelocity.x;
		moveServices->m_ModernJump().m_flLastLandedVelocityY = tickData->modernJump.lastLandedVelocity.y;
		moveServices->m_ModernJump().m_flLastLandedVelocityZ = tickData->modernJump.lastLandedVelocity.z;

		u32 playerFlagBits = (-1) & ~((u32)(FL_CLIENT | FL_BOT));
		pawn->m_fFlags = (pawn->m_fFlags & ~playerFlagBits) | (tickData->pre.entityFlags & playerFlagBits);
		pawn->v_angle = tickData->pre.angles;
	}

	void OnProcessMovement(KZPlayer *player)
	{
		if (!player || !bot::IsValidBot(player->GetController()))
		{
			return;
		}

		auto replay = data::GetCurrentReplay();
		if (!replay->playingReplay)
		{
			return;
		}

		auto pawn = player->GetPlayerPawn();
		u32 newFlags = pawn->m_fFlags() | FL_BOT;
		pawn->m_fFlags(newFlags);
	}

	void OnProcessMovementPost(KZPlayer *player)
	{
		if (!player || !bot::IsValidBot(player->GetController()))
		{
			return;
		}

		auto replay = data::GetCurrentReplay();
		if (!replay->playingReplay)
		{
			return;
		}

		auto pawn = player->GetPlayerPawn();
		u32 newFlags = pawn->m_fFlags() | FL_BOT;
		pawn->m_fFlags(newFlags);
	}

	void OnFinishMovePre(KZPlayer *player, CMoveData *mv)
	{
		if (!player || !bot::IsValidBot(player->GetController()))
		{
			return;
		}

		auto replay = data::GetCurrentReplay();
		if (!replay->playingReplay)
		{
			return;
		}

		// Setting the origin via teleport will break client interp, set the values directly.
		// We have to do it here to be ahead of SetAbsOrigin calls in FinishMove.
		// Позиция конца тика — это то, что реально увидит клиент (сеть отдаёт её раз в тик),
		// поэтому замедление живёт именно здесь: при скорости <1 плейхед за тик проходит
		// часть кадра, и бот едет плавно, а не замирает до следующего целого кадра.
		u32 endTick;
		f32 endFrac;
		EndOfTickPlayhead(replay, &endTick, &endFrac);
		SampledState post = SampleFrame(replay, endTick, endFrac);
		mv->m_vecAbsOrigin = post.origin;
		mv->m_vecVelocity = post.velocity;
		mv->m_vecViewAngles = post.angles;

		// Directly set the pawn's origin/angles/velocity to something else so that they will be resynchronized.
		auto pawn = player->GetPlayerPawn();
		pawn->m_CBodyComponent()->m_pSceneNode()->m_vecAbsOrigin(post.origin + Vector(0, 0, 1000)); // Move it up so it's obviously wrong
	}

	void OnPhysicsSimulatePost(KZPlayer *player)
	{
		if (!player || !bot::IsValidBot(player->GetController()))
		{
			return;
		}

		auto replay = data::GetCurrentReplay();
		if (!replay->playingReplay)
		{
			return;
		}

		auto pawn = player->GetPlayerPawn();
		TickData *tickData = &replay->tickData[replay->currentTick];

		auto moveServices = player->GetMoveServices();
		moveServices->m_nButtons().m_pButtonStates[0] = tickData->post.buttons[0];
		moveServices->m_nButtons().m_pButtonStates[1] = tickData->post.buttons[1];
		moveServices->m_nButtons().m_pButtonStates[2] = tickData->post.buttons[2];
		moveServices->m_LegacyJump().m_flJumpPressedTime =
			g_pKZUtils->GetServerGlobals()->curtime + tickData->post.jumpPressedTime - tickData->gameTime;

		moveServices->m_flDuckSpeed = tickData->post.duckSpeed;
		moveServices->m_flDuckAmount = tickData->post.duckAmount;
		moveServices->m_flLastDuckTime = g_pKZUtils->GetServerGlobals()->curtime + tickData->post.lastDuckTime - tickData->gameTime;
		moveServices->m_bDucking = tickData->post.replayFlags.ducking;
		moveServices->m_bDucked = tickData->post.replayFlags.ducked;
		moveServices->m_bDesiresDuck = tickData->post.replayFlags.desiresDuck;
		player->SetMoveType(tickData->post.moveType);
		u32 playerFlagBits = (-1) & ~((u32)(FL_CLIENT | FL_FAKECLIENT | FL_BOT));
		pawn->m_fFlags = (pawn->m_fFlags & ~playerFlagBits) | (tickData->post.entityFlags & playerFlagBits);
		// Углы конца тика — с того же сэмпла, что и позиция в OnFinishMovePre: v_angle
		// определяет взгляд наблюдателя за ботом, разъехавшись с телом он давал бы рывки
		// камеры на замедлении. При скорости 1 сэмпл равен post текущего кадра.
		{
			u32 angTick;
			f32 angFrac;
			EndOfTickPlayhead(replay, &angTick, &angFrac);
			pawn->v_angle = SampleFrame(replay, angTick, angFrac).angles;
		}

		// If replay is paused, disable gravity and freeze the bot at current tick
		if (replay->replayPaused)
		{
			// Disable gravity to prevent physics interference
			pawn->SetGravityScale(0.0f);
			// Keep velocity at zero to prevent movement
			pawn->m_vecAbsVelocity(Vector(0, 0, 0));
			// Don't advance tick or process events/jumps
			// «Таймер рана идёт» — это startTime != 0, а НЕ startTime > 0: якорь считается
			// как curtime - время события, а curtime обнуляется на смене карты, поэтому на
			// свежей карте якорь трёхминутного рана штатно отрицателен. С прежним `> 0`
			// пауза не морозила таймер первые минуты аптайма. Тот же признак, что в
			// GetReplayTime (data.cpp).
			if (replay->startTime != 0.0f)
			{
				// Зрительская пауза (не записанная): тик заморожен, а curtime идёт —
				// двигаем startTime, чтобы активное время не росло.
				replay->startTime += ENGINE_FIXED_TICK_INTERVAL;
				// Если ОДНОВРЕМЕННО активна записанная пауза — сдвигаем и её якорь, чтобы
				// замороженные зрительской паузой тики не попали в accumulatedPauseTime на резюме.
				if (replay->paused && replay->pauseStartTime > 0.0f)
				{
					replay->pauseStartTime += ENGINE_FIXED_TICK_INTERVAL;
				}
			}
			return;
		}

		// Restore normal gravity when not paused
		pawn->SetGravityScale(1.0f);

		// Update checkpoint state from current tick data
		replay->currentCpIndex = tickData->checkpoint.index;
		replay->currentCheckpoint = tickData->checkpoint.checkpointCount;
		replay->currentTeleport = tickData->checkpoint.teleportCount;

		// Process jumps and events after physics simulation
		events::CheckJumps(*player);
		events::CheckEvents(*player);

		// Время рана считается через аккумулятор пауз (GetReplayTime):
		// активное = (curtime - startTime) - accumulatedPauseTime. Записанная пауза
		// набирается в accumulatedPauseTime на TIMER_RESUME (events.cpp), поэтому здесь
		// тиковая компенсация startTime на записанной паузе НЕ нужна.

		// Продвижение плейхеда со скоростью воспроизведения: за серверный тик проигрывается
		// playbackSpeed кадров записи. Дробный остаток копится в tickFraction (по нему идёт
		// интерполяция позы), целая часть — сколько записанных кадров реально пройдено.
		// При скорости 1 это ровно `currentTick++`, как было.
		f32 playhead = replay->tickFraction + replay->playbackSpeed;
		u32 steps = (u32)playhead;
		replay->tickFraction = playhead - (f32)steps;

		// Время рана обязано идти по КАДРАМ, а не по стенным часам: за этот серверный тик
		// показано steps кадров, значит время рана выросло на steps интервалов, а curtime —
		// на один. Разницу гасим сдвигом якоря (та же механика, что у зрительской паузы,
		// которая есть частный случай steps == 0).
		// startTime != 0 (а не > 0) — по той же причине, что и в ветке паузы выше.
		if (steps != 1 && replay->startTime != 0.0f)
		{
			f32 drift = (1.0f - (f32)steps) * ENGINE_FIXED_TICK_INTERVAL;
			replay->startTime += drift;
			if (replay->paused && replay->pauseStartTime > 0.0f)
			{
				replay->pauseStartTime += drift;
			}
		}

		replay->currentTick += steps;
		// Пропуск записанных пауз: если следующий тик попал в паузный сегмент, прыгаем
		// сразу на кадр возобновления — бот не стоит на месте всю паузу записанного игрока.
		// Активное время не разъезжается: пропускаются РОВНО паузные тики, поэтому число
		// реально проигранных кадров = число активных тиков, и curtime-startTime и так даёт
		// активное время (accumulatedPauseTime остаётся ~0: TIMER_PAUSE и TIMER_RESUME
		// обрабатываются в CheckEvents одним кадром на возобновлении).
		u32 tickBeforeSkip = replay->currentTick;
		replay->currentTick = AdvancePastPauses(replay->currentTick);
		if (replay->currentTick != tickBeforeSkip)
		{
			// Перепрыгнули паузный сегмент — приземлились ровно на кадр возобновления,
			// доля внутри кадра к нему не относится.
			replay->tickFraction = 0.0f;
		}
		if (replay->currentTick >= replay->tickCount)
		{
			bot::KickBot();
			replay->playingReplay = false;
			ClearPauseSegments();
		}
	}

	void OnPlayerRunCommandPre(KZPlayer *player, PlayerCommand *command)
	{
		if (!player || !bot::IsValidBot(player->GetController()))
		{
			return;
		}

		auto replay = data::GetCurrentReplay();
		if (!replay->playingReplay)
		{
			return;
		}
		if (replay->currentTick >= replay->tickCount)
		{
			return;
		}

		TickData *tickData = &replay->tickData[replay->currentTick];
		command->mutable_base()->set_forwardmove(tickData->forward);
		command->mutable_base()->set_leftmove(tickData->left);
		command->mutable_base()->set_upmove(tickData->up);

		command->mutable_base()->mutable_viewangles()->Clear();
		if (tickData->pre.angles.x != 0)
		{
			command->mutable_base()->mutable_viewangles()->set_x(tickData->pre.angles.x);
		}
		if (tickData->pre.angles.y != 0)
		{
			command->mutable_base()->mutable_viewangles()->set_y(tickData->pre.angles.y);
		}
		if (tickData->pre.angles.z != 0)
		{
			command->mutable_base()->mutable_viewangles()->set_z(tickData->pre.angles.z);
		}

		if (tickData->pre.buttons[0])
		{
			command->mutable_base()->mutable_buttons_pb()->set_buttonstate1(tickData->pre.buttons[0]);
			command->buttonstates.m_pButtonStates[0] = tickData->pre.buttons[0];
		}
		if (tickData->pre.buttons[1])
		{
			command->mutable_base()->mutable_buttons_pb()->set_buttonstate2(tickData->pre.buttons[1]);
			command->buttonstates.m_pButtonStates[1] = tickData->pre.buttons[1];
		}
		if (tickData->pre.buttons[2])
		{
			command->mutable_base()->mutable_buttons_pb()->set_buttonstate3(tickData->pre.buttons[2]);
			command->buttonstates.m_pButtonStates[2] = tickData->pre.buttons[2];
		}

		if (tickData->leftHanded)
		{
			command->set_left_hand_desired(true);
		}
		player->SetMoveType(tickData->pre.moveType);

		SubtickData *subtickData = &replay->subtickData[replay->currentTick];

		// Bots should never have any subtick move, but who knows?
		command->mutable_base()->clear_subtick_moves();
		for (u32 i = 0; i < subtickData->numSubtickMoves && i < MAX_SUBTICK_MOVES; i++)
		{
			auto move = command->mutable_base()->add_subtick_moves();
			move->set_when(subtickData->subtickMoves[i].when);
			move->set_button(subtickData->subtickMoves[i].button);
			if (!subtickData->subtickMoves[i].IsAnalogInput())
			{
				move->set_pressed(subtickData->subtickMoves[i].pressed);
				if (subtickData->subtickMoves[i].analogMove.pitch_delta != 0)
				{
					move->set_pitch_delta(subtickData->subtickMoves[i].analogMove.pitch_delta);
				}
				if (subtickData->subtickMoves[i].analogMove.yaw_delta != 0)
				{
					move->set_yaw_delta(subtickData->subtickMoves[i].analogMove.yaw_delta);
				}
			}
			else
			{
				if (subtickData->subtickMoves[i].analogMove.analog_forward_delta != 0)
				{
					move->set_analog_forward_delta(subtickData->subtickMoves[i].analogMove.analog_forward_delta);
				}
				if (subtickData->subtickMoves[i].analogMove.analog_left_delta != 0)
				{
					move->set_analog_left_delta(subtickData->subtickMoves[i].analogMove.analog_left_delta);
				}
			}
		}
		KZ_LOG_DEBUG(LogChannel::Replays, "%s\n", command->DebugString().c_str());
		CheckWeapon(*player, *command);
	}

	void CheckWeapon(KZPlayer &player, PlayerCommand &cmd)
	{
		auto replay = data::GetCurrentReplay();
		if (!replay->playingReplay)
		{
			return;
		}

		TickData *tickData = &replay->tickData[replay->currentTick];

		// Check what the current weapon should be.
		i32 weaponIndex = tickData->weapon;
		i32 found = -1;
		for (i32 i = 0; i < replay->weaponTableSize; i++)
		{
			if (replay->weaponIndices[i] == weaponIndex)
			{
				found = i;
				break;
			}
		}
		if (found == -1)
		{
			player.GetPlayerPawn()->m_pWeaponServices()->m_hActiveWeapon(nullptr);
			return;
		}
		EconInfo desiredWeapon = replay->weapons[found];
		EconInfo activeWeapon = player.GetPlayerPawn()->m_pWeaponServices()->m_hActiveWeapon().Get();

		if (desiredWeapon != activeWeapon)
		{
			if (desiredWeapon.mainInfo.itemDef == 0)
			{
				// If weapon is 0, then remove the active weapon from the player.
				player.GetPlayerPawn()->m_pWeaponServices()->m_hActiveWeapon(nullptr);
				return;
			}

			// Find the weapon in the player's inventory.
			CUtlVector<CHandle<CBasePlayerWeapon>> *weapons = player.GetPlayerPawn()->m_pWeaponServices()->m_hMyWeapons();
			for (i32 i = 0; i < weapons->Count(); i++)
			{
				CBasePlayerWeapon *invWeapon = weapons->Element(i).Get();
				if (desiredWeapon == EconInfo(invWeapon))
				{
					cmd.mutable_base()->set_weaponselect(invWeapon->entindex());
					return;
				}
			}

			// Стоп-кровь: если выдача этого itemDef уже фейлилась — не пытаться каждый тик
			// (повторный цикл RemoveAllItems+null-give раздевал бота, включая пистолет).
			if ((i32)desiredWeapon.mainInfo.itemDef == g_lastFailedGiveItemDef)
			{
				return;
			}

			// Check if we can get away with not stripping all weapons here.
			gear_slot_t desiredGearSlot = KZ::replaysystem::item::GetWeaponGearSlot(desiredWeapon.mainInfo.itemDef);
			for (i32 i = 0; i < weapons->Count(); i++)
			{
				CBasePlayerWeapon *invWeapon = weapons->Element(i).Get();
				gear_slot_t invGearSlot =
					KZ::replaysystem::item::GetWeaponGearSlot(invWeapon->m_AttributeManager().m_Item().m_iItemDefinitionIndex());
				if (invGearSlot == desiredGearSlot)
				{
					// We have a weapon in the same gear slot, so we need to strip all weapons to avoid conflicts.
					player.GetPlayerPawn()->m_pItemServices()->RemoveAllItems(false);
					break;
				}
			}

			std::string weaponName = KZ::replaysystem::item::GetWeaponName(desiredWeapon.mainInfo.itemDef);
			CBasePlayerWeapon *newWeapon = player.GetPlayerPawn()->m_pItemServices()->GiveNamedItem(weaponName.c_str());
			if (newWeapon)
			{
				KZ::replaysystem::item::ApplyItemAttributesToWeapon(*newWeapon, desiredWeapon);
				cmd.mutable_base()->set_weaponselect(newWeapon->entindex());
				g_lastFailedGiveItemDef = -1;
			}
			else
			{
				g_lastFailedGiveItemDef = desiredWeapon.mainInfo.itemDef;
			}
			// Диагностика скинов бота: что записано в реплее и что выдали (ok=0 больше не ретраится).
			// fflush — stdout контейнера буферизуется (урок kzt-саги).
			Msg("[replay-item] give '%s' itemDef=%d attrs=%d ok=%d\n", weaponName.c_str(), desiredWeapon.mainInfo.itemDef,
				desiredWeapon.mainInfo.numAttributes, newWeapon != nullptr);
			fflush(stdout);
		}
	}

	void InitializeWeapons()
	{
		auto replay = data::GetCurrentReplay();
		CCSPlayerController *bot = bot::GetBot();
		if (!bot)
		{
			return;
		}

		CCSPlayerPawn *pawn = bot->GetPlayerPawn();
		pawn->m_pItemServices()->RemoveAllItems(false);
	}

	void StartReplay()
	{
		auto replay = data::GetCurrentReplay();
		replay->playingReplay = true;
		replay->replayPaused = false;
		replay->currentTick = 0;
		// Новый реплей всегда стартует в обычной скорости: прошлый зритель мог оставить
		// замедление, а следующий об этом не знает (бот и реплей в сети — одни на всех).
		replay->playbackSpeed = 1.0f;
		replay->tickFraction = 0.0f;
		g_lastFailedGiveItemDef = -1;
		// Разбор диапазонов записанных пауз (для их пропуска при воспроизведении).
		BuildPauseSegments();
	}

	void ApplyTickState(KZPlayer *player, const TickData *tickData)
	{
		if (!player || !tickData)
		{
			return;
		}

		auto pawn = player->GetPlayerPawn();

		// Set position and physics state
		pawn->m_CBodyComponent()->m_pSceneNode()->m_vecAbsOrigin(tickData->post.origin);
		pawn->m_angEyeAngles(tickData->post.angles);
		pawn->m_vecAbsVelocity(tickData->post.velocity);
		pawn->v_angle = tickData->post.angles;

		// Set movement state
		auto moveServices = player->GetMoveServices();
		moveServices->m_flDuckSpeed = tickData->post.duckSpeed;
		moveServices->m_flDuckAmount = tickData->post.duckAmount;
		moveServices->m_flLastDuckTime = g_pKZUtils->GetServerGlobals()->curtime + tickData->post.lastDuckTime - tickData->gameTime;
		moveServices->m_bDucking = tickData->post.replayFlags.ducking;
		moveServices->m_bDucked = tickData->post.replayFlags.ducked;
		moveServices->m_bDesiresDuck = tickData->post.replayFlags.desiresDuck;
		player->SetMoveType(tickData->post.moveType);

		// Set entity flags
		u32 playerFlagBits = (-1) & ~((u32)(FL_CLIENT | FL_FAKECLIENT | FL_BOT));
		pawn->m_fFlags = (pawn->m_fFlags & ~playerFlagBits) | (tickData->post.entityFlags & playerFlagBits);
	}

} // namespace KZ::replaysystem::playback
