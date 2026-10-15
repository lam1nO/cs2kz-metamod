// Одноразовая перезапись настроек худа новыми дефолтами (решение пользователя 08.09: «новый
// худ должен быть абсолютно у всех, старые дефолты перезаписать»). Новые значения дефолтов
// уже стоят в коде (layout/layout.h, kz_hud.cpp, пункты меню hud/prefs/hud_prefs.cpp) — но их
// получают ТОЛЬКО игроки без записи в БД: у остальных в префах лежит собственное значение
// (в т.ч. записанное старым дефолтом при первом же клике по пункту меню), и чтение с новым
// дефолтом до него не доходит. Эта миграция дописывает новые значения в префы явно.
//
// ПОЧЕМУ ОДНОРАЗОВО. Перезапись — разрушающая: чужие персональные настройки худа теряются.
// Делать её на каждый заход значило бы отбирать у игрока настройку сразу после того, как он
// её поменял. Поэтому в префах живёт маркер применённой ревизии (hudDefaultsRev): пока он
// равен текущей ревизии, миграция не делает НИЧЕГО — правки, сделанные игроком ПОСЛЕ
// перезаписи, никогда не откатываются. Следующая массовая перезапись = подъём
// KZHUDService::HUD_DEFAULTS_REV на 1 (и только он).
#include "kz/hud/layout/layout.h"
#include "kz/option/kz_option.h"
#include <cmath>
#include "utils/logging.h"

#include "tier0/memdbgon.h"

