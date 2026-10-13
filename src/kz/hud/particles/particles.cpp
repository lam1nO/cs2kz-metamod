#include "kz/hud/kz_hud.h"
#include "kz/option/kz_option.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "sdk/entity/cparticlesystem.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/cvarquery.h"

#include "tier0/memdbgon.h"

// Particle-MHUD — фолбэк к panorama-худу для игроков с cl_draw_only_deathnotices 1: этот cvar
// прячет весь худ игры вместе с custom_hud_layout, а частицы рисует мир, их он не трогает.
// Логика рисования — из удалённого 08.09 particle-пути (fba3b47a^:src/kz/hud/particles/
// particles.cpp) без меню и команд: позиции и масштаб пока фиксированные (старые дефолты),
// шрифт verdana без обводки. Ассеты — 15 скомпилированных .vpcf_c и атласы из бэкапа айтема
// cs2kz 3469155349 от 11.06, лежат в нашем аддоне GYMSTRIKE-KZ теми же путями.

// Два аргумента: font (lato/verdana) × outline (outline/no_outline).
#define MHUD_PARTICLE_NUMBERS_PATTERN     "particles/velo/velo_overlay_large_%s_%s.vpcf"
#define MHUD_PARTICLE_TIMER_DELIM_PATTERN "particles/timer_delimiter/timer_delimiter_%s_%s.vpcf"
#define MHUD_PARTICLE_INPUTS_PATTERN      "particles/inputs/inputs_%s_%s.vpcf"

static_global constexpr const char *MHUD_PARTICLE_FONTS[] = {"lato", "verdana"};
// Решение владельца 13.10: verdana без обводки. Настройка шрифта/обводки — отдельной задачей.
#define MHUD_PARTICLE_FONT    "verdana"
#define MHUD_PARTICLE_OUTLINE false

#define MHUD_TIMER_DIGIT_STEP 1.6f
#define MHUD_TIMER_DELIM_COLON 0 // sequence двоеточия в timer_delimiter
#define MHUD_TIMER_DELIM_DOT   1 // sequence точки

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

// Опрос cvar: раз в секунду. Ответа ждём 5 с; клиента, который промолчал, дальше спрашиваем
// раз в 30 с — пока не ответит снова.
#define MHUD_DEATHNOTICES_QUERY_INTERVAL       1.0
#define MHUD_DEATHNOTICES_QUERY_TIMEOUT        5.0
#define MHUD_DEATHNOTICES_QUERY_SILENT_INTERVAL 30.0

// Преф пункта «Показывать MHUD при скрытии худа» (hud/prefs/hud_prefs.cpp), дефолт — вкл.
#define MHUD_ON_HIDDEN_HUD_PREF    "mhudOnHiddenHud"
#define MHUD_ON_HIDDEN_HUD_DEFAULT true

static_function void BuildParticlePath(char *buf, size_t bufSize, const char *pattern, const char *font, bool outline)
{
	V_snprintf(buf, bufSize, pattern, font, outline ? "outline" : "no_outline");
}

void KZHUDService::PrecacheParticles(IEntityResourceManifest *pResourceManifest)
{
	// Все варианты, а не только текущий: будущая настройка шрифта/обводки переключает их живьём,
	// а незарегистрированный в манифесте ассет клиент не прекеширует и не рисует.
	char path[256];
	for (const char *font : MHUD_PARTICLE_FONTS)
	{
		for (bool outline : {false, true})
		{
			BuildParticlePath(path, sizeof(path), MHUD_PARTICLE_NUMBERS_PATTERN, font, outline);
			pResourceManifest->AddResource(path);
			BuildParticlePath(path, sizeof(path), MHUD_PARTICLE_TIMER_DELIM_PATTERN, font, outline);
			pResourceManifest->AddResource(path);
			BuildParticlePath(path, sizeof(path), MHUD_PARTICLE_INPUTS_PATTERN, font, outline);
			pResourceManifest->AddResource(path);
		}
	}
}

// === Опрос cl_draw_only_deathnotices ================================================

