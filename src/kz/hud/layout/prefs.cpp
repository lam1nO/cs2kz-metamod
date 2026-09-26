// Префы panorama-худа (Task 5): читает LAYOUT_ELEMENTS (Task 4, entity.cpp) и наполняет
// MHUDLayoutPrefs. Таблицу LAYOUT_ELEMENTS здесь НЕ определяем — второе определение того же
// символа сломало бы линковку, она уже определена в entity.cpp и объявлена extern в kz_hud.h.
#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/panorama_tables.h"
#include "kz/lead/kz_lead.h" // SetProgressWanted — преф элемента «Прогресс» держит путь маршрута
#include "kz/option/kz_option.h"
#include "kz/spec/kz_spec.h" // GetSpectatedPlayer — цель мимикрии (mhudMimicSpec)

#include "tier0/memdbgon.h"

// Ступени масштаба карточки меню реплея (см. layout.h). Держать в ОДНОМ порядке с набором
// классов .rp-scale--N в аддоне: лишняя ступень здесь — класс, которого нет в CSS, и карточка
// молча останется прежнего размера.
const i32 RPMENU_SCALE_STEPS[] = {75, 85, 100, 115, 130};
const i32 RPMENU_SCALE_STEP_COUNT = (i32)KZ_ARRAYSIZE(RPMENU_SCALE_STEPS);

i32 SnapReplayMenuScale(i32 percent)
{
	const auto dist = [percent](i32 step) { return step > percent ? step - percent : percent - step; };
	i32 best = RPMENU_SCALE_STEPS[0];
	for (i32 i = 1; i < RPMENU_SCALE_STEP_COUNT; i++)
	{
		if (dist(RPMENU_SCALE_STEPS[i]) < dist(best))
		{
			best = RPMENU_SCALE_STEPS[i];
		}
	}
	return best;
}

const MHUDLayoutPrefs &KZHUDService::GetOwnLayoutPrefs()
{
	return this->layoutPrefs;
}

// Порт апстримного KZHUDService::GetPrefs (origin/master:src/kz/hud/layout/preferences.cpp:75-89)
// на наш кэш: у апстрима набор ленивый (GetOwnPrefs досчитывает по prefsDirty), у нас —
// заполненное поле layoutPrefs, поэтому вместо вызова досчёта берём чужое поле как есть
// (кэш наблюдаемого поддерживается его же событиями: OnPlayerPreferencesLoaded и
// RefreshLayoutPrefs после каждой записи из меню, см. layout/menu.cpp).
const MHUDLayoutPrefs &KZHUDService::GetLayoutPrefs()
{
	const MHUDLayoutPrefs &own = this->GetOwnLayoutPrefs();
	if (!own.mimicSpec)
	{
		return own;
	}
	// У ботов (в т.ч. у реплей-бота) своих настроек нет — мимикрия под бота просто погасила бы
	// худ наблюдателю. Апстримная проверка один-в-один, плюс наша `loaded`: там нулевого набора
	// не бывает вовсе (ленивый досчёт), у нас он существует до OnPlayerPreferencesLoaded.
	KZPlayer *target = this->player->specService->GetSpectatedPlayer();
	if (!target || target == this->player || target->IsFakeClient())
	{
		return own;
	}
	const MHUDLayoutPrefs &mimicked = target->hudService->GetOwnLayoutPrefs();
	return mimicked.loaded ? mimicked : own;
}

// Миграция обводки: до задачи 4 обводка была ОДНИМ тумблером hudOutline на все пять элементов.
// Молча упасть на дефолт нельзя — игрок, выключивший обводку, получил бы её обратно, а это
// самый незаметный способ испортить худ (см. журнал решений задачи). Проверено фактом: HTML-путь
// (kz_hud.cpp) hudOutline вообще не читает — обводка там не рисуется, ключ был нужен только
// panorama-худу. Значит после этой миграции hudOutline в горячем пути нигде больше не читается —
// он остаётся исключительно источником одноразового переноса значения в mhud*Outline.
//
// HasPreference в нашей базе нет (см. git grep) — отсутствие поэлементного ключа определяем
// чтением с двумя РАЗНЫМИ дефолтами: если ключ реально сохранён, оба чтения вернут одно и то же
// (настоящее) значение, независимо от дефолта; если ключа нет, оба чтения вернут свои дефолты —
// и разойдутся. Расхождение = ключа нет, берём значение из общего hudOutline (миграция),
// совпадение = ключ есть, доверяем ему.
//
// Один ответ на два потребителя (худ ниже и пункт меню, hud/prefs/hud_prefs.cpp): второе,
// независимое чтение mhud*Outline с дефолтом true расходилось бы с применённым значением.
bool KZHUDService::GetElementOutlinePref(LayoutElement element)
{
	auto *opts = this->MHUDSettingsSource()->optionService;
	const char *outlineKey = LAYOUT_ELEMENTS[(i32)element].outlineKey;
	const bool withTrueDefault = opts->GetPreferenceBool(outlineKey, true);
	const bool withFalseDefault = opts->GetPreferenceBool(outlineKey, false);
	// Дефолт false синхронизирован с текущими настройками игрока (задача hud-defaults) — тот
	// же ответ обязан отдавать SetItemPref(..., outlineKey, ..., 0) в hud_prefs.cpp (кнопка Reset).
	return (withTrueDefault == withFalseDefault) ? withTrueDefault : opts->GetPreferenceBool("hudOutline", false);
}