void KZHUDService::ApplyHudDefaults()
{
	// Пишем строго в СВОИ префы: MHUDSettingsSource() отдаёт того же игрока, но миграция —
	// про хранилище настроек, а не про источник отображения (спектейт её не касается).
	KZOptionService *opts = this->player->optionService;
	// Fail-closed: до успешной InitializeLocalPrefs prefKV — пустая таблица по умолчанию, и
	// запись в неё затёрла бы реальные префы игрока (тот же дефект, что разбирает
	// SaveLocalPrefs/GetHudType). Единственный вызывающий — OnPlayerPreferencesLoaded, то
	// есть гейт штатно уже открыт; проверка держит инвариант, а не лечит известный случай.
	if (!opts->IsLoaded())
	{
		return;
	}
	const i64 storedRev = opts->GetPreferenceInt(HUD_DEFAULTS_REV_KEY, 0);
	if (storedRev >= HUD_DEFAULTS_REV)
	{
		return;
	}

	i32 written = 0;
	// Ревизия 1 у игрока уже есть — ревизии 2 и 3 ВЫБОРОЧНЫЕ: трогают только то, что игрок не
	// менял. Цепочкой 1 → 2 → 3 (у каждой свой пакет и свой маркер). Без ревизии вообще — полная
	// перезапись ниже, уже текущими дефолтами, и сразу маркер HUD_DEFAULTS_REV.
	if (storedRev >= 1 && storedRev <= 3)
	{
		if (storedRev == 1)
		{
			this->ApplyHudDefaultsRev2(written);
			written = 0;
		}
		if (storedRev <= 2)
		{
			this->ApplyHudDefaultsRev3(written);
			written = 0;
		}
		this->ApplyHudDefaultsRev4(written);
		return;
	}
	{
		// Одна запись в БД на всю миграцию: каждый SetPreference* флашит префы ЦЕЛИКОМ
		// (SaveLocalPrefs внутри), без пакета 60+ ключей дали бы 60+ полных сериализаций и
		// 60+ запросов на один заход игрока. Флаш делает деструктор BatchScope.
		KZOptionService::BatchScope batch(opts);

		// ПОРЯДОК. Сперва отрабатывают существующие легаси-переносы, потом наша перезапись —
		// иначе легаси-значение приехало бы ПОВЕРХ новых дефолтов.
		// 1) hudType: mhudMaster / сохранённая 1 (удалённый particle-худ) → GetHudType()
		//    нормализует и перезаписывает преф сам; после нашей записи ниже он уже не сработает
		//    ни разу (stored == 3 не попадает ни в одну ветку миграции).
		this->GetHudType();
		// 2) hudOutline → пять поэлементных mhud*Outline: этот перенос БЕЗ записи, он
		//    разрешается на каждом чтении (GetElementOutlinePref, layout/prefs.cpp — probe
		//    двумя дефолтами). Явная запись поэлементных ключей ниже закрывает его навсегда:
		//    probe видит сохранённый ключ и на общий hudOutline больше не смотрит. Поэтому
		//    вернуться поверх нашего значения он не может, и «прогонять» его тут нечего.

		// Тип худа — сам по себе ключ худа, и без него перезапись бессмысленна: игрок с
		// сохранённым hudType=0 (HTML-панель) или 2 (Off) нового худа не увидел бы вовсе.
		opts->SetPreferenceInt("hudType", HUD_TYPE_PANORAMA);
		written++;

		// Все элементы LAYOUT_ELEMENTS × 7 ключей: тумблер, позиция (X/Y, проценты), размер,
		// шрифт, обводка, прозрачность. Ключи и значения — из LAYOUT_ELEMENTS (layout/entity.cpp) и
		// LAYOUT_DEF_*/LAYOUT_DEFAULT_FONT (layout/layout.h), то есть ровно те дефолты, что
		// читает RefreshLayoutPrefs. Тип записи обязан совпадать с типом чтения: X/Y/размер
		// читаются Float, прозрачность — Int (см. layout/prefs.cpp).
		for (i32 e = 0; e < (i32)LayoutElement::Count; e++)
		{
			const LayoutElementDef &def = LAYOUT_ELEMENTS[e];
			// Дефолт тумблера — поэлементный (def.enabledDefault; с 28.09 все включены), и одноразовая
			// перезапись оформления ставит ровно его (тот же ответ дают
			// RefreshLayoutPrefs и пункт меню).
			opts->SetPreferenceBool(def.enabledKey, def.enabledDefault);
			opts->SetPreferenceFloat(def.xKey, (f64)def.xDefault);
			opts->SetPreferenceFloat(def.yKey, (f64)def.yDefault);
			opts->SetPreferenceFloat(def.sizeKey, (f64)def.sizeDefault);
			opts->SetPreferenceStr(def.fontKey, def.fontDefault);
			opts->SetPreferenceBool(def.outlineKey, LayoutOutlineDefault((LayoutElement)e));
			opts->SetPreferenceInt(def.opacityKey, 100);
			written += 7;
		}

		// mhudMimicSpec в набор НЕ входит: это поведение спектейта, а не вид худа —
		// перезаписывать чужой выбор им незачем.
		// Тумблеры худа вне LAYOUT_ELEMENTS — дефолты те же, что читает RefreshLayoutPrefs
		// (layout/prefs.cpp) и показывают пункты меню (hud/prefs/hud_prefs.cpp).
		const struct
		{
			const char *key;
			bool value;
		} kBools[] = {
			{"hudTimerDetail", true},           // сотые доли и часы в таймере
			{"hudKeysOverlap", true},           // подсветка конфликтующих клавиш
			{"mhudKeysOverlapAxis", false},     // тонировать только конфликтующую пару
			{"mhudKeysLetters", true},          // буквы на клавишах
			{"mhudKeysSquare", true},           // квадратные клавиши
			{"mhudKeysBorder", false},          // рамка клавиши
			{"mhudKeysGlow", false},            // свечение клавиши
			{"mhudKeysFill", false},            // заливка клавиши
			{"mhudSpeedPrecise", false},        // скорость с двумя знаками
			{"mhudPrespeedPrecise", false},     // престрейф с двумя знаками
			{"mhudPrespeedBrackets", false},    // престрейф в скобках
			{"mhudPrespeedHideWalkOff", false}, // не показывать престрейф при уходе с края
			{"hudIndicators", true},            // бейджи PERF/JB/CJ под престрейфом
			{"hudPbNub", true},                 // ячейка PB NUB элемента PB/WR
			{"hudPbPro", true},                 // ячейка PB PRO
			{"hudWrNub", true},                 // ячейка WR NUB
			{"hudWrPro", true},                 // ячейка WR PRO
		};

		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(kBools); i++)
		{
			opts->SetPreferenceBool(kBools[i].key, kBools[i].value);
			written++;
		}

		// Целочисленные ключи худа: чем показывать ненажатую клавишу (0/1/2) и режим дельты
		// таймера.
		opts->SetPreferenceInt("mhudKeysIdle", MHUD_DEF_KEYS_IDLE);
		opts->SetPreferenceInt("mhudKeysGap", MHUD_DEF_KEYS_GAP);
		opts->SetPreferenceInt(MHUD_KEYS_GAP_KEY, MHUD_DEF_KEYS_GAP);
		// Дельта таймера: 0 выкл / 1 к PB / 2 к WR (§4.1 спеки редактора).
		opts->SetPreferenceInt("hudTimerCompare", 2);
		written += 3;

		// Цвета: тот же упакованный int, что читает GetMHUDColorPref, — пишем через
		// SetMHUDColorPref, чтобы упаковка была ровно одна на оба направления.
		const struct
		{
			const char *key;
			const Color *color;
		} kColors[] = {
			{"mhudTimerProColor", &MHUD_DEF_TIMER_PRO_COLOR},
			{"mhudTimerTpColor", &MHUD_DEF_TIMER_TP_COLOR},
			{"mhudTimerPausedColor", &MHUD_DEF_TIMER_PAUSED_COLOR},
			{"mhudTimerStoppedColor", &MHUD_DEF_TIMER_STOPPED_COLOR},
			{"mhudSpeedColor", &MHUD_DEF_BASE_COLOR},
			{"mhudSpeedCjColor", &MHUD_DEF_CJ_COLOR},
			{"mhudPrespeedColor", &MHUD_DEF_BASE_COLOR},
			{"mhudPrespeedPerfColor", &MHUD_DEF_PERF_COLOR},
			{"mhudPrespeedJumpbugColor", &MHUD_DEF_JUMPBUG_COLOR},
			{"mhudKeysColor", &MHUD_DEF_KEYS_COLOR},
			{"mhudKeysOverlapColor", &MHUD_DEF_KEYS_OVERLAP_COLOR},
			{"mhudKeysPressedColor", &MHUD_DEF_KEYS_PRESSED_COLOR},
			{"mhudKeysOverlapGlowColor", &MHUD_DEF_KEYS_OVERLAP_GLOW_COLOR},
			{"mhudCheckpointColor", &MHUD_DEF_BASE_COLOR},
			{"mhudDeltaAheadColor", &MHUD_DEF_DELTA_AHEAD_COLOR},
			{"mhudDeltaBehindColor", &MHUD_DEF_DELTA_BEHIND_COLOR},
			{"mhudPbWrColor", &MHUD_DEF_BASE_COLOR},
			{"mhudShowPosColor", &MHUD_DEF_SHOWPOS_COLOR},
			{"mhudCourseColor", &MHUD_DEF_BASE_COLOR},
			{"mhudLeadProgressColor", &MHUD_DEF_LEADPROGRESS_COLOR},
		};

		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(kColors); i++)
		{
			this->SetMHUDColorPref(kColors[i].key, *kColors[i].color);
			written++;
		}

		// Меню реплея — 75 % всем (ревизия 4).
		opts->SetPreferenceInt("rpmenuScale", RPMENU_DEF_SCALE);
		written++;

		// Маркер — последней записью внутри того же пакета: он уезжает в БД той же
		// сериализацией, что и сами ключи. Отдельная запись после пакета могла бы сохранить
		// маркер без ключей (падение/дисконнект между двумя запросами) и навсегда закрыть
		// миграцию, ничего не перезаписав.
		opts->SetPreferenceInt(HUD_DEFAULTS_REV_KEY, HUD_DEFAULTS_REV);
	}

	// Смена состояния, не восстановимая из БД (прежние значения затёрты) — один info на
	// игрока, с актором и машинно-читаемыми полями. В игровом такте не логируется: этот
	// путь проходится один раз за заход, и только когда маркер реально поднялся.
	KZ_LOG_INFO(LogChannel::Option, "[cyb] hud_defaults_forced steam_id=%llu keys=%i rev_from=%lli rev_to=%i\n", this->player->GetSteamId64(false),
				written, (i64)storedRev, (i32)HUD_DEFAULTS_REV);
}

