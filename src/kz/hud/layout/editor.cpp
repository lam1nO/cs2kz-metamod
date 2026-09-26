// Редактор худа `!hud` (спека 2026-09-26-hud-editor-options §3.2/§4.5). Живёт на сущности окна
// !options (ownedMenuLayout, cyber/options.xml, корень edit_root): одна сущность, один режим в
// момент времени, тот же курсорный захват и те же пять путей его снятия (шапка layout/menu.cpp).
//
// Экран редактора:
//   - edit_hud — реплика худа: каждый элемент — кнопка e_<name> (позиция/прозрачность/показ,
//     класс sel у выбранного), внутри те же панели, что в mhud.xml, с префиксом x_ (текст, кегль,
//     шрифт, цвет, обводка). Классы ставит ТОТ ЖЕ код, что у настоящего худа
//     (ApplyLayoutElementTo/ApplyTimerDelta/ApplyKeysLook/ApplyPbWrCells/ApplyShowPosLines), со
//     своими кэшами editorElements/editorKeys/editorExtra. Тексты — плейсхолдеры (таймер идёт),
//     PB/WR, showpos и курс — настоящие, если есть.
//   - edit_grid — 64×36 кнопок g{c}_{r}: клик ставит ВЫБРАННЫЙ элемент центром в ячейку. Классов и
//     переменных на ячейки сервер не ставит никогда (бюджет интернирования, menu.cpp).
//   - edit_list — тумблеры элементов el{i}/et{i}, «Сбросить всё» (через confirm_popup), «Готово».
//   - edit_props — свойства выбранного: стрелки с шагом 1/5, кегль, шрифт (list_popup), цвет
//     (color_popup), цвета дельты (только таймер), прозрачность, обводка, сброс элемента; у
//     клавиш — ещё их вид (тумблеры ep_tg*, интервал ep_s_*, «в покое» ep_sg*).
//
// Позиции элементов хранятся в процентах ОТ ЦЕНТРА экрана (.element — центр + x/y классы
// positions.css, layout/prefs.cpp): видимая область — -50..50. Сетка отдаёт проценты от
// левого-верхнего угла (KZ::hudfmt::GridCellToPercent, 0..100) — перевод здесь, в EditorSetPos.
//
// Настоящий худ на время редактора снесён (DrawPanels гасит ownedLayout, пока editorOpen) и
// пересоздаётся с force на первом такте после закрытия — отдельный флаг «принудительно
// перерисовать» не нужен: свежая сущность и так пересылает всё.
#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/menu.h"
#include "kz/hud/layout/panorama_tables.h"
#include "kz/hud/hud_format.h"
#include "kz/hud/share/hud_share.h"
#include "kz/option/kz_option.h"
#include "kz/option/menu/model.h"
#include "kz/language/kz_language.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "utils/utils.h"
#include "utils/logging.h"
#include "cs2kz.h"

#include "tier0/memdbgon.h"

// Реплика элемента в cyber/options.xml. Порядок = LayoutElement (Timer, Speed, Prespeed, Keys,
// Checkpoint, LeadProgress, PbWr, ShowPos, Course, RunType) — он же порядок el{i}/et{i}.
struct EditorReplicaDef
{
	const char *buttonId; // e_<name>: позиция, прозрачность, hidden, sel
	const char *labelId;  // x_<панель mhud.xml>: текст/кегль/шрифт/цвет/обводка
	const char *varName;
	const char *tagVar;  // {s:tag_*} — ярлык над рамкой
	const char *nameKey; // фраза имени элемента (ярлык, список, заголовок панели свойств)
};

// clang-format off
static_global const EditorReplicaDef EDITOR_REPLICA[(i32)LayoutElement::Count] =
{
	{"e_timer",        "x_mhud_timer",        "timer",        "tag_timer",        "HUD Editor - Element Timer"},
	{"e_speed",        "x_mhud_speed",        "speed",        "tag_speed",        "HUD Editor - Element Speed"},
	{"e_prespeed",     "x_mhud_prespeed",     "prespeed",     "tag_prespeed",     "HUD Editor - Element Prespeed"},
	{"e_keys",         "x_mhud_keys",         "keys",         "tag_keys",         "HUD Editor - Element Keys"},
	{"e_checkpoint",   "x_mhud_checkpoint",   "checkpoint",   "tag_checkpoint",   "HUD Editor - Element Checkpoint"},
	{"e_leadprogress", "x_mhud_progress_pct", "progress",     "tag_leadprogress", "HUD Editor - Element LeadProgress"},
	{"e_pbwr",         "x_mhud_pbwr",         "pbwr",         "tag_pbwr",         "HUD Editor - Element PbWr"},
	{"e_showpos",      "x_mhud_showpos",      "pos",          "tag_showpos",      "HUD Editor - Element ShowPos"},
	{"e_course",       "x_mhud_course",       "course",       "tag_course",       "HUD Editor - Element Course"},
	{"e_runtype",      "x_mhud_runtype",      "runtype",      "tag_runtype",      "HUD Editor - Element RunType"},
};
// clang-format on

static_assert(KZ_ARRAYSIZE(EDITOR_REPLICA) == KZ_EDITOR_ITEMS, "el{i}/et{i} разметки — по одному на LayoutElement");

// Доп. цвета элемента в панели свойств: строки ep_xrow1..3 (подпись {s:px1..3}, свотч ep_xc1..3).
// Это состояния элемента, у которых свой цвет помимо основного (ep_color): таймер с ТП/на
// паузе/остановлен, скорость на кроуч-джампе, престрейф perf/jumpbug, клавиши при перекрытии и
// нажатые. До редактора их правила страница элемента в окне; после ужатия раздела пункты
// остались только в скрытых узлах — без этих строк у игрока не было способа их поменять.
// Клик открывает тот же попап цвета, что ep_color/ep_dcol_* (цель — пункт реестра по ключу).
struct EditorExtraColorDef
{
	const char *prefKey;
	const char *phraseKey;
	const Color *def;
};

