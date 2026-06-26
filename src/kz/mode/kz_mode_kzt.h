#pragma once
#include "version_gen.h"

#include "kz_mode.h"
#include "sdk/datatypes.h"

#define MODE_NAME_SHORT_KZT "KZT"
#define MODE_NAME_KZT       "KZTimer"
// Rampbug fix related
#define MAX_BUMPS                   4
#define RAMP_PIERCE_DISTANCE        0.0625f
#define RAMP_BUG_THRESHOLD          0.98f
#define RAMP_BUG_VELOCITY_THRESHOLD 0.95f
#define NEW_RAMP_THRESHOLD          0.95f

#define SPEED_NORMAL 250.0f
// KZTimer prestrafe: tick-counter velMod model (ported from gokz CalcPrestrafeVelMod)
#define PRE_VELMOD_MAX 1.104f // Max prestrafe velocity modifier: 250 * 1.104 = 276 u/s
// Bhop related — gokz TweakJump: cap horizontal speed at perf to 380 u/s
#define PERF_SPEED_CAP 380.0f
// Misc
#define DUCK_SPEED_NORMAL  8.0f
#define DUCK_SPEED_MINIMUM 6.0234375f // Equal to if you just ducked/unducked for the first time in a while

class KZTimerModePlugin : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late);
	bool Unload(char *error, size_t maxlen);
	bool Pause(char *error, size_t maxlen);
	bool Unpause(char *error, size_t maxlen);

public:
	const char *GetAuthor()
	{
		return PLUGIN_AUTHOR;
	}

	const char *GetName()
	{
		return "CS2KZ-Mode-KZTimer";
	}

	const char *GetDescription()
	{
		return "KZTimer mode plugin for CS2KZ";
	}

	const char *GetURL()
	{
		return PLUGIN_URL;
	}

	const char *GetLicense()
	{
		return PLUGIN_LICENSE;
	}

	const char *GetVersion()
	{
		return PLUGIN_FULL_VERSION;
	}

	const char *GetDate()
	{
		return __DATE__;
	}

	const char *GetLogTag()
	{
		return PLUGIN_LOGTAG;
	}
};

class KZTimerModeService : public KZModeService
{
	using KZModeService::KZModeService;

	f32 distanceTiers[JUMPTYPE_COUNT - 3][DISTANCETIER_COUNT] = {
		{217.0f, 265.0f, 270.0f, 275.0f, 280.0f, 284.0f}, // LJ
		{217.0f, 275.0f, 280.0f, 287.0f, 292.0f, 295.0f}, // BH
		{217.0f, 275.0f, 280.0f, 287.0f, 292.0f, 295.0f}, // MBH
		{217.0f, 275.0f, 280.0f, 287.0f, 292.0f, 295.0f}, // WJ
		{120.0f, 160.0f, 170.0f, 180.0f, 190.0f, 200.0f}, // LAJ
		{217.0f, 260.0f, 265.0f, 270.0f, 275.0f, 278.0f}, // LAH
		{217.0f, 275.0f, 280.0f, 287.0f, 292.0f, 295.0f}, // JB
	};

	static inline CVValue_t modeCvarValues[] = {
		(float)6.5f,          // sv_accelerate
		(bool)false,          // sv_accelerate_use_weapon_speed
		(float)100.0f,        // sv_airaccelerate
		(float)30.0f,         // sv_air_max_wishspeed
		(bool)false,          // sv_autobunnyhopping
		(float)0.0f,          // sv_bounce
		(bool)true,           // sv_enablebunnyhopping
		(float)5.0f,          // sv_friction          (KZTimer 5.0, not 5.2)
		(float)800.0f,        // sv_gravity
		(float)301.993377f,   // sv_jump_impulse      (KZTimer, not 302.0)
		(bool)false,          // sv_jump_precision_enable
		(float)0.0f,          // sv_jump_spam_penalty_time
		(float)-0.707f,       // sv_ladder_angle
		(float)1.0f,          // sv_ladder_dampen
		(float)1.0f,          // sv_ladder_scale_speed
		(float)320.0f,        // sv_maxspeed
		(float)2000.0f,       // sv_maxvelocity       (KZTimer 2000, not 3500)
		(float)0.0f,          // sv_staminajumpcost
		(float)0.0f,          // sv_staminalandcost
		(float)0.0f,          // sv_staminamax
		(float)9999.0f,       // sv_staminarecoveryrate
		(float)0.7f,          // sv_standable_normal
		(float)64.0f,         // sv_step_move_vel_min
		(float)0.0f,          // sv_timebetweenducks
		(float)0.7f,          // sv_walkable_normal
		(float)10.0f,         // sv_wateraccelerate
		(float)1.0f,          // sv_waterfriction
		(float)0.9f,          // sv_water_slow_amount
		(int)0,               // mp_solid_teammates
		(int)0,               // mp_solid_enemies
		(bool)false,          // sv_subtick_movement_view_angles
		(bool)false,          // sv_legacy_jump       (KZT: modern/subtick jump, not legacy)
		// Starting value; empirical tuning in Task 10.
		// CS:GO KZTimer perf ≈ one grounded 128-tick frame (1/128 s).
		// cs2kz window is symmetric ±(w/2) around the subtick landing point
		// (kz_player.cpp:828-840), so we seed at 1/128 and adjust after live validation.
		(float)0.0078125f     // sv_bhop_time_window  (= 1/128; final value: Task 10)
	};
	static_assert(KZ_ARRAYSIZE(modeCvarValues) == MODECVAR_COUNT, "Array modeCvarValues length is not the same as MODECVAR_COUNT!");

