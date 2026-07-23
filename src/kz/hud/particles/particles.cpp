#include "kz/hud/kz_hud.h"
#include "kz/option/kz_option.h"
#include "kz/language/kz_language.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "sdk/entity/cparticlesystem.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "utils/gamesystem.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include "tier0/memdbgon.h"

// Меню-движок cs2menus (определён в cs2kz.cpp); может быть nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

// === Particle asset paths ============================================================
//
// Апстрим cs2kz пути. Два аргумента: font (lato/verdana) × outline (outline/no_outline).
#define UPSTREAM_PARTICLE_NUMBERS_PATTERN     "particles/velo/velo_overlay_large_%s_%s.vpcf"
#define UPSTREAM_PARTICLE_TIMER_DELIM_PATTERN "particles/timer_delimiter/timer_delimiter_%s_%s.vpcf"
#define UPSTREAM_PARTICLE_INPUTS_PATTERN      "particles/inputs/inputs_%s_%s.vpcf"

static_global constexpr const char *UPSTREAM_AVAILABLE_FONTS[] = {"lato", "verdana"};

// === Layout constants ================================================================
// Таймер: шаг между разрядами (двузначные пары).
#define MHUD_TIMER_DIGIT_STEP     1.6f
#define MHUD_TIMER_DELIM_COLON    0    // sequence колона в timer_delim
#define MHUD_TIMER_DELIM_DOT      1    // sequence точки

// === Default preferences =============================================================

#define MHUD_DEF_SPEED_OFFSET_X    0.0f
#define MHUD_DEF_SPEED_OFFSET_Y    -4.5f
#define MHUD_DEF_SPEED_SCALE       0.04f
#define MHUD_DEF_PRESPEED_OFFSET_X 0.0f
#define MHUD_DEF_PRESPEED_OFFSET_Y -7.2f
#define MHUD_DEF_PRESPEED_SCALE    0.03f
#define MHUD_DEF_TIMER_OFFSET_X    0.0f
#define MHUD_DEF_TIMER_OFFSET_Y    -20.0f
#define MHUD_DEF_TIMER_SCALE       0.03f
#define MHUD_DEF_KEYS_OFFSET_X     0.0f
#define MHUD_DEF_KEYS_OFFSET_Y     -6.0f
#define MHUD_DEF_KEYS_SCALE        0.075f

static_global const Color MHUD_DEF_BASE_COLOR(255, 255, 255, 255);
static_global const Color MHUD_DEF_PERF_COLOR(0x40, 0xFF, 0x40, 0xFF);
static_global const Color MHUD_DEF_JUMPBUG_COLOR(0xFF, 0xFF, 0x20, 0xFF);
static_global const Color MHUD_DEF_CJ_COLOR(0x71, 0xEE, 0xB8, 0xFF);
static_global const Color MHUD_DEF_TIMER_TP_COLOR(255, 255, 255, 255);
static_global const Color MHUD_DEF_TIMER_PRO_COLOR(0x5F, 0x99, 0xD9, 0xFF);
static_global const Color MHUD_DEF_TIMER_PAUSED_COLOR(0xFF, 0xFF, 0x00, 0xFF);
static_global const Color MHUD_DEF_TIMER_STOPPED_COLOR(0xFF, 0xA0, 0xA0, 0xFF);
static_global const Color MHUD_DEF_KEYS_OVERLAP_COLOR(0xFF, 0x40, 0x40, 0xFF);

// === Helpers ========================================================================

static_function i64 PackColor(const Color &c)
{
	return ((i64)c.r() << 24) | ((i64)c.g() << 16) | ((i64)c.b() << 8) | (i64)c.a();
}

static_function Color UnpackColor(i64 packed)
{
	return Color((u8)((packed >> 24) & 0xFF), (u8)((packed >> 16) & 0xFF), (u8)((packed >> 8) & 0xFF), (u8)(packed & 0xFF));
}

Color KZHUDService::GetMHUDColorPref(const char *name, const Color &defaultColor)
{
	// Цвета — настройка: источник настроек (сам игрок / спектатор), не данные.
	i64 packed = this->MHUDSettingsSource()->optionService->GetPreferenceInt(name, PackColor(defaultColor));
	return UnpackColor(packed);
}

// Апстрим-helper: строит путь из pattern + font + outline суффикс.
static_function void BuildUpstreamPath(char *buf, size_t bufSize, const char *pattern, const char *font, bool outline)
{
	V_snprintf(buf, bufSize, pattern, font, outline ? "outline" : "no_outline");
}

// Возвращает нижний регистр mhudFont preference (lato/verdana) из optionService игрока.
// settingsPlayer — всегда игрок-владелец настроек (не наблюдаемый при спектировании).
static_function const char *GetUpstreamFont(KZPlayer *settingsPlayer, char *out_buf, size_t bufSize)
{
	const char *font = settingsPlayer->optionService->GetPreferenceStr("mhudFont", UPSTREAM_AVAILABLE_FONTS[0]);
	V_strncpy(out_buf, font, bufSize);
	V_strlower(out_buf);
	// Проверяем, что шрифт из допустимого списка; иначе дефолт.
	bool valid = false;
	for (const char *f : UPSTREAM_AVAILABLE_FONTS)
	{
		if (KZ_STREQ(out_buf, f)) { valid = true; break; }
	}
	if (!valid) V_strncpy(out_buf, UPSTREAM_AVAILABLE_FONTS[0], bufSize);
	return out_buf;
}

void KZHUDService::PrecacheParticles(IEntityResourceManifest *pResourceManifest)
{
	// Прекешируем все пути, которые могут понадобиться в рантайме.
	char path[256];

	// Апстрим: velo / inputs / timer_delimiter, font × outline.
	for (const char *font : UPSTREAM_AVAILABLE_FONTS)
	{
		for (bool outline : {false, true})
		{
			BuildUpstreamPath(path, sizeof(path), UPSTREAM_PARTICLE_NUMBERS_PATTERN, font, outline);
			pResourceManifest->AddResource(path);
			BuildUpstreamPath(path, sizeof(path), UPSTREAM_PARTICLE_TIMER_DELIM_PATTERN, font, outline);
			pResourceManifest->AddResource(path);
			BuildUpstreamPath(path, sizeof(path), UPSTREAM_PARTICLE_INPUTS_PATTERN, font, outline);
			pResourceManifest->AddResource(path);
		}
	}
}

// === Particle creation ==============================================================

static_function CParticleSystem *CreateMHUDParticle(const char *particleName, const Color &color, const f32 sequence, const f32 size,
													  const f32 offsetX, const f32 offsetY)
{
	CParticleSystem *particleSystem = utils::CreateEntityByName<CParticleSystem>("info_particle_system");
	if (!particleSystem)
	{
		Warning("[KZ] Failed to create particle system for MHUD!\n");
		return nullptr;
	}
	CEntityKeyValues *pKeyValues = new CEntityKeyValues();
	pKeyValues->SetString("effect_name", particleName);
	pKeyValues->SetBool("start_active", true);
	pKeyValues->SetInt("tint_cp", 16);
	pKeyValues->SetColor("tint_cp_color", color);
	pKeyValues->SetInt("data_cp", 17);
	// CP17: x=sequence, y=size, z=self-illum (1.0 = включить glow-канал в .vpcf)
	pKeyValues->SetVector("data_cp_value", Vector(sequence, size, 1.0f));
	// Помечаем как plugin-particle для kz_quiet CheckTransmit.
	particleSystem->m_iTeamNum(CUSTOM_PARTICLE_SYSTEM_TEAM);
	particleSystem->DispatchSpawn(pKeyValues);
	particleSystem->SetControlPointValue(18, Vector(offsetX, offsetY, 1.0f));
	return particleSystem;
}

bool KZHUDService::OwnsParticle(const CEntityHandle &handle) const
{
	// Таймер
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerTextParticles); i++)
	{
		if (handle == this->timerTextParticles[i])
		{
			return true;
		}
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerDelimiterParticles); i++)
	{
		if (handle == this->timerDelimiterParticles[i])
		{
			return true;
		}
	}
	// MHUD (апстрим) particle'ы
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->upstreamSpeedParticles); i++)
	{
		if (handle == this->upstreamSpeedParticles[i] || handle == this->upstreamPrespeedParticles[i])
		{
			return true;
		}
	}
	if (handle == this->keysParticle)
	{
		return true;
	}
	return false;
}

// === Cleanup ========================================================================

static_function void DestroyHandle(CHandle<CParticleSystem> &handle)
{
	CParticleSystem *p = handle.Get();
	if (p)
	{
		g_pKZUtils->RemoveEntity(p);
	}
	handle = {};
}

void KZHUDService::DestroyAllParticles()
{
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->upstreamSpeedParticles); i++)
	{
		DestroyHandle(this->upstreamSpeedParticles[i]);
		DestroyHandle(this->upstreamPrespeedParticles[i]);
	}
	DestroyHandle(this->keysParticle);

	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerTextParticles); i++)
	{
		DestroyHandle(this->timerTextParticles[i]);
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerDelimiterParticles); i++)
	{
		DestroyHandle(this->timerDelimiterParticles[i]);
	}
}

void KZHUDService::OnClientDisconnect()
{
	this->DestroyAllParticles();
}

// === Preferences ====================================================================

// === Тип худа (hudType) ============================================================
// 0 = Standard (классическая HTML-панель по центру), 1 = MHUD (particle-оверлей).
// Нарисованный cyberkz-HUD выпилен в cyb.27 — Standard теперь всегда HTML-путь.

int KZHUDService::GetHudType()
{
	// Читаем из настроек источника (сам игрок / спектатор).
	// Миграция: если hudType не задан и mhudMaster=true (старый конфиг) → возвращаем 1 (MHUD).
	auto *opts = this->MHUDSettingsSource()->optionService;
	int stored = opts->GetPreferenceInt("hudType", -1);
	if (stored == -1)
	{
		// Первый запрос — мигрируем из mhudMaster.
		bool legacyMaster = opts->GetPreferenceBool("mhudMaster", false);
		int migrated = legacyMaster ? 1 : 0;
		opts->SetPreferenceInt("hudType", migrated);
		return migrated;
	}
	return stored;
}