static_global const char *const EDITOR_XROW_IDS[3] = {"ep_xrow1", "ep_xrow2", "ep_xrow3"};
static_global const char *const EDITOR_XROW_VARS[3] = {"px1", "px2", "px3"};
static_global const char *const EDITOR_XC_IDS[3] = {"ep_xc1", "ep_xc2", "ep_xc3"};

// clang-format off
static_global const EditorExtraColorDef EDITOR_EXTRA_COLORS[(i32)LayoutElement::Count][3] =
{
	/* Timer */      {{"mhudTimerTpColor", "HUD - Menu Label TpColor", &MHUD_DEF_TIMER_TP_COLOR},
	                  {"mhudTimerPausedColor", "HUD - Menu Label PausedColor", &MHUD_DEF_TIMER_PAUSED_COLOR},
	                  {"mhudTimerStoppedColor", "HUD - Menu Label StoppedColor", &MHUD_DEF_TIMER_STOPPED_COLOR}},
	/* Speed */      {{"mhudSpeedCjColor", "HUD - Menu Label CjColor", &MHUD_DEF_CJ_COLOR}},
	/* Prespeed */   {{"mhudPrespeedPerfColor", "HUD - Menu Label PerfColor", &MHUD_DEF_PERF_COLOR},
	                  {"mhudPrespeedJumpbugColor", "HUD - Menu Label JumpbugColor", &MHUD_DEF_JUMPBUG_COLOR}},
	/* Keys */       {{"mhudKeysOverlapColor", "HUD - Menu Label OverlapColor", &MHUD_DEF_KEYS_OVERLAP_COLOR},
	                  {"mhudKeysPressedColor", "HUD - Menu Label PressedColor", &MHUD_DEF_KEYS_PRESSED_COLOR},
	                  {"mhudKeysOverlapGlowColor", "HUD - Menu Label OverlapGlowColor", &MHUD_DEF_KEYS_OVERLAP_GLOW_COLOR}},
	/* Checkpoint, LeadProgress, PbWr, ShowPos, Course, RunType — доп. цветов нет */
};
// clang-format on

// Вид клавиш в панели свойств (решение владельца 26.09: страница «Клавиши» ушла из !options в
// редактор, где изменение сразу видно на реплике). Тумблеры — строки ep_trow1..8 по порядку
// таблицы, лишние строки скрыты. gate — тумблер, без которого строка серая и клик мимо (как
// enabledBy пункта реестра). Ключи и дефолты — те же, что у пунктов в узле элемента
// (hud_prefs.cpp) и у RefreshLayoutPrefs.
struct EditorKeysToggleDef
{
	const char *prefKey;
	const char *phraseKey;
	bool def;
	const char *gate;
};

#define KZ_EDITOR_TROWS 8
static_global const char *const EDITOR_TROW_IDS[KZ_EDITOR_TROWS] = {"ep_trow1", "ep_trow2", "ep_trow3", "ep_trow4",
																	 "ep_trow5", "ep_trow6", "ep_trow7", "ep_trow8"};
static_global const char *const EDITOR_TG_IDS[KZ_EDITOR_TROWS] = {"ep_tg1", "ep_tg2", "ep_tg3", "ep_tg4", "ep_tg5", "ep_tg6", "ep_tg7", "ep_tg8"};
static_global const char *const EDITOR_TROW_VARS[KZ_EDITOR_TROWS] = {"pt1", "pt2", "pt3", "pt4", "pt5", "pt6", "pt7", "pt8"};
static_global const char *const EDITOR_SG_IDS[3] = {"ep_sg0", "ep_sg1", "ep_sg2"};
static_global const char *const EDITOR_SG_VARS[3] = {"pg0", "pg1", "pg2"};

// clang-format off
static_global const EditorKeysToggleDef EDITOR_KEYS_TOGGLES[] =
{
	{"mhudKeysLetters",     "HUD - Menu Label Letters",       true,  NULL},
	{"mhudKeysSquare",      "HUD Editor - Prop KeysSquare",   true,  NULL},
	{"mhudKeysBorder",      "HUD - Menu Label Border",        false, NULL},
	{"mhudKeysGlow",        "HUD - Menu Label Glow",          false, NULL},
	{"mhudKeysFill",        "HUD - Menu Label Fill",          false, NULL},
	{"hudKeysOverlap",      "HUD - Menu Label Overlap",       true,  NULL},
	{"mhudKeysOverlapAxis", "HUD Editor - Prop KeysAxisOnly", false, "hudKeysOverlap"},
};
// clang-format on
static_assert(KZ_ARRAYSIZE(EDITOR_KEYS_TOGGLES) <= KZ_EDITOR_TROWS, "строк ep_trow в разметке восемь");

// Престрейф: бейджи PERF/JB/CJ (решение владельца 26.09) — тем же тумблером ep_trow1.
static_global const EditorKeysToggleDef EDITOR_PRESPEED_TOGGLES[] =
{
	{"hudIndicators", "HUD Editor - Prop Indicators", true, NULL},
};

// Тумблеры панели свойств выбранного элемента; у остальных элементов строк ep_trow нет.
static_function const EditorKeysToggleDef *GetEditorToggles(i32 element, i32 &count)
{
	switch ((LayoutElement)element)
	{
		case LayoutElement::Keys:
			count = (i32)KZ_ARRAYSIZE(EDITOR_KEYS_TOGGLES);
			return EDITOR_KEYS_TOGGLES;
		case LayoutElement::Prespeed:
			count = (i32)KZ_ARRAYSIZE(EDITOR_PRESPEED_TOGGLES);
			return EDITOR_PRESPEED_TOGGLES;
		default:
			count = 0;
			return NULL;
	}
}

