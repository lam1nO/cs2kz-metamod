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
// Perf window under legacy jump: jump within this much time after landing = perf.
// Mode detects perf itself (base sets inPerf only for modern jump). 1/128 s = ~один
// 128-tick кадр после приземления — строгий KZTimer-перф (важное условие режима).
#define KZT_PERF_WINDOW 0.0078125f // 1/128
// Исполнение «у границы тика» для forward-квантовки when (перенос события в
// следующую команду невозможен — протобаф не переписываем).
#define KZT_WHEN_TICK_END 0.9999f
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

	// Пороги ДИСТАНЦИИ (u) по типу прыжка × [0]=нормальные / [1]=lowpre × тир.
	// Метрика дистанции НЕ меняется (канон). Ось lowpre выбирается по prespeed (takeoffSpeed):
	// prespeed >= катофф → [0], иначе → [1]. Катофф: BH/MBH/JB = 360, WJ = 300 (см. .cpp).
	// Пороги сверены с refs/gokz kztimer distance-тирами — достижимо: KZT-перф даёт до ~380
	// спида, бхопы реально доходят до 340–360+ по дистанции.
	// Тир Meh = 0.0 (по ТЗ «meh 0–…»), т.е. любой валидный прыжок ≥ Meh. Типы без lowpre — [1]==[0].
	f32 distanceTiers[JUMPTYPE_COUNT - 3][2][DISTANCETIER_COUNT] = {
		// LJ — без lowpre
		{{217.0f, 265.0f, 270.0f, 275.0f, 280.0f, 284.0f}, {217.0f, 265.0f, 270.0f, 275.0f, 280.0f, 284.0f}},
		// BH: normal meh0/imp340/perf345/god350/own355/wreck360 ; lowpre 0/325/330/335/340/345
		{{0.0f, 340.0f, 345.0f, 350.0f, 355.0f, 360.0f}, {0.0f, 325.0f, 330.0f, 335.0f, 340.0f, 345.0f}},
		// MBH: как BH
		{{0.0f, 340.0f, 345.0f, 350.0f, 355.0f, 360.0f}, {0.0f, 325.0f, 330.0f, 335.0f, 340.0f, 345.0f}},
		// WJ: normal 0/300/305/310/315/320 ; lowpre 0/280/285/290/295/300
		{{0.0f, 300.0f, 305.0f, 310.0f, 315.0f, 320.0f}, {0.0f, 280.0f, 285.0f, 290.0f, 295.0f, 300.0f}},
		// LAJ — без lowpre
		{{120.0f, 160.0f, 170.0f, 180.0f, 190.0f, 200.0f}, {120.0f, 160.0f, 170.0f, 180.0f, 190.0f, 200.0f}},
		// LAH — без lowpre
		{{217.0f, 260.0f, 265.0f, 270.0f, 275.0f, 278.0f}, {217.0f, 260.0f, 265.0f, 270.0f, 275.0f, 278.0f}},
		// JB: пороги bhop (normal+lowpre) по ТЗ
		{{0.0f, 340.0f, 345.0f, 350.0f, 355.0f, 360.0f}, {0.0f, 325.0f, 330.0f, 335.0f, 340.0f, 345.0f}},
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
		// KZT: legacy (тиковый) прыжок — даёт стабильную высоту 55.83 и на бхопе (как CKZ).
		// Под ним база inPerf по субтиковому окну НЕ ставит — режим детектит перф сам по
		// timeOnGround <= KZT_PERF_WINDOW (см. OnStopTouchGround).
		(bool)true,           // sv_legacy_jump
		(float)0.0078125f     // sv_bhop_time_window  (под legacy не используется базой; перф — по KZT_PERF_WINDOW)
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

	// Мёртвая граница (GO@128): чек, чей отрыв совпал бы с регистрацией касания
	// (tog<=0), подавляется — биты и защёлка прячутся парно и возвращаются в Post.
	u64 savedJumpBits[3] = {};
	bool jumpSuppressed = false;
	bool savedOldJumpPressed = false;
	f32 savedJumpPressedTime = 0.0f;

	// Скорость в момент касания — источник GO-формулы скорости отрыва (GO@128).
	f32 lastLandingSpeed = -1.0f;

public:
	virtual void Reset() override;
	virtual void Cleanup() override;
	virtual const char *GetModeName() override;
	virtual const char *GetModeShortName() override;

	virtual bool EnableWaterFix() override;

	virtual DistanceTier GetDistanceTier(JumpType jumpType, f32 distance, f32 takeoffSpeed = -1.0f) override;
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
	virtual void OnCheckJumpButtonLegacy() override;
	virtual void OnCheckJumpButtonLegacyPost() override;
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
