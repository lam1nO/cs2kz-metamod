#include "cs_usercmd.pb.h"
#include "cs2kz.h"
#include "movement.h"
#include "utils/detours.h"
#include "utils/gameconfig.h"
#include "tier0/memdbgon.h"
#include "sdk/usercmd.h"
#include "vprof.h"
#ifdef DEBUG_TPM
#include "fmtstr.h"

CUtlVector<TraceHistory> traceHistory;
#endif
extern CGameConfig *g_pGameConfig;

bool movement::InitDetours()
{
	bool ok = true;

	// Фаза 1 — только резолв сигнатур и funchook_prepare: игра ещё не тронута.
	CREATE_DETOUR(g_pGameConfig, PhysicsSimulate, ok);
	CREATE_DETOUR(g_pGameConfig, ProcessUsercmds, ok);
	CREATE_DETOUR(g_pGameConfig, SetupMove, ok);
	CREATE_DETOUR(g_pGameConfig, ProcessMovement, ok);
	CREATE_DETOUR(g_pGameConfig, PlayerMove, ok);
	CREATE_DETOUR(g_pGameConfig, CheckParameters, ok);
	CREATE_DETOUR(g_pGameConfig, FullWalkMove, ok);
	CREATE_DETOUR(g_pGameConfig, CheckWater, ok);
	CREATE_DETOUR(g_pGameConfig, WaterMove, ok);
	CREATE_DETOUR(g_pGameConfig, CheckVelocity, ok);
	CREATE_DETOUR(g_pGameConfig, Duck, ok);
	CREATE_DETOUR(g_pGameConfig, CanUnduck, ok);
	CREATE_DETOUR(g_pGameConfig, LadderMove, ok);
	CREATE_DETOUR(g_pGameConfig, CheckJumpButtonLegacy, ok);
	CREATE_DETOUR(g_pGameConfig, CheckJumpButtonModern, ok);
	CREATE_DETOUR(g_pGameConfig, OnJumpLegacy, ok);
	CREATE_DETOUR(g_pGameConfig, OnJumpModern, ok);
	CREATE_DETOUR(g_pGameConfig, AirMove, ok);
	CREATE_DETOUR(g_pGameConfig, AirAccelerate, ok);
	CREATE_DETOUR(g_pGameConfig, Friction, ok);
	CREATE_DETOUR(g_pGameConfig, WalkMove, ok);
	CREATE_DETOUR(g_pGameConfig, TryPlayerMove, ok);
	CREATE_DETOUR(g_pGameConfig, CategorizePosition, ok);
	CREATE_DETOUR(g_pGameConfig, CheckFalling, ok);
	CREATE_DETOUR(g_pGameConfig, PostThink, ok);

	if (!ok)
	{
		// Ни один детур не установлен, снимаем подготовленные и честно отказываем.
		FlushAllDetours();
		return false;
	}

	// Фаза 2 — установка. Провал здесь означает отказ funchook, а не сигнатуры.
	ENABLE_DETOUR(PhysicsSimulate, ok);
	ENABLE_DETOUR(ProcessUsercmds, ok);
	ENABLE_DETOUR(SetupMove, ok);
	ENABLE_DETOUR(ProcessMovement, ok);
	ENABLE_DETOUR(PlayerMove, ok);
	ENABLE_DETOUR(CheckParameters, ok);
	ENABLE_DETOUR(FullWalkMove, ok);
	ENABLE_DETOUR(CheckWater, ok);
	ENABLE_DETOUR(WaterMove, ok);
	ENABLE_DETOUR(CheckVelocity, ok);
	ENABLE_DETOUR(Duck, ok);
	ENABLE_DETOUR(CanUnduck, ok);
	ENABLE_DETOUR(LadderMove, ok);
	ENABLE_DETOUR(CheckJumpButtonLegacy, ok);
	ENABLE_DETOUR(CheckJumpButtonModern, ok);
	ENABLE_DETOUR(OnJumpLegacy, ok);
	ENABLE_DETOUR(OnJumpModern, ok);
	ENABLE_DETOUR(AirMove, ok);
	ENABLE_DETOUR(AirAccelerate, ok);
	ENABLE_DETOUR(Friction, ok);
	ENABLE_DETOUR(WalkMove, ok);
	ENABLE_DETOUR(TryPlayerMove, ok);
	ENABLE_DETOUR(CategorizePosition, ok);
	ENABLE_DETOUR(CheckFalling, ok);
	ENABLE_DETOUR(PostThink, ok);

	if (!ok)
	{
		FlushAllDetours();
		return false;
	}

	// CanMove и MoveInit больше не детурятся: их хуки (OnCanMove/OnMoveInit и Post) —
	// пустые виртуальные во ВСЕХ реализациях дерева (movement.h, kz.h/kz_player.cpp только
	// пробрасывает, kz_mode.h, kz_style.h; ни один режим и ни один стиль их не
	// переопределяет). Держать под них сигнатуры значило платить пересъёмкой после каждого
	// апдейта Valve за поведение, которого нет. ВНИМАНИЕ: режимы и стили грузятся как чужие
	// .so из addons/cs2kz/modes (KZ::mode::LoadModePlugins), и внешний режим, реализовавший
	// эти хуки, соберётся и загрузится, но вызван не будет — молча. Возвращаешь детуры —
	// верни и записи в gamedata, пересняв сигнатуры: старые мертвы с билда 25470087.
	return true;
}