// Порядок сегментов = значение mhudKeysIdle (0 показ, 1 скрыть, 2 подчёркивание).
static_global const char *const EDITOR_KEYS_IDLE_PHRASES[3] = {"HUD Editor - Prop KeysIdleShow", "HUD Editor - Prop KeysIdleHide",
															  "HUD Editor - Prop KeysIdleUnderscore"};

// Видимая область экрана в процентах от центра (см. шапку).
#define KZ_EDITOR_POS_MIN (-50)
#define KZ_EDITOR_POS_MAX 50
// Плейсхолдер таймера стартует с 01:23.456 и идёт по серверному времени (§4.5).
#define KZ_EDITOR_TIMER_START 83.456
#define KZ_EDITOR_DELTA_PLACEHOLDER 0.312
#define KZ_EDITOR_PROGRESS_PLACEHOLDER 38
#define KZ_EDITOR_PROGRESS_TEXT "38%"
// Панель свойств уезжает влево (flip), если выбранный элемент в правой нижней части экрана:
// x > 60% и y > 60% от левого-верхнего угла = больше 10 от центра.
#define KZ_EDITOR_FLIP_FROM 10

#define SLOT_ID(fn, fmt) \
	static_function const char *fn(i32 i) \
	{ \
		static_persist char buf[16]; \
		V_snprintf(buf, sizeof(buf), fmt, i); \
		return buf; \
	}

SLOT_ID(ElPanel, "el%i")
SLOT_ID(EtPanel, "et%i")
SLOT_ID(EnVar, "en%i")
#undef SLOT_ID

// === Открытие / закрытие ======================================================================

void KZHUDService::OpenHudEditor()
{
	if (this->editorOpen)
	{
		return;
	}
	// Одна сущность — один режим (§4.5): окно закрывается, захват переходит редактору.
	if (this->menuOpen)
	{
		this->CloseLayoutMenu();
	}
	this->RecycleMenuLayoutIfFull();
	bool created = false;
	CCSCustomHudLayout *layout = this->EnsureMenuLayout(created);
	if (!layout)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] hud_editor_open_denied reason=%s slot=%i\n",
					KZHUDService::IsHudLayoutKillSwitchOff() ? "hud_layout_disabled" : "mhud_unavailable", this->player->GetPlayerSlot().Get());
		this->player->languageService->PrintChat(true, false, "MHUD - Unavailable");
		return;
	}
	if (!layout->GetPlayerLayoutState(this->player->GetPlayerSlot()))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] hud_editor_open_denied reason=no_player_layout_state slot=%i\n", this->player->GetPlayerSlot().Get());
		this->player->languageService->PrintChat(true, false, "MHUD - Unavailable");
		return;
	}
	this->editorOpen = true;
	this->editorSelected = -1;
	this->editorStep = 1;
	this->editorOpenedAt = g_pKZUtils->GetServerGlobals()->curtime;
	this->menuPopup = MenuPopup::None;
	this->menuPopupTarget = NULL;
	this->menuConfirmTarget = NULL;
	// Настоящий худ под редактором не нужен (реплика стоит на его месте) — сносим сразу, DrawPanels
	// держит его снесённым, пока редактор открыт.
	this->DestroyOwnedLayout();
	// Переводит игрока в режим курсора — снятие на всех путях закрытия (шапка layout/menu.cpp).
	layout->SetInputCaptureEnabled(this->player->GetPlayerSlot(), true);
	this->RenderEditor();
}

void KZHUDService::CloseHudEditor()
{
	if (!this->editorOpen)
	{
		return;
	}
	const KZOptItem *it = this->menuPopupTarget;
	if (this->menuPopup != MenuPopup::None && this->menuPopup != MenuPopup::Confirm && it && it->onEdit)
	{
		it->onEdit(this->player, it->tag, false);
	}
	this->editorOpen = false;
	this->editorSelected = -1;
	this->menuPopup = MenuPopup::None;
	this->menuPopupTarget = NULL;
	this->menuConfirmTarget = NULL;
	if (CBaseEntity *ent = this->ownedMenuLayout.Get())
	{
		CCSCustomHudLayout *layout = (CCSCustomHudLayout *)ent;
		this->SetMenuBoolClass(layout, "edit_root", "hidden", this->menuApplied.editHidden, true);
		this->SetMenuBoolClass(layout, "color_popup", "hidden", this->menuApplied.colorPopupHidden, true);
		this->SetMenuBoolClass(layout, "list_popup", "hidden", this->menuApplied.listPopupHidden, true);
		this->SetMenuBoolClass(layout, "confirm_popup", "hidden", this->menuApplied.confirmPopupHidden, true);
		// Без этой строки игрок остаётся в курсоре навсегда (см. CloseLayoutMenu).
		layout->SetInputCaptureEnabled(this->player->GetPlayerSlot(), false);
	}
	// Настоящий худ пересоздаётся на следующем такте DrawPanels (created → force) уже по свежим префам.
	this->RefreshLayoutPrefs();
}

// === Рендер ===================================================================================

