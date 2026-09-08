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

		// Пять элементов × 7 ключей: тумблер, позиция (X/Y, проценты), размер, шрифт,
		// обводка, прозрачность. Ключи и значения — из LAYOUT_ELEMENTS (layout/entity.cpp) и
		// LAYOUT_DEF_*/LAYOUT_DEFAULT_FONT (layout/layout.h), то есть ровно те дефолты, что
		// читает RefreshLayoutPrefs. Тип записи обязан совпадать с типом чтения: X/Y/размер
		// читаются Float, прозрачность — Int (см. layout/prefs.cpp).
		for (i32 e = 0; e < (i32)LayoutElement::Count; e++)
		{
			const LayoutElementDef &def = LAYOUT_ELEMENTS[e];
			opts->SetPreferenceBool(def.enabledKey, true);
			opts->SetPreferenceFloat(def.xKey, (f64)def.xDefault);
			opts->SetPreferenceFloat(def.yKey, (f64)def.yDefault);
			opts->SetPreferenceFloat(def.sizeKey, (f64)def.sizeDefault);
			opts->SetPreferenceStr(def.fontKey, LAYOUT_DEFAULT_FONT);
			opts->SetPreferenceBool(def.outlineKey, false);
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
			{"mhudCrosshair", true},            // реплика прицела игрока панелями худа
		};

		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(kBools); i++)
		{
			opts->SetPreferenceBool(kBools[i].key, kBools[i].value);
			written++;
		}

		// Целочисленные ключи худа: чем показывать ненажатую клавишу (0/1/2) и масштаб
		// прицела в процентах.
		opts->SetPreferenceInt("mhudKeysIdle", 2);
		opts->SetPreferenceInt("mhudCrosshairScale", 100);
		written += 2;

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
			{"mhudKeysColor", &MHUD_DEF_BASE_COLOR},
			{"mhudKeysOverlapColor", &MHUD_DEF_KEYS_OVERLAP_COLOR},
			{"mhudKeysPressedColor", &MHUD_DEF_KEYS_PRESSED_COLOR},
			{"mhudKeysOverlapGlowColor", &MHUD_DEF_KEYS_OVERLAP_GLOW_COLOR},
			{"mhudCheckpointColor", &MHUD_DEF_BASE_COLOR},
		};

		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(kColors); i++)
		{
			this->SetMHUDColorPref(kColors[i].key, *kColors[i].color);
			written++;
		}

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
