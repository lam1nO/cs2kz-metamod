// Префы panorama-худа (Task 5): читает LAYOUT_ELEMENTS (Task 4, entity.cpp) и наполняет
// MHUDLayoutPrefs. Таблицу LAYOUT_ELEMENTS здесь НЕ определяем — второе определение того же
// символа сломало бы линковку, она уже определена в entity.cpp и объявлена extern в kz_hud.h.
#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/panorama_tables.h"

#include "tier0/memdbgon.h"

const MHUDLayoutPrefs &KZHUDService::GetLayoutPrefs()
{
	return this->layoutPrefs;
}

void KZHUDService::RefreshLayoutPrefs()
{
	// Источник настроек — как у остального худа: наблюдаемый при спектейте, иначе сам игрок.
	auto *opts = this->MHUDSettingsSource()->optionService;
	for (i32 e = 0; e < (i32)LayoutElement::Count; e++)
	{
		const LayoutElementDef &def = LAYOUT_ELEMENTS[e];
		MHUDLayoutPrefs::Element &element = this->layoutPrefs.elements[e];
		element.enabled = opts->GetPreferenceBool(def.enabledKey, true);
		element.x = panorama::SnapToStep((i32)opts->GetPreferenceFloat(def.xKey, (f32)def.xDefault), -100, 100);
		element.y = panorama::SnapToStep((i32)opts->GetPreferenceFloat(def.yKey, (f32)def.yDefault), -100, 100);
		element.size = panorama::SnapToStep((i32)opts->GetPreferenceFloat(def.sizeKey, (f32)def.sizeDefault), LAYOUT_SIZE_MIN, LAYOUT_SIZE_MAX);
		// Шрифт разрешается в css-класс уже здесь: UpdateLayoutElement (Task 4) кладёт его на
		// панель напрямую, повторный резолв в геймтике не нужен и не делается.
		element.fontClass = panorama::ResolveFontClass(opts->GetPreferenceStr(def.fontKey, LAYOUT_DEFAULT_FONT), LAYOUT_DEFAULT_FONT);
		// outlineKey у всех элементов один и тот же общий тумблер ("hudOutline") — поэлементных
		// mhud*Outline в нашей базе нет, апстримную россыпь не заводим.
		element.outline = opts->GetPreferenceBool(def.outlineKey, true);
		element.opacity = Clamp((i32)opts->GetPreferenceInt(def.opacityKey, 100), 0, 100);
	}

	// Цвета — ОБЩИЕ с particle-путём: ключи префов совпадают, поэтому настройки игроков
	// переезжают между путями сами (см. дефолты в kz_hud.h). GetPreferenceColor в нашей базе
	// нет — цвет читается GetMHUDColorPref (преф хранит упакованный int).
	this->layoutPrefs.timerPro = this->GetMHUDColorPref("mhudTimerProColor", MHUD_DEF_TIMER_PRO_COLOR);
	this->layoutPrefs.timerTp = this->GetMHUDColorPref("mhudTimerTpColor", MHUD_DEF_TIMER_TP_COLOR);
	this->layoutPrefs.timerPaused = this->GetMHUDColorPref("mhudTimerPausedColor", MHUD_DEF_TIMER_PAUSED_COLOR);
	this->layoutPrefs.timerStopped = this->GetMHUDColorPref("mhudTimerStoppedColor", MHUD_DEF_TIMER_STOPPED_COLOR);
	this->layoutPrefs.speed = this->GetMHUDColorPref("mhudSpeedColor", MHUD_DEF_BASE_COLOR);
	this->layoutPrefs.speedCj = this->GetMHUDColorPref("mhudSpeedCjColor", MHUD_DEF_CJ_COLOR);
	this->layoutPrefs.prespeed = this->GetMHUDColorPref("mhudPrespeedColor", MHUD_DEF_BASE_COLOR);
	this->layoutPrefs.prespeedPerf = this->GetMHUDColorPref("mhudPrespeedPerfColor", MHUD_DEF_PERF_COLOR);
	this->layoutPrefs.prespeedJumpbug = this->GetMHUDColorPref("mhudPrespeedJumpbugColor", MHUD_DEF_JUMPBUG_COLOR);
	this->layoutPrefs.keys = this->GetMHUDColorPref("mhudKeysColor", MHUD_DEF_BASE_COLOR);
	this->layoutPrefs.keysOverlap = this->GetMHUDColorPref("mhudKeysOverlapColor", MHUD_DEF_KEYS_OVERLAP_COLOR);
	this->layoutPrefs.checkpoint = this->GetMHUDColorPref("mhudCheckpointColor", MHUD_DEF_BASE_COLOR);

	this->layoutPrefs.timerDetailed = opts->GetPreferenceBool("hudTimerDetail", true);
	this->layoutPrefs.keysOverlapEnabled = opts->GetPreferenceBool("hudKeysOverlap", true);
	this->layoutPrefs.speedPrecise = opts->GetPreferenceBool("mhudSpeedPrecise", false);

	// Крестик (Task 10) — самостоятельный тумблер, не элемент LAYOUT_ELEMENTS: у него нет
	// текста/шрифта/позиции в процентах, только масштаб (crosshairScale, доли device-пикселя).
	this->layoutPrefs.crosshair = opts->GetPreferenceBool("mhudCrosshair", false);
	this->layoutPrefs.crosshairScale = panorama::SnapToStep((i32)opts->GetPreferenceInt("mhudCrosshairScale", 100), 0, 500);
}

bool KZHUDService::IsLayoutElementEnabled(LayoutElement element)
{
	return this->layoutPrefs.elements[(i32)element].enabled;
}