void KZHUDService::RenderEditor()
{
	bool created = false;
	CCSCustomHudLayout *layout = this->EnsureMenuLayout(created);
	if (!layout || !this->editorOpen)
	{
		return;
	}
	this->SetMenuBoolClass(layout, "opt_root", "hidden", this->menuApplied.optHidden, true);
	this->SetMenuBoolClass(layout, "edit_root", "hidden", this->menuApplied.editHidden, false);

	const char *lang = this->player->languageService->GetLanguage();
	auto phrase = [lang](const char *key) { return KZLanguageService::PrepareMessageWithLang(lang, key); };
	const MHUDLayoutPrefs &prefs = this->GetOwnLayoutPrefs();

	// Статичные подписи (диф-кэш SetMenuVar шлёт их один раз на сущность).
	this->SetMenuVar(layout, "edit_list", "ed_title", phrase("HUD Editor - Title").c_str());
	this->SetMenuVar(layout, "ed_reset_all", "ed_reset", phrase("HUD Editor - Reset").c_str());
	this->SetMenuVar(layout, "ed_done", "ed_done", phrase("HUD Editor - Done").c_str());
	this->SetMenuVar(layout, "edit_root", "ed_hint", phrase("HUD Editor - Hint").c_str());
	static const char *const propLabels[][2] = {
		{"p_pos", "HUD Editor - Prop Position"}, {"p_step", "HUD Editor - Prop Step"},       {"p_size", "HUD Editor - Prop Size"},
		{"p_font", "HUD Editor - Prop Font"},    {"p_color", "HUD Editor - Prop Color"},     {"p_delta", "HUD Editor - Prop Delta"},
		{"p_opacity", "HUD Editor - Prop Opacity"}, {"p_outline", "HUD Editor - Prop Outline"}, {"p_reset", "HUD Editor - Prop Reset"},
		{"p_hint", "HUD Editor - Prop Hint"},
	};
	for (const auto &pl : propLabels)
	{
		this->SetMenuVar(layout, "edit_props", pl[0], phrase(pl[1]).c_str());
	}

	this->RenderEditorReplica(layout, false);

	// Список элементов: on = элемент включён (и у строки, и у её тумблера).
	for (i32 i = 0; i < KZ_EDITOR_ITEMS; i++)
	{
		const EditorReplicaDef &r = EDITOR_REPLICA[i];
		const bool enabled = prefs.elements[i].enabled;
		this->SetMenuVar(layout, ElPanel(i), EnVar(i), phrase(r.nameKey).c_str());
		this->SetMenuVar(layout, r.buttonId, r.tagVar, phrase(r.nameKey).c_str());
		this->SetMenuBoolClass(layout, ElPanel(i), "on", this->menuApplied.elOn[i], enabled);
		this->SetMenuBoolClass(layout, EtPanel(i), "on", this->menuApplied.etOn[i], enabled);
		this->SetMenuBoolClass(layout, r.buttonId, "sel", this->menuApplied.eSel[i], i == this->editorSelected);
	}

	// Панель свойств — только при выбранном элементе.
	const i32 sel = this->editorSelected;
	const bool hasSel = sel >= 0 && sel < (i32)LayoutElement::Count;
	if (hasSel)
	{
		const LayoutElementDef &def = LAYOUT_ELEMENTS[sel];
		const MHUDLayoutPrefs::Element &el = prefs.elements[sel];
		this->SetMenuVar(layout, "edit_props", "pn", phrase(EDITOR_REPLICA[sel].nameKey).c_str());
		this->SetMenuBoolClass(layout, "ep_on", "on", this->menuApplied.epOn, el.enabled);
		char buf[48];
		// Позиция — в хранимом виде (проценты от центра), как её видят !hud share и БД.
		V_snprintf(buf, sizeof(buf), "%i \xC2\xB7 %i", el.x, el.y);
		this->SetMenuVar(layout, "edit_props", "xy", buf);
		this->SetMenuBoolClass(layout, "ep_step1", "on", this->menuApplied.epStep1, this->editorStep == 1);
		this->SetMenuBoolClass(layout, "ep_step5", "on", this->menuApplied.epStep5, this->editorStep == 5);
		V_snprintf(buf, sizeof(buf), "%ipx", el.size);
		this->SetMenuVar(layout, "edit_props", "sz", buf);
		const char *slug = this->player->optionService->GetPreferenceStr(def.fontKey, def.fontDefault);
		this->SetMenuVar(layout, "ep_font", "font", panorama::GetFontDisplayName(slug, def.fontDefault));
		Color colorDef;
		const char *colorKey = GetElementColorKey((LayoutElement)sel, colorDef);
		const char *swatch = colorKey ? panorama::ResolveSwatchClass(this->GetMHUDColorPref(colorKey, colorDef)) : NULL;
		this->SetMenuSwapClass(layout, "ep_color", this->menuApplied.epColor, swatch);
		this->SetMenuBoolClass(layout, "ep_color", "hidden", this->menuApplied.epColorHidden, colorKey == NULL);
		const bool isTimer = sel == (i32)LayoutElement::Timer;
		this->SetMenuBoolClass(layout, "ep_drow", "hidden", this->menuApplied.epDrowHidden, !isTimer);
		if (isTimer)
		{
			this->SetMenuSwapClass(layout, "ep_dcol_ahead", this->menuApplied.epDAhead, panorama::ResolveSwatchClass(prefs.deltaAhead));
			this->SetMenuSwapClass(layout, "ep_dcol_behind", this->menuApplied.epDBehind, panorama::ResolveSwatchClass(prefs.deltaBehind));
		}
		for (i32 k = 0; k < 3; k++)
		{
			const EditorExtraColorDef &xc = EDITOR_EXTRA_COLORS[sel][k];
			if (xc.prefKey)
			{
				this->SetMenuVar(layout, "edit_props", EDITOR_XROW_VARS[k], phrase(xc.phraseKey).c_str());
				this->SetMenuSwapClass(layout, EDITOR_XC_IDS[k], this->menuApplied.epXc[k],
									   panorama::ResolveSwatchClass(this->GetMHUDColorPref(xc.prefKey, *xc.def)));
			}
			this->SetMenuBoolClass(layout, EDITOR_XROW_IDS[k], "hidden", this->menuApplied.epXrowHidden[k], xc.prefKey == NULL);
		}
		const bool isKeys = sel == (i32)LayoutElement::Keys;
		i32 toggleCount = 0;
		const EditorKeysToggleDef *toggles = GetEditorToggles(sel, toggleCount);
		for (i32 k = 0; k < KZ_EDITOR_TROWS; k++)
		{
			const EditorKeysToggleDef *t = k < toggleCount ? &toggles[k] : NULL;
			if (t)
			{
				this->SetMenuVar(layout, "edit_props", EDITOR_TROW_VARS[k], phrase(t->phraseKey).c_str());
				this->SetMenuBoolClass(layout, EDITOR_TG_IDS[k], "on", this->menuApplied.epTgOn[k],
									   this->player->optionService->GetPreferenceBool(t->prefKey, t->def));
				// Гейт-тумблер с дефолтом true — как IsMenuItemEnabled окна.
				const bool dis = t->gate && !this->player->optionService->GetPreferenceBool(t->gate, true);
				this->SetMenuBoolClass(layout, EDITOR_TROW_IDS[k], "dis", this->menuApplied.epTrowDis[k], dis);
			}
			this->SetMenuBoolClass(layout, EDITOR_TROW_IDS[k], "hidden", this->menuApplied.epTrowHidden[k], t == NULL);
		}
		if (isKeys)
		{
			this->SetMenuVar(layout, "edit_props", "psl", phrase("HUD Editor - Prop KeysGap").c_str());
			if (prefs.keysGap < 0)
			{
				this->SetMenuVar(layout, "edit_props", "ps", phrase("HUD Editor - Prop KeysGapAuto").c_str());
			}
			else
			{
				V_snprintf(buf, sizeof(buf), "%i px", prefs.keysGap);
				this->SetMenuVar(layout, "edit_props", "ps", buf);
			}
			this->SetMenuVar(layout, "edit_props", "pgl", phrase("HUD - Menu Label Idle").c_str());
			for (i32 k = 0; k < 3; k++)
			{
				this->SetMenuVar(layout, "edit_props", EDITOR_SG_VARS[k], phrase(EDITOR_KEYS_IDLE_PHRASES[k]).c_str());
				this->SetMenuBoolClass(layout, EDITOR_SG_IDS[k], "on", this->menuApplied.epSgOn[k], prefs.keysIdle == k);
			}
		}
		this->SetMenuBoolClass(layout, "ep_srow", "hidden", this->menuApplied.epSrowHidden, !isKeys);
		this->SetMenuBoolClass(layout, "ep_grow", "hidden", this->menuApplied.epGrowHidden, !isKeys);
		V_snprintf(buf, sizeof(buf), "%i%%", el.opacity);
		this->SetMenuVar(layout, "edit_props", "op", buf);
		this->SetMenuBoolClass(layout, "ep_outline", "on", this->menuApplied.epOutline, el.outline);
		this->SetMenuBoolClass(layout, "edit_props", "flip", this->menuApplied.propsFlip, el.x > KZ_EDITOR_FLIP_FROM && el.y > KZ_EDITOR_FLIP_FROM);
	}
	this->SetMenuBoolClass(layout, "edit_props", "hidden", this->menuApplied.propsHidden, !hasSel);

	this->RenderMenuPopups(layout);
	// Смена класса панели не доезжает до стилей детей без полного пересчёта (см. RenderMenu).
	layout->GetGlobalLayoutState()->MarkFullChanged();
}

