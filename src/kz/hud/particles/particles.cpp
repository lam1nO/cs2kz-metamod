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

// Меню-движок cs2menus (определён в kz_hud.cpp); может быть nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

// === Particle asset paths (Path B — per-glyph из workshop-аддона 3759276798) =========
//
// Одна схема cyberkz, без font/outline суффиксов.
// Outline управляется выбором vpcf: velo_plain ↔ velo_glow.

#define PARTICLE_VELO_PATTERN       "particles/cyberkz/velo_%s.vpcf"
#define PARTICLE_TIMER_DIGIT_PATH   "particles/cyberkz/timer_digit.vpcf"
#define PARTICLE_TIMER_DELIM_PATH   "particles/cyberkz/timer_delim.vpcf"
#define PARTICLE_KEY_PATTERN        "particles/cyberkz/key_%s.vpcf"
#define PARTICLE_PILL_PATH          "particles/cyberkz/pill.vpcf"

// CP-TP использует те же листы что таймер
#define PARTICLE_CPTP_DIGIT_PATH    "particles/cyberkz/timer_digit.vpcf"
#define PARTICLE_CPTP_DELIM_PATH    "particles/cyberkz/timer_delim.vpcf"

// === Layout constants ================================================================
// Скорость: 4 разряда, шаг масштабируется со scale.
#define MHUD_SPEED_DIGIT_STEP     0.3125f
// Таймер: шаг между разрядами (двузначные пары).
#define MHUD_TIMER_DIGIT_STEP     1.6f
#define MHUD_TIMER_DELIM_COLON    0    // sequence колона в timer_delim
#define MHUD_TIMER_DELIM_DOT      1    // sequence точки
#define MHUD_TIMER_DELIM_SLASH    2    // sequence слэша (для CP/TP)
// CP/TP: шаг между цифрами
#define MHUD_CPTP_DIGIT_STEP      0.9f
// Клавиши: шаг внутри группы и зазор между WASD и JC
#define MHUD_KEYS_GAP             0.7f
#define MHUD_KEY_STEP             0.32f

// Имена клавиш в порядке keyParticles[]
static const char *const KEY_NAMES[6] = {"W", "A", "S", "D", "J", "C"};

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
#define MHUD_DEF_CPTP_OFFSET_X     0.0f
#define MHUD_DEF_CPTP_OFFSET_Y     -15.0f
#define MHUD_DEF_CPTP_SCALE        0.022f
#define MHUD_DEF_PILL_OFFSET_X     0.0f
#define MHUD_DEF_PILL_OFFSET_Y     -17.5f
#define MHUD_DEF_PILL_SCALE        0.06f

static_global const Color MHUD_DEF_BASE_COLOR(255, 255, 255, 255);
static_global const Color MHUD_DEF_PERF_COLOR(0x40, 0xFF, 0x40, 0xFF);
static_global const Color MHUD_DEF_JUMPBUG_COLOR(0xFF, 0xFF, 0x20, 0xFF);
static_global const Color MHUD_DEF_CJ_COLOR(0x71, 0xEE, 0xB8, 0xFF);
static_global const Color MHUD_DEF_TIMER_TP_COLOR(255, 255, 255, 255);
static_global const Color MHUD_DEF_TIMER_PRO_COLOR(0x5F, 0x99, 0xD9, 0xFF);
static_global const Color MHUD_DEF_TIMER_PAUSED_COLOR(0xFF, 0xFF, 0x00, 0xFF);
static_global const Color MHUD_DEF_TIMER_STOPPED_COLOR(0xFF, 0xA0, 0xA0, 0xFF);
static_global const Color MHUD_DEF_KEYS_OVERLAP_COLOR(0xFF, 0x40, 0x40, 0xFF);
// CP/TP — немного muted относительно таймера
static_global const Color MHUD_DEF_CPTP_COLOR(0x9A, 0xA3, 0xAF, 0xFF);

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

// Возвращает путь .vpcf для скорости: outline=true → glow (additive), false → plain.
static_function void BuildVeloPath(char *buf, size_t bufSize, bool outline)
{
	V_snprintf(buf, bufSize, PARTICLE_VELO_PATTERN, outline ? "glow" : "plain");
}

// Возвращает путь .vpcf для клавиши (W/A/S/D/J/C).
static_function void BuildKeyPath(char *buf, size_t bufSize, const char *keyName)
{
	V_snprintf(buf, bufSize, PARTICLE_KEY_PATTERN, keyName);
}

