#include "cs_usercmd.pb.h"
#include "kz_mode_kzt.h"
#include "utils/addresses.h"
#include "utils/interfaces.h"
#include "utils/gameconfig.h"
#include "sdk/usercmd.h"
#include "sdk/tracefilter.h"
#include "sdk/navphysicsinterface.h"
#include "sdk/entity/cbasetrigger.h"

KZTimerModePlugin g_KZTimerModePlugin;

CGameConfig *g_pGameConfig = NULL;
KZUtils *g_pKZUtils = NULL;
KZModeManager *g_pModeManager = NULL;
MappingInterface *g_pMappingApi = NULL;
ModeServiceFactory g_ModeFactory = [](KZPlayer *player) -> KZModeService * { return new KZTimerModeService(player); };
PLUGIN_EXPOSE(KZTimerModePlugin, g_KZTimerModePlugin);

CConVarRef<f32> sv_standable_normal("sv_standable_normal");
CConVar<bool> kz_kzt_jump_collapse("kz_kzt_jump_collapse", FCVAR_NONE,
	"Учитывать только первое нажатие прыжка за тик (перф-хит как в GO@128)", true);

bool KZTimerModePlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();
	// Load mode
	int success;
	g_pModeManager = (KZModeManager *)g_SMAPI->MetaFactory(KZ_MODE_MANAGER_INTERFACE, &success, 0);
	if (success == META_IFACE_FAILED)
	{
		V_snprintf(error, maxlen, "Failed to find %s interface", KZ_MODE_MANAGER_INTERFACE);
		return false;
	}
	g_pKZUtils = (KZUtils *)g_SMAPI->MetaFactory(KZ_UTILS_INTERFACE, &success, 0);
	if (success == META_IFACE_FAILED)
	{
		V_snprintf(error, maxlen, "Failed to find %s interface", KZ_UTILS_INTERFACE);
		return false;
	}
	g_pMappingApi = (MappingInterface *)g_SMAPI->MetaFactory(KZ_MAPPING_INTERFACE, &success, 0);
	if (success == META_IFACE_FAILED)
	{
		V_snprintf(error, maxlen, "Failed to find %s interface", KZ_MAPPING_INTERFACE);
		return false;
	}
	modules::Initialize();
	if (!interfaces::Initialize(ismm, error, maxlen))
	{
		V_snprintf(error, maxlen, "Failed to initialize interfaces");
		return false;
	}

	if (nullptr == (g_pGameConfig = g_pKZUtils->GetGameConfig()))
	{
		V_snprintf(error, maxlen, "Failed to get game config");
		return false;
	}

	if (!g_pModeManager->RegisterMode(g_PLID, MODE_NAME_SHORT_KZT, MODE_NAME_KZT, g_ModeFactory))
	{
		V_snprintf(error, maxlen, "Failed to register mode");
		return false;
	}

	ConVar_Register();
	return true;
}

bool KZTimerModePlugin::Unload(char *error, size_t maxlen)
{
	g_pModeManager->UnregisterMode(g_PLID);
	return true;
}

bool KZTimerModePlugin::Pause(char *error, size_t maxlen)
{
	g_pModeManager->UnregisterMode(g_PLID);
	return true;
}

bool KZTimerModePlugin::Unpause(char *error, size_t maxlen)
{
	if (!g_pModeManager->RegisterMode(g_PLID, MODE_NAME_SHORT_KZT, MODE_NAME_KZT, g_ModeFactory))
	{
		return false;
	}
	return true;
}

CGameEntitySystem *GameEntitySystem()
{
	return g_pKZUtils->GetGameEntitySystem();
}

/*
	Actual mode stuff.
*/

void KZTimerModeService::Reset()
{
	this->hasValidDesiredViewAngle = {};
	this->lastValidDesiredViewAngle = vec3_angle;
	this->lastJumpReleaseTime = {};
	this->oldDuckPressed = {};
	this->forcedUnduck = {};
	this->postProcessMovementZSpeed = {};

	this->preVelMod = 1.0f;
	this->effectivePreVelMod = 1.0f;
	this->preTickCounter = {};
	this->preVelModLastChange = {};

	this->didTPM = {};
	this->overrideTPM = {};
	this->tpmVelocity = vec3_origin;
	this->tpmOrigin = vec3_origin;
	this->lastValidPlane = vec3_origin;

	this->airMoving = {};
	this->tpmTriggerFixOrigins.RemoveAll();

	this->lastJumpPressTick = -1;
	this->jumpSuppressed = false;
}

void KZTimerModeService::Cleanup()
{
	auto pawn = this->player->GetPlayerPawn();
	if (pawn)
	{
		pawn->m_flVelocityModifier(1.0f);
	}
}

const char *KZTimerModeService::GetModeName()
{
	return MODE_NAME_KZT;
}

const char *KZTimerModeService::GetModeShortName()
{
	return MODE_NAME_SHORT_KZT;
}

bool KZTimerModeService::EnableWaterFix()
{
	return this->player->IsButtonPressed(IN_JUMP);
}

// Катофф prespeed (скорость отрыва): ниже → lowpre-набор порогов. Совпадает с gokz
// AdjustLowpreJumptypes (BH<360 → lowpre bhop, WJ<300 → lowpre weirdjump).
#define KZT_LOWPRE_CUTOFF_BHOP 360.0f
#define KZT_LOWPRE_CUTOFF_WJ   300.0f

