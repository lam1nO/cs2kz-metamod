// Panorama-реплика собственного прицела игрока (Task 10). Перенесено с апстрима
// (origin/master:src/kz/hud/layout/crosshair.cpp), расхождения — см.
// docs/superpowers/sdd/2026-09-08-panorama-hud/base-facts.md (R1/R2/R4) и ниже:
//   - апстрим читает cl_crosshair* через свой cvarquery (введён ПОЗЖЕ, чем наша база branch'
//     нулась от апстрима, — base-facts.md этого расхождения не описывал); у нас клиентский
//     cvar читается тем же механизмом, что anticheat/detectors/cvars.cpp и kz_language.cpp:
//     IClientCvarValue (g_pClientCvarValue) с колбэком ECvarValueStatus/CvarValueCallback.
//   - GetPreferenceColor/ResolveSwatchClass в базе нет — цвет резолвится panorama::ResolveColorClass
//     (см. entity.cpp/UpdateLayoutElement, тот же резолвер).
//   - this->GetLayoutPrefs() вместо GetPrefs() (R4), mhudCrosshair/mhudCrosshairScale читаются
//     в prefs.cpp (RefreshLayoutPrefs), не здесь.

// Geometry from client.dll's painter, classic static only (no recoil, friendly-fire warning or
// weapon-based gap):
//   length    = int(screenHeight / 480 * cl_crosshairsize)
//   thickness = max(1, int(screenHeight / 480 * cl_crosshairthickness))
//   gap       = int(cl_crosshairgap + 4)              raw pixels, not screen-scaled
//   arm       = [center + thickness/2 + gap, + length]
//   dot       = thickness-sized square; cl_crosshair_t drops the top arm
//   alpha     = cl_crosshairusealpha ? cl_crosshairalpha : 200

#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/panorama_tables.h"
#include "kz/option/kz_option.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "utils/ctimer.h"
#include <vendor/ClientCvarValue/public/iclientcvarvalue.h>

#include "tier0/memdbgon.h"

extern IClientCvarValue *g_pClientCvarValue;

// Panorama's 1080px reference over the client's 480px crosshair scale. Exact at 1080p; elsewhere
// mhudCrosshairScale carries the correction, since no convar reports the client's resolution.
#define MHUD_XH_SCALE     2.25f
#define MHUD_XH_MIN_SCALE 25
#define MHUD_XH_MAX_SCALE 400
// Largest suffix xh-w--/xh-h-- define.
#define MHUD_XH_MAX_PX 56
// xh-m--N is a margin of N quarter pixels, biased so arms can cross the centre. Quarters because a
// whole layout unit is coarser than a device pixel wherever the scale is not 100%.
#define MHUD_XH_MARGIN_BIAS 8
#define MHUD_XH_MAX_MARGIN  24
#define MHUD_XH_MARGIN_STEP 4
// The game caps cl_crosshair_outlinethickness at 3, in raw pixels; scaling up needs more classes.
#define MHUD_XH_MAX_OUTLINE    3
#define MHUD_XH_MAX_OUTLINE_PX 8
// Opacity classes are 5% steps.
#define MHUD_XH_OPACITY_STEPS 20
#define MHUD_XH_POLL_INTERVAL 2.5f

// Alpha goes on each painted panel, not the container: parent opacity does not reach children. The
// border carrying the outline is part of the same panel, so it fades with the bar as the game does.
static_global const char *const XH_PAINTED[] = {"xh_left", "xh_right", "xh_top", "xh_bottom", "xh_dot"};
// The centre pixel belongs to neither bar, so the far arms sit one device pixel further out.
static_global const char *const XH_ARMS_NEAR[] = {"xh_left", "xh_top"};
static_global const char *const XH_ARMS_FAR[] = {"xh_right", "xh_bottom"};
static_global const char *const XH_HORIZONTAL[] = {"xh_left", "xh_right"};
static_global const char *const XH_VERTICAL[] = {"xh_top", "xh_bottom"};
static_global const char *const XH_TINTED[] = {"xh_left", "xh_right", "xh_top", "xh_bottom", "xh_dot"};
static_global const char *const XH_DOT[] = {"xh_dot"};

// === Чтение клиентских cvar'ов ======================================================

struct MHUDCrosshairCvar
{
	const char *name;
	void (*apply)(MHUDCrosshairSettings &settings, const char *value);
};

// Бул-cvar'ы приходят строками "true"/"false", а не 0/1.
static_function bool ParseBool(const char *value)
{
	return V_stricmp(value, "true") == 0 || V_stricmp(value, "yes") == 0 || atof(value) != 0.0;
}