void KZHUDService::PollDeathnoticesCvar()
{
	if (this->player->IsFakeClient())
	{
		return;
	}
	f64 now = g_pKZUtils->GetServerGlobals()->realtime;
	f64 since = now - this->deathnoticesQueryTime;
	// realtime мог пойти заново (смена карты) — тогда since < 0, спрашиваем сразу.
	if (since >= 0.0)
	{
		f64 wait = MHUD_DEATHNOTICES_QUERY_INTERVAL;
		if (this->deathnoticesQueryPending)
		{
			wait = this->deathnoticesQueryTimeouts > 0 ? MHUD_DEATHNOTICES_QUERY_SILENT_INTERVAL : MHUD_DEATHNOTICES_QUERY_TIMEOUT;
		}
		if (since < wait)
		{
			return;
		}
	}
	if (this->deathnoticesQueryPending)
	{
		// Клиент не ответил. Значение не трогаем: молчащий клиент с последним известным 0
		// остаётся на panorama, с 1 — на частицах до следующего ответа. В лог — один раз за серию.
		if (this->deathnoticesQueryTimeouts++ == 0)
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] mhud_deathnotices_query_timeout steam_id=%llu\n", this->player->GetSteamId64(false));
		}
	}
	this->deathnoticesQueryTime = now;
	this->deathnoticesQueryPending = cvarquery::Query(this->player->GetPlayerSlot(), "cl_draw_only_deathnotices",
													  [](CPlayerSlot slot, cvarquery::Status status, const char *name, const char *value)
													  { KZHUDService::OnDeathnoticesCvarValue(slot, status == cvarquery::Status::ValueIntact, value); });
}

void KZHUDService::OnDeathnoticesCvarValue(CPlayerSlot slot, bool ok, const char *value)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(slot);
	if (!player || !player->hudService)
	{
		return;
	}
	KZHUDService *hud = player->hudService;
	hud->deathnoticesQueryPending = false;
	hud->deathnoticesQueryTimeouts = 0;
	// Не тот статус (cvar защищён/не найден в будущем билде) — это «худ не прячется», panorama.
	bool hidden = ok && value && atoi(value) != 0;
	if (hidden != hud->drawOnlyDeathnotices)
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb] mhud_deathnotices steam_id=%llu ok=%d value=%s hidden=%d\n", player->GetSteamId64(false), ok,
					value ? value : "", hidden);
	}
	hud->drawOnlyDeathnotices = hidden;
}

bool KZHUDService::WantsParticleMHUD()
{
	return this->drawOnlyDeathnotices
		   && this->MHUDSettingsSource()->optionService->GetPreferenceBool(MHUD_ON_HIDDEN_HUD_PREF, MHUD_ON_HIDDEN_HUD_DEFAULT);
}

// === Сущности =======================================================================

static_function CParticleSystem *CreateMHUDParticle(const char *particleName, const Color &color, f32 sequence, f32 size, f32 offsetX,
													f32 offsetY)
{
	CParticleSystem *particleSystem = utils::CreateEntityByName<CParticleSystem>("info_particle_system");
	if (!particleSystem)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] mhud_particle_create_failed effect=%s\n", particleName);
		return nullptr;
	}
	CEntityKeyValues *pKeyValues = new CEntityKeyValues();
	pKeyValues->SetString("effect_name", particleName);
	pKeyValues->SetBool("start_active", true);
	pKeyValues->SetInt("tint_cp", 16);
	pKeyValues->SetColor("tint_cp_color", color);
	pKeyValues->SetInt("data_cp", 17);
	// CP17: x=sequence, y=size, z=self-illum (1.0 = glow-канал .vpcf)
	pKeyValues->SetVector("data_cp_value", Vector(sequence, size, 1.0f));
	// Метка plugin-частицы: KZ::quiet::OnCheckTransmit отдаёт её только владельцу (OwnsParticle).
	particleSystem->m_iTeamNum(CUSTOM_PARTICLE_SYSTEM_TEAM);
	particleSystem->DispatchSpawn(pKeyValues);
	particleSystem->SetControlPointValue(18, Vector(offsetX, offsetY, 1.0f));
	return particleSystem;
}