void KZHUDService::PrecacheParticles(IEntityResourceManifest *pResourceManifest)
{
	// Прекешируем все пути, которые могут понадобиться в рантайме.
	char path[256];

	// Скорость: два варианта (plain / glow)
	BuildVeloPath(path, sizeof(path), false);
	pResourceManifest->AddResource(path);
	BuildVeloPath(path, sizeof(path), true);
	pResourceManifest->AddResource(path);

	// Таймер и разделители
	pResourceManifest->AddResource(PARTICLE_TIMER_DIGIT_PATH);
	pResourceManifest->AddResource(PARTICLE_TIMER_DELIM_PATH);

	// Клавиши
	for (const char *key : KEY_NAMES)
	{
		BuildKeyPath(path, sizeof(path), key);
		pResourceManifest->AddResource(path);
	}

	// Пилюля
	pResourceManifest->AddResource(PARTICLE_PILL_PATH);
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
	// Скорость (4 разряда + 4 prespeed)
	for (i32 i = 0; i < MHUD_SPEED_DIGITS; i++)
	{
		if (handle == this->speedParticles[i] || handle == this->prespeedParticles[i])
		{
			return true;
		}
	}
	// Клавиши (6 штук)
	for (i32 i = 0; i < MHUD_KEY_COUNT; i++)
	{
		if (handle == this->keyParticles[i])
		{
			return true;
		}
	}
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
	// CP/TP
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->cptpParticles); i++)
	{
		if (handle == this->cptpParticles[i])
		{
			return true;
		}
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->cptpDelimParticles); i++)
	{
		if (handle == this->cptpDelimParticles[i])
		{
			return true;
		}
	}
	// Пилюля
	if (handle == this->pillParticle)
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
	for (i32 i = 0; i < MHUD_SPEED_DIGITS; i++)
	{
		DestroyHandle(this->speedParticles[i]);
		DestroyHandle(this->prespeedParticles[i]);
	}
	for (i32 i = 0; i < MHUD_KEY_COUNT; i++)
	{
		DestroyHandle(this->keyParticles[i]);
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerTextParticles); i++)
	{
		DestroyHandle(this->timerTextParticles[i]);
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerDelimiterParticles); i++)
	{
		DestroyHandle(this->timerDelimiterParticles[i]);
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->cptpParticles); i++)
	{
		DestroyHandle(this->cptpParticles[i]);
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->cptpDelimParticles); i++)
	{
		DestroyHandle(this->cptpDelimParticles[i]);
	}
	DestroyHandle(this->pillParticle);
}

void KZHUDService::OnClientDisconnect()
{
	this->DestroyAllParticles();
}

// === Preferences ====================================================================

// === Тип худа (hudType) ============================================================

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
bool KZHUDService::IsMHUDMasterEnabled()
{
	// Теперь мастер-тумблер = hudType==1.
	return this->GetHudType() == 1;
}

// Per-element тумблеры — общие для обоих типов худа (Стандартный и MHUD).
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

// === Speed + prespeed (Path B: до 4 отдельных particle'ов per значение) ==============

// Вычисляет позиции разрядов числа val (0..9999), записывает в out_x/out_y/out_seq.
// Ведущие нули не рисуем: numDigits = кол-во видимых разрядов (мин. 1).
// Разряды выровнены по центру baseOffsetX.
static_function i32 LayoutSpeedDigits(i32 val, f32 baseOffsetX, f32 baseOffsetY, f32 digit_step,
									   f32 out_x[KZHUDService::MHUD_SPEED_DIGITS], f32 out_y[KZHUDService::MHUD_SPEED_DIGITS],
									   f32 out_seq[KZHUDService::MHUD_SPEED_DIGITS])
{
	if (val < 0)    val = 0;
	if (val > 9999) val = 9999;

	// Разбиваем на цифры (4 разряда, старший первый)
	i32 digits[4];
	digits[0] = val / 1000;
	digits[1] = (val / 100) % 10;
	digits[2] = (val / 10) % 10;
	digits[3] = val % 10;

	// Считаем сколько значимых разрядов (без ведущих нулей, минимум 1)
	i32 numDigits = 1;
	if (digits[0] != 0)      numDigits = 4;
	else if (digits[1] != 0) numDigits = 3;
	else if (digits[2] != 0) numDigits = 2;

	i32 start = 4 - numDigits; // начинаем с digits[start]
	for (i32 i = 0; i < KZHUDService::MHUD_SPEED_DIGITS; i++)
	{
		if (i < numDigits)
		{
			// Центровка: slot = i - (numDigits-1)*0.5
			f32 slot = (f32)i - (numDigits - 1) * 0.5f;
			out_x[i] = baseOffsetX + slot * digit_step;
			out_y[i] = baseOffsetY;
			out_seq[i] = (f32)digits[start + i];
		}
		else
		{
			out_x[i]   = baseOffsetX;
			out_y[i]   = baseOffsetY;
			out_seq[i] = 0.0f;
		}
	}
	return numDigits;
}