DistanceTier KZTimerModeService::GetDistanceTier(JumpType jumpType, f32 distance, f32 takeoffSpeed)
{
	// No tiers given for 'Invalid' jumps.
	if (jumpType == JumpType_Invalid || jumpType == JumpType_FullInvalid || jumpType == JumpType_Fall || jumpType == JumpType_Other
		|| distance > 500.0f)
	{
		return DistanceTier_None;
	}

	// Выбор набора порогов: [0] нормальные / [1] lowpre. Гейт по prespeed (takeoffSpeed).
	// takeoffSpeed < 0 (не передан) → нормальный набор. Lowpre-катофф — только для типов,
	// у которых [1] отличается от [0] (BH/MBH/JB → 360, WJ → 300); прочие: [1]==[0].
	i32 lowpre = 0;
	if (takeoffSpeed >= 0.0f)
	{
		if (jumpType == JumpType_Bhop || jumpType == JumpType_MultiBhop || jumpType == JumpType_Jumpbug)
		{
			lowpre = (takeoffSpeed < KZT_LOWPRE_CUTOFF_BHOP) ? 1 : 0;
		}
		else if (jumpType == JumpType_WeirdJump)
		{
			lowpre = (takeoffSpeed < KZT_LOWPRE_CUTOFF_WJ) ? 1 : 0;
		}
	}

	// Get highest tier distance that the jump beats
	DistanceTier tier = DistanceTier_None;
	while (tier + 1 < DISTANCETIER_COUNT && distance >= distanceTiers[jumpType][lowpre][tier])
	{
		tier = (DistanceTier)(tier + 1);
	}

	return tier;
}

const CVValue_t *KZTimerModeService::GetModeConVarValues()
{
	return modeCvarValues;
}

void KZTimerModeService::OnStopTouchGround()
{
	if (this->player->GetMoveType() != MOVETYPE_WALK)
	{
		return;
	}

	Vector velocity;
	this->player->GetVelocity(&velocity);

	// Перф в KZT — прыжок в окне KZT_PERF_WINDOW после приземления, детектим по времени на
	// земле (как CKZ). inPerf ПЕРЕЗАПИСЫВАЕМ: Detour_OnJumpLegacy уже поставил его по широкой
	// эвристике !oldWalkMoved (~целый тик), и без перезаписи HUD красил «перфы», к которым
	// кап/высота не применялись. Как в GOKZ: один флаг гейтит и кап, и HUD, и старт таймера.
	// Высоту полной 55.83 на бхопе даёт сам legacy-прыжок движка; здесь — только скорость и
	// перф-высота (выравнивание origin для консистентности jumpstats).
	f32 timeOnGround = this->player->takeoffTime - this->player->landingTime;
	bool perf = this->player->jumped && timeOnGround <= KZT_PERF_WINDOW && !this->player->possibleLadderHop && !this->player->takeoffFromLadder;
	this->player->inPerf = perf;
	if (perf)
	{
		// gokz TweakJump: режем горизонталь до PERF_SPEED_CAP (KZTimer-механика, не CKZ-логарифм).
		f32 horizSpeed = velocity.Length2D();
		if (horizSpeed > PERF_SPEED_CAP)
		{
			// Масштабируем горизонтальные компоненты, сохраняя направление.
			f32 scale = PERF_SPEED_CAP / horizSpeed;
			velocity.x *= scale;
			velocity.y *= scale;
			this->player->SetVelocity(velocity);
		}
		// takeoffVelocity обновляем при КАЖДОМ перфе (после возможного cap),
		// иначе при скорости ≤380 jumpstats получает устаревшее значение.
		this->player->takeoffVelocity = velocity;

		// Перф-высота: выровнять origin.z по поверхности земли (консистентность jumpstats, как CKZ).
		Vector origin;
		this->player->GetOrigin(&origin);
		origin.z = this->player->GetGroundPosition();
		this->player->SetOrigin(origin);
		this->player->takeoffOrigin = origin;
	}
}

void KZTimerModeService::OnStartTouchGround()
{
	this->SlopeFix();
	bbox_t bounds;
	this->player->GetBBoxBounds(&bounds);
	Vector ground = this->player->landingOrigin;
	ground.z = this->player->GetGroundPosition() - 0.03125f;
	this->player->TouchTriggersAlongPath(this->player->landingOrigin, ground, bounds);
}

void KZTimerModeService::OnPhysicsSimulate()
{
	CCSPlayer_MovementServices *moveServices = this->player->GetMoveServices();
	if (!moveServices)
	{
		return;
	}
	u32 tickCount = g_pKZUtils->GetServerGlobals()->tickcount;

	f32 subtickMoveTime = (tickCount - 0.5) * ENGINE_FIXED_TICK_INTERVAL;
	for (u32 i = 0; i < 4; i++)
	{
		if (fabs(subtickMoveTime - moveServices->m_arrForceSubtickMoveWhen[i]) < 0.001)
		{
			return;
		}
		if (subtickMoveTime > moveServices->m_arrForceSubtickMoveWhen[i])
		{
			moveServices->SetForcedSubtickMove(i, subtickMoveTime, false);
			return;
		}
	}
}

