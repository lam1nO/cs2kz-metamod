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

	// Источник ДАННЫХ для MHUD (скорость/клавиши/таймер/CP-TP); nullptr → сам игрок.
	// ПОСЛЕ cyb.36 particle-путь идёт ТОЛЬКО живому владельцу (player == target в
	// DrawPanels), спектатор — всегда HTML, так что фактически mhudSource всегда nullptr:
	// механика «чужой источник данных» сохранена как generic-задел (развязка data/settings),
	// вызывающей стороной не используется. НЕ включать её обратно для спектатора —
	// particle-оверлей позиционируется по взгляду владельца и у спектатора уезжает за
	// экран (регрессия cyb.34, см. комментарий в DrawPanels).
	// ВАЖНО: это ТОЛЬКО источник данных. Источник НАСТРОЕК (тумблеры/цвета/раскладка) —
	// всегда this->player, см. MHUDSettingsSource().
	KZPlayer *mhudSource {};

	// Игрок, чьи ДАННЫЕ читает MHUD (mhudSource, если задан; после cyb.36 — всегда сам игрок).
	KZPlayer *MHUDDataSource()
	{
		return mhudSource ? mhudSource : this->player;
	}

	// Игрок, чьи НАСТРОЙКИ (тумблеры/цвета/раскладка) читает MHUD — ВСЕГДА сам игрок.
	// Та же развязка data/settings живёт в HTML-пути (BuildVersionCHud/GetSpeedText):
	// там спектатор реально рисует чужие данные своей раскладкой.
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

	// Тип худа: 0 = Standard (HTML-панель), 1 = MHUD (particle-оверлей). Персистентный int-pref.
	int GetHudType();
	void SetHudType(int type);

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

	// Погасить particle-MHUD немедленно при уходе в спеки: OnProcessMovement у
	// обсервера не тикает, штатный транзишен не сработает — партикли зависают.
	void OnJoinSpectator();

	void OnClientDisconnect();

	// CheckTransmit support (see kz_quiet.cpp).
	bool OwnsParticle(const CEntityHandle &handle) const;

	// Per-element enable flags (also consulted for panel suppression).
	bool IsMHUDSpeedEnabled();
	bool IsMHUDPrespeedEnabled();
	bool IsMHUDTimerEnabled();
	bool IsMHUDKeysEnabled();
	bool IsMHUDCpTpEnabled();
	bool IsMHUDTimerDetailed();
	bool IsMHUDKeysOverlapEnabled();
	bool IsMHUDOutlineEnabled();

	// kz_hud / kz_mhud — печатает сводку текущего конфига.
	void PrintHUDSummary();

	// kz_hud (без аргументов) — интерактивное меню через cs2menus.
	// Фолбэк на PrintHUDSummary(), если меню-движок недоступен.
	void OpenHUDMenu();

	// Построить HUD-меню без показа (возвращает MenuHandle == u32; 0 = не построено).
	// backToOptions — добавить первым пунктом «← Назад» в корень !options
	// (используется подменю HUD в kz_option_menu.cpp).
	u32 CreateHUDMenu(bool backToOptions);

	// kz_mhud без аргументов — алиас OpenHUDMenu (обратная совместимость).
	void OpenMHUDMenu();

	// Константы геометрии particle-MHUD (public: нужны из файловых функций particles.cpp).
	// Клавиши: W A S D J C.
	static constexpr i32 MHUD_KEY_COUNT = 6;

private:
	// dataSource = источник данных (наблюдаемый при спектировании); nullptr → сам игрок.
	// Настройки (цвета perf/CJ) всегда идут с this (получателя) — см. GetMHUDColorPref.
	std::string GetSpeedText(const char *language = KZ_DEFAULT_LANGUAGE, KZPlayer *dataSource = nullptr);
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

	// Control point mapping (общий контракт particle-ассетов апстрима):
	// CP16       = RGB tint (0..255)
	// CP17.x     = sequence (кадр)
	// CP17.y     = size (масштаб)
	// CP17.z     = self-illum / init field1 (1.0 = вкл.)
	// CP18.x/y   = screen-space offset

	// Таймер: до 4 разрядов (каждый = двузначное значение) + до 3 разделителей.
	CHandle<CParticleSystem> timerTextParticles[4];
	CHandle<CParticleSystem> timerDelimiterParticles[3];

	// === MHUD (апстрим cs2kz particle-пути: velo/inputs/timer_delimiter) =================
	// Скорость: два particle'а на пары-разрядов (апстрим-схема hi/lo).
	CHandle<CParticleSystem> upstreamSpeedParticles[2];
	CHandle<CParticleSystem> upstreamPrespeedParticles[2];

	// Клавиши: один particle, sequence = 6-битная маска кнопок.
	enum KeyParticleFlags : u8
	{
		KPF_Forward = 1 << 0,
		KPF_Left    = 1 << 1,
		KPF_Back    = 1 << 2,
		KPF_Right   = 1 << 3,
		KPF_Jump    = 1 << 4,
		KPF_Duck    = 1 << 5,
	};
	CHandle<CParticleSystem> keysParticle;

	void UpdateMHUDSpeed();
	void SetMHUDSpeedParticleVelocity(const Vector &speed, const Vector *prespeed);

	void CheckMHUDTimerParticles();
	void UpdateMHUDTimer();

	void CheckMHUDKeyParticle();
	void UpdateMHUDKeys();

	// Preference helpers.
	Color GetMHUDColorPref(const char *name, const Color &defaultColor);
};