// Текст таймера-плейсхолдера: идёт от 01:23.456, точность — как у настоящего таймера.
static_function void FormatEditorTimer(f64 time, bool detailed, char *out, u32 len)
{
	utils::FormatTime(time, out, len);
	if (!detailed)
	{
		// Как UpdateTimerElement: без hudTimerDetail дробная часть отбрасывается целиком.
		if (char *dot = strchr(out, '.'))
		{
			*dot = '\0';
		}
	}
}

// tickOnly — такт из DrawPanels: только то, что меняется само (таймер, showpos).
void KZHUDService::RenderEditorReplica(CCSCustomHudLayout *layout, bool tickOnly)
{
	const MHUDLayoutPrefs &prefs = this->GetOwnLayoutPrefs();
	auto apply = [&](LayoutElement e, bool show, const char *text, const Color &color)
	{
		const EditorReplicaDef &r = EDITOR_REPLICA[(i32)e];
		this->ApplyLayoutElementTo(layout, prefs.elements[(i32)e], this->editorElements[(i32)e], r.labelId, r.varName, r.buttonId,
								   show && prefs.elements[(i32)e].enabled, text, color, false);
	};

	char timer[64];
	const f64 elapsed = MAX(0.0, g_pKZUtils->GetServerGlobals()->curtime - this->editorOpenedAt);
	FormatEditorTimer(KZ_EDITOR_TIMER_START + elapsed, prefs.timerDetailed, timer, sizeof(timer));
	apply(LayoutElement::Timer, true, timer, prefs.timerPro);

	const bool showPos = prefs.elements[(i32)LayoutElement::ShowPos].enabled;
	if (showPos)
	{
		char pos[64];
		char ang[48];
		// Без пешки (смерть/спектейт под редактором) строки держат прошлое значение.
		if (FormatShowPos(this->player, pos, sizeof(pos), ang, sizeof(ang)))
		{
			this->ApplyShowPosLines(layout, "x_", this->editorExtra, pos, ang, prefs.elements[(i32)LayoutElement::ShowPos], prefs.showPosColor);
		}
	}
	apply(LayoutElement::ShowPos, true, NULL, prefs.showPosColor);
	if (tickOnly)
	{
		return;
	}

	// Дельта: «позади» (+0.312, d-behind) — видна, когда в префах выбрано сравнение.
	this->ApplyTimerDelta(layout, "x_", this->editorExtra, prefs, prefs.elements[(i32)LayoutElement::Timer].enabled && prefs.timerCompare != 0,
						  KZ_EDITOR_DELTA_PLACEHOLDER);

	apply(LayoutElement::Speed, true, prefs.speedPrecise ? "327.00" : "327", prefs.speed);
	const char *prespeed = prefs.prespeedBrackets ? (prefs.prespeedPrecise ? "(291.00)" : "(291)") : (prefs.prespeedPrecise ? "291.00" : "291");
	apply(LayoutElement::Prespeed, true, prespeed, prefs.prespeed);
	// Плейсхолдер бейджей: горят PERF и CJ — оба вида (перф/джамбаг и присед) видны сразу.
	const bool indLit[3] = {true, false, true};
	this->ApplyJumpIndicators(layout, "x_", this->editorExtra, prefs.elements[(i32)LayoutElement::Prespeed].enabled && prefs.indicators, indLit,
							  prefs.elements[(i32)LayoutElement::Prespeed].size);

	// Клавиши: нажаты W, A, J (порядок C W J A S D, как KEY_PANELS в mhud.cpp).
	apply(LayoutElement::Keys, true, NULL, prefs.keys);
	if (prefs.elements[(i32)LayoutElement::Keys].enabled)
	{
		const bool keys[MHUD_KEY_COUNT] = {false, true, true, true, false, false};
		const bool none[MHUD_KEY_COUNT] = {};
		this->ApplyKeysLook(layout, "x_", this->editorKeys, prefs, keys, false, none);
	}

	char cp[48];
	KZ::hudfmt::FormatCheckpointLine(3, 2, cp, sizeof(cp));
	apply(LayoutElement::Checkpoint, true, cp, prefs.checkpoint);
	// «Прогресс»: 38% — плейсхолдер дизайна (подпись, процент и полоса та же запись, что у худа).
	if (prefs.elements[(i32)LayoutElement::LeadProgress].enabled)
	{
		this->ApplyLeadProgressParts(layout, "x_", this->editorExtra, KZ_EDITOR_PROGRESS_PLACEHOLDER,
									 prefs.elements[(i32)LayoutElement::LeadProgress], prefs.leadProgressColor);
	}
	apply(LayoutElement::LeadProgress, true, KZ_EDITOR_PROGRESS_TEXT, prefs.leadProgressColor);

	// PB/WR — настоящие по курсу игрока, если есть; иначе плейсхолдеры дизайна.
	const bool cells[4] = {prefs.pbNub, prefs.pbPro, prefs.wrNub, prefs.wrPro};
	const bool anyCell = cells[0] || cells[1] || cells[2] || cells[3];
	if (anyCell && prefs.elements[(i32)LayoutElement::PbWr].enabled)
	{
		static const char *const detailedPh[4] = {"01:21.900", "01:08.102", "01:25.310", "01:12.744"};
		static const char *const shortPh[4] = {"01:21.90", "01:08.10", "01:25.31", "01:12.74"};
		char buf[4][32];
		const char *texts[4];
		this->BuildPbWrTexts(this->player, GetHudDisplayCourse(this->player), prefs.timerDetailed, prefs.timerDetailed ? detailedPh : shortPh, buf,
							 texts);
		this->ApplyPbWrCells(layout, "x_", this->editorExtra, texts, cells, prefs.elements[(i32)LayoutElement::PbWr], prefs.pbwrColor);
	}
	apply(LayoutElement::PbWr, anyCell, NULL, prefs.pbwrColor);

	// Курс — настоящий (курс · режим · стили игрока), до первого курса карты — плейсхолдер.
	char line[128];
	const KZCourseDescriptor *course = GetHudDisplayCourse(this->player);
	const char *styles[8];
	i32 styleCount = 0;
	for (i32 i = 0; i < this->player->styleServices.Count() && styleCount < (i32)KZ_ARRAYSIZE(styles); i++)
	{
		styles[styleCount++] = this->player->styleServices[i]->GetStyleName();
	}
	KZ::hudfmt::FormatCourseLine(course ? course->name : "Main", this->player->modeService ? this->player->modeService->GetModeShortName() : "CKZ",
								 styles, styleCount, line, sizeof(line));
	apply(LayoutElement::Course, true, line, prefs.courseColor);
	// Тип рана: бейдж PRO (класс rt-pro стоит в разметке реплики статически).
	apply(LayoutElement::RunType, true, "PRO", MHUD_DEF_BASE_COLOR);
}