void KZHUDService::SetHudType(int type)
{
	this->MHUDSettingsSource()->optionService->SetPreferenceInt("hudType", type);
	// При смене типа уничтожаем все particle'ы, чтобы корректно переключить состояние.
	this->DestroyAllParticles();
}

// Все тумблеры/раскладка — НАСТРОЙКИ: читаем из источника настроек (сам игрок/спектатор),
// а не из данных наблюдаемого. Иначе у спектатора «прыгал» бы HUD при смене цели.
// Per-element тумблеры.
bool KZHUDService::IsMHUDSpeedEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudSpeed", true);
}

bool KZHUDService::IsMHUDTimerEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudTimer", true);
}

bool KZHUDService::IsMHUDKeysEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudKeys", true);
}

bool KZHUDService::IsMHUDCpTpEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudCpTp", true);
}

bool KZHUDService::IsMHUDTimerDetailed()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudTimerDetail", true);
}

bool KZHUDService::IsMHUDKeysOverlapEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudKeysOverlap", true);
}

bool KZHUDService::IsMHUDPrespeedEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudPrespeed", true);
}

bool KZHUDService::IsMHUDOutlineEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudOutline", true);
}

// === Helpers для particle ============================================================

static_function void SetParticleTint(CParticleSystem *particle, const Color &color)
{
	if (!particle)
	{
		return;
	}
	particle->SetControlPointValue(16, Vector((f32)color.r(), (f32)color.g(), (f32)color.b()));
}

// Обновить CP17 (sequence, scale, self-illum=1.0) и CP18 (X, Y, 1.0) у существующей particle.
static_function void UpdateParticleLayout(CParticleSystem *p, f32 sequence, f32 scale, f32 x, f32 y)
{
	if (!p)
	{
		return;
	}
	// CP17.z = 1.0 — self-illum включён (glow-канал .vpcf); совпадает с CreateMHUDParticle.
	p->SetControlPointValue(17, Vector(sequence, scale, 1.0f));
	p->SetControlPointValue(18, Vector(x, y, 1.0f));
}

// === Speed + prespeed ================================================================

// Два particle'а — пары разрядов hi/lo (cp17.z = 0.0f, не 1.0f — апстрим-контракт).
static_function void LayoutDigitPair(i32 value, f32 size, f32 baseOffsetX, f32 baseOffsetY,
									  CParticleSystem *p0, CParticleSystem *p1, bool transmit[2])
{
	// Апстрим-константы позиций
	static constexpr f32 X_LEFT       = -0.625f;
	static constexpr f32 X_CENTER     =  0.0f;
	static constexpr f32 X_RIGHT_3DIG =  0.3125f;
	static constexpr f32 X_RIGHT_4DIG =  0.625f;

	transmit[0] = true;
	transmit[1] = true;
	if (value >= 1000)
	{
		i32 hi = value / 100;
		i32 lo = value % 100;
		if (lo < 10) lo += 100; // вариант "0X"
		p0->SetControlPointValue(17, Vector((f32)hi, size, 0.0f));
		p0->SetControlPointValue(18, Vector(baseOffsetX + X_LEFT, baseOffsetY, 1.0f));
		p1->SetControlPointValue(17, Vector((f32)lo, size, 0.0f));
		p1->SetControlPointValue(18, Vector(baseOffsetX + X_RIGHT_4DIG, baseOffsetY, 1.0f));
	}
	else if (value >= 100)
	{
		i32 hi = value / 100;
		i32 lo = value % 100;
		if (lo < 10) lo += 100;
		p0->SetControlPointValue(17, Vector((f32)hi, size, 0.0f));
		p0->SetControlPointValue(18, Vector(baseOffsetX + X_LEFT, baseOffsetY, 1.0f));
		p1->SetControlPointValue(17, Vector((f32)lo, size, 0.0f));
		p1->SetControlPointValue(18, Vector(baseOffsetX + X_RIGHT_3DIG, baseOffsetY, 1.0f));
	}
	else
	{
		p0->SetControlPointValue(17, Vector((f32)value, size, 0.0f));
		p0->SetControlPointValue(18, Vector(baseOffsetX + X_CENTER, baseOffsetY, 1.0f));
		transmit[1] = false;
	}
}

void KZHUDService::UpdateMHUDSpeed()
{
	bool speedEnabled    = this->IsMHUDSpeedEnabled();
	bool prespeedEnabled = this->IsMHUDPrespeedEnabled();

	if (!speedEnabled && !prespeedEnabled)
	{
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->upstreamSpeedParticles); i++)
		{
			if (this->upstreamSpeedParticles[i])    DestroyHandle(this->upstreamSpeedParticles[i]);
			if (this->upstreamPrespeedParticles[i]) DestroyHandle(this->upstreamPrespeedParticles[i]);
		}
		return;
	}

	const Color baseColor         = this->GetMHUDColorPref("mhudSpeedColor",    MHUD_DEF_BASE_COLOR);
	const Color prespeedBaseColor = this->GetMHUDColorPref("mhudPrespeedColor", MHUD_DEF_BASE_COLOR);
	const f32 speedOffsetX   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudSpeedOffsetX",   MHUD_DEF_SPEED_OFFSET_X);
	const f32 speedOffsetY   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudSpeedOffsetY",   MHUD_DEF_SPEED_OFFSET_Y);
	const f32 speedScale     = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudSpeedScale",     MHUD_DEF_SPEED_SCALE);
	const f32 prespeedOffsetX = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPrespeedOffsetX", MHUD_DEF_PRESPEED_OFFSET_X);
	const f32 prespeedOffsetY = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPrespeedOffsetY", MHUD_DEF_PRESPEED_OFFSET_Y);
	const f32 prespeedScale   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPrespeedScale",   MHUD_DEF_PRESPEED_SCALE);
	bool outline = this->IsMHUDOutlineEnabled();

	char fontBuf[64];
	const char *font = GetUpstreamFont(this->player, fontBuf, sizeof(fontBuf));
	char numbersPath[256];
	BuildUpstreamPath(numbersPath, sizeof(numbersPath), UPSTREAM_PARTICLE_NUMBERS_PATTERN, font, outline);

	if (!speedEnabled)
	{
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->upstreamSpeedParticles); i++)
			if (this->upstreamSpeedParticles[i]) DestroyHandle(this->upstreamSpeedParticles[i]);
	}
	if (!prespeedEnabled)
	{
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->upstreamPrespeedParticles); i++)
			if (this->upstreamPrespeedParticles[i]) DestroyHandle(this->upstreamPrespeedParticles[i]);
	}
	if (speedEnabled)
	{
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->upstreamSpeedParticles); i++)
			if (!this->upstreamSpeedParticles[i])
				this->upstreamSpeedParticles[i] = CreateMHUDParticle(numbersPath, baseColor, 0.0f, speedScale, speedOffsetX, speedOffsetY);
	}
	if (prespeedEnabled)
	{
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->upstreamPrespeedParticles); i++)
			if (!this->upstreamPrespeedParticles[i])
				this->upstreamPrespeedParticles[i] = CreateMHUDParticle(numbersPath, prespeedBaseColor, 0.0f, prespeedScale, prespeedOffsetX, prespeedOffsetY);
	}

	KZPlayer *src = this->MHUDDataSource();
	Vector velocity, baseVelocity;
	src->GetVelocity(&velocity);
	src->GetBaseVelocity(&baseVelocity);
	velocity += baseVelocity;

	bool useTakeoff = !((src->GetPlayerPawn()->m_fFlags() & FL_ONGROUND
						 && g_pKZUtils->GetServerGlobals()->curtime - src->landingTime > KZ_HUD_ON_GROUND_THRESHOLD)
						|| (src->GetPlayerPawn()->m_MoveType() == MOVETYPE_LADDER && !src->IsButtonPressed(IN_JUMP)));

	this->SetMHUDSpeedParticleVelocity(velocity, useTakeoff ? &src->takeoffVelocity : nullptr);

	const Color perfColor    = this->GetMHUDColorPref("mhudPrespeedPerfColor",  MHUD_DEF_PERF_COLOR);
	const Color jumpbugColor = this->GetMHUDColorPref("mhudPrespeedJumpbugColor", MHUD_DEF_JUMPBUG_COLOR);
	const Color cjColor      = this->GetMHUDColorPref("mhudSpeedCjColor",       MHUD_DEF_CJ_COLOR);
	bool perfing = src->IsPerfing() && !src->possibleLadderHop && !src->takeoffFromLadder;

	const Color speedColor    = useTakeoff && src->hudService->crouchJumping ? cjColor : baseColor;
	const Color prespeedColor = perfing ? (src->hudService->fromDuckbug ? jumpbugColor : perfColor) : prespeedBaseColor;

	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->upstreamSpeedParticles); i++)
	{
		SetParticleTint(this->upstreamSpeedParticles[i].Get(),    speedColor);
		SetParticleTint(this->upstreamPrespeedParticles[i].Get(), prespeedColor);
	}
}

void KZHUDService::SetMHUDSpeedParticleVelocity(const Vector &speed, const Vector *prespeed)
{
	const f32 speedOffsetX   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudSpeedOffsetX",   MHUD_DEF_SPEED_OFFSET_X);
	const f32 speedOffsetY   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudSpeedOffsetY",   MHUD_DEF_SPEED_OFFSET_Y);
	const f32 speedScale     = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudSpeedScale",     MHUD_DEF_SPEED_SCALE);
	const f32 prespeedOffsetX = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPrespeedOffsetX", MHUD_DEF_PRESPEED_OFFSET_X);
	const f32 prespeedOffsetY = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPrespeedOffsetY", MHUD_DEF_PRESPEED_OFFSET_Y);
	const f32 prespeedScale   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPrespeedScale",   MHUD_DEF_PRESPEED_SCALE);

	// === MHUD (апстрим): два particle'а, пары разрядов, апстрим LayoutDigitPair ===
	if (this->upstreamSpeedParticles[0] && this->upstreamSpeedParticles[1])
	{
		i32 val = RoundFloatToInt(speed.Length2D());
		if (val < 0)    val = 0;
		if (val > 9999) val = 9999;
		bool transmit[2];
		LayoutDigitPair(val, speedScale, speedOffsetX, speedOffsetY,
						this->upstreamSpeedParticles[0].Get(), this->upstreamSpeedParticles[1].Get(), transmit);
		this->upstreamSpeedParticles[0].Get()->Start();
		if (transmit[1])
			this->upstreamSpeedParticles[1].Get()->Start();
		else
			this->upstreamSpeedParticles[1].Get()->Destroy();
	}
	if (this->upstreamPrespeedParticles[0] && this->upstreamPrespeedParticles[1])
	{
		if (prespeed)
		{
			i32 val = RoundFloatToInt(prespeed->Length2D());
			if (val < 0)    val = 0;
			if (val > 9999) val = 9999;
			bool transmit[2];
			LayoutDigitPair(val, prespeedScale, prespeedOffsetX, prespeedOffsetY,
							this->upstreamPrespeedParticles[0].Get(), this->upstreamPrespeedParticles[1].Get(), transmit);
			this->upstreamPrespeedParticles[0].Get()->Start();
			if (transmit[1])
				this->upstreamPrespeedParticles[1].Get()->Start();
			else
				this->upstreamPrespeedParticles[1].Get()->Destroy();
		}
		else
		{
			this->upstreamPrespeedParticles[0].Get()->Destroy();
			this->upstreamPrespeedParticles[1].Get()->Destroy();
		}
	}
}