void KZTimerModeService::OnPhysicsSimulatePost()
{
	CCSPlayer_MovementServices *moveServices = this->player->GetMoveServices();
	if (!moveServices)
	{
		return;
	}
	u32 tickCount = g_pKZUtils->GetServerGlobals()->tickcount;

	f32 subtickMoveTime = (tickCount + 0.5) * ENGINE_FIXED_TICK_INTERVAL;
	for (u32 i = 0; i < 4; i++)
	{
		if (fabs(subtickMoveTime - moveServices->m_arrForceSubtickMoveWhen[i]) < 0.001)
		{
			subtickMoveTime += ENGINE_FIXED_TICK_INTERVAL;
			continue;
		}
		if (subtickMoveTime > moveServices->m_arrForceSubtickMoveWhen[i])
		{
			moveServices->SetForcedSubtickMove(i, subtickMoveTime);
			subtickMoveTime += ENGINE_FIXED_TICK_INTERVAL;
		}
	}
}

void KZTimerModeService::OnSetupMove(PlayerCommand *pc)
{
	for (i32 j = 0; j < pc->mutable_base()->subtick_moves_size(); j++)
	{
		CSubtickMoveStep *subtickMove = pc->mutable_base()->mutable_subtick_moves(j);
		if (subtickMove->button() == IN_ATTACK || subtickMove->button() == IN_ATTACK2 || subtickMove->button() == IN_RELOAD)
		{
			continue;
		}
		float when = subtickMove->when();
		if (subtickMove->button() == IN_JUMP)
		{
			f32 inputTime = (g_pKZUtils->GetGlobals()->tickcount + when - 1) * ENGINE_FIXED_TICK_INTERVAL;
			if (when != 0)
			{
				if (subtickMove->pressed() && inputTime - this->lastJumpReleaseTime > 0.5 * ENGINE_FIXED_TICK_INTERVAL)
				{
					this->player->GetMoveServices()->m_LegacyJump().m_bOldJumpPressed = false;
				}
				if (!subtickMove->pressed())
				{
					this->lastJumpReleaseTime = (g_pKZUtils->GetGlobals()->tickcount + when - 1) * ENGINE_FIXED_TICK_INTERVAL;
				}
			}
		}
		subtickMove->set_when(when >= 0.5 ? 0.5 : 0);
	}
}

// Схлопываем прыжковые попытки до одной на тик: в CS2 каждый щелчок колеса — отдельный
// сабтиковый сегмент со своей проверкой прыжка, в GO был один бит на команду. Повторные
// свежие нажатия в том же тике прячем от движка на время проверки и возвращаем обратно,
// чтобы остальной код (AC, реплеи, HUD) видел ввод нетронутым.
void KZTimerModeService::OnCheckJumpButtonLegacy()
{
	this->jumpSuppressed = false;
	if (!kz_kzt_jump_collapse.GetBool())
	{
		return;
	}
	CCSPlayer_MovementServices *ms = this->player->GetMoveServices();
	if (!ms)
	{
		return;
	}
	CInButtonState &buttons = ms->m_nButtons();
	if (!buttons.IsButtonNewlyPressed(IN_JUMP))
	{
		return; // удержание/отпускание не гейтим — legacy-прыжок и так требует нового нажатия
	}
	i64 tick = g_pKZUtils->GetServerGlobals()->tickcount;
	if (this->lastJumpPressTick != tick)
	{
		this->lastJumpPressTick = tick; // первая попытка в тике — пропускаем
		return;
	}
	for (int i = 0; i < 3; i++)
	{
		this->savedJumpBits[i] = buttons.m_pButtonStates[i] & IN_JUMP;
		buttons.m_pButtonStates[i] &= ~IN_JUMP;
	}
	this->savedOldJumpPressed = ms->m_LegacyJump().m_bOldJumpPressed();
	this->savedJumpPressedTime = ms->m_LegacyJump().m_flJumpPressedTime();
	this->jumpSuppressed = true;
}

void KZTimerModeService::OnCheckJumpButtonLegacyPost()
{
	if (!this->jumpSuppressed)
	{
		return;
	}
	CCSPlayer_MovementServices *ms = this->player->GetMoveServices();
	if (ms)
	{
		CInButtonState &buttons = ms->m_nButtons();
		for (int i = 0; i < 3; i++)
		{
			buttons.m_pButtonStates[i] |= this->savedJumpBits[i];
		}
		ms->m_LegacyJump().m_bOldJumpPressed = this->savedOldJumpPressed;
		ms->m_LegacyJump().m_flJumpPressedTime = this->savedJumpPressedTime;
	}
	this->jumpSuppressed = false;
}

void KZTimerModeService::OnProcessMovement()
{
	this->didTPM = false;
	if (this->player->GetPlayerPawn()->m_flVelocityModifier() != 1.0f)
	{
		this->player->GetPlayerPawn()->m_flVelocityModifier(1.0f);
	}
	this->CheckVelocityQuantization();
	this->RemoveCrouchJumpBind();
	this->ReduceDuckSlowdown();
	this->InterpolateViewAngles();
	// Update prestrafe velMod each movement tick; capture effective value for this tick
	this->effectivePreVelMod = this->CalcPrestrafeVelMod();
}

void KZTimerModeService::OnPlayerMove()
{
	this->originalMaxSpeed = this->player->currentMoveData->m_flMaxSpeed;
	// Apply KZTimer prestrafe: effective ground max speed = SPEED_NORMAL * effectivePreVelMod
	this->player->currentMoveData->m_flMaxSpeed = SPEED_NORMAL * this->effectivePreVelMod;
}

