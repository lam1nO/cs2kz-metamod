#include "kz/hud/kz_hud.h"
#include "kz/option/kz_option.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "kz/lead/kz_lead.h"
#include "kz/prac/kz_prac.h"
#include "sdk/entity/cparticlesystem.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/cvarquery.h"
#include "utils/gamesystem.h"

#include "tier0/memdbgon.h"

// Particle-MHUD — фолбэк к panorama-худу для игроков с cl_draw_only_deathnotices 1: этот cvar
// прячет весь худ игры вместе с custom_hud_layout, а частицы рисует мир, их он не трогает.
// Логика рисования — из удалённого 08.09 particle-пути (fba3b47a^:src/kz/hud/particles/
// particles.cpp) без меню и команд. Ассеты — 15 скомпилированных .vpcf_c и атласы из бэкапа айтема
// cs2kz 3469155349 от 11.06, лежат в нашем аддоне GYMSTRIKE-KZ теми же путями.
//
// Настройки — те же, что у panorama-худа (GetLayoutPrefs: позиция, кегль, цвета, обводка, вкл/выкл
// элемента), своих у частиц нет: игрок двигает худ в редакторе !hud, и частицы едут туда же.
// Шрифт — только verdana (решение владельца 13.10), обводка — поэлементная, как в панораме.
// Прозрачность элемента частицы не умеют (альфа зашита литералом в .vpcf) — игнорируется.

// Два аргумента: font (lato/verdana) × outline (outline/no_outline).
#define MHUD_PARTICLE_NUMBERS_PATTERN     "particles/velo/velo_overlay_large_%s_%s.vpcf"
#define MHUD_PARTICLE_TIMER_DELIM_PATTERN "particles/timer_delimiter/timer_delimiter_%s_%s.vpcf"
#define MHUD_PARTICLE_INPUTS_PATTERN      "particles/inputs/inputs_%s_%s.vpcf"
// Знак дельты (sequence 0 «+», 1 «−») — наш ассет в аддоне, только verdana: ячейка 121×57 и кегль
// как у velo_overlay_large, поэтому геометрия та же, что у цифр.
#define MHUD_PARTICLE_SIGN_PATTERN        "particles/gymstrike/mhud_sign_%s_%s.vpcf"

static_global constexpr const char *MHUD_PARTICLE_FONTS[] = {"lato", "verdana"};
#define MHUD_PARTICLE_FONT "verdana"

#define MHUD_TIMER_DIGIT_STEP 1.6f
#define MHUD_TIMER_DELIM_COLON 0 // sequence двоеточия в timer_delimiter
#define MHUD_TIMER_DELIM_DOT   1 // sequence точки

// Дельта к PB/WR (как mhud_delta в panorama): справа от таймера, кегль 0.65 таймера, тот же центр по Y.
// Ширины — в единицах размера частицы: разряд 0.625 (см. LayoutDigitPair), пара — 1.25,
// разделитель с зазорами — 0.35 (шаг таймера 1.6 = пара + разделитель).
#define MHUD_DELTA_SCALE        0.65f
#define MHUD_DELTA_GAP          0.3f // зазор таймер → дельта, в единицах размера таймера
#define MHUD_DIGIT_WIDTH        0.625f
#define MHUD_DELIM_WIDTH        0.35f
#define MHUD_SIGN_WIDTH         0.7f
#define MHUD_SIGN_PLUS          0
#define MHUD_SIGN_MINUS         1

// Перевод геометрии panorama в параметры частицы. Частица висит перед глазами (SetControlPointToPlayer +
// PositionLock), рендерится спрайтом экранного размера CP17.y (m_flMinSize), а CP18 (x, y) сдвигает её
// центр в единицах этого размера (m_flCenterXOffset/YOffset) — поэтому смещение делится на размер.
// Панорама ставит центр элемента в (50 + x)% ширины и (50 + y)% высоты, кегль — в px.
// Коэффициенты подобраны по скриншотам (16:9); cvar'ы — для подстройки без пересборки.
// Отладка калибровки: частицы рисуются ПОВЕРХ panorama (без cl_draw_only_deathnotices) — видно расхождение.
CConVar<bool> kz_mhud_particle_overlay("kz_mhud_particle_overlay", FCVAR_NONE,
									   "Particle-MHUD: рисовать частицы вместе с panorama всем живым (отладка калибровки)", false);