void KZHUDService::RefreshLayoutPrefs()
{
	// Источник настроек ЗДЕСЬ — ВСЕГДА сам игрок (MHUDSettingsSource() не смотрит на спектейт,
	// см. её объявление в kz_hud.h): этот метод наполняет СВОЙ набор игрока. Мимикрия под
	// наблюдаемого (mhudMimicSpec) живёт исключительно в GetLayoutPrefs выше — иначе она стала
	// бы транзитивной (чужой mimicSpec попал бы в наш кэш) и зацикливалась бы на чтении.
	auto *opts = this->MHUDSettingsSource()->optionService;
	for (i32 e = 0; e < (i32)LayoutElement::Count; e++)
	{
		const LayoutElementDef &def = LAYOUT_ELEMENTS[e];
		MHUDLayoutPrefs::Element &element = this->layoutPrefs.elements[e];
		// Дефолт тумблера — ПОЭЛЕМЕНТНЫЙ (def.enabledDefault): «Прогресс» по умолчанию выключен,
		// остальные пять включены. Тот же ответ обязан давать пункт меню (AddHudElementItems).
		element.enabled = opts->GetPreferenceBool(def.enabledKey, def.enabledDefault);
		element.x = panorama::SnapToStep((i32)opts->GetPreferenceFloat(def.xKey, (f32)def.xDefault), -100, 100);
		element.y = panorama::SnapToStep((i32)opts->GetPreferenceFloat(def.yKey, (f32)def.yDefault), -100, 100);
		element.size = panorama::SnapToStep((i32)opts->GetPreferenceFloat(def.sizeKey, (f32)def.sizeDefault), LAYOUT_SIZE_MIN, LAYOUT_SIZE_MAX);
		// Шрифт разрешается в css-класс уже здесь: UpdateLayoutElement (Task 4) кладёт его на
		// панель напрямую, повторный резолв в геймтике не нужен и не делается.
		element.fontClass = panorama::ResolveFontClass(opts->GetPreferenceStr(def.fontKey, LAYOUT_DEFAULT_FONT), LAYOUT_DEFAULT_FONT);
		// Миграция общего hudOutline — в GetElementOutlinePref (выше): тот же ответ показывает
		// пункт меню, второе чтение с другим дефолтом развело бы меню с худом.
		element.outline = this->GetElementOutlinePref((LayoutElement)e);
		element.opacity = Clamp((i32)opts->GetPreferenceInt(def.opacityKey, 100), 0, 100);
	}

	// Цвета — те же ключи префов, что были у удалённого particle-MHUD (задача 12): настройки
	// игроков остались валидны без миграции (см. дефолты в kz_hud.h). GetPreferenceColor в
	// нашей базе нет — цвет читается GetMHUDColorPref (преф хранит упакованный int).
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
	this->layoutPrefs.keysPressed = this->GetMHUDColorPref("mhudKeysPressedColor", MHUD_DEF_KEYS_PRESSED_COLOR);
	this->layoutPrefs.keysOverlapGlow = this->GetMHUDColorPref("mhudKeysOverlapGlowColor", MHUD_DEF_KEYS_OVERLAP_GLOW_COLOR);
	this->layoutPrefs.checkpoint = this->GetMHUDColorPref("mhudCheckpointColor", MHUD_DEF_BASE_COLOR);
	this->layoutPrefs.pbwrColor = this->GetMHUDColorPref("mhudPbWrColor", MHUD_DEF_BASE_COLOR);
	this->layoutPrefs.showPosColor = this->GetMHUDColorPref("mhudShowPosColor", MHUD_DEF_BASE_COLOR);
	this->layoutPrefs.courseColor = this->GetMHUDColorPref("mhudCourseColor", MHUD_DEF_BASE_COLOR);
	this->layoutPrefs.leadProgressColor = this->GetMHUDColorPref("mhudLeadProgressColor", MHUD_DEF_BASE_COLOR);

	this->layoutPrefs.timerDetailed = opts->GetPreferenceBool("hudTimerDetail", true);
	// Поля редактора `!hud`. Дефолты — те же, что пишет ApplyHudDefaults (layout/defaults.cpp)
	// и обязаны показывать пункты меню.
	this->layoutPrefs.pbNub = opts->GetPreferenceBool("hudPbNub", true);
	this->layoutPrefs.pbPro = opts->GetPreferenceBool("hudPbPro", true);
	this->layoutPrefs.wrNub = opts->GetPreferenceBool("hudWrNub", true);
	this->layoutPrefs.wrPro = opts->GetPreferenceBool("hudWrPro", true);
	// Клэмп обязателен: преф мог прийти из БД/импорта мимо меню, а 3+ UpdateTimerElement
	// прочитал бы как «сравнивать с WR».
	this->layoutPrefs.timerCompare = Clamp((i32)opts->GetPreferenceInt("hudTimerCompare", 1), 0, 2);
	this->layoutPrefs.deltaAhead = this->GetMHUDColorPref("mhudDeltaAheadColor", MHUD_DEF_DELTA_AHEAD_COLOR);
	this->layoutPrefs.deltaBehind = this->GetMHUDColorPref("mhudDeltaBehindColor", MHUD_DEF_DELTA_BEHIND_COLOR);
	// hudKeysOverlap читался кодом до этой задачи, но пункта в реестре меню у него не было
	// (одна из шести находок транша) — hud_prefs.cpp теперь заводит тумблер на этот же ключ.
	this->layoutPrefs.keysOverlapEnabled = opts->GetPreferenceBool("hudKeysOverlap", true);
	this->layoutPrefs.keysOverlapAxis = opts->GetPreferenceBool("mhudKeysOverlapAxis", false);
	// Дефолты пяти тумблеров ниже и mhudKeysIdle синхронизированы с текущими настройками игрока
	// (задача hud-defaults) — тот же ответ обязаны отдавать пункты меню в hud_prefs.cpp.
	this->layoutPrefs.keysLetters = opts->GetPreferenceBool("mhudKeysLetters", true);
	this->layoutPrefs.keysSquare = opts->GetPreferenceBool("mhudKeysSquare", true);
	this->layoutPrefs.keysBorder = opts->GetPreferenceBool("mhudKeysBorder", false);
	this->layoutPrefs.keysGlow = opts->GetPreferenceBool("mhudKeysGlow", false);
	this->layoutPrefs.keysFill = opts->GetPreferenceBool("mhudKeysFill", false);
	this->layoutPrefs.keysIdle = Clamp((i32)opts->GetPreferenceInt("mhudKeysIdle", 2), 0, 2);
	// -1 «авто» — отдельное значение, а не 0: 0 это «вплотную», авто — пропорциональный отступ
	// keys.css. Клэмп — преф мог прийти из БД/обмена мимо редактора, а класса key-gap--99 нет.
	// Нечётное не округляем: классы есть на каждый пиксель 0..24, степпер редактора с нечётного
	// просто шагает дальше по 2.
	{
		const i32 gap = (i32)opts->GetPreferenceInt("mhudKeysGap", -1);
		this->layoutPrefs.keysGap = gap < 0 ? -1 : Clamp(gap, 0, 24);
	}
	this->layoutPrefs.speedPrecise = opts->GetPreferenceBool("mhudSpeedPrecise", false);
	// Престрейф — три префа апстрима, до этой задачи не читавшиеся вообще (дефолты его же,
	// origin/master:src/kz/hud/layout/preferences.cpp:46-48).
	this->layoutPrefs.prespeedPrecise = opts->GetPreferenceBool("mhudPrespeedPrecise", false);
	this->layoutPrefs.prespeedBrackets = opts->GetPreferenceBool("mhudPrespeedBrackets", false);
	this->layoutPrefs.prespeedHideWalkOff = opts->GetPreferenceBool("mhudPrespeedHideWalkOff", false);
	// mimicSpec — из СВОЕГО набора и только отсюда: GetLayoutPrefs его не мимикрирует
	// (origin/master:src/kz/hud/kz_hud.h:127, preferences.cpp:61).
	this->layoutPrefs.mimicSpec = opts->GetPreferenceBool("mhudMimicSpec", false);

	// Крестик (Task 10) — самостоятельный тумблер, не элемент LAYOUT_ELEMENTS: у него нет
	// текста/шрифта/позиции в процентах, только масштаб (crosshairScale, доли device-пикселя).
	// Дефолт true — синхронизирован с текущими настройками игрока (задача hud-defaults).
	this->layoutPrefs.crosshair = opts->GetPreferenceBool("mhudCrosshair", true);
	this->layoutPrefs.crosshairScale = panorama::SnapToStep((i32)opts->GetPreferenceInt("mhudCrosshairScale", 100), 0, 500);

	// Меню реплея: обе настройки — индексы/ступени, страница применяет их классами
	// (.rp-scale--N и .rp-pos--<slug> на панели replay_card). Остальные ключи rpmenu* удалены
	// 17.09.2026 как мёртвые — см. layout.h.
	this->layoutPrefs.replayMenu.scale = SnapReplayMenuScale((i32)opts->GetPreferenceInt("rpmenuScale", RPMENU_DEF_SCALE));
	// Позиция — проценты от центра, та же сетка и тот же снап, что у элементов худа.
	this->layoutPrefs.replayMenu.posX = panorama::SnapToStep((i32)opts->GetPreferenceFloat("rpmenuPosX", (f32)RPMENU_DEF_POS_X), -100, 100);
	this->layoutPrefs.replayMenu.posY = panorama::SnapToStep((i32)opts->GetPreferenceFloat("rpmenuPosY", (f32)RPMENU_DEF_POS_Y), -100, 100);

	// Последней строкой: набор целиком заполнен, мимикрия (GetLayoutPrefs) может его брать.
	// Аналог апстримного `prefsDirty = false` в конце RefreshPrefs.
	this->layoutPrefs.loaded = true;

	// Элемент «Прогресс» — единственная настройка худа, которой нужны ДАННЫЕ извне: маршрут
	// `!lead` (kz/lead). Поэтому преф сам инициирует загрузку и освобождение пути, а этот
	// метод — единственное место, где он вообще меняется: его зовут и на загрузке префов
	// игрока, и после каждой правки в меню (layout/menu.cpp), и после применения чужого
	// набора (hud/share). Гейт по типу худа обязателен: на Standard/Off элемента нет вовсе, и
	// докачивать реплей ради невидимого процента значило бы жечь сеть и память впустую.
	// Своё, НЕ мимикрированное значение: путь грузится игроку, а не его цели наблюдения.
	if (this->player->leadService)
	{
		const bool progressWanted =
			this->layoutPrefs.elements[(i32)LayoutElement::LeadProgress].enabled && this->GetHudType() == KZHUDService::HUD_TYPE_PANORAMA;
		this->player->leadService->SetProgressWanted(progressWanted);
	}
}