void KZTimerModeService::OnProcessMovementPost()
{
	this->player->UpdateTriggerTouchList();
	this->RestoreInterpolatedViewAngles();
	this->oldDuckPressed = this->forcedUnduck || this->player->IsButtonPressed(IN_DUCK, true);
	this->oldJumpPressed = this->player->IsButtonPressed(IN_JUMP);
	Vector velocity;
	this->player->GetVelocity(&velocity);
	this->postProcessMovementZSpeed = velocity.z;
	if (!this->didTPM)
	{
		this->lastValidPlane = vec3_origin;
	}
	f32 velMod = this->originalMaxSpeed > 0.0f ? (SPEED_NORMAL * this->effectivePreVelMod) / this->originalMaxSpeed : 1.0f;
	if (this->player->GetPlayerPawn()->m_flVelocityModifier() != velMod)
	{
		this->player->GetPlayerPawn()->m_flVelocityModifier(velMod);
	}
}

void KZTimerModeService::InterpolateViewAngles()
{
	// Second half of the movement, no change.
	CGlobalVars *globals = g_pKZUtils->GetGlobals();
	f64 subtickFraction, whole;
	subtickFraction = modf((f64)globals->curtime * ENGINE_FIXED_TICK_RATE, &whole);
	if (subtickFraction < 0.001)
	{
		return;
	}

	// First half of the movement, tweak the angle to be the middle of the desired angle and the last angle
	QAngle newAngles = player->currentMoveData->m_vecViewAngles;
	QAngle oldAngles = this->hasValidDesiredViewAngle ? this->lastValidDesiredViewAngle : this->player->moveDataPost.m_vecViewAngles;
	if (newAngles[YAW] - oldAngles[YAW] > 180)
	{
		newAngles[YAW] -= 360.0f;
	}
	else if (newAngles[YAW] - oldAngles[YAW] < -180)
	{
		newAngles[YAW] += 360.0f;
	}

	for (u32 i = 0; i < 3; i++)
	{
		newAngles[i] += oldAngles[i];
		newAngles[i] *= 0.5f;
	}
	player->currentMoveData->m_vecViewAngles = newAngles;
}

void KZTimerModeService::RestoreInterpolatedViewAngles()
{
	player->currentMoveData->m_vecViewAngles = player->moveDataPre.m_vecViewAngles;
	if (g_pKZUtils->GetGlobals()->frametime > 0.0f)
	{
		this->hasValidDesiredViewAngle = true;
		this->lastValidDesiredViewAngle = player->currentMoveData->m_vecViewAngles;
	}
}

void KZTimerModeService::RemoveCrouchJumpBind()
{
	this->forcedUnduck = false;

	bool onGround = this->player->GetPlayerPawn()->m_fFlags & FL_ONGROUND;
	bool justJumped = !this->oldJumpPressed && this->player->IsButtonPressed(IN_JUMP);

	if (onGround && !this->oldDuckPressed && justJumped)
	{
		this->player->GetMoveServices()->m_nButtons().m_pButtonStates[0] &= ~IN_DUCK;
		this->forcedUnduck = true;
	}
}

void KZTimerModeService::ReduceDuckSlowdown()
{
	if (!this->player->GetMoveServices()->m_bDucking && this->player->GetMoveServices()->m_flDuckSpeed < DUCK_SPEED_NORMAL - EPSILON)
	{
		this->player->GetMoveServices()->m_flDuckSpeed = DUCK_SPEED_NORMAL;
	}
	else if (this->player->GetMoveServices()->m_flDuckSpeed < DUCK_SPEED_MINIMUM - EPSILON)
	{
		this->player->GetMoveServices()->m_flDuckSpeed = DUCK_SPEED_MINIMUM;
	}
}

// Ported from gokz CalcPrestrafeVelMod (KZTimerGlobal).
// Returns the current prestrafe velocity modifier; also updates this->preVelMod.
// Must be called once per movement tick in OnProcessMovement.
f32 KZTimerModeService::CalcPrestrafeVelMod()
{
	bool onGround = (this->player->GetPlayerPawn()->m_fFlags & FL_ONGROUND) != 0;
	f32 curtime = g_pKZUtils->GetGlobals()->curtime;

	if (!onGround)
	{
		return this->preVelMod;
	}

	TurnState turning = this->player->GetTurning();

	if (turning == TURN_NONE)
	{
		if (curtime - this->preVelModLastChange > 0.2f)
		{
			this->preVelMod = 1.0f;
			this->preVelModLastChange = curtime;
		}
		else if (this->preVelMod > PRE_VELMOD_MAX + 0.007f)
		{
			// Return without committing — intentional per gokz source
			return PRE_VELMOD_MAX - 0.001f;
		}
	}
	else if ((this->player->IsButtonPressed(IN_MOVELEFT) || this->player->IsButtonPressed(IN_MOVERIGHT))
		&& this->player->currentMoveData->m_vecVelocity.Length2D() > 248.9f)
	{
		f32 increment = (this->preVelMod > 1.04f) ? 0.001f : 0.0009f;

		bool forwards = this->GetClientMovingDirection() > 0.0f;

		bool goodSync = (this->player->IsButtonPressed(IN_MOVERIGHT) && turning == TURN_RIGHT)
			|| (turning == TURN_LEFT && !forwards)
			|| (this->player->IsButtonPressed(IN_MOVELEFT) && turning == TURN_LEFT)
			|| (turning == TURN_RIGHT && !forwards);

		if (goodSync)
		{
			this->preTickCounter++;

			if (this->preTickCounter < 75)
			{
				this->preVelMod += increment;
				if (this->preVelMod > PRE_VELMOD_MAX)
				{
					if (this->preVelMod > PRE_VELMOD_MAX + 0.007f)
					{
						this->preVelMod = PRE_VELMOD_MAX - 0.001f;
					}
					else
					{
						this->preVelMod -= 0.007f;
					}
				}
				// Double increment — intentional in gokz source, preserved 1:1
				this->preVelMod += increment;
			}
			else
			{
				this->preVelMod -= 0.0045f;
				this->preTickCounter -= 2;

				if (this->preVelMod < 1.0f)
				{
					this->preVelMod = 1.0f;
					this->preTickCounter = 0;
				}
			}
		}
		else
		{
			this->preVelMod -= 0.04f;

			if (this->preVelMod < 1.0f)
			{
				this->preVelMod = 1.0f;
			}
		}

		this->preVelModLastChange = curtime;
	}
	else
	{
		// Has strafing inputs but below speed threshold, or no strafing input
		this->preTickCounter = 0;
		// Return without committing — intentional per gokz source
		return 1.0f;
	}

	return this->preVelMod;
}