	bool hasValidDesiredViewAngle {};
	QAngle lastValidDesiredViewAngle;
	f32 lastJumpReleaseTime {};
	bool oldDuckPressed {};
	bool oldJumpPressed {};
	bool forcedUnduck {};
	f32 postProcessMovementZSpeed {};

	// KZTimer prestrafe (tick-counter velMod model, ported from gokz)
	f32 preVelMod {1.0f};        // persistent state across ticks
	f32 effectivePreVelMod {1.0f}; // return value of CalcPrestrafeVelMod for current tick
	i32 preTickCounter {};
	f32 preVelModLastChange {};
	f32 originalMaxSpeed {};

	bool didTPM {};
	bool overrideTPM {};
	Vector tpmVelocity = vec3_invalid;
	Vector tpmOrigin = vec3_invalid;
	Vector lastValidPlane = vec3_origin;

	// Keep track of TryPlayerMove path for triggerfixing.
	bool airMoving {};
	CUtlVector<Vector> tpmTriggerFixOrigins;

public:
	virtual void Reset() override;
	virtual void Cleanup() override;
	virtual const char *GetModeName() override;
	virtual const char *GetModeShortName() override;

	virtual bool EnableWaterFix() override;

	virtual DistanceTier GetDistanceTier(JumpType jumpType, f32 distance) override;
	virtual const CVValue_t *GetModeConVarValues() override;

	virtual void OnPhysicsSimulate() override;
	virtual void OnPhysicsSimulatePost() override;
	virtual void OnSetupMove(PlayerCommand *pc) override;
	virtual void OnProcessMovement() override;
	virtual void OnPlayerMove() override;
	virtual void OnProcessMovementPost() override;
	virtual void OnCategorizePosition(bool bStayOnGround) override;
	virtual void OnDuckPost() override;
	virtual void OnAirMove() override;
	virtual void OnAirMovePost() override;
	virtual void OnAirAccelerate(Vector &wishdir, f32 &wishspeed, f32 &accel) override;
	virtual void OnWaterMove() override;
	virtual void OnWaterMovePost() override;
	virtual void OnStartTouchGround() override;
	virtual void OnStopTouchGround() override;
	virtual void OnTryPlayerMove(Vector *pFirstDest, trace_t *pFirstTrace, bool *bIsSurfing) override;
	virtual void OnTryPlayerMovePost(Vector *pFirstDest, trace_t *pFirstTrace, bool *bIsSurfing) override;
	virtual void OnTeleport(const Vector *newPosition, const QAngle *newAngles, const Vector *newVelocity) override;

	virtual bool CanTouchTimerZone() override;
	virtual bool OnTriggerStartTouch(CBaseTrigger *trigger) override;
	virtual bool OnTriggerTouch(CBaseTrigger *trigger) override;
	virtual bool OnTriggerEndTouch(CBaseTrigger *trigger) override;

	// Insert subtick timing to be called later. Should only call this in PhysicsSimulate.
	void InsertSubtickTiming(float time);

	void InterpolateViewAngles();
	void RestoreInterpolatedViewAngles();

	// KZTimer prestrafe: tick-counter velMod model (ported from gokz CalcPrestrafeVelMod)
	f32 CalcPrestrafeVelMod();
	f32 GetClientMovingDirection();

	void CheckVelocityQuantization();
	void RemoveCrouchJumpBind();
	/*
		Ported from DanZay's SimpleKZ:
		Duck speed is reduced by the game upon ducking or unducking.
		The goal here is to accept that duck speed is reduced, but
		stop it from being reduced further when spamming duck.

		This is done by enforcing a minimum duck speed equivalent to
		the value as if the player only ducked once. When not in not
		in the middle of ducking, duck speed is reset to its normal
		value in effort to reduce the number of times the minimum
		duck speed is enforced. This should reduce noticeable lag.
	*/
	void ReduceDuckSlowdown();

	void SlopeFix();
};