// === Ревизия 2: худ v2 (спека h-spec-v2, 14.10) ===============================================
// Ревизия 1 записала каждому игроку ВСЕ ключи худа явно, поэтому «не менял элемент» = все его
// ключи до сих пор равны значениям ревизии 1 (таблица ниже — копия дефолтов на 28.09, НЕ
// текущие LAYOUT_DEF_*). Ключа нет (элемент появился позже первой перезаписи этого игрока) —
// чтение возвращает значение ревизии 1, то есть «не менял».
//
// Решения владельца 14.10: клавиши/скорость/престрейф — новые дефолты только у тех, кто их не
// трогал; PB/WR, прогресс, курс, тип рана — новое оформление у всех (это разметка, не префы),
// тумблер включается всем один раз, размер/позиция/шрифт/цвет остаются личными, если менялись.
namespace
{
	struct Rev1Element
	{
		f64 x, y, size;
		const char *font;
	};

	// Порядок — LayoutElement (Timer … RunType).
	const Rev1Element HUD_REV1[(i32)LayoutElement::Count] = {
		{0, 35, 24, "lato-bold"},          // Timer
		{0, 8, 34, "lato-bold"},           // Speed
		{0, 12, 22, "lato-bold"},          // Prespeed
		{0, 20, 30, "lato-bold"},          // Keys
		{0, 30, 20, "lato-bold"},          // Checkpoint
		{-42, -5, 18, "stratum2-medium"},  // LeadProgress
		{-41, -44, 12, "lato-bold"},       // PbWr
		{43, -40, 13, "stratum2-mono"},    // ShowPos
		{0, -39, 13, "stratum2-bold"},     // Course
		{0, 40, 15, "stratum2-bold"},      // RunType
	};
	static_assert((i32)LayoutElement::Count == 10, "HUD_REV1: новый элемент — добавить его значения ревизии 1");