// Ported from gokz GetClientMovingDirection (KZTimerGlobal).
// Returns dot product of normalized velocity and normalized view direction.
// Positive = moving forwards relative to view; negative = backwards.
f32 KZTimerModeService::GetClientMovingDirection()
{
	Vector velocity;
	this->player->GetVelocity(&velocity);

	QAngle eyeAngles;
	this->player->GetAngles(&eyeAngles);

	// Clamp pitch to ±70 degrees, as in the gokz original
	if (eyeAngles.x > 70.0f)
	{
		eyeAngles.x = 70.0f;
	}
	if (eyeAngles.x < -70.0f)
	{
		eyeAngles.x = -70.0f;
	}

	Vector viewDir;
	AngleVectors(eyeAngles, &viewDir, nullptr, nullptr);

	velocity = g_pKZUtils->NormalizeVector(velocity);
	viewDir = g_pKZUtils->NormalizeVector(viewDir);

	return DotProduct(velocity, viewDir);
}

void KZTimerModeService::CheckVelocityQuantization()
{
	if (this->postProcessMovementZSpeed > this->player->currentMoveData->m_vecVelocity.z
		&& this->postProcessMovementZSpeed - this->player->currentMoveData->m_vecVelocity.z < 0.03125f
		// Colliding with a flat floor can result in a velocity of +0.0078125u/s, and this breaks ladders.
		// The quantization accidentally fixed this bug...
		&& fabs(this->player->currentMoveData->m_vecVelocity.z) > 0.03125f)
	{
		this->player->currentMoveData->m_vecVelocity.z = this->postProcessMovementZSpeed;
	}
}

// ORIGINAL AUTHORS : Mev & Blacky
// URL: https://forums.alliedmods.net/showthread.php?p=2322788
void KZTimerModeService::SlopeFix()
{
	CTraceFilterPlayerMovementCS filter(this->player->GetPlayerPawn());

	Vector ground = this->player->currentMoveData->m_vecAbsOrigin;
	ground.z -= 2;

	f32 standableZ = 0.7f; // Equal to the mode's cvar.

	if (sv_standable_normal.IsValidRef() && sv_standable_normal.IsConVarDataAvailable())
	{
		standableZ = sv_standable_normal.Get();
	}
	bbox_t bounds;
	this->player->GetBBoxBounds(&bounds);
	trace_t trace;

	INavPhysicsInterface::TraceShape(Ray_t(bounds.mins, bounds.maxs), this->player->currentMoveData->m_vecAbsOrigin, ground, &filter, &trace);

	// Doesn't hit anything, fall back to the original ground
	if (trace.m_bStartInSolid || trace.m_flFraction == 1.0f)
	{
		return;
	}

	if (standableZ <= trace.m_vHitNormal.z && trace.m_vHitNormal.z < 1.0f)
	{
		// Copy the ClipVelocity function from sdk2013
		float backoff;
		float change;
		Vector newVelocity;

		backoff = DotProduct(this->player->landingVelocity, trace.m_vHitNormal) * 1;

		for (u32 i = 0; i < 3; i++)
		{
			change = trace.m_vHitNormal[i] * backoff;
			newVelocity[i] = this->player->landingVelocity[i] - change;
		}

		f32 adjust = DotProduct(newVelocity, trace.m_vHitNormal);
		if (adjust < 0.0f)
		{
			newVelocity -= (trace.m_vHitNormal * adjust);
		}
		// Make sure the player is going down a ramp by checking if they actually will gain speed from the boost.
		if (newVelocity.Length2D() >= this->player->landingVelocity.Length2D())
		{
			this->player->currentMoveData->m_vecVelocity.x = newVelocity.x;
			this->player->currentMoveData->m_vecVelocity.y = newVelocity.y;
			this->player->landingVelocity.x = newVelocity.x;
			this->player->landingVelocity.y = newVelocity.y;
		}
	}
}

// 1:1 with CS2.
static_function void ClipVelocity(Vector &in, Vector &normal, Vector &out)
{
	f32 backoff = -((in.x * normal.x) + ((normal.z * in.z) + (in.y * normal.y))) * 1;
	backoff = fmaxf(backoff, 0.0) + 0.03125;

	out = normal * backoff + in;
}