static_function void DestroyHandle(CHandle<CParticleSystem> &handle)
{
	CParticleSystem *p = handle.Get();
	if (p)
	{
		g_pKZUtils->RemoveEntity(p);
	}
	handle = {};
}

static_function void EnsureParticle(CHandle<CParticleSystem> &handle, const char *path, const Color &color, f32 scale, f32 x, f32 y)
{
	if (!handle.Get())
	{
		handle = CreateMHUDParticle(path, color, 0.0f, scale, x, y);
	}
}

bool KZHUDService::OwnsParticle(const CEntityHandle &handle) const
{
	for (const auto &h : this->mhudSpeedParticles)
	{
		if (handle == h)
		{
			return true;
		}
	}
	for (const auto &h : this->mhudPrespeedParticles)
	{
		if (handle == h)
		{
			return true;
		}
	}
	for (const auto &h : this->mhudTimerDigitParticles)
	{
		if (handle == h)
		{
			return true;
		}
	}
	for (const auto &h : this->mhudTimerDelimParticles)
	{
		if (handle == h)
		{
			return true;
		}
	}
	return handle == this->mhudKeysParticle;
}

void KZHUDService::DestroyAllParticles()
{
	if (!this->particlesActive)
	{
		return;
	}
	for (auto &h : this->mhudSpeedParticles)
	{
		DestroyHandle(h);
	}
	for (auto &h : this->mhudPrespeedParticles)
	{
		DestroyHandle(h);
	}
	for (auto &h : this->mhudTimerDigitParticles)
	{
		DestroyHandle(h);
	}
	for (auto &h : this->mhudTimerDelimParticles)
	{
		DestroyHandle(h);
	}
	DestroyHandle(this->mhudKeysParticle);
	this->particlesActive = false;
}

static_function void SetParticleTint(CParticleSystem *particle, const Color &color)
{
	if (particle)
	{
		particle->SetControlPointValue(16, Vector((f32)color.r(), (f32)color.g(), (f32)color.b()));
	}
}

static_function void SetParticleLayout(CParticleSystem *p, f32 sequence, f32 scale, f32 x, f32 y)
{
	p->SetControlPointValue(17, Vector(sequence, scale, 1.0f));
	p->SetControlPointValue(18, Vector(x, y, 1.0f));
}

// === Скорость и престрейф ===========================================================

// Число до 9999 двумя частицами-парами разрядов; ведущий ноль младшей пары — sequence +100.
// Вторая частица не нужна для чисел < 100 — её гасим.
static_function void LayoutDigitPair(i32 value, f32 size, f32 baseX, f32 baseY, CParticleSystem *p0, CParticleSystem *p1)
{
	static constexpr f32 X_LEFT = -0.625f;
	static constexpr f32 X_CENTER = 0.0f;
	static constexpr f32 X_RIGHT_3DIG = 0.3125f;
	static constexpr f32 X_RIGHT_4DIG = 0.625f;

	value = Clamp(value, 0, 9999);
	if (value >= 100)
	{
		i32 hi = value / 100;
		i32 lo = value % 100;
		if (lo < 10)
		{
			lo += 100;
		}
		p0->SetControlPointValue(17, Vector((f32)hi, size, 0.0f));
		p0->SetControlPointValue(18, Vector(baseX + X_LEFT, baseY, 1.0f));
		p1->SetControlPointValue(17, Vector((f32)lo, size, 0.0f));
		p1->SetControlPointValue(18, Vector(baseX + (value >= 1000 ? X_RIGHT_4DIG : X_RIGHT_3DIG), baseY, 1.0f));
		p0->Start();
		p1->Start();
	}
	else
	{
		p0->SetControlPointValue(17, Vector((f32)value, size, 0.0f));
		p0->SetControlPointValue(18, Vector(baseX + X_CENTER, baseY, 1.0f));
		p0->Start();
		p1->Destroy();
	}
}