	// До 28.09 «Прогресс» жил под таймером и выключенным — такие значения тоже «не менял».
	constexpr f64 HUD_REV0_LEADPROGRESS_X = 0;
	constexpr f64 HUD_REV0_LEADPROGRESS_Y = 41;
	constexpr const char *HUD_REV0_LEADPROGRESS_FONT = "lato-bold";
	const Color HUD_REV1_LEADPROGRESS_COLOR(0x00, 0xAA, 0xFF, 0xFF);
} // namespace

void KZHUDService::ApplyHudDefaultsRev2(i32 &written)
{
	KZOptionService *opts = this->player->optionService;
	const auto feq = [&](const char *key, f64 v) { return fabs(opts->GetPreferenceFloat(key, v) - v) < 0.01; };
	const auto ieq = [&](const char *key, i64 v) { return opts->GetPreferenceInt(key, v) == v; };
	const auto beq = [&](const char *key, bool v) { return opts->GetPreferenceBool(key, v) == v; };
	// Пустая строка шрифта (бывает после обмена худом) — это дефолт, а не личный выбор.
	const auto seq = [&](const char *key, const char *v)
	{
		const char *cur = opts->GetPreferenceStr(key, v);
		return !cur[0] || V_stricmp(cur, v) == 0;
	};
	const auto ceq = [&](const char *key, const Color &v)
	{
		const Color c = this->GetMHUDColorPref(key, v);
		return c.r() == v.r() && c.g() == v.g() && c.b() == v.b();
	};
	// Общие 7 ключей элемента против ревизии 1 (тумблер, X/Y, размер, шрифт, обводка, прозрачность).
	const auto untouched = [&](LayoutElement e)
	{
		const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)e];
		const Rev1Element &r = HUD_REV1[(i32)e];
		return beq(def.enabledKey, true) && feq(def.xKey, r.x) && feq(def.yKey, r.y) && feq(def.sizeKey, r.size) && seq(def.fontKey, r.font)
			   && beq(def.outlineKey, false) && ieq(def.opacityKey, 100);
	};
	const auto writeElement = [&](LayoutElement e)
	{
		const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)e];
		opts->SetPreferenceBool(def.enabledKey, def.enabledDefault);
		opts->SetPreferenceFloat(def.xKey, (f64)def.xDefault);
		opts->SetPreferenceFloat(def.yKey, (f64)def.yDefault);
		opts->SetPreferenceFloat(def.sizeKey, (f64)def.sizeDefault);
		opts->SetPreferenceStr(def.fontKey, def.fontDefault);
		opts->SetPreferenceBool(def.outlineKey, LayoutOutlineDefault(e));
		opts->SetPreferenceInt(def.opacityKey, 100);
		written += 7;
	};

	char changed[160] = "";
	const auto mark = [&](const char *name) { V_strncat(changed, changed[0] ? "," : "", sizeof(changed)); V_strncat(changed, name, sizeof(changed)); };

	{
		KZOptionService::BatchScope batch(opts);

		// Клавиши: элемент + все свои настройки вида ревизии 1.
		const bool keysUntouched = untouched(LayoutElement::Keys) && beq("hudKeysOverlap", true) && beq("mhudKeysOverlapAxis", false)
								   && beq("mhudKeysLetters", true) && beq("mhudKeysSquare", true) && beq("mhudKeysBorder", false)
								   && beq("mhudKeysGlow", false) && beq("mhudKeysFill", false) && ieq("mhudKeysIdle", 2) && ieq("mhudKeysGap", -1)
								   && ceq("mhudKeysColor", MHUD_DEF_BASE_COLOR) && ceq("mhudKeysOverlapColor", MHUD_DEF_KEYS_OVERLAP_COLOR)
								   && ceq("mhudKeysPressedColor", MHUD_DEF_KEYS_PRESSED_COLOR)
								   && ceq("mhudKeysOverlapGlowColor", MHUD_DEF_KEYS_OVERLAP_GLOW_COLOR);
		if (keysUntouched)
		{
			writeElement(LayoutElement::Keys);
			opts->SetPreferenceInt("mhudKeysIdle", MHUD_DEF_KEYS_IDLE);
			opts->SetPreferenceInt("mhudKeysGap", MHUD_DEF_KEYS_GAP);
			opts->SetPreferenceInt(MHUD_KEYS_GAP_KEY, MHUD_DEF_KEYS_GAP);
			this->SetMHUDColorPref("mhudKeysColor", MHUD_DEF_KEYS_COLOR);
			written += 3;
			mark("keys");
		}
		const bool speedUntouched = untouched(LayoutElement::Speed) && beq("mhudSpeedPrecise", false) && ceq("mhudSpeedColor", MHUD_DEF_BASE_COLOR)
									&& ceq("mhudSpeedCjColor", MHUD_DEF_CJ_COLOR);
		if (speedUntouched)
		{
			writeElement(LayoutElement::Speed);
			mark("speed");
		}
		// Престрейф: дефолты v2 совпадают с ревизией 1 (у владельца отличается только слаг
		// verdana-bold — тот же Arial Bold), писать нечего.

		// Новое оформление: тумблер — всем; размер/позиция/шрифт/цвет — только нетронутые.
		const LayoutElement redesigned[] = {LayoutElement::LeadProgress, LayoutElement::PbWr, LayoutElement::Course, LayoutElement::RunType};
		for (LayoutElement e : redesigned)
		{
			const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)e];
			const Rev1Element &r = HUD_REV1[(i32)e];
			const bool progress = e == LayoutElement::LeadProgress;
			opts->SetPreferenceBool(def.enabledKey, true);
			written++;
			const bool posDefault = (feq(def.xKey, r.x) && feq(def.yKey, r.y))
									|| (progress && feq(def.xKey, HUD_REV0_LEADPROGRESS_X) && feq(def.yKey, HUD_REV0_LEADPROGRESS_Y));
			if (posDefault)
			{
				opts->SetPreferenceFloat(def.xKey, (f64)def.xDefault);
				opts->SetPreferenceFloat(def.yKey, (f64)def.yDefault);
				written += 2;
			}
			if (feq(def.sizeKey, r.size))
			{
				opts->SetPreferenceFloat(def.sizeKey, (f64)def.sizeDefault);
				written++;
			}
			if (seq(def.fontKey, r.font) || (progress && seq(def.fontKey, HUD_REV0_LEADPROGRESS_FONT)))
			{
				opts->SetPreferenceStr(def.fontKey, def.fontDefault);
				written++;
			}
			if (progress && (ceq("mhudLeadProgressColor", HUD_REV1_LEADPROGRESS_COLOR) || ceq("mhudLeadProgressColor", MHUD_DEF_BASE_COLOR)))
			{
				this->SetMHUDColorPref("mhudLeadProgressColor", MHUD_DEF_LEADPROGRESS_COLOR);
				written++;
			}
		}
		mark("redesign");

		// Литерал, а не HUD_DEFAULTS_REV: следующая ревизия пойдёт своим шагом после этой.
		opts->SetPreferenceInt(HUD_DEFAULTS_REV_KEY, 2);
	}

	KZ_LOG_INFO(LogChannel::Option, "[cyb] hud_defaults_rev2 steam_id=%llu keys=%i changed=%s\n", this->player->GetSteamId64(false), written, changed);
}