// clang-format off
static_global const MHUDCrosshairCvar CROSSHAIR_CVARS[] = {
	{"cl_crosshairsize",             [](MHUDCrosshairSettings &s, const char *v) { s.size = (f32)atof(v); }},
	{"cl_crosshairthickness",        [](MHUDCrosshairSettings &s, const char *v) { s.thickness = (f32)atof(v); }},
	{"cl_crosshairgap",              [](MHUDCrosshairSettings &s, const char *v) { s.gap = (f32)atof(v); }},
	{"cl_crosshair_outlinethickness",[](MHUDCrosshairSettings &s, const char *v) { s.outlineThickness = (f32)atof(v); }},
	{"cl_crosshair_drawoutline",     [](MHUDCrosshairSettings &s, const char *v) { s.drawOutline = ParseBool(v); }},
	{"cl_crosshairdot",              [](MHUDCrosshairSettings &s, const char *v) { s.dot = ParseBool(v); }},
	{"cl_crosshair_t",               [](MHUDCrosshairSettings &s, const char *v) { s.tStyle = ParseBool(v); }},
	{"cl_crosshaircolor",            [](MHUDCrosshairSettings &s, const char *v) { s.color = atoi(v); }},
	{"cl_crosshaircolor_r",          [](MHUDCrosshairSettings &s, const char *v) { s.r = atoi(v); }},
	{"cl_crosshaircolor_g",          [](MHUDCrosshairSettings &s, const char *v) { s.g = atoi(v); }},
	{"cl_crosshaircolor_b",          [](MHUDCrosshairSettings &s, const char *v) { s.b = atoi(v); }},
	{"cl_crosshairalpha",            [](MHUDCrosshairSettings &s, const char *v) { s.alpha = atoi(v); }},
	{"cl_crosshairusealpha",         [](MHUDCrosshairSettings &s, const char *v) { s.useAlpha = ParseBool(v); }},
};
// clang-format on

static_function void OnCrosshairCvarQueried(CPlayerSlot nSlot, ECvarValueStatus eStatus, const char *pszCvarName, const char *pszCvarValue)
{
	if (eStatus != ECvarValueStatus::ValueIntact)
	{
		return;
	}
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(nSlot);
	if (player && player->IsInGame())
	{
		player->hudService->OnCrosshairCvarValue(pszCvarName, pszCvarValue);
	}
}

void KZHUDService::QueryCrosshairCvars()
{
	if (!g_pClientCvarValue || this->player->IsFakeClient() || this->player->IsCSTV())
	{
		return;
	}
	for (const MHUDCrosshairCvar &cvar : CROSSHAIR_CVARS)
	{
		g_pClientCvarValue->QueryCvarValue(this->player->GetPlayerSlot(), cvar.name, OnCrosshairCvarQueried);
	}
}

static_function f64 PollCrosshairCvars(CPlayerUserId userID)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
	if (!player)
	{
		return 0.0f;
	}
	// Настройки никто не читает, пока крестик выключен, но таймер держим взведённым всё
	// равно — включение обязано подхватить актуальные cl_crosshair* без нового коннекта.
	if (player->IsInGame() && player->optionService->GetPreferenceBool("mhudCrosshair", false))
	{
		player->hudService->QueryCrosshairCvars();
	}
	return MHUD_XH_POLL_INTERVAL;
}

void KZHUDService::StartCrosshairPolling()
{
	if (this->player->IsFakeClient() || this->player->IsCSTV() || !this->player->GetClient())
	{
		return;
	}
	this->crosshair = MHUDCrosshairSettings();
	this->QueryCrosshairCvars();
	StartTimer<CPlayerUserId>(PollCrosshairCvars, this->player->GetClient()->GetUserID(), MHUD_XH_POLL_INTERVAL, true, true);
}

void KZHUDService::OnCrosshairCvarValue(const char *name, const char *value)
{
	for (const MHUDCrosshairCvar &cvar : CROSSHAIR_CVARS)
	{
		if (V_strcmp(cvar.name, name) == 0)
		{
			cvar.apply(this->crosshair, value);
			// Первый живой ответ клиента — с этого момента this->crosshair это НАСТОЯЩИЕ
			// cl_crosshair* игрока, а не хардкод-дефолты конструктора; ApplyCrosshair до этого
			// флага крестик не рисует вовсе (см. комментарий у MHUDCrosshairSettings::confirmed).
			this->crosshair.confirmed = true;
			return;
		}
	}
}