// === Timer ==========================================================================

void KZHUDService::CheckMHUDTimerParticles()
{
	bool enabled = this->IsMHUDTimerEnabled();

	if (!enabled)
	{
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerTextParticles); i++)
		{
			if (this->timerTextParticles[i]) DestroyHandle(this->timerTextParticles[i]);
		}
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerDelimiterParticles); i++)
		{
			if (this->timerDelimiterParticles[i]) DestroyHandle(this->timerDelimiterParticles[i]);
		}
		return;
	}

	const Color tpColor = this->GetMHUDColorPref("mhudTimerTpColor", MHUD_DEF_TIMER_TP_COLOR);
	const f32 scale     = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudTimerScale",   MHUD_DEF_TIMER_SCALE);
	const f32 offsetY   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudTimerOffsetY", MHUD_DEF_TIMER_OFFSET_Y);
	bool outline = this->IsMHUDOutlineEnabled();

	char fontBuf[64];
	const char *font = GetUpstreamFont(this->player, fontBuf, sizeof(fontBuf));
	char numbersPath[256], delimPath[256];
	BuildUpstreamPath(numbersPath, sizeof(numbersPath), UPSTREAM_PARTICLE_NUMBERS_PATTERN, font, outline);
	BuildUpstreamPath(delimPath,   sizeof(delimPath),   UPSTREAM_PARTICLE_TIMER_DELIM_PATTERN, font, outline);

	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerTextParticles); i++)
	{
		if (!this->timerTextParticles[i])
			this->timerTextParticles[i] = CreateMHUDParticle(numbersPath, tpColor, 0.0f, scale, 0.0f, offsetY);
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerDelimiterParticles); i++)
	{
		if (!this->timerDelimiterParticles[i])
			this->timerDelimiterParticles[i] = CreateMHUDParticle(delimPath, tpColor, 0.0f, scale, 0.0f, offsetY);
	}
}

void KZHUDService::UpdateMHUDTimer()
{
	this->CheckMHUDTimerParticles();
	if (!this->timerTextParticles[0])
	{
		return;
	}

	KZPlayer *src      = this->MHUDDataSource();
	bool timerRunning  = src->timerService->GetTimerRunning();
	bool showAfterStop = src->hudService->ShouldShowTimerAfterStop();
	if (!timerRunning && !showAfterStop)
	{
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerTextParticles); i++)
		{
			CParticleSystem *p = this->timerTextParticles[i].Get();
			if (p) p->Destroy();
		}
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerDelimiterParticles); i++)
		{
			CParticleSystem *p = this->timerDelimiterParticles[i].Get();
			if (p) p->Destroy();
		}
		return;
	}

	f64 time = timerRunning ? src->timerService->GetTime() : src->hudService->currentTimeWhenTimerStopped;
	if (time < 0.0) time = 0.0;

	bool detailed = this->IsMHUDTimerDetailed();
	bool paused   = src->timerService->GetPaused();

	i32 totalSeconds = (i32)time;
	i32 hours   = totalSeconds / 3600;
	i32 minutes = (totalSeconds / 60) % 60;
	i32 seconds = totalSeconds % 60;
	i32 centis  = (i32)(time * 100.0) % 100;
	if (centis < 0) centis = 0;

	// Разряды: digit[i] — двузначное пары-значение; ведущий ноль = +100.
	// Форматы:  HH:MM:SS.CC  HH:MM:SS  MM:SS.CC  MM:SS
	i32 digit[4] = {0, 0, 0, 0};
	bool digitVisible[4] = {false, false, false, false};
	i32 delimTypes[3] = {-1, -1, -1};
	i32 numDelimiters = 0;

	if (hours > 0 && detailed)
	{
		digit[0] = hours > 9 ? (hours > 99 ? 99 : hours) : hours + 100;
		digit[1] = (minutes < 10) ? minutes + 100 : minutes;
		digit[2] = (seconds < 10) ? seconds + 100 : seconds;
		digit[3] = (centis  < 10) ? centis  + 100 : centis;
		digitVisible[0] = digitVisible[1] = digitVisible[2] = digitVisible[3] = true;
		delimTypes[0] = MHUD_TIMER_DELIM_COLON;
		delimTypes[1] = MHUD_TIMER_DELIM_COLON;
		delimTypes[2] = MHUD_TIMER_DELIM_DOT;
		numDelimiters = 3;
	}
	else if (hours > 0)
	{
		digit[0] = hours > 9 ? (hours > 99 ? 99 : hours) : hours + 100;
		digit[1] = (minutes < 10) ? minutes + 100 : minutes;
		digit[2] = (seconds < 10) ? seconds + 100 : seconds;
		digitVisible[0] = digitVisible[1] = digitVisible[2] = true;
		delimTypes[0] = MHUD_TIMER_DELIM_COLON;
		delimTypes[1] = MHUD_TIMER_DELIM_COLON;
		numDelimiters = 2;
	}
	else if (detailed)
	{
		digit[0] = (minutes < 10) ? minutes + 100 : minutes;
		digit[1] = (seconds < 10) ? seconds + 100 : seconds;
		digit[2] = (centis  < 10) ? centis  + 100 : centis;
		digitVisible[0] = digitVisible[1] = digitVisible[2] = true;
		delimTypes[0] = MHUD_TIMER_DELIM_COLON;
		delimTypes[1] = MHUD_TIMER_DELIM_DOT;
		numDelimiters = 2;
	}
	else
	{
		digit[0] = (minutes < 10) ? minutes + 100 : minutes;
		digit[1] = (seconds < 10) ? seconds + 100 : seconds;
		digitVisible[0] = digitVisible[1] = true;
		delimTypes[0] = MHUD_TIMER_DELIM_COLON;
		numDelimiters = 1;
	}

	const f32 offsetX = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudTimerOffsetX", MHUD_DEF_TIMER_OFFSET_X);
	const f32 offsetY = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudTimerOffsetY", MHUD_DEF_TIMER_OFFSET_Y);
	const f32 scale   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudTimerScale",   MHUD_DEF_TIMER_SCALE);

	// Центрируем N видимых разрядов. Шаг фиксированный — апстрим-контракт не масштабирует.
	i32 numVisible = 0;
	for (i32 i = 0; i < 4; i++) numVisible += digitVisible[i] ? 1 : 0;

	f32 timerStep = MHUD_TIMER_DIGIT_STEP;

	f32 layoutX[4] = {};
	for (i32 i = 0; i < numVisible; i++)
	{
		layoutX[i] = offsetX + ((f32)i - (numVisible - 1) * 0.5f) * timerStep;
	}

	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerTextParticles); i++)
	{
		CParticleSystem *p = this->timerTextParticles[i].Get();
		if (!p) continue;
		if (digitVisible[i])
		{
			UpdateParticleLayout(p, (f32)digit[i], scale, layoutX[i], offsetY);
			p->Start();
		}
		else
		{
			p->Destroy();
		}
	}
	for (i32 d = 0; d < (i32)KZ_ARRAYSIZE(this->timerDelimiterParticles); d++)
	{
		CParticleSystem *p = this->timerDelimiterParticles[d].Get();
		if (!p) continue;
		if (d < numDelimiters)
		{
			f32 delimX = layoutX[d] + timerStep * 0.5f;
			UpdateParticleLayout(p, (f32)delimTypes[d], scale, delimX, offsetY);
			p->Start();
		}
		else
		{
			p->Destroy();
		}
	}

	// Тинт по состоянию
	Color color;
	if (!timerRunning)
	{
		color = this->GetMHUDColorPref("mhudTimerStoppedColor", MHUD_DEF_TIMER_STOPPED_COLOR);
	}
	else if (paused)
	{
		color = this->GetMHUDColorPref("mhudTimerPausedColor", MHUD_DEF_TIMER_PAUSED_COLOR);
	}
	else if (src->checkpointService->GetTeleportCount() > 0)
	{
		color = this->GetMHUDColorPref("mhudTimerTpColor", MHUD_DEF_TIMER_TP_COLOR);
	}
	else
	{
		color = this->GetMHUDColorPref("mhudTimerProColor", MHUD_DEF_TIMER_PRO_COLOR);
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerTextParticles); i++)
	{
		SetParticleTint(this->timerTextParticles[i].Get(), color);
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerDelimiterParticles); i++)
	{
		SetParticleTint(this->timerDelimiterParticles[i].Get(), color);
	}
}

// === Keys ============================================================================