void KZHUDService::UpdateMHUDSpeed()
{
	bool speedEnabled    = this->IsMHUDSpeedEnabled();
	bool prespeedEnabled = this->IsMHUDPrespeedEnabled();

	if (!speedEnabled)
	{
		for (i32 i = 0; i < MHUD_SPEED_DIGITS; i++)
		{
			if (this->speedParticles[i]) DestroyHandle(this->speedParticles[i]);
		}
	}
	if (!prespeedEnabled)
	{
		for (i32 i = 0; i < MHUD_SPEED_DIGITS; i++)
		{
			if (this->prespeedParticles[i]) DestroyHandle(this->prespeedParticles[i]);
		}
	}
	if (!speedEnabled && !prespeedEnabled)
	{
		return;
	}

	const Color baseColor        = this->GetMHUDColorPref("mhudSpeedColor", MHUD_DEF_BASE_COLOR);
	const Color prespeedBaseColor = this->GetMHUDColorPref("mhudPrespeedColor", MHUD_DEF_BASE_COLOR);
	const f32 speedOffsetX   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudSpeedOffsetX",   MHUD_DEF_SPEED_OFFSET_X);
	const f32 speedOffsetY   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudSpeedOffsetY",   MHUD_DEF_SPEED_OFFSET_Y);
	const f32 speedScale     = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudSpeedScale",     MHUD_DEF_SPEED_SCALE);
	const f32 prespeedOffsetX = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPrespeedOffsetX", MHUD_DEF_PRESPEED_OFFSET_X);
	const f32 prespeedOffsetY = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPrespeedOffsetY", MHUD_DEF_PRESPEED_OFFSET_Y);
	const f32 prespeedScale   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPrespeedScale",   MHUD_DEF_PRESPEED_SCALE);

	// Outline определяет vpcf (glow / plain). Смена outline → DestroyAllParticles в SCMD.
	bool outline = this->IsMHUDOutlineEnabled();
	char veloPath[256];
	BuildVeloPath(veloPath, sizeof(veloPath), outline);

	// Lazy-create: все 4 разряда создаются одним .vpcf, позиции проставляются ниже.
	if (speedEnabled)
	{
		for (i32 i = 0; i < MHUD_SPEED_DIGITS; i++)
		{
			if (!this->speedParticles[i])
			{
				this->speedParticles[i] = CreateMHUDParticle(veloPath, baseColor, 0.0f, speedScale, speedOffsetX, speedOffsetY);
			}
		}
	}
	if (prespeedEnabled)
	{
		for (i32 i = 0; i < MHUD_SPEED_DIGITS; i++)
		{
			if (!this->prespeedParticles[i])
			{
				this->prespeedParticles[i] = CreateMHUDParticle(veloPath, prespeedBaseColor, 0.0f, prespeedScale, prespeedOffsetX, prespeedOffsetY);
			}
		}
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

	const Color perfColor    = this->GetMHUDColorPref("mhudPrespeedPerfColor", MHUD_DEF_PERF_COLOR);
	const Color jumpbugColor = this->GetMHUDColorPref("mhudPrespeedJumpbugColor", MHUD_DEF_JUMPBUG_COLOR);
	const Color cjColor      = this->GetMHUDColorPref("mhudSpeedCjColor", MHUD_DEF_CJ_COLOR);
	bool perfing = src->IsPerfing() && !src->possibleLadderHop && !src->takeoffFromLadder;

	const Color speedColor    = useTakeoff && src->hudService->crouchJumping ? cjColor : baseColor;
	const Color prespeedColor = perfing ? (src->hudService->fromDuckbug ? jumpbugColor : perfColor) : prespeedBaseColor;

	for (i32 i = 0; i < MHUD_SPEED_DIGITS; i++)
	{
		SetParticleTint(this->speedParticles[i].Get(), speedColor);
		SetParticleTint(this->prespeedParticles[i].Get(), prespeedColor);
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

	// Шаг масштабируется вместе со scale, чтобы разряды не разъезжались при изменении размера.
	const f32 speedStep    = MHUD_SPEED_DIGIT_STEP * (speedScale / MHUD_DEF_SPEED_SCALE);
	const f32 prespeedStep = MHUD_SPEED_DIGIT_STEP * (prespeedScale / MHUD_DEF_PRESPEED_SCALE);

	// Скорость
	if (this->speedParticles[0])
	{
		i32 val = RoundFloatToInt(speed.Length2D());
		if (val < 0)    val = 0;
		if (val > 9999) val = 9999;

		f32 out_x[MHUD_SPEED_DIGITS], out_y[MHUD_SPEED_DIGITS], out_seq[MHUD_SPEED_DIGITS];
		i32 numDigits = LayoutSpeedDigits(val, speedOffsetX, speedOffsetY, speedStep, out_x, out_y, out_seq);

		for (i32 i = 0; i < MHUD_SPEED_DIGITS; i++)
		{
			CParticleSystem *p = this->speedParticles[i].Get();
			if (!p) continue;
			if (i < numDigits)
			{
				UpdateParticleLayout(p, out_seq[i], speedScale, out_x[i], out_y[i]);
				p->Start();
			}
			else
			{
				p->Destroy(); // скрываем незначащие разряды
			}
		}
	}

	// Preспeed
	if (this->prespeedParticles[0])
	{
		if (prespeed)
		{
			i32 val = RoundFloatToInt(prespeed->Length2D());
			if (val < 0)    val = 0;
			if (val > 9999) val = 9999;

			f32 out_x[MHUD_SPEED_DIGITS], out_y[MHUD_SPEED_DIGITS], out_seq[MHUD_SPEED_DIGITS];
			i32 numDigits = LayoutSpeedDigits(val, prespeedOffsetX, prespeedOffsetY, prespeedStep, out_x, out_y, out_seq);

			for (i32 i = 0; i < MHUD_SPEED_DIGITS; i++)
			{
				CParticleSystem *p = this->prespeedParticles[i].Get();
				if (!p) continue;
				if (i < numDigits)
				{
					UpdateParticleLayout(p, out_seq[i], prespeedScale, out_x[i], out_y[i]);
					p->Start();
				}
				else
				{
					p->Destroy();
				}
			}
		}
		else
		{
			for (i32 i = 0; i < MHUD_SPEED_DIGITS; i++)
			{
				CParticleSystem *p = this->prespeedParticles[i].Get();
				if (p) p->Destroy();
			}
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

	// Таймер и разделители: единый .vpcf (не зависит от font/outline)
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerTextParticles); i++)
	{
		if (!this->timerTextParticles[i])
		{
			this->timerTextParticles[i] = CreateMHUDParticle(PARTICLE_TIMER_DIGIT_PATH, tpColor, 0.0f, scale, 0.0f, offsetY);
		}
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->timerDelimiterParticles); i++)
	{
		if (!this->timerDelimiterParticles[i])
		{
			this->timerDelimiterParticles[i] = CreateMHUDParticle(PARTICLE_TIMER_DELIM_PATH, tpColor, 0.0f, scale, 0.0f, offsetY);
		}
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

	// Центрируем N видимых разрядов
	i32 numVisible = 0;
	for (i32 i = 0; i < 4; i++) numVisible += digitVisible[i] ? 1 : 0;
	f32 timerStep = MHUD_TIMER_DIGIT_STEP * (scale / MHUD_DEF_TIMER_SCALE);
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

// === Keys (Path B: 6 отдельных particle'ов, sequence=0/1) ==========================

void KZHUDService::CheckMHUDKeyParticles()
{
	bool enabled = this->IsMHUDKeysEnabled();
	if (!enabled)
	{
		for (i32 i = 0; i < MHUD_KEY_COUNT; i++)
		{
			if (this->keyParticles[i]) DestroyHandle(this->keyParticles[i]);
		}
		return;
	}

	const f32 offsetX = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysOffsetX", MHUD_DEF_KEYS_OFFSET_X);
	const f32 offsetY = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysOffsetY", MHUD_DEF_KEYS_OFFSET_Y);
	const f32 scale   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysScale",   MHUD_DEF_KEYS_SCALE);
	const Color color = this->GetMHUDColorPref("mhudKeysColor", MHUD_DEF_BASE_COLOR);

	for (i32 i = 0; i < MHUD_KEY_COUNT; i++)
	{
		if (!this->keyParticles[i])
		{
			char keyPath[256];
			BuildKeyPath(keyPath, sizeof(keyPath), KEY_NAMES[i]);
			this->keyParticles[i] = CreateMHUDParticle(keyPath, color, 0.0f, scale, offsetX, offsetY);
		}
	}
}

void KZHUDService::UpdateMHUDKeys()
{
	this->CheckMHUDKeyParticles();
	if (!this->keyParticles[0])
	{
		return;
	}

	KZPlayer *src = this->MHUDDataSource();

	// Маска нажатых кнопок
	bool pressed[MHUD_KEY_COUNT];
	pressed[0] = src->IsButtonPressed(IN_FORWARD);                                         // W
	pressed[1] = src->IsButtonPressed(IN_MOVELEFT);                                        // A
	pressed[2] = src->IsButtonPressed(IN_BACK);                                            // S
	pressed[3] = src->IsButtonPressed(IN_MOVERIGHT);                                       // D
	pressed[4] = src->hudService->jumpedThisTick || src->IsButtonPressed(IN_JUMP);         // J
	pressed[5] = src->IsButtonPressed(IN_DUCK);                                            // C

	const f32 offsetX = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysOffsetX", MHUD_DEF_KEYS_OFFSET_X);
	const f32 offsetY = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysOffsetY", MHUD_DEF_KEYS_OFFSET_Y);
	const f32 scale   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudKeysScale",   MHUD_DEF_KEYS_SCALE);
	const f32 keyStep = MHUD_KEY_STEP * (scale / MHUD_DEF_KEYS_SCALE);
	const f32 gap     = MHUD_KEYS_GAP * (scale / MHUD_DEF_KEYS_SCALE);

	// Раскладка блоком (не строкой): W над рядом ASD, J/C справа.
	//   W          J
	//  A S D       C
	// Ряд задаётся смещением по Y на один keyStep; знак rowUp зависит от того,
	// как .vpcf CenterYOffset (CP18.y) мапит экранную ось — проверить смоуком.
	const f32 rowUp   = -keyStep;                                       // «выше» на экране (см. знак offsetY)
	const f32 wasd_center_x = offsetX - (gap * 0.5f + keyStep);         // центр блока WASD (столбца S)
	const f32 jc_center_x   = offsetX + (gap * 0.5f + keyStep * 0.5f);  // центр блока JC

	f32 keyX[MHUD_KEY_COUNT];
	f32 keyY[MHUD_KEY_COUNT];
	// W — верхний ряд, по центру над S
	keyX[0] = wasd_center_x;            keyY[0] = offsetY + rowUp; // W
	// A S D — нижний ряд
	keyX[1] = wasd_center_x - keyStep;  keyY[1] = offsetY;         // A
	keyX[2] = wasd_center_x;            keyY[2] = offsetY;         // S
	keyX[3] = wasd_center_x + keyStep;  keyY[3] = offsetY;         // D
	// J — верхний ряд справа, C — под ним
	keyX[4] = jc_center_x;              keyY[4] = offsetY + rowUp; // J
	keyX[5] = jc_center_x;              keyY[5] = offsetY;         // C

	bool hasOverlap         = (pressed[0] && pressed[2]) || (pressed[1] && pressed[3]);
	const Color overlapColor = this->GetMHUDColorPref("mhudKeysOverlapColor", MHUD_DEF_KEYS_OVERLAP_COLOR);
	const Color baseColor    = this->GetMHUDColorPref("mhudKeysColor", MHUD_DEF_BASE_COLOR);

	for (i32 i = 0; i < MHUD_KEY_COUNT; i++)
	{
		CParticleSystem *p = this->keyParticles[i].Get();
		if (!p) continue;

		// sequence: 0=inactive, 1=active
		f32 seq = pressed[i] ? 1.0f : 0.0f;
		UpdateParticleLayout(p, seq, scale, keyX[i], keyY[i]);
		p->Start();

		// Overlap-цвет применяем только к WASD (индексы 0-3) при конфликте
		if (hasOverlap && this->IsMHUDKeysOverlapEnabled() && i < 4)
		{
			SetParticleTint(p, overlapColor);
		}
		else
		{
			SetParticleTint(p, baseColor);
		}
	}
}

// === CP/TP ==========================================================================
//
// Рисуем: "CP N / M  TP N" — два числа + разделитель для CP, одно число для TP.
//   cptpParticles[0]      = CP current (последняя цифра)
//   cptpParticles[1]      = CP total   (последняя цифра)
//   cptpParticles[2]      = TP count   (последняя цифра)
//   cptpDelimParticles[0] = "/" между cpCurrent и cpTotal (sequence = SLASH)
// Пилюля рисуется позади всего.

void KZHUDService::CheckMHUDCpTpParticles()
{
	bool enabled = this->IsMHUDCpTpEnabled() && this->IsMHUDAvailable();
	if (!enabled)
	{
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->cptpParticles); i++)
		{
			if (this->cptpParticles[i]) DestroyHandle(this->cptpParticles[i]);
		}
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->cptpDelimParticles); i++)
		{
			if (this->cptpDelimParticles[i]) DestroyHandle(this->cptpDelimParticles[i]);
		}
		if (this->pillParticle) DestroyHandle(this->pillParticle);
		return;
	}

	const Color color = this->GetMHUDColorPref("mhudCpTpColor", MHUD_DEF_CPTP_COLOR);
	const f32 scale   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudCpTpScale",   MHUD_DEF_CPTP_SCALE);
	const f32 offsetY = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudCpTpOffsetY", MHUD_DEF_CPTP_OFFSET_Y);

	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->cptpParticles); i++)
	{
		if (!this->cptpParticles[i])
		{
			this->cptpParticles[i] = CreateMHUDParticle(PARTICLE_CPTP_DIGIT_PATH, color, 0.0f, scale, 0.0f, offsetY);
		}
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->cptpDelimParticles); i++)
	{
		if (!this->cptpDelimParticles[i])
		{
			this->cptpDelimParticles[i] = CreateMHUDParticle(PARTICLE_CPTP_DELIM_PATH, color, 0.0f, scale, 0.0f, offsetY);
		}
	}

	// Пилюля
	if (!this->pillParticle)
	{
		const f32 pillOffsetX = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPillOffsetX", MHUD_DEF_PILL_OFFSET_X);
		const f32 pillOffsetY = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPillOffsetY", MHUD_DEF_PILL_OFFSET_Y);
		const f32 pillScale   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudPillScale",   MHUD_DEF_PILL_SCALE);
		const Color white(255, 255, 255, 255);
		this->pillParticle = CreateMHUDParticle(PARTICLE_PILL_PATH, white, 0.0f, pillScale, pillOffsetX, pillOffsetY);
	}
}