// === Отрисовка =======================================================================

// cl_crosshaircolor 0..4 — пресеты; 5 значит "смотреть в cl_crosshaircolor_*".
static_function Color GetCrosshairColor(const MHUDCrosshairSettings &settings)
{
	switch (settings.color)
	{
		case 0:
			return Color(250, 50, 50, 255);
		case 2:
			return Color(250, 250, 50, 255);
		case 3:
			return Color(50, 50, 250, 255);
		case 4:
			return Color(50, 250, 250, 255);
		case 5:
			return Color(Clamp(settings.r, 0, 255), Clamp(settings.g, 0, 255), Clamp(settings.b, 0, 255), 255);
		default:
			return Color(50, 250, 50, 255);
	}
}

// Переносит одно числовое класс-семейство с oldValue на newValue на всех перечисленных панелях.
// Кэш держит вызывающий: одно значение может водить сразу два семейства.
static_function void ApplyValueClass(CCSCustomHudLayout *layout, const char *const *panels, i32 count, const char *prefix, i32 oldValue, i32 newValue)
{
	if (oldValue == newValue)
	{
		return;
	}
	char className[32];
	for (i32 i = 0; i < count; i++)
	{
		if (oldValue >= 0)
		{
			V_snprintf(className, sizeof(className), "%s%i", prefix, oldValue);
			layout->SetHasClass(panels[i], className, k_eHudPanelClassStatus_DoesNotHaveClass);
		}
		V_snprintf(className, sizeof(className), "%s%i", prefix, newValue);
		layout->SetHasClass(panels[i], className, k_eHudPanelClassStatus_HasClass);
	}
}

// Игра считает в device-пикселях. Один device-пиксель — `scale` единиц раскладки.
static_function i32 ToLayout(i32 devicePixels, f32 scale)
{
	return (i32)(devicePixels * scale + 0.5f);
}

// Та же конвертация, но в класс-индекс четверть-пикселя, в котором сгенерированы margin-классы.
static_function i32 ToMarginClass(i32 devicePixels, f32 scale, i32 outline)
{
	const i32 quarters = (i32)(devicePixels * scale * MHUD_XH_MARGIN_STEP + 0.5f) - outline * MHUD_XH_MARGIN_STEP;
	return Clamp(quarters, -MHUD_XH_MARGIN_BIAS * MHUD_XH_MARGIN_STEP, MHUD_XH_MAX_MARGIN * MHUD_XH_MARGIN_STEP)
		   + MHUD_XH_MARGIN_BIAS * MHUD_XH_MARGIN_STEP;
}

static_function void ApplyFlagClass(CCSCustomHudLayout *layout, const char *panelId, const char *className, i32 &cache, bool set)
{
	if (cache == (i32)set)
	{
		return;
	}
	cache = (i32)set;
	layout->SetHasClass(panelId, className, set ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass);
}