MovementPlayerManager *playerManager = static_cast<MovementPlayerManager *>(g_pPlayerManager);

void FASTCALL movement::Detour_PhysicsSimulate(CCSPlayerController *controller)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (controller->m_bIsHLTV)
	{
		return;
	}
	g_KZPlugin.simulatingPhysics = true;
	MovementPlayer *player = playerManager->ToPlayer(controller);
	player->OnPhysicsSimulate();
	PhysicsSimulate(controller);
	player->OnPhysicsSimulatePost();
	g_KZPlugin.simulatingPhysics = false;
}

i32 FASTCALL movement::Detour_ProcessUsercmds(CCSPlayerController *controller, PlayerCommand *cmds, int numcmds, bool paused, float margin)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(controller);
	player->OnProcessUsercmds(cmds, numcmds);
	auto retValue = ProcessUsercmds(controller, cmds, numcmds, paused, margin);
	player->OnProcessUsercmdsPost(cmds, numcmds);
	// Явный выход из скоупа снят: он и так был лишним — VPROF_BUDGET выше это RAII, скоуп
	// закрывался дважды. Билд CS2 25470087 убрал CVProfile::ExitScope из tier0, и лишний
	// вызов стал неразрешимым символом, из-за которого metamod не грузил плагин целиком.
	return retValue;
}

void FASTCALL movement::Detour_SetupMove(CCSPlayer_MovementServices *ms, PlayerCommand *pc, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	CBasePlayerController *controller = player->GetController();
	player->currentMoveData = mv;
	player->moveDataPre = CMoveData(*mv);
	player->OnSetupMove(pc);
	SetupMove(ms, pc, mv);
	player->OnSetupMovePost(pc);
}

void FASTCALL movement::Detour_ProcessMovement(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->currentMoveData = mv;
	player->moveDataPre = CMoveData(*mv);
	player->OnProcessMovement();
	ProcessMovement(ms, mv);
	player->moveDataPost = CMoveData(*mv);
	player->OnProcessMovementPost();
}

bool FASTCALL movement::Detour_PlayerMove(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnPlayerMove();
	auto retValue = PlayerMove(ms, mv);
	player->OnPlayerMovePost();
	return retValue;
}

void FASTCALL movement::Detour_CheckParameters(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnCheckParameters();
	CheckParameters(ms, mv);
	player->OnCheckParametersPost();
}

bool FASTCALL movement::Detour_CanMove(CCSPlayerPawnBase *pawn)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(pawn);
	player->OnCanMove();
	auto retValue = CanMove(pawn);
	player->OnCanMovePost();
	return retValue;
}