void KZHUDService::UpdateMHUDCpTp()
{
	this->CheckMHUDCpTpParticles();
	if (!this->cptpParticles[0])
	{
		return;
	}

	KZPlayer *src   = this->MHUDDataSource();
	i32 cpCurrent   = src->checkpointService->GetCurrentCpIndex() + 1; // 1-based
	i32 cpTotal     = src->checkpointService->GetCheckpointCount();
	i32 tpCount     = src->checkpointService->GetTeleportCount();

	const f32 offsetX = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudCpTpOffsetX", MHUD_DEF_CPTP_OFFSET_X);
	const f32 offsetY = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudCpTpOffsetY", MHUD_DEF_CPTP_OFFSET_Y);
	const f32 scale   = (f32)this->MHUDSettingsSource()->optionService->GetPreferenceFloat("mhudCpTpScale",   MHUD_DEF_CPTP_SCALE);
	const f32 step    = MHUD_CPTP_DIGIT_STEP * (scale / MHUD_DEF_CPTP_SCALE);

	bool hasCp = (cpTotal > 0);

	// CP current
	{
		CParticleSystem *p = this->cptpParticles[0].Get();
		if (p)
		{
			if (hasCp)
			{
				UpdateParticleLayout(p, (f32)(cpCurrent % 10), scale, offsetX - step * 1.5f, offsetY);
				p->Start();
			}
			else
			{
				p->Destroy();
			}
		}
	}
	// Разделитель /
	{
		CParticleSystem *p = this->cptpDelimParticles[0].Get();
		if (p)
		{
			if (hasCp)
			{
				UpdateParticleLayout(p, (f32)MHUD_TIMER_DELIM_SLASH, scale, offsetX - step * 0.5f, offsetY);
				p->Start();
			}
			else
			{
				p->Destroy();
			}
		}
	}
	// CP total
	{
		CParticleSystem *p = this->cptpParticles[1].Get();
		if (p)
		{
			if (hasCp)
			{
				UpdateParticleLayout(p, (f32)(cpTotal % 10), scale, offsetX + step * 0.5f, offsetY);
				p->Start();
			}
			else
			{
				p->Destroy();
			}
		}
	}
	// TP count — рисуем только если были телепорты (аналогично hasCp у CP-элементов)
	{
		CParticleSystem *p = this->cptpParticles[2].Get();
		if (p)
		{
			if (tpCount > 0)
			{
				UpdateParticleLayout(p, (f32)(tpCount % 10), scale, offsetX + step * 1.5f + step, offsetY);
				p->Start();
			}
			else
			{
				p->Destroy();
			}
		}
	}

	const Color color = this->GetMHUDColorPref("mhudCpTpColor", MHUD_DEF_CPTP_COLOR);
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->cptpParticles); i++)
	{
		SetParticleTint(this->cptpParticles[i].Get(), color);
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->cptpDelimParticles); i++)
	{
		SetParticleTint(this->cptpDelimParticles[i].Get(), color);
	}
}