// Один particle, 6-битная маска.
void KZHUDService::CheckMHUDKeyParticle()
{
	bool enabled = this->IsMHUDKeysEnabled();
	if (!enabled)
	{
		if (this->keysParticle) DestroyHandle(this->keysParticle);
		return;
	}

	const Color color = this->GetMHUDColorPref("mhudKeysColor", MHUD_DEF_BASE_COLOR);
	const f32 offsetX = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysOffsetX", MHUD_DEF_KEYS_OFFSET_X);
	const f32 offsetY = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysOffsetY", MHUD_DEF_KEYS_OFFSET_Y);
	const f32 scale   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysScale",   MHUD_DEF_KEYS_SCALE);
	bool outline = this->IsMHUDOutlineEnabled();

	char fontBuf[64];
	const char *font = GetUpstreamFont(this->player, fontBuf, sizeof(fontBuf));
	char inputsPath[256];
	BuildUpstreamPath(inputsPath, sizeof(inputsPath), UPSTREAM_PARTICLE_INPUTS_PATTERN, font, outline);

	if (!this->keysParticle)
		this->keysParticle = CreateMHUDParticle(inputsPath, color, 0.0f, scale, offsetX, offsetY);
}

void KZHUDService::UpdateMHUDKeys()
{
	KZPlayer *src = this->MHUDDataSource();

	bool pressed[MHUD_KEY_COUNT];
	pressed[0] = src->IsButtonPressed(IN_FORWARD);                                  // W
	pressed[1] = src->IsButtonPressed(IN_MOVELEFT);                                 // A
	pressed[2] = src->IsButtonPressed(IN_BACK);                                     // S
	pressed[3] = src->IsButtonPressed(IN_MOVERIGHT);                                // D
	pressed[4] = src->hudService->jumpedThisTick || src->IsButtonPressed(IN_JUMP);  // J
	pressed[5] = src->IsButtonPressed(IN_DUCK);                                     // C

	const f32 offsetX = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysOffsetX", MHUD_DEF_KEYS_OFFSET_X);
	const f32 offsetY = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysOffsetY", MHUD_DEF_KEYS_OFFSET_Y);
	const f32 scale   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysScale",   MHUD_DEF_KEYS_SCALE);

	bool hasOverlap         = (pressed[0] && pressed[2]) || (pressed[1] && pressed[3]);
	const Color overlapColor = this->GetMHUDColorPref("mhudKeysOverlapColor", MHUD_DEF_KEYS_OVERLAP_COLOR);
	const Color baseColor    = this->GetMHUDColorPref("mhudKeysColor",        MHUD_DEF_BASE_COLOR);

	this->CheckMHUDKeyParticle();
	CParticleSystem *p = this->keysParticle.Get();
	if (!p) return;

	u8 mask = 0;
	if (pressed[0]) mask |= KPF_Forward;
	if (pressed[1]) mask |= KPF_Left;
	if (pressed[2]) mask |= KPF_Back;
	if (pressed[3]) mask |= KPF_Right;
	if (pressed[4]) mask |= KPF_Jump;
	if (pressed[5]) mask |= KPF_Duck;

	p->SetControlPointValue(17, Vector((f32)mask, scale, 0.0f));
	p->SetControlPointValue(18, Vector(offsetX, offsetY, 1.0f));
	p->Start();

	Color tint = hasOverlap && this->IsMHUDKeysOverlapEnabled() ? overlapColor : baseColor;
	SetParticleTint(p, tint);
}


// === UpdateParticles ================================================================

void KZHUDService::UpdateParticles(KZPlayer *source)
{
	// Данные — из mhudSource (source, если это не сам игрок), настройки — всегда из
	// this->player. После cyb.36 DrawPanels вызывает это ТОЛЬКО при player == target
	// (живой владелец), т.е. source == this->player и mhudSource остаётся nullptr;
	// spectator-ветка сохранена как generic-задел и вызывающей стороной не используется
	// (спектатор всегда идёт HTML-путём, см. гейт useParticles в DrawPanels).
	this->mhudSource = (source && source != this->player) ? source : nullptr;
	this->UpdateMHUDSpeed();
	this->UpdateMHUDTimer();
	this->UpdateMHUDKeys();
	this->mhudSource = nullptr;
}

// === Command helpers ================================================================

// Toggle a MHUD bool preference and print the result.
static_function void MHUDToggle(KZPlayer *p, const char *prefKey, bool defaultValue, const char *enabledKey, const char *disabledKey)
{
	bool next = !p->optionService->GetPreferenceBool(prefKey, defaultValue);
	p->optionService->SetPreferenceBool(prefKey, next);
	p->languageService->PrintChat(true, false, next ? enabledKey : disabledKey);
}

// Set offset x+y for a MHUD element.
static_function void MHUDSetOffset(KZPlayer *p, const CCommand *args, int argOffset, const char *prefKeyX, const char *prefKeyY, f32 defaultX,
									f32 defaultY, const char *usageKey, const char *setKey)
{
	if (args->ArgC() < argOffset + 2 || !utils::IsNumeric(args->Arg(argOffset)) || !utils::IsNumeric(args->Arg(argOffset + 1)))
	{
		f32 cx = (f32)p->optionService->GetPreferenceFloat(prefKeyX, defaultX);
		f32 cy = (f32)p->optionService->GetPreferenceFloat(prefKeyY, defaultY);
		char cxs[16], cys[16];
		V_snprintf(cxs, sizeof(cxs), "%.4f", cx);
		V_snprintf(cys, sizeof(cys), "%.4f", cy);
		p->languageService->PrintChat(true, false, usageKey, cxs, cys);
		return;
	}
	f32 x = (f32)atof(args->Arg(argOffset));
	f32 y = (f32)atof(args->Arg(argOffset + 1));
	p->optionService->SetPreferenceFloat(prefKeyX, x);
	p->optionService->SetPreferenceFloat(prefKeyY, y);
	char xs[16], ys[16];
	V_snprintf(xs, sizeof(xs), "%.4f", x);
	V_snprintf(ys, sizeof(ys), "%.4f", y);
	p->languageService->PrintChat(true, false, setKey, xs, ys);
}

// Set scale for a MHUD element.
static_function void MHUDSetScale(KZPlayer *p, const CCommand *args, int argOffset, const char *prefKey, f32 defaultValue, const char *usageKey,
								   const char *setKey)
{
	if (args->ArgC() < argOffset + 1 || !utils::IsNumeric(args->Arg(argOffset)))
	{
		f32 cv = (f32)p->optionService->GetPreferenceFloat(prefKey, defaultValue);
		char cvs[16];
		V_snprintf(cvs, sizeof(cvs), "%.4f", cv);
		p->languageService->PrintChat(true, false, usageKey, cvs);
		return;
	}
	f32 v = (f32)atof(args->Arg(argOffset));
	p->optionService->SetPreferenceFloat(prefKey, v);
	char vs[16];
	V_snprintf(vs, sizeof(vs), "%.4f", v);
	p->languageService->PrintChat(true, false, setKey, vs);
}

// Set a color preference (packed as int).
static_function void SetColorPref(KZPlayer *p, const CCommand *args, int argOffset, const char *prefKey, const char *usageKey, const char *setKey)
{
	Color c;
	bool named = args->ArgC() >= argOffset + 1 && utils::ParseColorName(args->Arg(argOffset), c);
	if (!named && !utils::ParseColorArgs(args, argOffset, c))
	{
		p->languageService->PrintChat(true, false, usageKey);
		return;
	}
	p->optionService->SetPreferenceInt(prefKey, PackColor(c));
	char colorStr[32];
	V_snprintf(colorStr, sizeof(colorStr), "(%d, %d, %d, %d)", c.r(), c.g(), c.b(), c.a());
	p->languageService->PrintChat(true, false, setKey, colorStr);
}

enum class MHUDElement
{
	Speed,
	Prespeed,
	Timer,
	Keys,
	CpTp
};

static_function void ResetElementPrefs(KZPlayer *p, MHUDElement element)
{
	switch (element)
	{
		case MHUDElement::Speed:
			p->optionService->SetPreferenceBool("hudSpeed", true);
			p->optionService->SetPreferenceFloat("mhudSpeedOffsetX", MHUD_DEF_SPEED_OFFSET_X);
			p->optionService->SetPreferenceFloat("mhudSpeedOffsetY", MHUD_DEF_SPEED_OFFSET_Y);
			p->optionService->SetPreferenceFloat("mhudSpeedScale", MHUD_DEF_SPEED_SCALE);
			p->optionService->SetPreferenceInt("mhudSpeedColor", PackColor(MHUD_DEF_BASE_COLOR));
			p->optionService->SetPreferenceInt("mhudSpeedCjColor", PackColor(MHUD_DEF_CJ_COLOR));
			break;
		case MHUDElement::Prespeed:
			p->optionService->SetPreferenceBool("hudPrespeed", true);
			p->optionService->SetPreferenceFloat("mhudPrespeedOffsetX", MHUD_DEF_PRESPEED_OFFSET_X);
			p->optionService->SetPreferenceFloat("mhudPrespeedOffsetY", MHUD_DEF_PRESPEED_OFFSET_Y);
			p->optionService->SetPreferenceFloat("mhudPrespeedScale", MHUD_DEF_PRESPEED_SCALE);
			p->optionService->SetPreferenceInt("mhudPrespeedColor", PackColor(MHUD_DEF_BASE_COLOR));
			p->optionService->SetPreferenceInt("mhudPrespeedPerfColor", PackColor(MHUD_DEF_PERF_COLOR));
			p->optionService->SetPreferenceInt("mhudPrespeedJumpbugColor", PackColor(MHUD_DEF_JUMPBUG_COLOR));
			break;
		case MHUDElement::Timer:
			p->optionService->SetPreferenceBool("hudTimer", true);
			p->optionService->SetPreferenceBool("hudTimerDetail", true);
			p->optionService->SetPreferenceFloat("mhudTimerOffsetX", MHUD_DEF_TIMER_OFFSET_X);
			p->optionService->SetPreferenceFloat("mhudTimerOffsetY", MHUD_DEF_TIMER_OFFSET_Y);
			p->optionService->SetPreferenceFloat("mhudTimerScale", MHUD_DEF_TIMER_SCALE);
			p->optionService->SetPreferenceInt("mhudTimerTpColor", PackColor(MHUD_DEF_TIMER_TP_COLOR));
			p->optionService->SetPreferenceInt("mhudTimerProColor", PackColor(MHUD_DEF_TIMER_PRO_COLOR));
			p->optionService->SetPreferenceInt("mhudTimerPausedColor", PackColor(MHUD_DEF_TIMER_PAUSED_COLOR));
			p->optionService->SetPreferenceInt("mhudTimerStoppedColor", PackColor(MHUD_DEF_TIMER_STOPPED_COLOR));
			break;
		case MHUDElement::Keys:
			p->optionService->SetPreferenceBool("hudKeys", true);
			p->optionService->SetPreferenceBool("hudKeysOverlap", true);
			p->optionService->SetPreferenceFloat("mhudKeysOffsetX", MHUD_DEF_KEYS_OFFSET_X);
			p->optionService->SetPreferenceFloat("mhudKeysOffsetY", MHUD_DEF_KEYS_OFFSET_Y);
			p->optionService->SetPreferenceFloat("mhudKeysScale", MHUD_DEF_KEYS_SCALE);
			p->optionService->SetPreferenceInt("mhudKeysColor", PackColor(MHUD_DEF_BASE_COLOR));
			p->optionService->SetPreferenceInt("mhudKeysOverlapColor", PackColor(MHUD_DEF_KEYS_OVERLAP_COLOR));
			break;
		case MHUDElement::CpTp:
			p->optionService->SetPreferenceBool("hudCpTp", true);
			break;
	}
}