void KZHUDService::TickHudEditor()
{
	if (!this->editorOpen)
	{
		return;
	}
	if (CBaseEntity *ent = this->ownedMenuLayout.Get())
	{
		this->RenderEditorReplica((CCSCustomHudLayout *)ent, true);
	}
}

// === Правки выбранного элемента ================================================================

void KZHUDService::EditorSetPos(i32 element, i32 x, i32 y)
{
	if (element < 0 || element >= (i32)LayoutElement::Count)
	{
		return;
	}
	const LayoutElementDef &def = LAYOUT_ELEMENTS[element];
	// Float — тот же тип, что читает RefreshLayoutPrefs (layout/prefs.cpp).
	x = panorama::SnapToStep(Clamp(x, KZ_EDITOR_POS_MIN, KZ_EDITOR_POS_MAX), -100, 100);
	y = panorama::SnapToStep(Clamp(y, KZ_EDITOR_POS_MIN, KZ_EDITOR_POS_MAX), -100, 100);
	this->player->optionService->SetPreferenceFloat(def.xKey, (f64)x);
	this->player->optionService->SetPreferenceFloat(def.yKey, (f64)y);
	this->RefreshLayoutPrefs();
	this->RenderEditor();
}

void KZHUDService::EditorNudge(i32 dx, i32 dy)
{
	if (this->editorSelected < 0)
	{
		return;
	}
	const MHUDLayoutPrefs::Element &el = this->GetOwnLayoutPrefs().elements[this->editorSelected];
	this->EditorSetPos(this->editorSelected, el.x + dx, el.y + dy);
}