void KZHUDService::UpdateMHUDSpeedParticles()
{
	KZOptionService *opts = this->MHUDSettingsSource()->optionService;
	bool speedEnabled = this->IsMHUDSpeedEnabled();
	bool prespeedEnabled = opts->GetPreferenceBool("hudPrespeed", true);

	char numbersPath[256];
	BuildParticlePath(numbersPath, sizeof(numbersPath), MHUD_PARTICLE_NUMBERS_PATTERN, MHUD_PARTICLE_FONT, MHUD_PARTICLE_OUTLINE);

	const SpeedInfo info = this->GetSpeedInfo();
	const Color baseColor = this->GetMHUDColorPref("mhudSpeedColor", MHUD_DEF_BASE_COLOR);
	const Color prespeedBaseColor = this->GetMHUDColorPref("mhudPrespeedColor", MHUD_DEF_BASE_COLOR);
	const Color perfColor = this->GetMHUDColorPref("mhudPrespeedPerfColor", MHUD_DEF_PERF_COLOR);
	const Color jumpbugColor = this->GetMHUDColorPref("mhudPrespeedJumpbugColor", MHUD_DEF_JUMPBUG_COLOR);
	const Color cjColor = this->GetMHUDColorPref("mhudSpeedCjColor", MHUD_DEF_CJ_COLOR);

	if (speedEnabled)
	{
		for (auto &h : this->mhudSpeedParticles)
		{
			EnsureParticle(h, numbersPath, baseColor, MHUD_DEF_SPEED_SCALE, MHUD_DEF_SPEED_OFFSET_X, MHUD_DEF_SPEED_OFFSET_Y);
		}
		if (this->mhudSpeedParticles[0].Get() && this->mhudSpeedParticles[1].Get())
		{
			LayoutDigitPair(RoundFloatToInt(info.velocity.Length2D()), MHUD_DEF_SPEED_SCALE, MHUD_DEF_SPEED_OFFSET_X, MHUD_DEF_SPEED_OFFSET_Y,
							this->mhudSpeedParticles[0].Get(), this->mhudSpeedParticles[1].Get());
			const Color color = info.crouchJump ? cjColor : baseColor;
			SetParticleTint(this->mhudSpeedParticles[0].Get(), color);
			SetParticleTint(this->mhudSpeedParticles[1].Get(), color);
		}
	}
	else
	{
		for (auto &h : this->mhudSpeedParticles)
		{
			DestroyHandle(h);
		}
	}

	if (prespeedEnabled)
	{
		for (auto &h : this->mhudPrespeedParticles)
		{
			EnsureParticle(h, numbersPath, prespeedBaseColor, MHUD_DEF_PRESPEED_SCALE, MHUD_DEF_PRESPEED_OFFSET_X, MHUD_DEF_PRESPEED_OFFSET_Y);
		}
		CParticleSystem *p0 = this->mhudPrespeedParticles[0].Get();
		CParticleSystem *p1 = this->mhudPrespeedParticles[1].Get();
		if (p0 && p1)
		{
			if (info.hasPrespeed)
			{
				LayoutDigitPair(RoundFloatToInt(info.prespeed.Length2D()), MHUD_DEF_PRESPEED_SCALE, MHUD_DEF_PRESPEED_OFFSET_X,
								MHUD_DEF_PRESPEED_OFFSET_Y, p0, p1);
			}
			else
			{
				p0->Destroy();
				p1->Destroy();
			}
			const Color color = info.perfing ? (info.jumpbug ? jumpbugColor : perfColor) : prespeedBaseColor;
			SetParticleTint(p0, color);
			SetParticleTint(p1, color);
		}
	}
	else
	{
		for (auto &h : this->mhudPrespeedParticles)
		{
			DestroyHandle(h);
		}
	}
}

// === Таймер =========================================================================