// === Ревизия 3: дефолты по худу владельца (15.10) ============================================
// «Для дефолта — прогресс и PB/WR как у меня, клавиши белыми, бейдж типа рана 16; и такие
// настройки тем, кто их не менял». Шрифт, обводка, прозрачность, цвет прогресса и PB/WR у
// владельца равны дефолтам ревизии 2 — писать нечего. Ключ за ключом: «не менял» = ключа нет
// (чтение отдаёт старый дефолт) или он равен дефолту ревизии 2; позиция — парой X/Y, как в
// ревизии 2 (это одна настройка). Игрок, прошедший ревизию 2 в этом же заходе, получил там уже
// новые значения — здесь они не совпадут со старыми и останутся.
namespace
{
	// Дефолты ревизии 2 (14.10), НЕ текущие LAYOUT_DEF_*.
	constexpr f64 HUD_REV2_LEADPROGRESS_X = -42;
	constexpr f64 HUD_REV2_LEADPROGRESS_Y = -5;
	constexpr f64 HUD_REV2_LEADPROGRESS_SIZE = 15;
	constexpr f64 HUD_REV2_PBWR_SIZE = 15;
	constexpr f64 HUD_REV2_RUNTYPE_SIZE = 12;
	const Color HUD_REV2_KEYS_COLOR(0x40, 0xFF, 0x40, 0xFF);
} // namespace