// === UpdateParticles ================================================================

void KZHUDService::UpdateParticles(KZPlayer *source)
{
	// Данные читаются из mhudSource (наблюдаемый при спектировании),
	// настройки — всегда из this->player.
	this->mhudSource = (source && source != this->player) ? source : nullptr;
	this->UpdateMHUDSpeed();
	this->UpdateMHUDTimer();
	this->UpdateMHUDKeys();
	this->UpdateMHUDCpTp();
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
			p->optionService->SetPreferenceFloat("mhudCpTpOffsetX", MHUD_DEF_CPTP_OFFSET_X);
			p->optionService->SetPreferenceFloat("mhudCpTpOffsetY", MHUD_DEF_CPTP_OFFSET_Y);
			p->optionService->SetPreferenceFloat("mhudCpTpScale", MHUD_DEF_CPTP_SCALE);
			p->optionService->SetPreferenceInt("mhudCpTpColor", PackColor(MHUD_DEF_CPTP_COLOR));
			break;
	}
}

void KZHUDService::PrintHUDSummary()
{
	auto *p    = this->player;
	auto *opts = p->optionService;
	auto *lang = p->languageService;
	int hudType = this->GetHudType();
	// clang-format off
	lang->PrintChat(true, false, hudType == 1 ? "HUD - Type MHUD" : "HUD - Type Standard");
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

// Таблица per-element тумблеров (без первого пункта hudType — он особый, int-pref).
static const HUDMenuToggle s_hudToggles[] = {
	{"HUD - Menu Label Speed",        "hudSpeed",       true,  "MHUD - Speed Enabled",         "MHUD - Speed Disabled"        },
	{"HUD - Menu Label Prespeed",     "hudPrespeed",    true,  "MHUD - Prespeed Enabled",      "MHUD - Prespeed Disabled"     },
	{"HUD - Menu Label Timer",        "hudTimer",       true,  "MHUD - Timer Enabled",         "MHUD - Timer Disabled"        },
	{"HUD - Menu Label TimerDetail",  "hudTimerDetail", true,  "MHUD - Timer Detail Enabled",  "MHUD - Timer Detail Disabled" },
	{"HUD - Menu Label Keys",         "hudKeys",        true,  "MHUD - Keys Enabled",          "MHUD - Keys Disabled"         },
	{"HUD - Menu Label KeysOverlap",  "hudKeysOverlap", true,  "MHUD - Keys Overlap Enabled",  "MHUD - Keys Overlap Disabled" },
	{"HUD - Menu Label CpTp",         "hudCpTp",        true,  "MHUD - CP/TP Enabled",         "MHUD - CP/TP Disabled"        },
	{"HUD - Menu Label Outline",      "hudOutline",     true,  "MHUD - Outline Enabled",       "MHUD - Outline Disabled"      },
};

// info-тег специального первого пункта (тип худа).
static constexpr const char *HUD_MENU_TYPE_TAG = "__hudType__";

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

	// Первый пункт — переключение hudType.
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
			std::string elemLabel = KZLanguageService::PrepareMessageWithLang(lang, t.label);
			const char *statePhrase = nowOn ? "HUD - Menu On" : "HUD - Menu Off";
			std::string stateStr = KZLanguageService::PrepareMessageWithLang(lang, statePhrase);
			char newText[128];
			V_snprintf(newText, sizeof(newText), "%s: %s", elemLabel.c_str(), stateStr.c_str());
			g_pMenus->SetItemText(menu, item, newText);
			return;
		}
	}
}