bool KZHUDService::IsLayoutElementEnabled(LayoutElement element)
{
	// Через GetLayoutPrefs, а не напрямую по this->layoutPrefs: иначе при mhudMimicSpec
	// цвета/раскладка брались бы у наблюдаемого, а ВИДИМОСТЬ элементов — своя (апстрим зовёт
	// GetPrefs, origin/master:src/kz/hud/layout/preferences.cpp:96-99).
	return this->GetLayoutPrefs().elements[(i32)element].enabled;
}

// Основной цвет элемента для ep_color редактора !hud. Ключи — те же, что читает RefreshLayoutPrefs
// выше, и те же, что зарегистрированы пунктами Color в скрытых узлах элементов (hud_prefs.cpp):
// попап цвета редактора правит именно эти пункты реестра.
const char *GetElementColorKey(LayoutElement element, Color &def)
{
	def = MHUD_DEF_BASE_COLOR;
	switch (element)
	{
		case LayoutElement::Timer:
			def = MHUD_DEF_TIMER_PRO_COLOR;
			return "mhudTimerProColor";
		case LayoutElement::Speed:
			return "mhudSpeedColor";
		case LayoutElement::Prespeed:
			return "mhudPrespeedColor";
		case LayoutElement::Keys:
			return "mhudKeysColor";
		case LayoutElement::Checkpoint:
			return "mhudCheckpointColor";
		case LayoutElement::LeadProgress:
			return "mhudLeadProgressColor";
		case LayoutElement::PbWr:
			return "mhudPbWrColor";
		case LayoutElement::ShowPos:
			return "mhudShowPosColor";
		case LayoutElement::Course:
			return "mhudCourseColor";
		default:
			return NULL;
	}
}