void KZHUDService::UpdateMHUDTimerParticles()
{
	if (!this->IsMHUDTimerEnabled())
	{
		for (auto &h : this->mhudTimerDigitParticles)
		{
			DestroyHandle(h);
		}
		for (auto &h : this->mhudTimerDelimParticles)
		{
			DestroyHandle(h);
		}
		return;
	}

	KZPlayer *src = this->MHUDDataSource();
	const Color tpColor = this->GetMHUDColorPref("mhudTimerTpColor", MHUD_DEF_TIMER_TP_COLOR);
	char numbersPath[256], delimPath[256];
	BuildParticlePath(numbersPath, sizeof(numbersPath), MHUD_PARTICLE_NUMBERS_PATTERN, MHUD_PARTICLE_FONT, MHUD_PARTICLE_OUTLINE);
	BuildParticlePath(delimPath, sizeof(delimPath), MHUD_PARTICLE_TIMER_DELIM_PATTERN, MHUD_PARTICLE_FONT, MHUD_PARTICLE_OUTLINE);
	for (auto &h : this->mhudTimerDigitParticles)
	{
		EnsureParticle(h, numbersPath, tpColor, MHUD_DEF_TIMER_SCALE, 0.0f, MHUD_DEF_TIMER_OFFSET_Y);
	}
	for (auto &h : this->mhudTimerDelimParticles)
	{
		EnsureParticle(h, delimPath, tpColor, MHUD_DEF_TIMER_SCALE, 0.0f, MHUD_DEF_TIMER_OFFSET_Y);
	}

	bool timerRunning = src->timerService->GetTimerRunning();
	if (!timerRunning && !src->hudService->ShouldShowTimerAfterStop())
	{
		for (auto &h : this->mhudTimerDigitParticles)
		{
			if (h.Get())
			{
				h.Get()->Destroy();
			}
		}
		for (auto &h : this->mhudTimerDelimParticles)
		{
			if (h.Get())
			{
				h.Get()->Destroy();
			}
		}
		return;
	}

	f64 time = timerRunning ? src->timerService->GetTime() : src->hudService->currentTimeWhenTimerStopped;
	if (time < 0.0)
	{
		time = 0.0;
	}
	bool detailed = this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudTimerDetail", true);

	i32 totalSeconds = (i32)time;
	i32 hours = totalSeconds / 3600;
	i32 minutes = (totalSeconds / 60) % 60;
	i32 seconds = totalSeconds % 60;
	i32 centis = Max((i32)(time * 100.0) % 100, 0);
	auto pad = [](i32 v) { return v < 10 ? v + 100 : v; };

	// Пары разрядов: HH:MM:SS.CC, HH:MM:SS, MM:SS.CC, MM:SS.
	i32 digit[4] {};
	i32 numDigits = 0;
	i32 delimTypes[3] {};
	if (hours > 0)
	{
		digit[numDigits++] = hours > 9 ? Min(hours, 99) : hours + 100;
	}
	digit[numDigits++] = pad(minutes);
	digit[numDigits++] = pad(seconds);
	if (detailed)
	{
		digit[numDigits++] = pad(centis);
	}
	i32 numDelims = numDigits - 1;
	for (i32 d = 0; d < numDelims; d++)
	{
		delimTypes[d] = (detailed && d == numDelims - 1) ? MHUD_TIMER_DELIM_DOT : MHUD_TIMER_DELIM_COLON;
	}

	f32 layoutX[4] {};
	for (i32 i = 0; i < numDigits; i++)
	{
		layoutX[i] = MHUD_DEF_TIMER_OFFSET_X + ((f32)i - (numDigits - 1) * 0.5f) * MHUD_TIMER_DIGIT_STEP;
	}

	Color color;
	if (!timerRunning)
	{
		color = this->GetMHUDColorPref("mhudTimerStoppedColor", MHUD_DEF_TIMER_STOPPED_COLOR);
	}
	else if (src->timerService->GetPaused())
	{
		color = this->GetMHUDColorPref("mhudTimerPausedColor", MHUD_DEF_TIMER_PAUSED_COLOR);
	}
	else if (src->checkpointService->GetTeleportCount() > 0)
	{
		color = tpColor;
	}
	else
	{
		color = this->GetMHUDColorPref("mhudTimerProColor", MHUD_DEF_TIMER_PRO_COLOR);
	}

	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(this->mhudTimerDigitParticles); i++)
	{
		CParticleSystem *p = this->mhudTimerDigitParticles[i].Get();
		if (!p)
		{
			continue;
		}
		if (i < numDigits)
		{
			SetParticleLayout(p, (f32)digit[i], MHUD_DEF_TIMER_SCALE, layoutX[i], MHUD_DEF_TIMER_OFFSET_Y);
			SetParticleTint(p, color);
			p->Start();
		}
		else
		{
			p->Destroy();
		}
	}
	for (i32 d = 0; d < (i32)KZ_ARRAYSIZE(this->mhudTimerDelimParticles); d++)
	{
		CParticleSystem *p = this->mhudTimerDelimParticles[d].Get();
		if (!p)
		{
			continue;
		}
		if (d < numDelims)
		{
			SetParticleLayout(p, (f32)delimTypes[d], MHUD_DEF_TIMER_SCALE, layoutX[d] + MHUD_TIMER_DIGIT_STEP * 0.5f, MHUD_DEF_TIMER_OFFSET_Y);
			SetParticleTint(p, color);
			p->Start();
		}
		else
		{
			p->Destroy();
		}
	}
}