void KZHUDService::OpenHUDMenu()
{
	// Меню-движок не загружен → текстовая сводка.
	if (g_pMenus == nullptr)
	{
		this->PrintHUDSummary();
		return;
	}

	int slot = this->player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return;
	}

	// Один хэндл на слот — пересоздаём при повторном вызове.
	static MenuHandle s_hudMenu[MAXPLAYERS + 1] = {};
	if (s_hudMenu[slot] != kInvalidMenuHandle)
	{
		g_pMenus->DestroyMenu(s_hudMenu[slot]);
		s_hudMenu[slot] = kInvalidMenuHandle;
	}

	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, "HUD", &OnHUDMenuSelect);
	if (m == kInvalidMenuHandle)
	{
		this->PrintHUDSummary();
		return;
	}

	auto *opts = this->MHUDSettingsSource()->optionService;
	const char *lang = this->player->languageService->GetLanguage();

	// Пункт 1: тип худа (int-pref).
	int hudType = this->GetHudType();
	const char *typePhrase = (hudType == 0) ? "HUD - Menu Type Standard" : "HUD - Menu Type MHUD";
	std::string typeName = KZLanguageService::PrepareMessageWithLang(lang, typePhrase);
	std::string typeText = KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label Type", typeName.c_str());
	g_pMenus->AddItem(m, typeText.c_str(), HUD_MENU_TYPE_TAG, false);

	// Пункты 2-9: per-element тумблеры.
	for (const auto &t : s_hudToggles)
	{
		bool on = opts->GetPreferenceBool(t.prefKey, t.defaultValue);
		std::string elemLabel = KZLanguageService::PrepareMessageWithLang(lang, t.label);
		const char *statePhrase = on ? "HUD - Menu On" : "HUD - Menu Off";
		std::string stateStr = KZLanguageService::PrepareMessageWithLang(lang, statePhrase);
		char text[128];
		V_snprintf(text, sizeof(text), "%s: %s", elemLabel.c_str(), stateStr.c_str());
		g_pMenus->AddItem(m, text, t.prefKey, false);
	}

	// Не закрываем при выборе — текст пункта обновляется вживую.
	g_pMenus->SetCloseOnSelect(m, false);

	s_hudMenu[slot] = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