void KZHUDService::ApplyHudDefaultsRev3(i32 &written)
{
	KZOptionService *opts = this->player->optionService;
	const auto feq = [&](const char *key, f64 v) { return fabs(opts->GetPreferenceFloat(key, v) - v) < 0.01; };
	// Цвет читаем из СВОИХ префов (GetMHUDColorPref смотрит в источник настроек — при спектейте
	// это чужой игрок); сравнение по RGB, как в ревизии 2.
	const auto ceq = [&](const char *key, const Color &v)
	{
		const i64 packed = opts->GetPreferenceInt(key, ((i64)v.r() << 24) | ((i64)v.g() << 16) | ((i64)v.b() << 8) | (i64)v.a());
		return ((packed >> 24) & 0xFF) == v.r() && ((packed >> 16) & 0xFF) == v.g() && ((packed >> 8) & 0xFF) == v.b();
	};

	char changed[96] = "";
	const auto mark = [&](const char *name) { V_strncat(changed, changed[0] ? "," : "", sizeof(changed)); V_strncat(changed, name, sizeof(changed)); };

	{
		KZOptionService::BatchScope batch(opts);

		const LayoutElementDef &progress = LAYOUT_ELEMENTS[(i32)LayoutElement::LeadProgress];
		if (feq(progress.xKey, HUD_REV2_LEADPROGRESS_X) && feq(progress.yKey, HUD_REV2_LEADPROGRESS_Y))
		{
			opts->SetPreferenceFloat(progress.xKey, (f64)LAYOUT_DEF_LEADPROGRESS_X);
			opts->SetPreferenceFloat(progress.yKey, (f64)LAYOUT_DEF_LEADPROGRESS_Y);
			written += 2;
			mark("progress_pos");
		}
		const struct
		{
			LayoutElement element;
			f64 oldSize;
			f64 newSize;
			const char *name;
		} kSizes[] = {
			{LayoutElement::LeadProgress, HUD_REV2_LEADPROGRESS_SIZE, LAYOUT_DEF_LEADPROGRESS_SIZE, "progress_size"},
			{LayoutElement::PbWr, HUD_REV2_PBWR_SIZE, LAYOUT_DEF_PBWR_SIZE, "pbwr_size"},
			{LayoutElement::RunType, HUD_REV2_RUNTYPE_SIZE, LAYOUT_DEF_RUNTYPE_SIZE, "runtype_size"},
		};
		for (const auto &it : kSizes)
		{
			const char *key = LAYOUT_ELEMENTS[(i32)it.element].sizeKey;
			if (feq(key, it.oldSize))
			{
				opts->SetPreferenceFloat(key, it.newSize);
				written++;
				mark(it.name);
			}
		}
		if (ceq("mhudKeysColor", HUD_REV2_KEYS_COLOR))
		{
			this->SetMHUDColorPref("mhudKeysColor", MHUD_DEF_KEYS_COLOR);
			written++;
			mark("keys_color");
		}

		// Маркер — в том же пакете, что и ключи (см. полную перезапись выше).
		opts->SetPreferenceInt(HUD_DEFAULTS_REV_KEY, 3);
	}

	KZ_LOG_INFO(LogChannel::Option, "[cyb] hud_defaults_rev3 steam_id=%llu keys=%i changed=%s\n", this->player->GetSteamId64(false), written,
				changed[0] ? changed : "-");
}