void KZHUDService::EditorStepSize(i32 delta)
{
	if (this->editorSelected < 0)
	{
		return;
	}
	const LayoutElementDef &def = LAYOUT_ELEMENTS[this->editorSelected];
	const i32 size = this->GetOwnLayoutPrefs().elements[this->editorSelected].size;
	this->player->optionService->SetPreferenceFloat(def.sizeKey, (f64)panorama::SnapToStep(size + delta, LAYOUT_SIZE_MIN, LAYOUT_SIZE_MAX));
	this->RefreshLayoutPrefs();
	this->RenderEditor();
}

void KZHUDService::EditorStepOpacity(i32 delta)
{
	if (this->editorSelected < 0)
	{
		return;
	}
	const LayoutElementDef &def = LAYOUT_ELEMENTS[this->editorSelected];
	const i32 opacity = this->GetOwnLayoutPrefs().elements[this->editorSelected].opacity;
	// Int — тот же тип, что читает RefreshLayoutPrefs.
	this->player->optionService->SetPreferenceInt(def.opacityKey, Clamp(opacity + delta, 0, 100));
	this->RefreshLayoutPrefs();
	this->RenderEditor();
}

void KZHUDService::EditorToggleOutline()
{
	if (this->editorSelected < 0)
	{
		return;
	}
	const LayoutElement e = (LayoutElement)this->editorSelected;
	// Как OutlineOnActivate (hud_prefs.cpp): читает с миграцией старого общего hudOutline.
	this->player->optionService->SetPreferenceBool(LAYOUT_ELEMENTS[this->editorSelected].outlineKey, !this->GetElementOutlinePref(e));
	this->RefreshLayoutPrefs();
	this->RenderEditor();
}

void KZHUDService::EditorToggleElement(i32 element)
{
	if (element < 0 || element >= (i32)LayoutElement::Count)
	{
		return;
	}
	const LayoutElementDef &def = LAYOUT_ELEMENTS[element];
	this->player->optionService->SetPreferenceBool(def.enabledKey, !this->GetOwnLayoutPrefs().elements[element].enabled);
	this->RefreshLayoutPrefs();
	this->RenderEditor();
}

void KZHUDService::EditorResetSelected()
{
	if (this->editorSelected < 0)
	{
		return;
	}
	// Узел элемента в скрытой категории — тот, где лежит его пункт позиции (xKey нигде больше
	// не регистрируется). ResetNode пишет дефолты всех пунктов узла (вкл., позиция, кегль,
	// шрифт, обводка, прозрачность, цвета) — ровно прежняя кнопка «Сброс» страницы элемента.
	KZOptNode *node = KZMenuFindNodeByPref(LAYOUT_ELEMENTS[this->editorSelected].xKey);
	if (!node)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] hud_editor_reset_denied reason=node_not_found element=%i slot=%i\n", this->editorSelected,
					this->player->GetPlayerSlot().Get());
		return;
	}
	// Откат через обмен худом — как у кнопок сброса страниц (hud_prefs.cpp, SaveUndoBeforeReset).
	if (KZ::hudshare::SaveUndo(this->player))
	{
		this->player->languageService->PrintChat(true, false, "HUD Share - Undo Hint");
	}
	KZ::menu::ResetNode(this->player, node);
	this->RefreshLayoutPrefs();
	this->RenderEditor();
}

// Тумблеры панели свойств (строки ep_trow*; у клавиш ещё ep_srow/ep_grow): преф пишется сразу,
// реплика перерисовывается тем же RenderEditor. Строки нет у выбранного элемента — клик по
// устаревшему кадру игнорируем.
void KZHUDService::EditorToggleKeysPref(i32 row)
{
	i32 toggleCount = 0;
	const EditorKeysToggleDef *toggles = this->editorSelected >= 0 ? GetEditorToggles(this->editorSelected, toggleCount) : NULL;
	if (!toggles || row < 0 || row >= toggleCount)
	{
		return;
	}
	const EditorKeysToggleDef &t = toggles[row];
	if (t.gate && !this->player->optionService->GetPreferenceBool(t.gate, true))
	{
		return;
	}
	this->player->optionService->SetPreferenceBool(t.prefKey, !this->player->optionService->GetPreferenceBool(t.prefKey, t.def));
	this->RefreshLayoutPrefs();
	this->RenderEditor();
}

// Ступени интервала: авто (-1) → 0 → 2 → … → 24. С нечётного (из обмена/БД) шаг вниз уходит на
// ближайшее чётное снизу, а не мимо нуля сразу в «авто».
void KZHUDService::EditorStepKeysGap(i32 dir)
{
	if (this->editorSelected != (i32)LayoutElement::Keys)
	{
		return;
	}
	const i32 gap = this->GetOwnLayoutPrefs().keysGap;
	i32 next = gap;
	if (dir > 0)
	{
		next = gap < 0 ? 0 : MIN(gap + 2, 24);
	}
	else
	{
		next = gap <= 0 ? -1 : MAX(gap - 2 + (gap & 1), 0);
	}
	if (next == gap)
	{
		return;
	}
	this->player->optionService->SetPreferenceInt("mhudKeysGap", next);
	this->RefreshLayoutPrefs();
	this->RenderEditor();
}