static_function bool IsValidMovementTrace(trace_t &tr, bbox_t bounds, CTraceFilterPlayerMovementCS *filter)
{
	trace_t stuck;
	// Maybe we don't need this one.
	// if (tr.m_flFraction < FLT_EPSILON)
	//{
	//	return false;
	//}

	if (tr.m_bStartInSolid)
	{
		return false;
	}

	// We hit something but no valid plane data?
	if (tr.m_flFraction < 1.0f && fabs(tr.m_vHitNormal.x) < FLT_EPSILON && fabs(tr.m_vHitNormal.y) < FLT_EPSILON
		&& fabs(tr.m_vHitNormal.z) < FLT_EPSILON)
	{
		return false;
	}

	// Is the plane deformed?
	if (fabs(tr.m_vHitNormal.x) > 1.0f || fabs(tr.m_vHitNormal.y) > 1.0f || fabs(tr.m_vHitNormal.z) > 1.0f)
	{
		return false;
	}

	// Do an unswept trace and a backward trace just to be sure.
	INavPhysicsInterface::TraceShape(Ray_t(bounds.mins, bounds.maxs), tr.m_vEndPos, tr.m_vEndPos, filter, &stuck);
	if (stuck.m_bStartInSolid || stuck.m_flFraction < 1.0f - FLT_EPSILON)
	{
		return false;
	}

	INavPhysicsInterface::TraceShape(Ray_t(bounds.mins, bounds.maxs), tr.m_vEndPos, tr.m_vStartPos, filter, &stuck);
	// For whatever reason if you can hit something in only one direction and not the other way around.
	// Only happens since Call to Arms update, so this fraction check is commented out until it is fixed.
	if (stuck.m_bStartInSolid /*|| stuck.m_flFraction < 1.0f - FLT_EPSILON*/)
	{
		return false;
	}

	return true;
}