void FASTCALL movement::Detour_FullWalkMove(CCSPlayer_MovementServices *ms, CMoveData *mv, bool ground)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnFullWalkMove(ground);
	FullWalkMove(ms, mv, ground);
	player->OnFullWalkMovePost(ground);
}

bool FASTCALL movement::Detour_MoveInit(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnMoveInit();
	auto retValue = MoveInit(ms, mv);
	player->OnMoveInitPost();
	return retValue;
}

bool FASTCALL movement::Detour_CheckWater(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnCheckWater();
	auto retValue = CheckWater(ms, mv);
	player->OnCheckWaterPost();

	return retValue;
}

void FASTCALL movement::Detour_WaterMove(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnWaterMove();
#ifdef WATER_FIX
	if (player->enableWaterFix)
	{
		player->ignoreNextCategorizePosition = true;
	}
#endif
	WaterMove(ms, mv);
	player->OnWaterMovePost();
}

void FASTCALL movement::Detour_CheckVelocity(CCSPlayer_MovementServices *ms, CMoveData *mv, const char *a3)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnCheckVelocity(a3);
	CheckVelocity(ms, mv, a3);
	player->OnCheckVelocityPost(a3);
}

void FASTCALL movement::Detour_Duck(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnDuck();
	player->processingDuck = true;
	Duck(ms, mv);
	player->processingDuck = false;
	player->OnDuckPost();
}

bool FASTCALL movement::Detour_CanUnduck(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnCanUnduck();
	bool canUnduck = CanUnduck(ms, mv);
	player->OnCanUnduckPost(canUnduck);
	return canUnduck;
}

bool FASTCALL movement::Detour_LadderMove(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnLadderMove();
	Vector oldVelocity = mv->m_vecVelocity;
	MoveType_t oldMoveType = player->GetPlayerPawn()->m_MoveType();
	bool result = LadderMove(ms, mv);
	if (player->GetPlayerPawn()->m_lifeState() != LIFE_DEAD && !result && oldMoveType == MOVETYPE_LADDER)
	{
		// Do the setting part ourselves as well, but do not fire callback because it will be called later with the correct parameters.
		player->SetMoveType(MOVETYPE_WALK, false);
	}
	// Was on ladder before, not anymore
	if (!result && oldMoveType == MOVETYPE_LADDER)
	{
		// Ladderjump takeoff detection is delayed by one process movement call, we have to use the previous origin.
		player->RegisterTakeoff(false, true, &player->lastValidLadderOrigin);
		player->OnChangeMoveType(MOVETYPE_LADDER);
	}
	// Was not on ladder before, now is, and is not on the ground
	else if (result && oldMoveType != MOVETYPE_LADDER && player->GetPlayerPawn()->m_MoveType() == MOVETYPE_LADDER
			 && !(player->GetPlayerPawn()->m_fFlags & FL_ONGROUND))
	{
		player->RegisterLanding(oldVelocity, false);
		player->OnChangeMoveType(MOVETYPE_WALK);
	}
	// Player is on the ladder, pressing jump pushes them away from the ladder.
	else if (result && oldMoveType == MOVETYPE_LADDER && player->GetPlayerPawn()->m_MoveType() == MOVETYPE_WALK)
	{
		player->RegisterTakeoff(player->IsButtonPressed(IN_JUMP), true);
		player->possibleLadderHop = true;
		player->OnChangeMoveType(MOVETYPE_LADDER);
	}

	// Register the last known ladder position for jumpstats purposes.
	if (result && player->GetPlayerPawn()->m_MoveType() == MOVETYPE_LADDER)
	{
		player->GetOrigin(&player->lastValidLadderOrigin);
	}
	player->OnLadderMovePost();
	return result;
}

void FASTCALL movement::Detour_CheckJumpButtonLegacy(CCSPlayerLegacyJump *legacy, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	CCSPlayer_MovementServices *ms = legacy->m_pMovementServices;
	MovementPlayer *player = playerManager->ToPlayer(ms);
#ifdef WATER_FIX
	if (player->enableWaterFix && ms->pawn->m_MoveType() == MOVETYPE_WALK && ms->pawn->m_flWaterLevel() > 0.5f && ms->pawn->m_fFlags & FL_ONGROUND)
	{
		if (ms->m_nButtons().m_pButtonStates[0] & IN_JUMP)
		{
			ms->m_nButtons().m_pButtonStates[1] |= IN_JUMP;
		}
	}
#endif
	player->OnCheckJumpButtonLegacy();
	CheckJumpButtonLegacy(legacy, mv);
	player->OnCheckJumpButtonLegacyPost();
}