// Hierarchy:
//   kz_hud                                   → интерактивное меню (фолбэк: сводка)
//   kz_hud type                              → переключить hudType (Standard ↔ MHUD)
//   kz_hud speed / prespeed / timer / keys / cptp / outline → toggle per-element
//   kz_hud <element> offset|scale|color|...  → тонкая настройка
//
//   kz_mhud                                  → алиас kz_hud (обратная совместимость)
//   kz_mhud master                           → теперь переключает hudType 0↔1

// Общая логика субкоманд, разделённая между kz_hud и kz_mhud.
static META_RES HandleHUDSubcmd(KZPlayer *player, const CCommand *args)
{
	bool mhudAvail  = KZHUDService::IsMHUDAvailable();
	const char *element = args->Arg(1);
	const char *prop    = args->ArgC() >= 3 ? args->Arg(2) : nullptr;

	// type: переключает hudType 0↔1. Аналог старого master.
	if (KZ_STREQI(element, "type") || KZ_STREQI(element, "master"))
	{
		int current = player->hudService->GetHudType();
		int next = (current == 0) ? 1 : 0;
		player->hudService->SetHudType(next);
		if (next == 1 && !mhudAvail)
		{
			player->languageService->PrintChat(true, false, "MHUD - Unavailable");
		}
		player->languageService->PrintChat(true, false, next == 1 ? "MHUD - Master Enabled" : "MHUD - Master Disabled");
	}
	else if (KZ_STREQI(element, "cptp"))
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