void KZHUDService::PrintHUDSummary()
{
	auto *p    = this->player;
	auto *opts = p->optionService;
	auto *lang = p->languageService;
	// clang-format off
	lang->PrintChat(true, false, opts->GetPreferenceBool("hudSpeed",       true) ? "MHUD - Speed Enabled"        : "MHUD - Speed Disabled");
	lang->PrintChat(true, false, opts->GetPreferenceBool("hudPrespeed",    true) ? "MHUD - Prespeed Enabled"     : "MHUD - Prespeed Disabled");
	lang->PrintChat(true, false, opts->GetPreferenceBool("hudTimer",       true) ? "MHUD - Timer Enabled"        : "MHUD - Timer Disabled");
	lang->PrintChat(true, false, opts->GetPreferenceBool("hudTimerDetail", true) ? "MHUD - Timer Detail Enabled" : "MHUD - Timer Detail Disabled");
	lang->PrintChat(true, false, opts->GetPreferenceBool("hudKeys",        true) ? "MHUD - Keys Enabled"         : "MHUD - Keys Disabled");
	lang->PrintChat(true, false, opts->GetPreferenceBool("hudKeysOverlap", true) ? "MHUD - Keys Overlap Enabled" : "MHUD - Keys Overlap Disabled");
	lang->PrintChat(true, false, opts->GetPreferenceBool("hudCpTp",        true) ? "MHUD - CP/TP Enabled"        : "MHUD - CP/TP Disabled");
	lang->PrintChat(true, false, opts->GetPreferenceBool("hudOutline",     true) ? "MHUD - Outline Enabled"      : "MHUD - Outline Disabled");
	// clang-format on
}

// Алиас для обратной совместимости kz_mhud без аргументов.
void KZHUDService::OpenMHUDMenu()
{
	this->OpenHUDMenu();
}

// === Интерактивное меню kz_hud (cs2menus) ==========================================

struct HUDMenuToggle
{
	const char *label;
	const char *prefKey;
	bool defaultValue;
	const char *enabledKey;
	const char *disabledKey;
};

// Таблица per-element тумблеров.
static const HUDMenuToggle s_hudToggles[] = {
	{"HUD - Menu Label Speed",        "hudSpeed",       true,  "MHUD - Speed Enabled",         "MHUD - Speed Disabled"        },
	{"HUD - Menu Label Prespeed",     "hudPrespeed",    true,  "MHUD - Prespeed Enabled",      "MHUD - Prespeed Disabled"     },
	{"HUD - Menu Label Timer",        "hudTimer",       true,  "MHUD - Timer Enabled",         "MHUD - Timer Disabled"        },
	{"HUD - Menu Label TimerDetail",  "hudTimerDetail", true,  "MHUD - Timer Detail Enabled",  "MHUD - Timer Detail Disabled" },
	{"HUD - Menu Label Keys",         "hudKeys",        true,  "MHUD - Keys Enabled",          "MHUD - Keys Disabled"         },
	{"HUD - Menu Label KeysOverlap",  "hudKeysOverlap", true,  "MHUD - Keys Overlap Enabled",  "MHUD - Keys Overlap Disabled" },
	{"HUD - Menu Label CpTp",         "hudCpTp",        true,  "MHUD - CP/TP Enabled",         "MHUD - CP/TP Disabled"        },
	{"HUD - Menu Label Outline",      "hudOutline",     true,  "MHUD - Outline Enabled",       "MHUD - Outline Disabled"      },
	// showPos — строка координат HTML-панели (!showpos), не particle-элемент.
	{"HUD - Menu Label ShowPos",      "showPos",        false, "HUD Option - Show Pos - Enable", "HUD Option - Show Pos - Disable"},
};

// info-теги специальных пунктов (тип худа, HTML-панель, компактный режим, шрифт).
static constexpr const char *HUD_MENU_TYPE_TAG = "__hudType__";
static constexpr const char *HUD_MENU_PANEL_TAG = "__showPanel__";
static constexpr const char *HUD_MENU_COMPACT_TAG = "__compactPanel__";
static constexpr const char *HUD_MENU_FONT_TAG = "__mhudFont__";

// Обновить текст тумблер-пункта «<подпись>: On/Off».
static_function void SetHUDToggleItemText(MenuHandle menu, int item, const char *lang, const char *labelKey, bool on)
{
	std::string elemLabel = KZLanguageService::PrepareMessageWithLang(lang, labelKey);
	std::string stateStr = KZLanguageService::PrepareMessageWithLang(lang, on ? "HUD - Menu On" : "HUD - Menu Off");
	char newText[128];
	V_snprintf(newText, sizeof(newText), "%s: %s", elemLabel.c_str(), stateStr.c_str());
	g_pMenus->SetItemText(menu, item, newText);
}

// Колбэк выбора пункта меню !hud.
static_function void OnHUDMenuSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!p)
	{
		return;
	}
	const char *key = g_pMenus->GetItemInfo(menu, item);
	if (!key || !key[0])
	{
		return;
	}

	const char *lang = p->languageService->GetLanguage();

	// HTML-панель: TogglePanel держит кэш showPanel в синхроне с префом.
	if (KZ_STREQ(key, HUD_MENU_PANEL_TAG))
	{
		p->hudService->TogglePanel();
		SetHUDToggleItemText(menu, item, lang, "HUD - Menu Label Panel", p->hudService->IsShowingPanel());
		return;
	}

	if (KZ_STREQ(key, HUD_MENU_COMPACT_TAG))
	{
		p->hudService->ToggleCompactPanel();
		SetHUDToggleItemText(menu, item, lang, "HUD - Menu Label CompactPanel", p->hudService->IsCompactPanel());
		return;
	}

	// Шрифт particle-MHUD — цикл по доступным (lato/verdana).
	if (KZ_STREQ(key, HUD_MENU_FONT_TAG))
	{
		char fontBuf[32];
		GetUpstreamFont(p, fontBuf, sizeof(fontBuf));
		i32 idx = 0;
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(UPSTREAM_AVAILABLE_FONTS); i++)
		{
			if (KZ_STREQ(fontBuf, UPSTREAM_AVAILABLE_FONTS[i]))
			{
				idx = i;
				break;
			}
		}
		const char *next = UPSTREAM_AVAILABLE_FONTS[(idx + 1) % KZ_ARRAYSIZE(UPSTREAM_AVAILABLE_FONTS)];
		p->optionService->SetPreferenceStr("mhudFont", next);
		// Смена шрифта меняет .vpcf-пути → пересоздать particle'ы.
		p->hudService->DestroyAllParticles();
		std::string label = KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label Font");
		char newText[128];
		V_snprintf(newText, sizeof(newText), "%s: %s", label.c_str(), next);
		g_pMenus->SetItemText(menu, item, newText);
		return;
	}

	// Переключение hudType (Standard ↔ MHUD).
	if (KZ_STREQ(key, HUD_MENU_TYPE_TAG))
	{
		int current = p->hudService->GetHudType();
		int next = (current == 0) ? 1 : 0;
		p->hudService->SetHudType(next);
		if (next == 1 && !KZHUDService::IsMHUDAvailable())
		{
			p->languageService->PrintChat(true, false, "MHUD - Unavailable");
		}
		const char *typePhrase = (next == 0) ? "HUD - Menu Type Standard" : "HUD - Menu Type MHUD";
		std::string typeName = KZLanguageService::PrepareMessageWithLang(lang, typePhrase);
		std::string typeLabel = KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label Type", typeName.c_str());
		g_pMenus->SetItemText(menu, item, typeLabel.c_str());
		return;
	}

	// Per-element тумблеры.
	for (const auto &t : s_hudToggles)
	{
		if (KZ_STREQ(key, t.prefKey))
		{
			MHUDToggle(p, t.prefKey, t.defaultValue, t.enabledKey, t.disabledKey);
			bool nowOn = p->optionService->GetPreferenceBool(t.prefKey, t.defaultValue);
			// Outline: при смене пересоздаём particle'ы (vpcf-путь: plain ↔ glow).
			if (KZ_STREQ(t.prefKey, "hudOutline"))
			{
				p->hudService->DestroyAllParticles();
			}
			SetHUDToggleItemText(menu, item, lang, t.label, nowOn);
			return;
		}
	}
}

// === Подменю «Внешний вид MHUD» (регулируемые A/D-строки, интерфейс cs2menus 004) =====

// Диапазоны/шаги регулировки. Шаг приходит в колбэк как ±delta, клэмп — по min/max.
#define MHUD_AP_OFFSET_STEP 0.5f
#define MHUD_AP_OFFSET_MIN  (-50.0f)
#define MHUD_AP_OFFSET_MAX  50.0f
#define MHUD_AP_SCALE_STEP  0.005f
#define MHUD_AP_SCALE_MIN   0.005f
#define MHUD_AP_SCALE_MAX   0.5f

// Палитра цвета — тот же набор, что понимает utils::ParseColorName (и option-меню paint).
static_global constexpr const char *s_mhudColors[] = {"red", "white", "black", "blue", "brown", "green", "yellow", "purple"};