// === Клавиши ========================================================================

void KZHUDService::UpdateMHUDKeysParticle()
{
	if (!this->IsMHUDKeysEnabled())
	{
		DestroyHandle(this->mhudKeysParticle);
		return;
	}
	const Color baseColor = this->GetMHUDColorPref("mhudKeysColor", MHUD_DEF_BASE_COLOR);
	char inputsPath[256];
	BuildParticlePath(inputsPath, sizeof(inputsPath), MHUD_PARTICLE_INPUTS_PATTERN, MHUD_PARTICLE_FONT, MHUD_PARTICLE_OUTLINE);
	EnsureParticle(this->mhudKeysParticle, inputsPath, baseColor, MHUD_DEF_KEYS_SCALE, MHUD_DEF_KEYS_OFFSET_X, MHUD_DEF_KEYS_OFFSET_Y);
	CParticleSystem *p = this->mhudKeysParticle.Get();
	if (!p)
	{
		return;
	}

	KZPlayer *src = this->MHUDDataSource();
	bool forward = src->IsButtonPressed(IN_FORWARD);
	bool left = src->IsButtonPressed(IN_MOVELEFT);
	bool back = src->IsButtonPressed(IN_BACK);
	bool right = src->IsButtonPressed(IN_MOVERIGHT);
	u8 mask = 0;
	mask |= forward ? KPF_Forward : 0;
	mask |= left ? KPF_Left : 0;
	mask |= back ? KPF_Back : 0;
	mask |= right ? KPF_Right : 0;
	mask |= (src->hudService->jumpedThisTick || src->IsButtonPressed(IN_JUMP)) ? KPF_Jump : 0;
	mask |= src->IsButtonPressed(IN_DUCK) ? KPF_Duck : 0;

	p->SetControlPointValue(17, Vector((f32)mask, MHUD_DEF_KEYS_SCALE, 0.0f));
	p->SetControlPointValue(18, Vector(MHUD_DEF_KEYS_OFFSET_X, MHUD_DEF_KEYS_OFFSET_Y, 1.0f));
	p->Start();

	bool overlap = (forward && back) || (left && right);
	SetParticleTint(p, overlap && this->IsMHUDKeysOverlapEnabled() ? this->GetMHUDColorPref("mhudKeysOverlapColor", MHUD_DEF_KEYS_OVERLAP_COLOR)
																   : baseColor);
}

void KZHUDService::UpdateParticles()
{
	// Только живому владельцу (DrawPanels гейтит player == target), mhudSource не задаём:
	// данные и настройки — this->player.
	this->particlesActive = true;
	this->UpdateMHUDSpeedParticles();
	this->UpdateMHUDTimerParticles();
	this->UpdateMHUDKeysParticle();
}