void FASTCALL movement::Detour_CheckJumpButtonModern(CCSPlayerModernJump *modern, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	CCSPlayer_MovementServices *ms = modern->m_pMovementServices;
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnCheckJumpButtonModern();
	CheckJumpButtonModern(modern, mv);
	player->OnCheckJumpButtonModernPost();
}

void FASTCALL movement::Detour_OnJumpLegacy(CCSPlayerLegacyJump *legacy, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	CCSPlayer_MovementServices *ms = legacy->m_pMovementServices;
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnJumpLegacy();
	Vector oldOutWishVel = mv->m_outWishVel;
	OnJumpLegacy(legacy, mv);
	if (mv->m_outWishVel != oldOutWishVel)
	{
		player->inPerf = (!player->takeoffFromLadder && !player->oldWalkMoved);
		player->RegisterTakeoff(true, player->possibleLadderHop);
		player->OnStopTouchGround();
	}
	player->OnJumpLegacyPost();
}

void FASTCALL movement::Detour_OnJumpModern(CCSPlayerModernJump *modern, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	CCSPlayer_MovementServices *ms = modern->m_pMovementServices;
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnJumpModern();
	Vector oldOutWishVel = mv->m_outWishVel;
	OnJumpModern(modern, mv);
	if (mv->m_outWishVel != oldOutWishVel)
	{
		player->inPerf = (!player->takeoffFromLadder && !player->oldWalkMoved);
		player->RegisterTakeoff(true, player->possibleLadderHop);
		player->OnStopTouchGround();
	}
	player->OnJumpModernPost();
}

void FASTCALL movement::Detour_AirMove(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnAirMove();
	AirMove(ms, mv);
	player->OnAirMovePost();
}

void FASTCALL movement::Detour_AirAccelerate(CCSPlayer_MovementServices *ms, CMoveData *mv, Vector &wishdir, f32 wishspeed, f32 accel)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnAirAccelerate(wishdir, wishspeed, accel);
	AirAccelerate(ms, mv, wishdir, wishspeed, accel);
	player->OnAirAcceleratePost(wishdir, wishspeed, accel);
}

void FASTCALL movement::Detour_Friction(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnFriction();
	Friction(ms, mv);
	player->OnFrictionPost();
}

void FASTCALL movement::Detour_WalkMove(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnWalkMove();
	WalkMove(ms, mv);
	player->walkMoved = true;
	player->OnWalkMovePost();
}

void FASTCALL movement::Detour_TryPlayerMove(CCSPlayer_MovementServices *ms, CMoveData *mv, Vector *pFirstDest, trace_t *pFirstTrace,
											 bool *bIsSurfing)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