// Элемент MHUD, у которого настраивается позиция/размер/цвет. Порядок массива = порядок
// пунктов «Внешнего вида» и индексы 1.. в s_hudAppearanceMenu[slot].
struct MHUDAppearanceElement
{
	const char *titleKey; // фраза-заголовок подменю элемента
	const char *offXPref;
	f32 defOffX;
	const char *offYPref;
	f32 defOffY;
	const char *scalePref;
	f32 defScale;
	const char *colorPref;
	Color defColor;
};

// clang-format off
static_global const MHUDAppearanceElement s_apElements[] = {
	{"HUD - Menu Label Speed",    "mhudSpeedOffsetX",    MHUD_DEF_SPEED_OFFSET_X,    "mhudSpeedOffsetY",    MHUD_DEF_SPEED_OFFSET_Y,
	 "mhudSpeedScale",    MHUD_DEF_SPEED_SCALE,    "mhudSpeedColor",    MHUD_DEF_BASE_COLOR    },
	{"HUD - Menu Label Prespeed", "mhudPrespeedOffsetX", MHUD_DEF_PRESPEED_OFFSET_X, "mhudPrespeedOffsetY", MHUD_DEF_PRESPEED_OFFSET_Y,
	 "mhudPrespeedScale", MHUD_DEF_PRESPEED_SCALE, "mhudPrespeedColor", MHUD_DEF_BASE_COLOR    },
	{"HUD - Menu Label Timer",    "mhudTimerOffsetX",    MHUD_DEF_TIMER_OFFSET_X,    "mhudTimerOffsetY",    MHUD_DEF_TIMER_OFFSET_Y,
	 "mhudTimerScale",    MHUD_DEF_TIMER_SCALE,    "mhudTimerTpColor",  MHUD_DEF_TIMER_TP_COLOR},
	{"HUD - Menu Label Keys",     "mhudKeysOffsetX",     MHUD_DEF_KEYS_OFFSET_X,     "mhudKeysOffsetY",     MHUD_DEF_KEYS_OFFSET_Y,
	 "mhudKeysScale",     MHUD_DEF_KEYS_SCALE,     "mhudKeysColor",     MHUD_DEF_BASE_COLOR    },
};
// clang-format on

// Позиции строк в подменю элемента (фиксированный порядок построения).
enum
{
	MHUD_AP_ROW_POSX = 0,
	MHUD_AP_ROW_POSY,
	MHUD_AP_ROW_SIZE,
	MHUD_AP_ROW_COLOR,
	MHUD_AP_ROW_RESET,
};

// Поддерево «Внешний вид MHUD» на слот: [0] = корень «Внешний вид», [1..N] = подменю элементов
// (в порядке s_apElements). Уничтожается вместе с s_hudMenu при пересборке HUD-меню.
static_global MenuHandle s_hudAppearanceMenu[MAXPLAYERS + 1][1 + KZ_ARRAYSIZE(s_apElements)] = {};

// Текст регулируемой строки: «<подпись>: <значение>».
static_function void MHUDAppearanceRowText(const char *lang, const char *labelKey, const char *fmt, f32 value, char *out, int outSize)
{
	std::string label = KZLanguageService::PrepareMessageWithLang(lang, labelKey);
	char vbuf[32];
	V_snprintf(vbuf, sizeof(vbuf), fmt, value);
	V_snprintf(out, outSize, "%s: %s", label.c_str(), vbuf);
}

// Имя палитры для упакованного цвета (сравнение по RGB, альфа игнорируется); nullptr — цвет
// не из палитры (выставлен через !mhud ... color RGB).
static_function const char *MHUDColorName(i64 packed)
{
	Color cur = UnpackColor(packed);
	for (const char *name : s_mhudColors)
	{
		Color c;
		if (utils::ParseColorName(name, c) && c.r() == cur.r() && c.g() == cur.g() && c.b() == cur.b())
		{
			return name;
		}
	}
	return nullptr;
}

// Текст строки цвета: «<Цвет>: <имя|свой>».
static_function void MHUDColorRowText(const char *lang, i64 packed, char *out, int outSize)
{
	std::string label = KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label Color");
	const char *name = MHUDColorName(packed);
	if (name)
	{
		V_snprintf(out, outSize, "%s: %s", label.c_str(), name);
	}
	else
	{
		std::string custom = KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label ColorCustom");
		V_snprintf(out, outSize, "%s: %s", label.c_str(), custom.c_str());
	}
}

// Дескриптор регулируемого свойства по ключу префа (для A/D-колбэка): дефолт + подпись + формат.
static_function bool MHUDFindAdjustProp(const char *prefKey, f32 &defOut, const char *&labelKeyOut, const char *&fmtOut)
{
	for (const auto &e : s_apElements)
	{
		if (KZ_STREQ(prefKey, e.offXPref))
		{
			defOut = e.defOffX;
			labelKeyOut = "HUD - Menu Label PosX";
			fmtOut = "%.1f";
			return true;
		}
		if (KZ_STREQ(prefKey, e.offYPref))
		{
			defOut = e.defOffY;
			labelKeyOut = "HUD - Menu Label PosY";
			fmtOut = "%.1f";
			return true;
		}
		if (KZ_STREQ(prefKey, e.scalePref))
		{
			defOut = e.defScale;
			labelKeyOut = "HUD - Menu Label Size";
			fmtOut = "%.3f";
			return true;
		}
	}
	return false;
}

static_function const MHUDAppearanceElement *MHUDFindElementByColorPref(const char *colorPref)
{
	for (const auto &e : s_apElements)
	{
		if (KZ_STREQ(colorPref, e.colorPref))
		{
			return &e;
		}
	}
	return nullptr;
}

// A/D по регулируемой строке (позиция/размер): применить ±delta к префу, клэмп по [min,max],
// обновить текст строки. Преф читается рендером каждый тик — на худе видно вживую.
static_function void OnMHUDAppearanceAdjust(MenuHandle menu, int slot, int item, f32 delta, f32 minValue, f32 maxValue)
{
	KZPlayer *p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!p)
	{
		return;
	}
	const char *pref = g_pMenus->GetItemInfo(menu, item);
	if (!pref || !pref[0])
	{
		return;
	}
	f32 def = 0.0f;
	const char *labelKey = nullptr;
	const char *fmt = "%.2f";
	if (!MHUDFindAdjustProp(pref, def, labelKey, fmt))
	{
		return;
	}
	f32 next = (f32)p->optionService->GetPreferenceFloat(pref, def) + delta;
	if (next < minValue)
	{
		next = minValue;
	}
	if (next > maxValue)
	{
		next = maxValue;
	}
	p->optionService->SetPreferenceFloat(pref, next);

	char text[128];
	MHUDAppearanceRowText(p->languageService->GetLanguage(), labelKey, fmt, next, text, sizeof(text));
	g_pMenus->SetItemText(menu, item, text);
}

// E по строке цвета (цикл палитры) или «Сброс» (дефолты элемента). Регулируемые строки
// (позиция/размер) селект игнорируют — их тег не совпадает ни с «reset:», ни с цвет-префом.
static_function void OnMHUDAppearanceSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!p)
	{
		return;
	}
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (!tag || !tag[0])
	{
		return;
	}
	const char *lang = p->languageService->GetLanguage();

	// «Сброс»: тег «reset:<индекс элемента>». Возвращаем к дефолтам ТОЛЬКО то, что настраивает
	// это подменю (позиция/размер/цвет) — тумблер элемента и вспомогательные цвета не трогаем
	// (иначе текст тумблера в родительском HUD-меню разъехался бы: R его не перестраивает).
	if (KZ_STREQLEN(tag, "reset:", 6))
	{
		int idx = atoi(tag + 6);
		if (idx < 0 || idx >= (int)KZ_ARRAYSIZE(s_apElements))
		{
			return;
		}
		const MHUDAppearanceElement &e = s_apElements[idx];
		auto *opts = p->optionService;
		opts->SetPreferenceFloat(e.offXPref, e.defOffX);
		opts->SetPreferenceFloat(e.offYPref, e.defOffY);
		opts->SetPreferenceFloat(e.scalePref, e.defScale);
		opts->SetPreferenceInt(e.colorPref, PackColor(e.defColor));
		char text[128];
		MHUDAppearanceRowText(lang, "HUD - Menu Label PosX", "%.1f", e.defOffX, text, sizeof(text));
		g_pMenus->SetItemText(menu, MHUD_AP_ROW_POSX, text);
		MHUDAppearanceRowText(lang, "HUD - Menu Label PosY", "%.1f", e.defOffY, text, sizeof(text));
		g_pMenus->SetItemText(menu, MHUD_AP_ROW_POSY, text);
		MHUDAppearanceRowText(lang, "HUD - Menu Label Size", "%.3f", e.defScale, text, sizeof(text));
		g_pMenus->SetItemText(menu, MHUD_AP_ROW_SIZE, text);
		MHUDColorRowText(lang, PackColor(e.defColor), text, sizeof(text));
		g_pMenus->SetItemText(menu, MHUD_AP_ROW_COLOR, text);
		return;
	}

	// Иначе тег = ключ цвет-префа: цикл по палитре (E).
	const MHUDAppearanceElement *e = MHUDFindElementByColorPref(tag);
	if (!e)
	{
		return;
	}
	i64 packed = p->optionService->GetPreferenceInt(e->colorPref, PackColor(e->defColor));
	const char *curName = MHUDColorName(packed);
	int idx = -1; // -1 → следующий = первый цвет палитры (текущий не из палитры)
	for (int i = 0; i < (int)KZ_ARRAYSIZE(s_mhudColors); i++)
	{
		if (curName && KZ_STREQ(curName, s_mhudColors[i]))
		{
			idx = i;
			break;
		}
	}
	const char *nextName = s_mhudColors[(idx + 1) % (int)KZ_ARRAYSIZE(s_mhudColors)];
	Color c;
	utils::ParseColorName(nextName, c);
	i64 nextPacked = PackColor(c);
	p->optionService->SetPreferenceInt(e->colorPref, nextPacked);

	char text[128];
	MHUDColorRowText(lang, nextPacked, text, sizeof(text));
	g_pMenus->SetItemText(menu, item, text);
}