void KZTimerModeService::OnTryPlayerMove(Vector *pFirstDest, trace_t *pFirstTrace, bool *bIsSurfing)
{
	this->tpmTriggerFixOrigins.RemoveAll();
	this->overrideTPM = false;
	this->didTPM = true;
	CCSPlayerPawn *pawn = this->player->GetPlayerPawn();

	f32 timeLeft = g_pKZUtils->GetGlobals()->frametime;

	Vector start, velocity, end;
	this->player->GetOrigin(&start);
	this->player->GetVelocity(&velocity);

	this->tpmTriggerFixOrigins.AddToTail(start);
	if (velocity.Length() == 0.0f)
	{
		// No move required.
		return;
	}
	Vector primalVelocity = velocity;

	bool validPlane {};

	f32 allFraction {};
	trace_t pm;
	u32 bumpCount {};
	Vector planes[5];
	u32 numPlanes {};
	trace_t pierce;

	bbox_t bounds;
	this->player->GetBBoxBounds(&bounds);

	CTraceFilterPlayerMovementCS filter(pawn);

	bool potentiallyStuck {};

	for (bumpCount = 0; bumpCount < MAX_BUMPS; bumpCount++)
	{
		// Assume we can move all the way from the current origin to the end point.
		VectorMA(start, timeLeft, velocity, end);
		// See if we can make it from origin to end point.
		// If their velocity Z is 0, then we can avoid an extra trace here during WalkMove.
		if (pFirstDest && end == *pFirstDest)
		{
			pm = *pFirstTrace;
		}
		else
		{
			INavPhysicsInterface::TraceShape(Ray_t(bounds.mins, bounds.maxs), start, end, &filter, &pm);
			if (end == start)
			{
				continue;
			}
			if (IsValidMovementTrace(pm, bounds, &filter) && pm.m_flFraction == 1.0f)
			{
				// Player won't hit anything, nothing to do.
				break;
			}
			bool normalChanged = pm.m_vHitNormal.Dot(this->lastValidPlane) < RAMP_BUG_THRESHOLD;
			bool stuck = potentiallyStuck && pm.m_flFraction == 0.0f;
			bool lastValidPlaneWasStraightWall = this->lastValidPlane.z < 0.03125f;
			bool shouldConsiderRampbug = (normalChanged && !lastValidPlaneWasStraightWall) || stuck;
			if (this->lastValidPlane.Length() > FLT_EPSILON && shouldConsiderRampbug)
			{
				// We hit a plane that will significantly change our velocity.
				// Make sure that this plane is significant enough.
				Vector direction = g_pKZUtils->NormalizeVector(velocity);
				Vector offsetDirection;
				f32 offsets[] = {0.0f, -1.0f, 1.0f};
				bool success {};
				for (u32 i = 0; i < 3 && !success; i++)
				{
					for (u32 j = 0; j < 3 && !success; j++)
					{
						for (u32 k = 0; k < 3 && !success; k++)
						{
							if (i == 0 && j == 0 && k == 0)
							{
								offsetDirection = this->lastValidPlane;
							}
							else
							{
								offsetDirection = {offsets[i], offsets[j], offsets[k]};
								// Check if this random offset is even valid.
								if (this->lastValidPlane.Dot(offsetDirection) <= 0.0f)
								{
									continue;
								}
								trace_t test;
								INavPhysicsInterface::TraceShape(Ray_t(bounds.mins, bounds.maxs), start + offsetDirection * RAMP_PIERCE_DISTANCE,
																 start, &filter, &test);
								if (!IsValidMovementTrace(test, bounds, &filter))
								{
									continue;
								}
							}
							bool goodTrace {};
							f32 ratio {};
							bool hitNewPlane {};
							for (ratio = 0.25f; ratio <= 1.0f; ratio += 0.25f)
							{
								INavPhysicsInterface::TraceShape(Ray_t(bounds.mins, bounds.maxs),
																 start + offsetDirection * RAMP_PIERCE_DISTANCE * ratio,
																 end + offsetDirection * RAMP_PIERCE_DISTANCE * ratio, &filter, &pierce);
								if (!IsValidMovementTrace(pierce, bounds, &filter))
								{
									continue;
								}
								// Try until we hit a similar plane.
								// clang-format off
								validPlane = pierce.m_flFraction < 1.0f && pierce.m_flFraction > 0.1f
											 && pierce.m_vHitNormal.Dot(this->lastValidPlane) >= RAMP_BUG_THRESHOLD;

								hitNewPlane = pm.m_vHitNormal.Dot(pierce.m_vHitNormal) < NEW_RAMP_THRESHOLD
											  && this->lastValidPlane.Dot(pierce.m_vHitNormal) > NEW_RAMP_THRESHOLD;
								// clang-format on
								goodTrace = CloseEnough(pierce.m_flFraction, 1.0f, FLT_EPSILON) || validPlane;
								if (goodTrace)
								{
									break;
								}
							}
							if (goodTrace || hitNewPlane)
							{
								// Trace back to the original end point to find its normal.
								trace_t test;
								INavPhysicsInterface::TraceShape(Ray_t(bounds.mins, bounds.maxs), pierce.m_vEndPos, end, &filter, &test);
								pm = pierce;
								pm.m_vStartPos = start;
								pm.m_flFraction = Clamp((pierce.m_vEndPos - pierce.m_vStartPos).Length() / (end - start).Length(), 0.0f, 1.0f);
								pm.m_vEndPos = test.m_vEndPos;
								if (pierce.m_vHitNormal.Length() > 0.0f)
								{
									pm.m_vHitNormal = pierce.m_vHitNormal;
									this->lastValidPlane = pierce.m_vHitNormal;
								}
								else
								{
									pm.m_vHitNormal = test.m_vHitNormal;
									this->lastValidPlane = test.m_vHitNormal;
								}
								success = true;
								this->overrideTPM = true;
							}
						}
					}
				}
			}
			if (pm.m_vHitNormal.Length() > 0.99f)
			{
				this->lastValidPlane = pm.m_vHitNormal;
			}
			potentiallyStuck = pm.m_flFraction == 0.0f;
		}

		if (pm.m_flFraction * velocity.Length() > 0.03125f || pm.m_flFraction > 0.03125f)
		{
			allFraction += pm.m_flFraction;
			start = pm.m_vEndPos;
			numPlanes = 0;
		}

		this->tpmTriggerFixOrigins.AddToTail(pm.m_vEndPos);

		if (allFraction == 1.0f)
		{
			break;
		}
		timeLeft -= g_pKZUtils->GetGlobals()->frametime * pm.m_flFraction;

		// 2024-11-07 update also adds a low velocity check... This is only correct as long as you don't collide with other players.
		if (numPlanes >= 5 || (pm.m_vHitNormal.z >= 0.7f && velocity.Length2D() < 1.0f))
		{
			VectorCopy(vec3_origin, velocity);
			break;
		}

		planes[numPlanes] = pm.m_vHitNormal;
		numPlanes++;

		if (numPlanes == 1 && pawn->m_MoveType() == MOVETYPE_WALK && pawn->m_hGroundEntity().Get() == nullptr)
		{
			ClipVelocity(velocity, planes[0], velocity);
		}
		else
		{
			u32 i, j;
			for (i = 0; i < numPlanes; i++)
			{
				ClipVelocity(velocity, planes[i], velocity);
				for (j = 0; j < numPlanes; j++)
				{
					if (j != i)
					{
						// Are we now moving against this plane?
						if (velocity.Dot(planes[j]) < 0)
						{
							break; // not ok
						}
					}
				}

				if (j == numPlanes) // Didn't have to clip, so we're ok
				{
					break;
				}
			}
			// Did we go all the way through plane set
			if (i != numPlanes)
			{ // go along this plane
				// pmove.velocity is set in clipping call, no need to set again.
				;
			}
			else
			{ // go along the crease
				if (numPlanes != 2)
				{
					VectorCopy(vec3_origin, velocity);
					break;
				}
				Vector dir;
				f32 d;
				CrossProduct(planes[0], planes[1], dir);
				dir = g_pKZUtils->NormalizeVector(dir);
				d = dir.Dot(velocity);
				VectorScale(dir, d, velocity);

				if (velocity.Dot(primalVelocity) <= 0)
				{
					velocity = vec3_origin;
					break;
				}
			}
		}
	}
	this->tpmOrigin = pm.m_vEndPos;
	this->tpmVelocity = velocity;
}

void KZTimerModeService::OnTryPlayerMovePost(Vector *pFirstDest, trace_t *pFirstTrace, bool *bIsSurfing)
{
	Vector velocity;
	this->player->GetVelocity(&velocity);
	bool velocityHeavilyModified =
		g_pKZUtils->NormalizeVector(this->tpmVelocity).Dot(g_pKZUtils->NormalizeVector(velocity)) < RAMP_BUG_THRESHOLD
		|| (this->tpmVelocity.Length() > 50.0f && velocity.Length() / this->tpmVelocity.Length() < RAMP_BUG_VELOCITY_THRESHOLD);
	if (this->overrideTPM && velocityHeavilyModified && this->tpmOrigin != vec3_invalid && this->tpmVelocity != vec3_invalid)
	{
		this->player->SetOrigin(this->tpmOrigin);
		this->player->SetVelocity(this->tpmVelocity);
	}
	if (this->airMoving)
	{
		if (this->tpmTriggerFixOrigins.Count() > 1)
		{
			bbox_t bounds;
			// We need to shrink the bounds a bit to prevent touching triggers that we shouldn't be touching when doing triggerfix.
			bbox_t offset = {{0.03125f, 0.03125f, 0.0f}, {-0.03125f, -0.03125f, 0.0f}};
			this->player->GetBBoxBounds(&bounds, &offset);
			for (int i = 0; i < this->tpmTriggerFixOrigins.Count() - 1; i++)
			{
				this->player->TouchTriggersAlongPath(this->tpmTriggerFixOrigins[i], this->tpmTriggerFixOrigins[i + 1], bounds);
			}
		}
		this->player->UpdateTriggerTouchList();
	}
}