void KZHUDService::ApplyCrosshair(CCSCustomHudLayout *layout, bool show, bool force)
{
	LayoutCrosshairState &state = this->layoutCrosshair;
	if (force)
	{
		// force — сущность только что заспавнена (EnsureOwnedLayout), см. комментарий у
		// DestroyOwnedLayout: кэш обязан сброситься вместе с ней, иначе первый кадр решит,
		// что xh-* классы уже стоят, и крестик молча не появится.
		state = LayoutCrosshairState();
	}

	// this->crosshair.confirmed — клиент ещё не ответил ни на один cl_crosshair* (нет
	// ClientCvarValue на сервере, свежий коннект, ответ в пути): без гейта крестик красился бы
	// хардкод-дефолтами конструктора MHUDCrosshairSettings, выдавая их за настройки игрока —
	// «чужой» крестик хуже отсутствующего, поэтому до подтверждения ведём себя как при
	// выключенном префе (ни классов, ни панелей не трогаем).
	const bool enabled = show && this->GetLayoutPrefs().crosshair && this->crosshair.confirmed;
	ApplyFlagClass(layout, "mhud_crosshair", "hidden", state.shown, !enabled);
	if (!enabled)
	{
		// Спрятанный крестик классы не теряет — включить обратно ничего не стоит.
		return;
	}

	const MHUDCrosshairSettings &settings = this->crosshair;
	const f32 scale = Clamp(this->GetLayoutPrefs().crosshairScale, MHUD_XH_MIN_SCALE, MHUD_XH_MAX_SCALE) / 100.0f;
	// Всё, что игра считает от высоты экрана, — в device-пикселях, которые она бы нарисовала.
	const f32 screenScale = MHUD_XH_SCALE / scale;
	const i32 lengthDev = (i32)(screenScale * settings.size);
	const i32 thicknessDev = MAX(1, (i32)(screenScale * settings.thickness));
	// Зазор и обводка — сырые device-пиксели, экранным масштабом не умножаются.
	const i32 gapDev = (i32)(settings.gap + 4.0f);
	const i32 outlineDev = settings.drawOutline ? Clamp((i32)(settings.outlineThickness + 0.5f), 0, MHUD_XH_MAX_OUTLINE) : 0;

	const i32 armLength = Clamp(ToLayout(lengthDev, scale), 0, MHUD_XH_MAX_PX);
	const i32 thickness = Clamp(MAX(1, ToLayout(thicknessDev, scale)), 1, MHUD_XH_MAX_PX);
	const i32 outline = Clamp(ToLayout(outlineDev, scale), 0, MHUD_XH_MAX_OUTLINE_PX);
	// Обводка рисуется ВНУТРИ бокса, поэтому размер бара считаем с ней, а margin утягиваем
	// на ту же величину — снаружи должно остаться ровно armLength x thickness, как без обводки.
	const i32 boxLength = Clamp(armLength + 2 * outline, 0, MHUD_XH_MAX_PX);
	const i32 boxThickness = Clamp(thickness + 2 * outline, 1, MHUD_XH_MAX_PX);
	const i32 innerDev = thicknessDev / 2 + gapDev;
	const i32 margin = ToMarginClass(innerDev, scale, outline);
	const i32 marginFar = ToMarginClass(innerDev + 1, scale, outline);
	// Игра красит обводку тем же альфа-каналом, что и сами бары.
	const i32 alpha = Clamp(settings.useAlpha ? settings.alpha : 200, 0, 255);
	const i32 opacity = alpha * MHUD_XH_OPACITY_STEPS / 255;
	const char *colorClass = panorama::ResolveColorClass(GetCrosshairColor(settings));

	ApplyValueClass(layout, XH_HORIZONTAL, KZ_ARRAYSIZE(XH_HORIZONTAL), "xh-w--", state.armLength, boxLength);
	ApplyValueClass(layout, XH_VERTICAL, KZ_ARRAYSIZE(XH_VERTICAL), "xh-h--", state.armLength, boxLength);
	state.armLength = boxLength;

	ApplyValueClass(layout, XH_HORIZONTAL, KZ_ARRAYSIZE(XH_HORIZONTAL), "xh-h--", state.thickness, boxThickness);
	ApplyValueClass(layout, XH_VERTICAL, KZ_ARRAYSIZE(XH_VERTICAL), "xh-w--", state.thickness, boxThickness);
	ApplyValueClass(layout, XH_DOT, KZ_ARRAYSIZE(XH_DOT), "xh-w--", state.thickness, boxThickness);
	ApplyValueClass(layout, XH_DOT, KZ_ARRAYSIZE(XH_DOT), "xh-h--", state.thickness, boxThickness);
	state.thickness = boxThickness;

	ApplyValueClass(layout, XH_ARMS_NEAR, KZ_ARRAYSIZE(XH_ARMS_NEAR), "xh-m--", state.margin, margin);
	state.margin = margin;

	ApplyValueClass(layout, XH_ARMS_FAR, KZ_ARRAYSIZE(XH_ARMS_FAR), "xh-m--", state.marginFar, marginFar);
	state.marginFar = marginFar;

	ApplyValueClass(layout, XH_PAINTED, KZ_ARRAYSIZE(XH_PAINTED), "xh-ol--", state.outline, outline);
	state.outline = outline;

	ApplyValueClass(layout, XH_PAINTED, KZ_ARRAYSIZE(XH_PAINTED), "xh-op--", state.opacity, opacity);
	state.opacity = opacity;

	if (state.colorClass != colorClass)
	{
		for (const char *panelId : XH_TINTED)
		{
			if (state.colorClass)
			{
				layout->SetHasClass(panelId, state.colorClass, k_eHudPanelClassStatus_DoesNotHaveClass);
			}
			layout->SetHasClass(panelId, colorClass, k_eHudPanelClassStatus_HasClass);
		}
		state.colorClass = colorClass;
	}

	ApplyFlagClass(layout, "xh_dot", "hidden", state.dot, !settings.dot);
	if (state.noTopArm != (i32)settings.tStyle)
	{
		state.noTopArm = (i32)settings.tStyle;
		const auto status = settings.tStyle ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass;
		layout->SetHasClass("xh_top", "hidden", status);
	}
}