// Подменю одного элемента: позиция X/Y + размер (регулируемые A/D) + цвет (E) + сброс.
static_function MenuHandle BuildMHUDElementMenu(KZPlayer *player, const MHUDAppearanceElement &e)
{
	const char *lang = player->languageService->GetLanguage();
	std::string title = KZLanguageService::PrepareMessageWithLang(lang, e.titleKey);
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), &OnMHUDAppearanceSelect);
	if (m == kInvalidMenuHandle)
	{
		return m;
	}
	auto *opts = player->optionService;
	char text[128];

	MHUDAppearanceRowText(lang, "HUD - Menu Label PosX", "%.1f", (f32)opts->GetPreferenceFloat(e.offXPref, e.defOffX), text, sizeof(text));
	g_pMenus->AddAdjustableItem(m, text, e.offXPref, MHUD_AP_OFFSET_STEP, MHUD_AP_OFFSET_MIN, MHUD_AP_OFFSET_MAX);
	MHUDAppearanceRowText(lang, "HUD - Menu Label PosY", "%.1f", (f32)opts->GetPreferenceFloat(e.offYPref, e.defOffY), text, sizeof(text));
	g_pMenus->AddAdjustableItem(m, text, e.offYPref, MHUD_AP_OFFSET_STEP, MHUD_AP_OFFSET_MIN, MHUD_AP_OFFSET_MAX);
	MHUDAppearanceRowText(lang, "HUD - Menu Label Size", "%.3f", (f32)opts->GetPreferenceFloat(e.scalePref, e.defScale), text, sizeof(text));
	g_pMenus->AddAdjustableItem(m, text, e.scalePref, MHUD_AP_SCALE_STEP, MHUD_AP_SCALE_MIN, MHUD_AP_SCALE_MAX);
	// Цвет — обычный пункт (E циклит палитру), не регулируемый.
	MHUDColorRowText(lang, opts->GetPreferenceInt(e.colorPref, PackColor(e.defColor)), text, sizeof(text));
	g_pMenus->AddItem(m, text, e.colorPref, false);
	// Сброс — тег с индексом элемента.
	{
		std::string resetLabel = KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label Reset");
		int idx = (int)(&e - s_apElements);
		char resetTag[16];
		V_snprintf(resetTag, sizeof(resetTag), "reset:%d", idx);
		g_pMenus->AddItem(m, resetLabel.c_str(), resetTag, false);
	}

	g_pMenus->SetAdjustCallback(m, &OnMHUDAppearanceAdjust);
	// Пункт «Назад» не добавляем (назад = бинд R). Не закрываем при выборе — текст вживую.
	g_pMenus->SetCloseOnSelect(m, false);
	return m;
}

static_function void DestroyMHUDAppearanceMenus(int slot)
{
	for (auto &h : s_hudAppearanceMenu[slot])
	{
		if (h != kInvalidMenuHandle)
		{
			g_pMenus->DestroyMenu(h);
			h = kInvalidMenuHandle;
		}
	}
}

// Построить поддерево «Внешний вид MHUD» и вернуть корень (kInvalidMenuHandle при неудаче).
// Хэндлы (корень + элементы) складываются в s_hudAppearanceMenu[slot] для уничтожения при пересборке.
static_function MenuHandle BuildMHUDAppearanceMenu(KZPlayer *player, int slot)
{
	const char *lang = player->languageService->GetLanguage();
	std::string title = KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label Appearance");
	// У корня «Внешний вид» только submenu-навигация по элементам — свой select-колбэк не нужен.
	MenuHandle root = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), nullptr);
	if (root == kInvalidMenuHandle)
	{
		return kInvalidMenuHandle;
	}
	// Корень сохраняем ДО постройки детей: AddSubMenu ниже свяжет их parent с ним (R = назад).
	s_hudAppearanceMenu[slot][0] = root;
	for (int i = 0; i < (int)KZ_ARRAYSIZE(s_apElements); i++)
	{
		MenuHandle child = BuildMHUDElementMenu(player, s_apElements[i]);
		s_hudAppearanceMenu[slot][1 + i] = child;
		if (child != kInvalidMenuHandle)
		{
			std::string label = KZLanguageService::PrepareMessageWithLang(lang, s_apElements[i].titleKey);
			g_pMenus->AddSubMenu(root, label.c_str(), child, "");
		}
	}
	g_pMenus->SetCloseOnSelect(root, false);
	return root;
}

// Один хэндл HUD-меню на слот — пересоздаётся при каждом построении
// (и из !hud, и из подменю !options — экземпляр всегда один).
static_global MenuHandle s_hudMenu[MAXPLAYERS + 1] = {};

u32 KZHUDService::CreateHUDMenu()
{
	if (g_pMenus == nullptr)
	{
		return kInvalidMenuHandle;
	}

	int slot = this->player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return kInvalidMenuHandle;
	}

	if (s_hudMenu[slot] != kInvalidMenuHandle)
	{
		g_pMenus->DestroyMenu(s_hudMenu[slot]);
		s_hudMenu[slot] = kInvalidMenuHandle;
	}
	// Старое поддерево «Внешний вид MHUD» — уничтожаем вместе с корнем HUD-меню.
	DestroyMHUDAppearanceMenus(slot);

	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, "HUD", &OnHUDMenuSelect);
	if (m == kInvalidMenuHandle)
	{
		return kInvalidMenuHandle;
	}

	auto *opts = this->MHUDSettingsSource()->optionService;
	const char *lang = this->player->languageService->GetLanguage();

	auto addToggle = [&](const char *labelKey, const char *tag, bool on)
	{
		std::string elemLabel = KZLanguageService::PrepareMessageWithLang(lang, labelKey);
		std::string stateStr = KZLanguageService::PrepareMessageWithLang(lang, on ? "HUD - Menu On" : "HUD - Menu Off");
		char text[128];
		V_snprintf(text, sizeof(text), "%s: %s", elemLabel.c_str(), stateStr.c_str());
		g_pMenus->AddItem(m, text, tag, false);
	};

	// Тип худа (int-pref).
	int hudType = this->GetHudType();
	const char *typePhrase = (hudType == 0) ? "HUD - Menu Type Standard" : "HUD - Menu Type MHUD";
	std::string typeName = KZLanguageService::PrepareMessageWithLang(lang, typePhrase);
	std::string typeText = KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label Type", typeName.c_str());
	g_pMenus->AddItem(m, typeText.c_str(), HUD_MENU_TYPE_TAG, false);

	// HTML-панель и компактный режим.
	addToggle("HUD - Menu Label Panel", HUD_MENU_PANEL_TAG, this->IsShowingPanel());
	addToggle("HUD - Menu Label CompactPanel", HUD_MENU_COMPACT_TAG, this->IsCompactPanel());

	// Шрифт particle-MHUD.
	{
		char fontBuf[32];
		GetUpstreamFont(this->player, fontBuf, sizeof(fontBuf));
		std::string fontLabel = KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label Font");
		char text[128];
		V_snprintf(text, sizeof(text), "%s: %s", fontLabel.c_str(), fontBuf);
		g_pMenus->AddItem(m, text, HUD_MENU_FONT_TAG, false);
	}

	// Per-element тумблеры.
	for (const auto &t : s_hudToggles)
	{
		addToggle(t.label, t.prefKey, opts->GetPreferenceBool(t.prefKey, t.defaultValue));
	}

	// Подменю тонкой настройки внешнего вида particle-элементов (позиция/размер/цвет).
	MenuHandle appearance = BuildMHUDAppearanceMenu(this->player, slot);
	if (appearance != kInvalidMenuHandle)
	{
		std::string apLabel = KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label Appearance");
		g_pMenus->AddSubMenu(m, apLabel.c_str(), appearance, "");
	}

	// Не закрываем при выборе — текст пункта обновляется вживую.
	g_pMenus->SetCloseOnSelect(m, false);

	s_hudMenu[slot] = m;
	return m;
}

void KZHUDService::OpenHUDMenu()
{
	// Меню-движок не загружен → текстовая сводка.
	if (g_pMenus == nullptr)
	{
		this->PrintHUDSummary();
		return;
	}

	MenuHandle m = (MenuHandle)this->CreateHUDMenu();
	if (m == kInvalidMenuHandle)
	{
		this->PrintHUDSummary();
		return;
	}
	g_pMenus->DisplayMenu(m, this->player->GetPlayerSlot().Get(), 0);
}

// Hierarchy:
//   kz_hud                                   → интерактивное меню (фолбэк: сводка)
//   kz_hud speed / prespeed / timer / keys / cptp / outline → toggle per-element
//   kz_hud <element> offset|scale|color|...  → тонкая настройка
//
//   kz_mhud                                  → алиас kz_hud (обратная совместимость)