static CConVar<f32> kz_mhud_particle_digit_size("kz_mhud_particle_digit_size", FCVAR_NONE,
												 "Particle-MHUD: размер частицы-цифр (CP17.y) на 1 px кегля panorama", 0.00123f);
static CConVar<f32> kz_mhud_particle_keys_size("kz_mhud_particle_keys_size", FCVAR_NONE,
												"Particle-MHUD: размер частицы клавиш (CP17.y) на 1 ед. размера клавиш panorama", 0.0025f);
static CConVar<f32> kz_mhud_particle_pos_x("kz_mhud_particle_pos_x", FCVAR_NONE,
										   "Particle-MHUD: смещение CP18.x × размер на 1% ширины экрана", 0.02966f);
static CConVar<f32> kz_mhud_particle_pos_y("kz_mhud_particle_pos_y", FCVAR_NONE,
										   "Particle-MHUD: смещение CP18.y × размер на 1% высоты экрана (вниз)", 0.01668f);

// Опрос cvar: раз в 0.1 с (решение владельца 13.10 — переключение без заметной задержки); новый
// запрос не уходит, пока не пришёл ответ на прежний, то есть фактически не чаще раза за пинг.
// Ответа ждём 5 с; клиента, который промолчал, дальше спрашиваем раз в 30 с — пока не ответит снова.
#define MHUD_DEATHNOTICES_QUERY_INTERVAL       0.1
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
	for (bool outline : {false, true})
	{
		BuildParticlePath(path, sizeof(path), MHUD_PARTICLE_SIGN_PATTERN, MHUD_PARTICLE_FONT, outline);
		pResourceManifest->AddResource(path);
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
	// Булев cvar Source 2 клиент отдаёт строкой "true"/"false", не "1"/"0".
	bool hidden = ok && value && (!V_stricmp(value, "true") || atoi(value) != 0);
	// Первый ответ пишем всегда: по нему видно, что опрос вообще работает и в каком виде приходит значение.
	if (hidden != hud->drawOnlyDeathnotices || !hud->deathnoticesAnswered)
	{
		hud->deathnoticesAnswered = true;
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

// Эффект частицы задаётся только при спавне: смена обводки элемента — пересоздать всю группу.
// variant: -1 — группа ещё не создавалась, иначе последнее значение outline.
static_function void EnsureGroup(CHandle<CParticleSystem> *handles, i32 count, i32 &variant, bool outline, const char *path, const Color &color,
								 f32 scale, f32 x, f32 y)
{
	if (variant != -1 && variant != (i32)outline)
	{
		for (i32 i = 0; i < count; i++)
		{
			DestroyHandle(handles[i]);
		}
	}
	variant = (i32)outline;
	for (i32 i = 0; i < count; i++)
	{
		EnsureParticle(handles[i], path, color, scale, x, y);
	}
}

// Геометрия элемента panorama → размер и смещение частицы (см. kz_mhud_particle_*).
struct MHUDParticleGeom
{
	f32 size;
	f32 x;
	f32 y;
};

static_function MHUDParticleGeom ParticleGeom(const MHUDLayoutPrefs::Element &e, f32 sizePerUnit)
{
	MHUDParticleGeom g;
	g.size = Max((f32)e.size * sizePerUnit, 0.001f);
	g.x = (f32)e.x * kz_mhud_particle_pos_x.Get() / g.size;
	g.y = -(f32)e.y * kz_mhud_particle_pos_y.Get() / g.size;
	return g;
}

// Цвет элемента: градиент (маркер a == 1, см. panorama_tables) частица нарисовать не может — берём дефолт.
static_function Color ParticleColor(const Color &c, const Color &fallback)
{
	return c.a() == 1 ? fallback : c;
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
	for (const auto &h : this->mhudDeltaSignParticles)
	{
		if (handle == h)
		{
			return true;
		}
	}
	for (const auto &h : this->mhudDeltaDigitParticles)
	{
		if (handle == h)
		{
			return true;
		}
	}
	for (const auto &h : this->mhudDeltaDelimParticles)
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
	for (auto &h : this->mhudDeltaSignParticles)
	{
		DestroyHandle(h);
	}
	for (auto &h : this->mhudDeltaDigitParticles)
	{
		DestroyHandle(h);
	}
	for (auto &h : this->mhudDeltaDelimParticles)
	{
		DestroyHandle(h);
	}
	for (i32 &v : this->mhudParticleVariant)
	{
		v = -1;
	}
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

static_function void DestroyGroup(CHandle<CParticleSystem> *handles, i32 count, i32 &variant)
{
	for (i32 i = 0; i < count; i++)
	{
		DestroyHandle(handles[i]);
	}
	variant = -1;
}

void KZHUDService::UpdateMHUDSpeedParticles()
{
	const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
	const MHUDLayoutPrefs::Element &speedEl = prefs.elements[(i32)LayoutElement::Speed];
	const MHUDLayoutPrefs::Element &preEl = prefs.elements[(i32)LayoutElement::Prespeed];
	const SpeedInfo info = this->GetSpeedInfo();
	char path[256];

	if (speedEl.enabled)
	{
		const MHUDParticleGeom g = ParticleGeom(speedEl, kz_mhud_particle_digit_size.Get());
		const Color base = ParticleColor(prefs.speed, MHUD_DEF_BASE_COLOR);
		BuildParticlePath(path, sizeof(path), MHUD_PARTICLE_NUMBERS_PATTERN, MHUD_PARTICLE_FONT, speedEl.outline);
		EnsureGroup(this->mhudSpeedParticles, 2, this->mhudParticleVariant[0], speedEl.outline, path, base, g.size, g.x, g.y);
		CParticleSystem *p0 = this->mhudSpeedParticles[0].Get();
		CParticleSystem *p1 = this->mhudSpeedParticles[1].Get();
		if (p0 && p1)
		{
			LayoutDigitPair(RoundFloatToInt(info.velocity.Length2D()), g.size, g.x, g.y, p0, p1);
			const Color color = info.crouchJump ? ParticleColor(prefs.speedCj, MHUD_DEF_CJ_COLOR) : base;
			SetParticleTint(p0, color);
			SetParticleTint(p1, color);
		}
	}
	else
	{
		DestroyGroup(this->mhudSpeedParticles, 2, this->mhudParticleVariant[0]);
	}

	if (preEl.enabled)
	{
		const MHUDParticleGeom g = ParticleGeom(preEl, kz_mhud_particle_digit_size.Get());
		const Color base = ParticleColor(prefs.prespeed, MHUD_DEF_BASE_COLOR);
		BuildParticlePath(path, sizeof(path), MHUD_PARTICLE_NUMBERS_PATTERN, MHUD_PARTICLE_FONT, preEl.outline);
		EnsureGroup(this->mhudPrespeedParticles, 2, this->mhudParticleVariant[1], preEl.outline, path, base, g.size, g.x, g.y);
		CParticleSystem *p0 = this->mhudPrespeedParticles[0].Get();
		CParticleSystem *p1 = this->mhudPrespeedParticles[1].Get();
		if (p0 && p1)
		{
			if (info.hasPrespeed)
			{
				LayoutDigitPair(RoundFloatToInt(info.prespeed.Length2D()), g.size, g.x, g.y, p0, p1);
			}
			else
			{
				p0->Destroy();
				p1->Destroy();
			}
			Color color = base;
			if (info.perfing)
			{
				color = info.jumpbug ? ParticleColor(prefs.prespeedJumpbug, MHUD_DEF_JUMPBUG_COLOR)
									 : ParticleColor(prefs.prespeedPerf, MHUD_DEF_PERF_COLOR);
			}
			SetParticleTint(p0, color);
			SetParticleTint(p1, color);
		}
	}
	else
	{
		DestroyGroup(this->mhudPrespeedParticles, 2, this->mhudParticleVariant[1]);
	}
}

// === Таймер =========================================================================

void KZHUDService::UpdateMHUDTimerParticles()
{
	const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
	const MHUDLayoutPrefs::Element &el = prefs.elements[(i32)LayoutElement::Timer];
	if (!el.enabled)
	{
		DestroyGroup(this->mhudTimerDigitParticles, KZ_ARRAYSIZE(this->mhudTimerDigitParticles), this->mhudParticleVariant[2]);
		DestroyGroup(this->mhudTimerDelimParticles, KZ_ARRAYSIZE(this->mhudTimerDelimParticles), this->mhudParticleVariant[3]);
		this->UpdateMHUDDeltaParticles(0.0f, 0.0f, 0.0f, false, false);
		return;
	}

	KZPlayer *src = this->MHUDDataSource();
	const MHUDParticleGeom g = ParticleGeom(el, kz_mhud_particle_digit_size.Get());
	const Color tpColor = ParticleColor(prefs.timerTp, MHUD_DEF_TIMER_TP_COLOR);
	char numbersPath[256], delimPath[256];
	BuildParticlePath(numbersPath, sizeof(numbersPath), MHUD_PARTICLE_NUMBERS_PATTERN, MHUD_PARTICLE_FONT, el.outline);
	BuildParticlePath(delimPath, sizeof(delimPath), MHUD_PARTICLE_TIMER_DELIM_PATTERN, MHUD_PARTICLE_FONT, el.outline);
	EnsureGroup(this->mhudTimerDigitParticles, KZ_ARRAYSIZE(this->mhudTimerDigitParticles), this->mhudParticleVariant[2], el.outline, numbersPath,
				tpColor, g.size, g.x, g.y);
	EnsureGroup(this->mhudTimerDelimParticles, KZ_ARRAYSIZE(this->mhudTimerDelimParticles), this->mhudParticleVariant[3], el.outline, delimPath,
				tpColor, g.size, g.x, g.y);

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
		this->UpdateMHUDDeltaParticles(0.0f, 0.0f, 0.0f, false, false);
		return;
	}

	f64 time = timerRunning ? src->timerService->GetTime() : src->hudService->currentTimeWhenTimerStopped;
	if (time < 0.0)
	{
		time = 0.0;
	}
	bool detailed = prefs.timerDetailed;

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

	// Шаг разрядов — в единицах размера частицы, как и смещение: строка масштабируется вместе с кеглем.
	f32 layoutX[4] {};
	for (i32 i = 0; i < numDigits; i++)
	{
		layoutX[i] = g.x + ((f32)i - (numDigits - 1) * 0.5f) * MHUD_TIMER_DIGIT_STEP;
	}

	Color color;
	if (src->timerService->GetPaused())
	{
		color = ParticleColor(prefs.timerPaused, MHUD_DEF_TIMER_PAUSED_COLOR);
	}
	else if (!timerRunning)
	{
		color = ParticleColor(prefs.timerStopped, MHUD_DEF_TIMER_STOPPED_COLOR);
	}
	else if (src->checkpointService->GetTeleportCount() > 0)
	{
		color = tpColor;
	}
	else
	{
		color = ParticleColor(prefs.timerPro, MHUD_DEF_TIMER_PRO_COLOR);
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
			SetParticleLayout(p, (f32)digit[i], g.size, layoutX[i], g.y);
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
			SetParticleLayout(p, (f32)delimTypes[d], g.size, layoutX[d] + MHUD_TIMER_DIGIT_STEP * 0.5f, g.y);
			SetParticleTint(p, color);
			p->Start();
		}
		else
		{
			p->Destroy();
		}
	}

	// Правый край строки таймера (последняя пара) — в экранных единицах (смещение × размер).
	const f32 rightEdge = (layoutX[numDigits - 1] + MHUD_DIGIT_WIDTH) * g.size;
	this->UpdateMHUDDeltaParticles(rightEdge, g.y * g.size, g.size, el.outline, timerRunning);
}

// === Дельта к PB/WR =================================================================

// Тот же гейт и тот же источник, что у panorama (layout/mhud.cpp: UpdateTimerElement): путь
// сравнения `!lead` своего игрока, вид записи — hudTimerCompare (1 PB, 2 WR). Путь заказывает
// DrawPanels (SetCompareWanted), здесь только читаем. prac-часы к пути отношения не имеют.
void KZHUDService::UpdateMHUDDeltaParticles(f32 timerRightEdge, f32 timerY, f32 timerSize, bool outline, bool show)
{
	const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
	KZPlayer *src = this->MHUDDataSource();
	const bool inPrac = src->pracService && src->pracService->IsInPrac();
	f64 delta = 0.0;
	show = show && !inPrac && prefs.timerCompare != 0 && src->leadService && src->leadService->GetCompareDeltaSeconds(delta);
	if (!show)
	{
		DestroyGroup(this->mhudDeltaSignParticles, KZ_ARRAYSIZE(this->mhudDeltaSignParticles), this->mhudParticleVariant[5]);
		DestroyGroup(this->mhudDeltaDigitParticles, KZ_ARRAYSIZE(this->mhudDeltaDigitParticles), this->mhudParticleVariant[6]);
		DestroyGroup(this->mhudDeltaDelimParticles, KZ_ARRAYSIZE(this->mhudDeltaDelimParticles), this->mhudParticleVariant[7]);
		return;
	}

	// Округление до показываемой точности ДО разбиения на минуты (как hudfmt::FormatDelta).
	const bool detailed = prefs.timerDetailed;
	const i64 q = detailed ? 1000 : 100;
	i64 units = (i64)(fabs(delta) * (f64)q + 0.5);
	const i32 frac = (i32)(units % q);
	i64 wholeSecs = units / q;
	const i32 mins = (i32)Min(wholeSecs / 60, (i64)99);
	const i32 secs = (i32)(wholeSecs % 60);

	// Токены слева направо: знак, [минуты, «:»], секунды, «.», сотые, [тысячная].
	// sequence разряда: 0..99 — число без ведущего нуля, 100..109 — «00».. «09».
	struct Token
	{
		i32 kind; // 0 знак, 1 разряды, 2 разделитель
		i32 seq;
		f32 width;
	};
	Token tokens[8];
	i32 n = 0;
	tokens[n++] = {0, delta < 0.0 ? MHUD_SIGN_MINUS : MHUD_SIGN_PLUS, MHUD_SIGN_WIDTH};
	if (mins > 0)
	{
		tokens[n++] = {1, mins, mins >= 10 ? MHUD_DIGIT_WIDTH * 2 : MHUD_DIGIT_WIDTH};
		tokens[n++] = {2, MHUD_TIMER_DELIM_COLON, MHUD_DELIM_WIDTH};
		tokens[n++] = {1, secs < 10 ? secs + 100 : secs, MHUD_DIGIT_WIDTH * 2};
	}
	else
	{
		tokens[n++] = {1, secs, secs >= 10 ? MHUD_DIGIT_WIDTH * 2 : MHUD_DIGIT_WIDTH};
	}
	tokens[n++] = {2, MHUD_TIMER_DELIM_DOT, MHUD_DELIM_WIDTH};
	const i32 centis = detailed ? frac / 10 : frac;
	tokens[n++] = {1, centis < 10 ? centis + 100 : centis, MHUD_DIGIT_WIDTH * 2};
	if (detailed)
	{
		tokens[n++] = {1, frac % 10, MHUD_DIGIT_WIDTH};
	}

	const f32 size = Max(timerSize * MHUD_DELTA_SCALE, 0.001f);
	const bool ahead = delta < 0.0;
	const Color color = ahead ? ParticleColor(prefs.deltaAhead, MHUD_DEF_DELTA_AHEAD_COLOR) : ParticleColor(prefs.deltaBehind, MHUD_DEF_DELTA_BEHIND_COLOR);

	char numbersPath[256], delimPath[256], signPath[256];
	BuildParticlePath(numbersPath, sizeof(numbersPath), MHUD_PARTICLE_NUMBERS_PATTERN, MHUD_PARTICLE_FONT, outline);
	BuildParticlePath(delimPath, sizeof(delimPath), MHUD_PARTICLE_TIMER_DELIM_PATTERN, MHUD_PARTICLE_FONT, outline);
	BuildParticlePath(signPath, sizeof(signPath), MHUD_PARTICLE_SIGN_PATTERN, MHUD_PARTICLE_FONT, outline);
	EnsureGroup(this->mhudDeltaSignParticles, KZ_ARRAYSIZE(this->mhudDeltaSignParticles), this->mhudParticleVariant[5], outline, signPath, color,
				size, 0.0f, 0.0f);
	EnsureGroup(this->mhudDeltaDigitParticles, KZ_ARRAYSIZE(this->mhudDeltaDigitParticles), this->mhudParticleVariant[6], outline, numbersPath,
				color, size, 0.0f, 0.0f);
	EnsureGroup(this->mhudDeltaDelimParticles, KZ_ARRAYSIZE(this->mhudDeltaDelimParticles), this->mhudParticleVariant[7], outline, delimPath,
				color, size, 0.0f, 0.0f);

	// Левый край дельты — правый край таймера + зазор; координаты частицы — в единицах её размера.
	f32 cursor = (timerRightEdge + MHUD_DELTA_GAP * timerSize) / size;
	const f32 y = timerY / size;
	i32 usedDigits = 0, usedDelims = 0;
	for (i32 i = 0; i < n; i++)
	{
		const Token &t = tokens[i];
		CParticleSystem *p = NULL;
		if (t.kind == 0)
		{
			p = this->mhudDeltaSignParticles[0].Get();
		}
		else if (t.kind == 1)
		{
			p = this->mhudDeltaDigitParticles[usedDigits++].Get();
		}
		else
		{
			p = this->mhudDeltaDelimParticles[usedDelims++].Get();
		}
		if (p)
		{
			SetParticleLayout(p, (f32)t.seq, size, cursor + t.width * 0.5f, y);
			SetParticleTint(p, color);
			p->Start();
		}
		cursor += t.width;
	}
	for (i32 i = usedDigits; i < (i32)KZ_ARRAYSIZE(this->mhudDeltaDigitParticles); i++)
	{
		if (this->mhudDeltaDigitParticles[i].Get())
		{
			this->mhudDeltaDigitParticles[i].Get()->Destroy();
		}
	}
	for (i32 i = usedDelims; i < (i32)KZ_ARRAYSIZE(this->mhudDeltaDelimParticles); i++)
	{
		if (this->mhudDeltaDelimParticles[i].Get())
		{
			this->mhudDeltaDelimParticles[i].Get()->Destroy();
		}
	}
}

// === Клавиши ========================================================================

void KZHUDService::UpdateMHUDKeysParticle()
{
	const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
	const MHUDLayoutPrefs::Element &el = prefs.elements[(i32)LayoutElement::Keys];
	if (!el.enabled)
	{
		DestroyGroup(&this->mhudKeysParticle, 1, this->mhudParticleVariant[4]);
		return;
	}
	const MHUDParticleGeom g = ParticleGeom(el, kz_mhud_particle_keys_size.Get());
	const Color baseColor = ParticleColor(prefs.keys, MHUD_DEF_BASE_COLOR);
	char inputsPath[256];
	BuildParticlePath(inputsPath, sizeof(inputsPath), MHUD_PARTICLE_INPUTS_PATTERN, MHUD_PARTICLE_FONT, el.outline);
	EnsureGroup(&this->mhudKeysParticle, 1, this->mhudParticleVariant[4], el.outline, inputsPath, baseColor, g.size, g.x, g.y);
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

	p->SetControlPointValue(17, Vector((f32)mask, g.size, 0.0f));
	p->SetControlPointValue(18, Vector(g.x, g.y, 1.0f));
	p->Start();

	bool overlap = (forward && back) || (left && right);
	SetParticleTint(p, overlap && prefs.keysOverlapEnabled ? ParticleColor(prefs.keysOverlap, MHUD_DEF_KEYS_OVERLAP_COLOR) : baseColor);
}

void KZHUDService::UpdateParticles()
{
	// Только живому владельцу (DrawPanels гейтит player == target), mhudSource не задаём:
	// данные — this->player, настройки — GetLayoutPrefs() (с мимикрией, как у panorama).
	this->particlesActive = true;
	this->UpdateMHUDSpeedParticles();
	this->UpdateMHUDTimerParticles();
	this->UpdateMHUDKeysParticle();
}