void KZTimerModeService::OnCategorizePosition(bool bStayOnGround)
{
	// Already on the ground?
	// If we are already colliding on a standable valid plane, we don't want to do the check.
	if (bStayOnGround || this->lastValidPlane.Length() < EPSILON || this->lastValidPlane.z > 0.7f)
	{
		return;
	}
	// Only attempt to fix rampbugs while going down significantly enough.
	if (this->player->currentMoveData->m_vecVelocity.z > -64.0f)
	{
		return;
	}
	bbox_t bounds;
	this->player->GetBBoxBounds(&bounds);

	CTraceFilterPlayerMovementCS filter(this->player->GetPlayerPawn());

	trace_t trace;

	Vector origin, groundOrigin;
	this->player->GetOrigin(&origin);
	groundOrigin = origin;
	groundOrigin.z -= 2.0f;

	INavPhysicsInterface::TraceShape(Ray_t(bounds.mins, bounds.maxs), origin, groundOrigin, &filter, &trace);

	if (trace.m_flFraction == 1.0f)
	{
		return;
	}
	// Is this something that you should be able to actually stand on?
	if (trace.m_flFraction < 0.95f && trace.m_vHitNormal.z > 0.7f && this->lastValidPlane.Dot(trace.m_vHitNormal) < RAMP_BUG_THRESHOLD)
	{
		origin += this->lastValidPlane * 0.0625f;
		groundOrigin = origin;
		groundOrigin.z -= 2.0f;
		INavPhysicsInterface::TraceShape(Ray_t(bounds.mins, bounds.maxs), origin, groundOrigin, &filter, &trace);
		if (trace.m_bStartInSolid)
		{
			return;
		}
		if (trace.m_flFraction == 1.0f || this->lastValidPlane.Dot(trace.m_vHitNormal) >= RAMP_BUG_THRESHOLD)
		{
			this->player->SetOrigin(origin);
		}
	}
}

void KZTimerModeService::OnDuckPost()
{
	this->player->UpdateTriggerTouchList();
}

void KZTimerModeService::OnAirMove()
{
	this->airMoving = true;
	this->player->currentMoveData->m_flMaxSpeed = SPEED_NORMAL;
}

void KZTimerModeService::OnAirMovePost()
{
	this->airMoving = false;
	this->player->currentMoveData->m_flMaxSpeed = SPEED_NORMAL * this->effectivePreVelMod;
}

// KZTimer air acceleration: normalize wishspeed by effectivePreVelMod to prevent double-prestrafe.
// Ported from gokz DHooks_OnAirAccelerate_Pre.
void KZTimerModeService::OnAirAccelerate(Vector &wishdir, f32 &wishspeed, f32 &accel)
{
	if (this->effectivePreVelMod > 1.0f)
	{
		wishspeed /= this->effectivePreVelMod;
	}
}

void KZTimerModeService::OnWaterMove()
{
	this->player->currentMoveData->m_flMaxSpeed = SPEED_NORMAL;
}

void KZTimerModeService::OnWaterMovePost()
{
	this->player->currentMoveData->m_flMaxSpeed = SPEED_NORMAL * this->effectivePreVelMod;
}

void KZTimerModeService::OnTeleport(const Vector *newPosition, const QAngle *newAngles, const Vector *newVelocity)
{
	if (!this->player->processingMovement)
	{
		return;
	}
	// Only happens when triggerfix happens.
	if (newPosition)
	{
		this->player->currentMoveData->m_vecAbsOrigin = *newPosition;
	}
	if (newVelocity)
	{
		this->player->currentMoveData->m_vecVelocity = *newVelocity;
	}
}

bool KZTimerModeService::CanTouchTimerZone()
{
	f64 tick = g_pKZUtils->GetGlobals()->curtime * ENGINE_FIXED_TICK_RATE;
	return fabs(roundf(tick) - tick) < 0.001f || fabs(roundf(tick) - tick - 0.5f) < 0.001f;
}

// Only touch timer triggers on half ticks.
bool KZTimerModeService::OnTriggerStartTouch(CBaseTrigger *trigger)
{
	if (!g_pMappingApi->IsTriggerATimerZone(trigger))
	{
		return true;
	}
	return this->CanTouchTimerZone();
}

bool KZTimerModeService::OnTriggerTouch(CBaseTrigger *trigger)
{
	if (!g_pMappingApi->IsTriggerATimerZone(trigger))
	{
		return true;
	}
	return this->CanTouchTimerZone();
}

bool KZTimerModeService::OnTriggerEndTouch(CBaseTrigger *trigger)
{
	if (!g_pMappingApi->IsTriggerATimerZone(trigger))
	{
		return true;
	}
	return this->CanTouchTimerZone();
}