#ifdef DEBUG_TPM
	traceHistory.RemoveAll();
	f32 initialError = ms->m_flAccumulatedJumpError();
	Vector initialVelocity = mv->m_vecVelocity;
	f32 &liveError = ms->m_flAccumulatedJumpError();
	Vector &liveVelocity = mv->m_vecVelocity;
	TraceShape.EnableDetour();
	player->OnTryPlayerMove(pFirstDest, pFirstTrace, bIsSurfing);
	Vector oldVelocity = mv->m_vecVelocity;
	i32 count = traceHistory.Count();
	TryPlayerMove(ms, mv, pFirstDest, pFirstTrace, bIsSurfing);
	if (traceHistory.Count() != count)
	{
		for (i32 i = 0; i + count < traceHistory.Count(); i++)
		{
			if (traceHistory[i].end != traceHistory[i + count].end)
			{
				KZ_LOG_DEBUG(LogChannel::General, "Trace not matching! Previous traces (initial error %f, initial velocity %s):\n", initialError,
							 VecToString(initialVelocity));
				for (i32 j = 0; j <= i; j++)
				{
					KZ_LOG_DEBUG(LogChannel::General, "Pred %f %f %f -> %f %f %f, error %f, velocity %s ", traceHistory[j].start.x,
								 traceHistory[j].start.y, traceHistory[j].start.z, traceHistory[j].end.x, traceHistory[j].end.y,
								 traceHistory[j].end.z, traceHistory[j].error, VecToString(traceHistory[j].velocity));
					if (traceHistory[j].didHit)
					{
						KZ_LOG_DEBUG(LogChannel::General, "hit %s (normal %s, hitpoint %s)\n", VecToString(traceHistory[j].m_vEndPos),
									 VecToString(traceHistory[j].m_vHitNormal), VecToString(traceHistory[j].m_vHitPoint));
					}
					else
					{
						KZ_LOG_DEBUG(LogChannel::General, "missed\n");
					}
					KZ_LOG_DEBUG(LogChannel::General, "Real %f %f %f -> %f %f %f, error %f, velocity %s ", traceHistory[j + count].start.x,
								 traceHistory[j + count].start.y, traceHistory[j + count].start.z, traceHistory[j + count].end.x,
								 traceHistory[j + count].end.y, traceHistory[j + count].end.z, traceHistory[j + count].error,
								 VecToString(traceHistory[j + count].velocity));
					if (traceHistory[j + count].didHit)
					{
						KZ_LOG_DEBUG(LogChannel::General, "hit %s (normal %s, hitpoint %s)\n", VecToString(traceHistory[j + count].m_vEndPos),
									 VecToString(traceHistory[j + count].m_vHitNormal), VecToString(traceHistory[j + count].m_vHitPoint));
					}
					else
					{
						KZ_LOG_DEBUG(LogChannel::General, "missed\n");
					}
				}
				break;
			}
		}
	}
#else
	player->OnTryPlayerMove(pFirstDest, pFirstTrace, bIsSurfing);
	Vector oldVelocity = mv->m_vecVelocity;
	TryPlayerMove(ms, mv, pFirstDest, pFirstTrace, bIsSurfing);
#endif
	if (mv->m_vecVelocity != oldVelocity)
	{
		// Velocity changed, must have collided with something.
		// In case of walkmove+stairs, it's possible that this is incorrect because TryPlayerMove won't immediately change the player's data,
		// but for now this doesn't matter.
		player->SetCollidingWithWorld();
	}
	player->OnTryPlayerMovePost(pFirstDest, pFirstTrace, bIsSurfing);

#ifdef DEBUG_TPM
	TraceShape.DisableDetour();
#endif
}

void FASTCALL movement::Detour_CategorizePosition(CCSPlayer_MovementServices *ms, CMoveData *mv, bool bStayOnGround)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
#ifdef WATER_FIX
	if (player->enableWaterFix && player->ignoreNextCategorizePosition)
	{
		player->ignoreNextCategorizePosition = false;
		return;
	}
#endif
	player->OnCategorizePosition(bStayOnGround);
	Vector oldVelocity = mv->m_vecVelocity;
	bool oldOnGround = !!(player->GetPlayerPawn()->m_fFlags() & FL_ONGROUND);

	CategorizePosition(ms, mv, bStayOnGround);

	bool ground = !!(player->GetPlayerPawn()->m_fFlags() & FL_ONGROUND);

	if (oldOnGround != ground)
	{
		if (ground)
		{
			player->RegisterLanding(oldVelocity);
			player->OnStartTouchGround();
		}
		else
		{
			player->RegisterTakeoff(false);
			player->OnStopTouchGround();
		}
	}
	player->OnCategorizePositionPost(bStayOnGround);
}

void FASTCALL movement::Detour_CheckFalling(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnCheckFalling();
	CheckFalling(ms, mv);
	player->OnCheckFallingPost();
}

void FASTCALL movement::Detour_PostThink(CCSPlayerPawnBase *pawn)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(pawn);
	player->OnPostThink();
	PostThink(pawn);
	player->OnPostThinkPost();
}