// Общая логика субкоманд, разделённая между kz_hud и kz_mhud.
static META_RES HandleHUDSubcmd(KZPlayer *player, const CCommand *args)
{
	bool mhudAvail  = KZHUDService::IsMHUDAvailable();
	const char *element = args->Arg(1);
	const char *prop    = args->ArgC() >= 3 ? args->Arg(2) : nullptr;

	if (KZ_STREQI(element, "cptp"))
	{
		MHUDToggle(player, "hudCpTp", true, "MHUD - CP/TP Enabled", "MHUD - CP/TP Disabled");
	}
	else if (KZ_STREQI(element, "speed"))
	{
		if (!prop)
		{
			MHUDToggle(player, "hudSpeed", true, "MHUD - Speed Enabled", "MHUD - Speed Disabled");
		}
		else if (KZ_STREQI(prop, "offset"))
		{
			if (!mhudAvail)
			{
				player->languageService->PrintChat(true, false, "MHUD - Unavailable");
				return MRES_SUPERCEDE;
			}
			MHUDSetOffset(player, args, 3, "mhudSpeedOffsetX", "mhudSpeedOffsetY", MHUD_DEF_SPEED_OFFSET_X, MHUD_DEF_SPEED_OFFSET_Y,
						  "MHUD - Speed Offset Usage", "MHUD - Speed Offset Set");
		}
		else if (KZ_STREQI(prop, "scale"))
		{
			if (!mhudAvail)
			{
				player->languageService->PrintChat(true, false, "MHUD - Unavailable");
				return MRES_SUPERCEDE;
			}
			MHUDSetScale(player, args, 3, "mhudSpeedScale", MHUD_DEF_SPEED_SCALE, "MHUD - Speed Scale Usage", "MHUD - Speed Scale Set");
		}
		else if (KZ_STREQI(prop, "color"))
		{
			SetColorPref(player, args, 3, "mhudSpeedColor", "MHUD - Speed Color Usage", "MHUD - Speed Color Set");
		}
		else if (KZ_STREQI(prop, "crouchjumpcolor") || KZ_STREQI(prop, "cjcolor"))
		{
			SetColorPref(player, args, 3, "mhudSpeedCjColor", "MHUD - Speed CJ Color Usage", "MHUD - Speed CJ Color Set");
		}
		else if (KZ_STREQI(prop, "reset"))
		{
			ResetElementPrefs(player, MHUDElement::Speed);
			player->languageService->PrintChat(true, false, "MHUD - Speed Reset");
		}
		else
		{
			player->languageService->PrintChat(true, false, "MHUD - Speed Usage");
		}
	}
	else if (KZ_STREQI(element, "prespeed"))
	{
		if (!prop)
		{
			MHUDToggle(player, "hudPrespeed", true, "MHUD - Prespeed Enabled", "MHUD - Prespeed Disabled");
		}
		else if (KZ_STREQI(prop, "offset"))
		{
			if (!mhudAvail)
			{
				player->languageService->PrintChat(true, false, "MHUD - Unavailable");
				return MRES_SUPERCEDE;
			}
			MHUDSetOffset(player, args, 3, "mhudPrespeedOffsetX", "mhudPrespeedOffsetY", MHUD_DEF_PRESPEED_OFFSET_X, MHUD_DEF_PRESPEED_OFFSET_Y,
						  "MHUD - Prespeed Offset Usage", "MHUD - Prespeed Offset Set");
		}
		else if (KZ_STREQI(prop, "scale"))
		{
			if (!mhudAvail)
			{
				player->languageService->PrintChat(true, false, "MHUD - Unavailable");
				return MRES_SUPERCEDE;
			}
			MHUDSetScale(player, args, 3, "mhudPrespeedScale", MHUD_DEF_PRESPEED_SCALE, "MHUD - Prespeed Scale Usage", "MHUD - Prespeed Scale Set");
		}
		else if (KZ_STREQI(prop, "color"))
		{
			SetColorPref(player, args, 3, "mhudPrespeedColor", "MHUD - Prespeed Color Usage", "MHUD - Prespeed Color Set");
		}
		else if (KZ_STREQI(prop, "perfcolor"))
		{
			SetColorPref(player, args, 3, "mhudPrespeedPerfColor", "MHUD - Prespeed Perf Color Usage", "MHUD - Prespeed Perf Color Set");
		}
		else if (KZ_STREQI(prop, "jumpbugcolor") || KZ_STREQI(prop, "jbcolor"))
		{
			SetColorPref(player, args, 3, "mhudPrespeedJumpbugColor", "MHUD - Prespeed Jumpbug Color Usage", "MHUD - Prespeed Jumpbug Color Set");
		}
		else if (KZ_STREQI(prop, "reset"))
		{
			ResetElementPrefs(player, MHUDElement::Prespeed);
			player->languageService->PrintChat(true, false, "MHUD - Prespeed Reset");
		}
		else
		{
			player->languageService->PrintChat(true, false, "MHUD - Prespeed Usage");
		}
	}
	else if (KZ_STREQI(element, "timer"))
	{
		if (!prop)
		{
			MHUDToggle(player, "hudTimer", true, "MHUD - Timer Enabled", "MHUD - Timer Disabled");
		}
		else if (KZ_STREQI(prop, "detail"))
		{
			MHUDToggle(player, "hudTimerDetail", true, "MHUD - Timer Detail Enabled", "MHUD - Timer Detail Disabled");
		}
		else if (KZ_STREQI(prop, "offset"))
		{
			if (!mhudAvail)
			{
				player->languageService->PrintChat(true, false, "MHUD - Unavailable");
				return MRES_SUPERCEDE;
			}
			MHUDSetOffset(player, args, 3, "mhudTimerOffsetX", "mhudTimerOffsetY", MHUD_DEF_TIMER_OFFSET_X, MHUD_DEF_TIMER_OFFSET_Y,
						  "MHUD - Timer Offset Usage", "MHUD - Timer Offset Set");
		}
		else if (KZ_STREQI(prop, "scale"))
		{
			if (!mhudAvail)
			{
				player->languageService->PrintChat(true, false, "MHUD - Unavailable");
				return MRES_SUPERCEDE;
			}
			MHUDSetScale(player, args, 3, "mhudTimerScale", MHUD_DEF_TIMER_SCALE, "MHUD - Timer Scale Usage", "MHUD - Timer Scale Set");
		}
		else if (KZ_STREQI(prop, "tpcolor"))
		{
			SetColorPref(player, args, 3, "mhudTimerTpColor", "MHUD - Timer TP Color Usage", "MHUD - Timer TP Color Set");
		}
		else if (KZ_STREQI(prop, "procolor"))
		{
			SetColorPref(player, args, 3, "mhudTimerProColor", "MHUD - Timer Pro Color Usage", "MHUD - Timer Pro Color Set");
		}
		else if (KZ_STREQI(prop, "pausedcolor"))
		{
			SetColorPref(player, args, 3, "mhudTimerPausedColor", "MHUD - Timer Paused Color Usage", "MHUD - Timer Paused Color Set");
		}
		else if (KZ_STREQI(prop, "stoppedcolor"))
		{
			SetColorPref(player, args, 3, "mhudTimerStoppedColor", "MHUD - Timer Stopped Color Usage", "MHUD - Timer Stopped Color Set");
		}
		else if (KZ_STREQI(prop, "reset"))
		{
			ResetElementPrefs(player, MHUDElement::Timer);
			player->languageService->PrintChat(true, false, "MHUD - Timer Reset");
		}
		else
		{
			player->languageService->PrintChat(true, false, "MHUD - Timer Usage");
		}
	}
	else if (KZ_STREQI(element, "keys"))
	{
		if (!prop)
		{
			MHUDToggle(player, "hudKeys", true, "MHUD - Keys Enabled", "MHUD - Keys Disabled");
		}
		else if (KZ_STREQI(prop, "overlap"))
		{
			MHUDToggle(player, "hudKeysOverlap", true, "MHUD - Keys Overlap Enabled", "MHUD - Keys Overlap Disabled");
		}
		else if (KZ_STREQI(prop, "offset"))
		{
			if (!mhudAvail)
			{
				player->languageService->PrintChat(true, false, "MHUD - Unavailable");
				return MRES_SUPERCEDE;
			}
			MHUDSetOffset(player, args, 3, "mhudKeysOffsetX", "mhudKeysOffsetY", MHUD_DEF_KEYS_OFFSET_X, MHUD_DEF_KEYS_OFFSET_Y,
						  "MHUD - Keys Offset Usage", "MHUD - Keys Offset Set");
		}
		else if (KZ_STREQI(prop, "scale"))
		{
			if (!mhudAvail)
			{
				player->languageService->PrintChat(true, false, "MHUD - Unavailable");
				return MRES_SUPERCEDE;
			}
			MHUDSetScale(player, args, 3, "mhudKeysScale", MHUD_DEF_KEYS_SCALE, "MHUD - Keys Scale Usage", "MHUD - Keys Scale Set");
		}
		else if (KZ_STREQI(prop, "color"))
		{
			SetColorPref(player, args, 3, "mhudKeysColor", "MHUD - Keys Color Usage", "MHUD - Keys Color Set");
		}
		else if (KZ_STREQI(prop, "overlapcolor"))
		{
			SetColorPref(player, args, 3, "mhudKeysOverlapColor", "MHUD - Keys Overlap Color Usage", "MHUD - Keys Overlap Color Set");
		}
		else if (KZ_STREQI(prop, "reset"))
		{
			ResetElementPrefs(player, MHUDElement::Keys);
			player->languageService->PrintChat(true, false, "MHUD - Keys Reset");
		}
		else
		{
			player->languageService->PrintChat(true, false, "MHUD - Keys Usage");
		}
	}
	else if (KZ_STREQI(element, "outline"))
	{
		// Смена outline меняет .vpcf (plain↔glow) → нужно пересоздать particle'ы.
		bool next = !player->optionService->GetPreferenceBool("hudOutline", true);
		player->optionService->SetPreferenceBool("hudOutline", next);
		player->hudService->DestroyAllParticles();
		player->languageService->PrintChat(true, false, next ? "MHUD - Outline Enabled" : "MHUD - Outline Disabled");
	}
	else if (KZ_STREQI(element, "font"))
	{
		char cur[32];
		GetUpstreamFont(player, cur, sizeof(cur));
		char requested[32] = "";
		if (prop)
		{
			V_strncpy(requested, prop, sizeof(requested));
			V_strlower(requested);
		}
		bool valid = false;
		for (const char *f : UPSTREAM_AVAILABLE_FONTS)
		{
			if (KZ_STREQ(requested, f))
			{
				valid = true;
				break;
			}
		}
		if (!valid)
		{
			player->languageService->PrintChat(true, false, "MHUD - Font Usage", cur);
			return MRES_SUPERCEDE;
		}
		player->optionService->SetPreferenceStr("mhudFont", requested);
		// Смена шрифта меняет .vpcf-пути → пересоздать particle'ы.
		player->hudService->DestroyAllParticles();
		player->languageService->PrintChat(true, false, "MHUD - Font Set", requested);
	}
	else
	{
		player->languageService->PrintChat(true, false, "MHUD - Usage");
	}

	return MRES_SUPERCEDE;
}

SCMD(kz_hud, SCFL_HUD | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (args->ArgC() < 2)
	{
		player->hudService->OpenHUDMenu();
		return MRES_SUPERCEDE;
	}
	return HandleHUDSubcmd(player, args);
}

// kz_mhud — обратная совместимость: без аргументов открывает то же меню что и kz_hud.
SCMD(kz_mhud, SCFL_HUD | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (args->ArgC() < 2)
	{
		player->hudService->OpenMHUDMenu();
		return MRES_SUPERCEDE;
	}
	return HandleHUDSubcmd(player, args);
}