// === Ревизия 4: меню реплея 75 %, прогресс на одной линии с PB/WR (15.10) ===================
// «Размер меню реплея сделать для всех 75 % — потом если что сами поменяют»: rpmenuScale пишется
// ВСЕМ безусловно (на части разрешений карточка обрезалась слева). Прогресс: X ревизии 3 (−43)
// на текущий дефолт (−41, как у PB/WR) — только тем, у кого позиция ревизии 3 (не двигали).
void KZHUDService::ApplyHudDefaultsRev4(i32 &written)
{
	KZOptionService *opts = this->player->optionService;
	const auto feq = [&](const char *key, f64 v) { return fabs(opts->GetPreferenceFloat(key, v) - v) < 0.01; };
	{
		KZOptionService::BatchScope batch(opts);
		opts->SetPreferenceInt("rpmenuScale", RPMENU_DEF_SCALE);
		written++;
		const LayoutElementDef &progress = LAYOUT_ELEMENTS[(i32)LayoutElement::LeadProgress];
		constexpr f64 HUD_REV3_LEADPROGRESS_X = -43, HUD_REV3_LEADPROGRESS_Y = -4; // дефолт ревизии 3
		if (feq(progress.xKey, HUD_REV3_LEADPROGRESS_X) && feq(progress.yKey, HUD_REV3_LEADPROGRESS_Y))
		{
			opts->SetPreferenceFloat(progress.xKey, (f64)LAYOUT_DEF_LEADPROGRESS_X);
			written++;
		}
		opts->SetPreferenceInt(HUD_DEFAULTS_REV_KEY, 4);
	}
	KZ_LOG_INFO(LogChannel::Option, "[cyb] hud_defaults_rev4 steam_id=%llu keys=%i\n", this->player->GetSteamId64(false), written);
}