void KZHUDService::EditorPickKeysIdle(i32 idle)
{
	if (this->editorSelected != (i32)LayoutElement::Keys || idle < 0 || idle > 2)
	{
		return;
	}
	this->player->optionService->SetPreferenceInt("mhudKeysIdle", idle);
	this->RefreshLayoutPrefs();
	this->RenderEditor();
}

// === Клики ====================================================================================

bool KZHUDService::HandleEditorClick(const char *id)
{
	if (!this->editorOpen)
	{
		return false;
	}
	i32 x = 0;
	i32 y = 0;
	if (KZ::hudfmt::GridCellToPercent(id, KZ_EDITOR_GRID_COLS, KZ_EDITOR_GRID_ROWS, x, y))
	{
		// Review Focus 4: без выбранного элемента клик по сетке молча ничего не делает.
		if (this->editorSelected >= 0)
		{
			// Сетка — проценты от левого-верхнего угла, преф — от центра (см. шапку).
			this->EditorSetPos(this->editorSelected, x - 50, y - 50);
		}
		return true;
	}
	for (i32 e = 0; e < (i32)LayoutElement::Count; e++)
	{
		if (!V_strcmp(id, EDITOR_REPLICA[e].buttonId))
		{
			this->editorSelected = e;
			this->RenderEditor();
			return true;
		}
	}
	// el{i}/et{i}: et вложен в el — второе переключение того же элемента в тот же тик гасится.
	if ((id[0] == 'e' && (id[1] == 'l' || id[1] == 't')) && V_isdigit(id[2]))
	{
		char *end = NULL;
		const i32 i = (i32)strtol(id + 2, &end, 10);
		if (*end == '\0' && i >= 0 && i < KZ_EDITOR_ITEMS)
		{
			if (!this->IsDuplicateMenuAction(2000 + i))
			{
				this->NoteMenuAction(2000 + i);
				this->EditorToggleElement(i);
			}
			return true;
		}
	}
	const i32 step = this->editorStep;
	if (!V_strcmp(id, "ep_l"))
	{
		this->EditorNudge(-step, 0);
	}
	else if (!V_strcmp(id, "ep_r"))
	{
		this->EditorNudge(step, 0);
	}
	else if (!V_strcmp(id, "ep_u"))
	{
		this->EditorNudge(0, -step);
	}
	else if (!V_strcmp(id, "ep_d"))
	{
		this->EditorNudge(0, step);
	}
	else if (!V_strcmp(id, "ep_step1") || !V_strcmp(id, "ep_step5"))
	{
		this->editorStep = id[7] == '5' ? 5 : 1;
		this->RenderEditor();
	}
	else if (!V_strcmp(id, "ep_sz_dec") || !V_strcmp(id, "ep_sz_inc"))
	{
		this->EditorStepSize(id[6] == 'i' ? 1 : -1);
	}
	else if (!V_strcmp(id, "ep_op_dec") || !V_strcmp(id, "ep_op_inc"))
	{
		this->EditorStepOpacity(id[6] == 'i' ? 5 : -5);
	}
	else if (!V_strcmp(id, "ep_outline"))
	{
		this->EditorToggleOutline();
	}
	else if (!V_strcmp(id, "ep_on"))
	{
		this->EditorToggleElement(this->editorSelected);
	}
	else if (!V_strcmp(id, "ep_font"))
	{
		if (this->editorSelected >= 0)
		{
			this->OpenMenuPopup(MenuPopup::List, KZMenuFindItemByPref(LAYOUT_ELEMENTS[this->editorSelected].fontKey));
		}
	}
	else if (!V_strcmp(id, "ep_color"))
	{
		Color def;
		const char *key = this->editorSelected >= 0 ? GetElementColorKey((LayoutElement)this->editorSelected, def) : NULL;
		if (key)
		{
			this->OpenMenuPopup(MenuPopup::Color, KZMenuFindItemByPref(key));
		}
	}
	else if (!V_strcmp(id, "ep_dcol_ahead") || !V_strcmp(id, "ep_dcol_behind"))
	{
		if (this->editorSelected == (i32)LayoutElement::Timer)
		{
			this->OpenMenuPopup(MenuPopup::Color, KZMenuFindItemByPref(id[8] == 'a' ? "mhudDeltaAheadColor" : "mhudDeltaBehindColor"));
		}
	}
	else if (!V_strncmp(id, "ep_xc", 5) && id[5] >= '1' && id[5] <= '3' && id[6] == '\0')
	{
		const i32 k = id[5] - '1';
		const char *key = this->editorSelected >= 0 ? EDITOR_EXTRA_COLORS[this->editorSelected][k].prefKey : NULL;
		if (key)
		{
			this->OpenMenuPopup(MenuPopup::Color, KZMenuFindItemByPref(key));
		}
	}
	else if (!V_strncmp(id, "ep_tg", 5) && id[5] >= '1' && id[5] <= '8' && id[6] == '\0')
	{
		this->EditorToggleKeysPref(id[5] - '1');
	}
	else if (!V_strcmp(id, "ep_s_dec") || !V_strcmp(id, "ep_s_inc"))
	{
		this->EditorStepKeysGap(id[5] == 'i' ? 1 : -1);
	}
	else if (!V_strncmp(id, "ep_sg", 5) && id[5] >= '0' && id[5] <= '2' && id[6] == '\0')
	{
		this->EditorPickKeysIdle(id[5] - '0');
	}
	else if (!V_strcmp(id, "ep_reset"))
	{
		this->EditorResetSelected();
	}
	else if (!V_strcmp(id, "ed_reset_all"))
	{
		this->OpenMenuConfirm(KZMenuFindItemByPhrase(KZ_MENU_RESET_ALL_PHRASE));
	}
	else if (!V_strcmp(id, "ed_done") || !V_strcmp(id, "m_close"))
	{
		this->CloseHudEditor();
	}
	else
	{
		return false;
	}
	return true;
}
