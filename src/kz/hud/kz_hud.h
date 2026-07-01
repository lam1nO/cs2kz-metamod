#pragma once
#include "kz/kz.h"
#include "kz/timer/kz_timer.h"
#include "entityhandle.h"
#include "sdk/entity/cparticlesystem.h"

#define KZ_HUD_TIMER_STOPPED_GRACE_TIME 3.0f
#define KZ_HUD_ON_GROUND_THRESHOLD      0.07f
class IEntityResourceManifest;

class KZHUDService : public KZBaseService
{
	using KZBaseService::KZBaseService;

private:
	bool jumpedThisTick {};
	bool fromDuckbug {};
	bool crouchJumping {};
	bool showPanel {};
	bool particlesActive {};
	f64 timerStoppedTime {};
	f64 currentTimeWhenTimerStopped {};

	// Источник ДАННЫХ для MHUD (скорость/клавиши/таймер/CP-TP). При спектировании =
	// наблюдаемый игрок; nullptr → данные самого игрока.
	// ВАЖНО: это ТОЛЬКО источник данных. Источник НАСТРОЕК (тумблеры/цвета/раскладка) —
	// всегда this->player (сам игрок / спектатор), см. MHUDDataSource() vs this->player.
	KZPlayer *mhudSource {};

	// Игрок, чьи ДАННЫЕ читает MHUD (наблюдаемый при спектировании, иначе сам игрок).
	KZPlayer *MHUDDataSource()
	{
		return mhudSource ? mhudSource : this->player;
	}

	// Игрок, чьи НАСТРОЙКИ (тумблеры/цвета/раскладка) читает MHUD — ВСЕГДА сам игрок
	// (спектатор при спектировании). Так у спектатора своя раскладка по чужим данным.
	KZPlayer *MHUDSettingsSource()
	{
		return this->player;
	}

public:
	virtual void Reset() override;
	static void Init();

	// Returns true when the particle-based MHUD should be used.
	// Requires MultiAddonManager to be available, unless kz_force_mhud is set.
	static bool IsMHUDAvailable();

	static void PrecacheParticles(IEntityResourceManifest *pResourceManifest);
	// Draw the panel from a player to a specific target.
	static void DrawPanels(KZPlayer *player, KZPlayer *target);

	void ResetShowPanel();
	void TogglePanel();
	void ToggleCompactPanel();

	void OnPhysicsSimulate()
	{
		jumpedThisTick = false;
	}

	void OnProcessMovement();
	void OnProcessMovementPost();

	void OnJump(bool modern = false)
	{
		jumpedThisTick = modern ? this->player->IsButtonPressed(IN_JUMP) : true;
	}

	void OnStopTouchGround()
	{
		if (jumpedThisTick)
		{
			fromDuckbug = player->duckBugged;
			crouchJumping = player->GetPlayerPawn()->m_fFlags() & FL_DUCKING || player->GetMoveServices()->m_bDucking();
		}
		else
		{
			fromDuckbug = false;
			crouchJumping = false;
		}
	}

	bool IsShowingPanel()
	{
		return this->showPanel;
	}

	bool IsCompactPanel();

	void OnTimerStopped(f64 currentTimeWhenTimerStopped);

	bool ShouldShowTimerAfterStop()
	{
		return g_pKZUtils->GetServerGlobals()->curtime > KZ_HUD_TIMER_STOPPED_GRACE_TIME
			   && g_pKZUtils->GetServerGlobals()->curtime - timerStoppedTime < KZ_HUD_TIMER_STOPPED_GRACE_TIME;
	}

	// source = игрок-источник данных (наблюдаемый при спектировании); nullptr → сам игрок.
	void UpdateParticles(KZPlayer *source = nullptr);

	// Destroy all active MHUD particles (e.g. on death or disconnect).
	void DestroyAllParticles();

	void OnClientDisconnect();

	// CheckTransmit support (see kz_quiet.cpp).
	bool OwnsParticle(const CEntityHandle &handle) const;

	// Мастер-тумблер mhud: ON → дефолтный cs2kz-HUD гасится целиком, рисуются ТОЛЬКО
	// включённые per-element тумблеры (скорость/клавиши/время/CP-TP). OFF → HUD как раньше.
	bool IsMHUDMasterEnabled();

	// Per-element enable flags (also consulted for panel suppression).
	bool IsMHUDSpeedEnabled();
	bool IsMHUDPrespeedEnabled();
	bool IsMHUDTimerEnabled();
	bool IsMHUDKeysEnabled();
	bool IsMHUDCpTpEnabled();
	bool IsMHUDTimerDetailed();
	bool IsMHUDKeysOverlapEnabled();
	bool IsMHUDOutlineEnabled();

	// kz_mhud — prints a summary of all MHUD settings.
	void PrintMHUDSummary();

	// kz_mhud (без аргументов) — интерактивное меню тумблеров через cs2menus.
	// Фолбэк на PrintMHUDSummary(), если меню-движок недоступен.
	void OpenMHUDMenu();

private:
	std::string GetSpeedText(const char *language = KZ_DEFAULT_LANGUAGE);
	std::string GetKeyText(const char *language = KZ_DEFAULT_LANGUAGE);
	std::string GetCheckpointText(const char *language = KZ_DEFAULT_LANGUAGE);
	std::string GetTimerText(const char *language = KZ_DEFAULT_LANGUAGE);

	// Версия C: единый HTML-center HUD (крупная скорость, ряд клавиш, CP/TP, время|стейдж).
	// Вызывается на hudService ПОЛУЧАТЕЛЯ (спектатора): настройки берутся из this->player,
	// данные — из dataSource (наблюдаемый при спектировании; == this->player в норме).
	// suppress* — элементы, дублируемые particle-MHUD, чтобы не рисовать их дважды.
	// masterMode — мастер-тумблер: показываем ТОЛЬКО включённые per-element тумблеры.
	std::string BuildVersionCHud(KZPlayer *dataSource, bool suppressSpeed, bool suppressTimer, bool suppressKeys, bool masterMode,
								 const char *language);

	// Control point mapping:
	// 16 = RGB tint | 17X = sequence | 17Y = scale | 18X = X offset | 18Y = Y offset

	// Speed: two number-pair particles (each shows 00-99) so we can render up to 4 digits.
	CHandle<CParticleSystem> speedParticles[2];
	// Prespeed: same layout, smaller scale, different offset.
	CHandle<CParticleSystem> prespeedParticles[2];

	enum KeyParticleFlags : u8
	{
		Forward = 1 << 0,
		Left = 1 << 1,
		Back = 1 << 2,
		Right = 1 << 3,
		Jump = 1 << 4,
		Duck = 1 << 5,
	};

	CHandle<CParticleSystem> keysParticle;

	// Timer: up to 4 number-pair particles (h:mm:ss.cc) + up to 3 delimiter particles.
	CHandle<CParticleSystem> timerTextParticles[4];
	CHandle<CParticleSystem> timerDelimiterParticles[3];

	void UpdateMHUDSpeed();
	void SetMHUDSpeedParticleVelocity(const Vector &speed, const Vector *prespeed);

	void CheckMHUDTimerParticles();
	void UpdateMHUDTimer();

	void CheckMHUDKeyParticle();
	void UpdateMHUDKeys();

	// Preference helpers.
	Color GetMHUDColorPref(const char *name, const Color &defaultColor);
};
