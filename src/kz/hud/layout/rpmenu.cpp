// Меню управления реплеем спектатора на СВОЕЙ panorama-странице (KZ_RPMENU_LAYOUT, аддон
// GYMSTRIKE-KZ; исходники разметки — gymstrike-kz/content/panorama: rpmenu.xml, rpmenu.css,
// rpmenu-positions.css; спека — docs/design/2026-09-11-replay-player-panorama). Семантика
// пунктов и данные — KZ::replaysystem::menu (replays/menu.cpp), здесь — сущность, чтение
// клавиш и запись страницы.
//
// Всю карточку (шапка, время, шкала, семь строк, подсказка) рисует сама страница: размеры,
// шрифты, цвета и фон живут в её vcss. Сервер умеет над ней ровно две операции —
// SetDialogVariableString (текст слота) и SetHasClass (класс панели). Отсюда всё устройство
// файла: ОДНА сущность на игрока (одна страница — один custom_hud_layout), таблица слотов
// RPMENU_VARS и таблица строк RPMENU_ROWS. Костылей старой карточки на разметке ХУДА (добивка
// строк NBSP до одной длины, только моноширинные шрифты, константная ширина блока по самым
// длинным текстам, пять копий mhud.vxml_c) здесь больше нет и быть не должно: край строк
// задаёт CSS, а ширина карточки фиксированная и от текста не зависит.
//
// Произвольное ЧИСЛО в CSS сервером не поставить, поэтому переменная геометрия шкалы выражена
// классами с шагом 0.25 %: .w-p--N — ширина заполнения, .x-p--N — позиция ручки
// (rpmenu-positions.vcss). Про бюджет интерн-таблицы см. RPMENU_PROGRESS_STEPS ниже.
//
// Порядок строк на СТРАНИЦЕ (play, frame, seek, seek_fast, speed, restart, exit) не совпадает с порядком
// ReplayMenuLine: W/S ходят по странице (RPMENU_ROWS), иначе выбор прыгал бы по карточке
// вверх-вниз. Семантика каждой строки при этом прежняя — её задаёт поле line.
//
// Мышиный режим спеки (наведение, клики, перетаскивание шкалы) НЕ реализован: курсорного
// захвата у меню нет, ввод читается с наблюдательской пешки. Классы .hovered/.grabbed/.active
// и отметки чекпоинтов/телепортов/прыжков страница просто не получает и не показывает.
//
// Меню ВСЕГДА открыто, пока игрок наблюдает реплей-бота с идущим плейбеком: UpdateReplayMenu
// (тик из DrawPanels) сам открывает и закрывает его; команды нет, cs2menus-меню удалено.
#include "kz/hud/layout/layout.h"
#include "kz/language/kz_language.h"
#include "kz/spec/kz_spec.h"
#include "kz/replays/kz_replaysystem.h"
#include "kz/replays/menu.h"
#include "kz/replays/data.h"
#include "kz/replays/playback.h" // эффективная шкала тиков — в ней считаются отметки шкалы
#include "sdk/entity/ccscustomhudlayout.h"
#include "sdk/services.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/logging.h"
#include "cs2kz.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include <algorithm>

#include "tier0/memdbgon.h"

using KZ::replaysystem::menu::ReplayMenuInput;
using KZ::replaysystem::menu::ReplayMenuLine;
using KZ::replaysystem::menu::ReplayMenuStatus;

// Меню-движок cs2menus (определён в cs2kz.cpp); nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

// Автоповтор A/D на удержании (как adjust в cs2menus): период в тиках после 22-тиковой паузы.
// Подбирается живьём: перемотка ±10 с на удержании не должна улетать за секунды.
static CConVar<int> kz_rpmenu_repeat_ticks("kz_rpmenu_repeat_ticks", FCVAR_NONE,
										   "Replay menu (panorama): auto-repeat period of A/D while held, ticks (after a 22-tick delay).", 8);

// ДИАГНОСТИКА краша клиента при открытии меню реплея (17.09.2026). Клиент умирает молча, в
// тик создания сущности, не написав в консоль НИ ОДНОЙ панорамной строки: ни ошибки парсинга,
// ни «Unable to find panel». Разметку страницы проверить локально нельзя — клиент берёт
// панораму только из смонтированного аддона-VPK (россыпь и csgo_addons/<id> проверены, не
// работают), то есть каждая проба страницы стоит часового окна Steam. Зато ОБЪЁМ записи —
// наш, серверный, и переключается на лету:
//   0 — сущность создаётся, в неё не пишется ничего (падает ⇒ виновата сама страница);
//   1 — только dialog-переменные (тексты);
//   2 — + статические классы (rp-live, hidden, selected, focused, paused);
//   3 — всё, включая шкалу и 32 отметки (штатное поведение).
// Ручка временная: когда причина найдена, убрать вместе с ветками.
// Какую страницу грузить в сущность меню реплея. Ручка нужна ровно потому, что панораму
// НЕЛЬЗЯ проверить локально: клиент берёт разметку только из смонтированного аддона-VPK
// (россыпь в csgo/ и папка csgo_addons/<id> проверены 17.09.2026 — не работают), поэтому
// каждая проба страницы стоила бы часового окна публикации Steam. Кладём в аддон несколько
// вариантов разом и переключаем отсюда. Пустая строка = штатная KZ_RPMENU_LAYOUT.
// Смена подхватывается при следующем открытии меню (сущность создаётся заново).
static CConVar<CUtlString> cyb_rpmenu_page("cyb_rpmenu_page", FCVAR_NONE,
										   "Replay menu (panorama) DIAGNOSTIC: layout path to load; empty = default page.", "");

static CConVar<int> cyb_rpmenu_fill("cyb_rpmenu_fill", FCVAR_NONE,
									"Replay menu (panorama) DIAGNOSTIC: 0 nothing, 1 vars, 2 +static classes, 3 everything (default).", 3);

// Чем подписаны чипы строки «Кадр»: 1 — стрелки U+25C0/U+25B6 (как в макете), 0 — буквы A/D.
// Ручка, а не константа, ровно по той же причине, что cyb_rpmenu_page: наличие типографских
// глифов в игровом шрифте локально не проверить, а публикация аддона стоит часового окна
// Steam. Если вместо стрелок в игре «тофу» — ставится 0 по RCON, без публикации.
// Подхватывается на следующем кадре меню (слот идёт через диф-кэш).
static CConVar<bool> cyb_rpmenu_arrow_chips("cyb_rpmenu_arrow_chips", FCVAR_NONE,
										   "Replay menu (panorama): label the frame-step chips with arrow glyphs instead of A/D.", true);

namespace
{
	// Текстовые слоты страницы. panelId — панель с текстом, varName — имя dialog-переменной в
	// её text="{s:...}" (в rpmenu.xml они совпадают не везде: текст строки лежит на панели
	// row_*_label, а переменная зовётся row_*).
	enum class RPVar : i32
	{
		Nick,
		Meta,
		Badge,
		TimeCaption,
		TimeCurrent,
		TimeFinal,
		StateGlyph,
		StateText,
		PosTime,
		DurTime,
		SeekPreview,
		RowPlay,
		RowFrame,
		RowSeek,
		RowSeekFast,
		RowSpeed,
		RowRestart,
		RowExit,
		ChipKeyPlay,
		ChipKeyRestart,
		ChipKeyExit,
		ChipFramePrev,
		ChipFrameNext,
		ChipSeekBack,
		ChipSeekFwd,
		ChipSeekFastBack,
		ChipSeekFastFwd,
		ChipSpeedDec,
		ChipSpeedVal,
		ChipSpeedInc,
		Hint,
		Count
	};

	struct RPMenuVarDef
	{
		const char *panelId;
		const char *varName;
	};

	// clang-format off
	constexpr RPMenuVarDef RPMENU_VARS[] =
	{
		{"nick",                "nick"},
		{"meta",                "meta"},
		{"badge_type",          "badge"},
		{"time_caption",        "time_caption"},
		{"time_current",        "time_current"},
		{"time_final",          "time_final"},
		{"state_glyph",         "state_glyph"},
		{"state_text",          "state_text"},
		{"pos_time",            "pos_time"},
		{"dur_time",            "dur_time"},
		{"seek_preview",        "seek_preview"},
		{"row_play_label",      "row_play"},
		{"row_frame_label",     "row_frame"},
		{"row_seek_label",      "row_seek"},
		{"row_seek_fast_label", "row_seek_fast"},
		{"row_speed_label",     "row_speed"},
		{"row_restart_label",   "row_restart"},
		{"row_exit_label",      "row_exit"},
		{"chip_key_e_play",     "chip_key_e_play"},
		{"chip_key_e_restart",  "chip_key_e_restart"},
		{"chip_key_e_exit",     "chip_key_e_exit"},
		{"chip_frame_prev",     "chip_frame_prev"},
		{"chip_frame_next",     "chip_frame_next"},
		{"chip_seek_back",      "chip_seek_back"},
		{"chip_seek_fwd",       "chip_seek_fwd"},
		{"chip_seek_fast_back", "chip_seek_fast_back"},
		{"chip_seek_fast_fwd",  "chip_seek_fast_fwd"},
		{"chip_speed_dec",      "chip_speed_dec"},
		{"chip_speed_val",      "chip_speed_val"},
		{"chip_speed_inc",      "chip_speed_inc"},
		{"hint_text",           "hint"},
	};
	// clang-format on
	static_assert(KZ_ARRAYSIZE(RPMENU_VARS) == (size_t)RPVar::Count, "таблица слотов должна совпадать с RPVar");

	// Строка карточки: панель (на неё вешается selected), слот текста, семантика пункта и пара
	// чипов регулировки. chipDec/chipInc заданы РОВНО у регулируемых строк
	// (IsReplayMenuLineAdjustable) — на выбранной такой строке они подсвечиваются классом
	// focused: это и есть сигнал «сейчас работают A/D» (раньше его рисовали «< >» вокруг строки).
	struct RPMenuRowDef
	{
		const char *panelId;
		RPVar label;
		ReplayMenuLine line;
		const char *chipDec;
		const char *chipInc;
	};

	// clang-format off
	constexpr RPMenuRowDef RPMENU_ROWS[] =
	{
		{"row_play",      RPVar::RowPlay,     ReplayMenuLine::Pause,    NULL,                  NULL},
		{"row_frame",     RPVar::RowFrame,    ReplayMenuLine::Step,     "chip_frame_prev",     "chip_frame_next"},
		{"row_seek",      RPVar::RowSeek,     ReplayMenuLine::Seek,     "chip_seek_back",      "chip_seek_fwd"},
		{"row_seek_fast", RPVar::RowSeekFast, ReplayMenuLine::SeekFast, "chip_seek_fast_back", "chip_seek_fast_fwd"},
		{"row_speed",     RPVar::RowSpeed,    ReplayMenuLine::Speed,    "chip_speed_dec",      "chip_speed_inc"},
		{"row_restart",   RPVar::RowRestart,  ReplayMenuLine::Restart,  NULL,                  NULL},
		{"row_exit",      RPVar::RowExit,     ReplayMenuLine::End,      NULL,                  NULL},
	};
	// clang-format on
	constexpr i32 RPMENU_ROW_COUNT = (i32)KZ_ARRAYSIZE(RPMENU_ROWS);
	static_assert(RPMENU_ROW_COUNT == (i32)ReplayMenuLine::Count, "каждому пункту меню нужна своя строка на странице");

	// Чипы регулировки заполняются ПАРАМИ и ровно у регулируемых строк: рендер снимает и
	// ставит focused по chipDec/chipInc вместе, а проверяет только chipDec. Забытая половина
	// пары или чипы у нерегулируемой строки дали бы застрявшую подсветку — ловим сборкой,
	// а не глазами.
	constexpr bool RpMenuRowChipsConsistent()
	{
		for (size_t i = 0; i < KZ_ARRAYSIZE(RPMENU_ROWS); i++)
		{
			const bool hasDec = RPMENU_ROWS[i].chipDec != NULL;
			const bool hasInc = RPMENU_ROWS[i].chipInc != NULL;
			if (hasDec != hasInc || hasDec != KZ::replaysystem::menu::IsReplayMenuLineAdjustable(RPMENU_ROWS[i].line))
			{
				return false;
			}
		}
		return true;
	}

	static_assert(RpMenuRowChipsConsistent(), "chipDec и chipInc задаются парой и ровно у регулируемых строк");

	// Шкала: шаг классов .w-p--N/.x-p--N — 1 % (rpmenu-positions.vcss), то есть 101 значение
	// на каждый из двух префиксов.
	//
	// ШАГ 1 %, КАК У АПСТРИМА. Был 0.25 % (802 правила). Я сперва счёл этот файл причиной
	// краша клиента — БЫЛО НЕВЕРНО: бисект на канарейке 17.09.2026 показал, что страница без
	// файла позиций падала так же, а виноваты конструкции в rpmenu.css (см. шапку того файла).
	// Шаг всё равно оставлен 1 %, но по честным причинам: вчетверо меньше пар (панель, класс)
	// в m_vecHasClasses — а пара добавляется туда навсегда и уходит клиенту, отметок 32 — и
	// вчетверо меньше имён в интерн-таблице. На глаз неотличимо: шаг < 4 пикселей на шкале.
	// Генератор — tools/build_rpmenu_positions.py в аддоне.
	//
	// БЮДЖЕТ ИНТЕРНОВ (отдельная, более ранняя граница). HUD_LAYOUT_MAX_INTERNED_STRINGS =
	// 1024 действует на КАЖДЫЙ вектор имён сущности отдельно, и узкое место — вектор ИМЁН
	// КЛАССОВ: 101 + 101 на шкалу плюс статические классы карточки (RPMENU_STATIC_CLASSES).
	// Запас считает static_assert ниже. При переполнении InternString возвращает -1,
	// SetHasClass молча вернёт false, и на панели НАКОПЯТСЯ оба класса — старый снять уже
	// не выйдет, шкала замрёт с двумя ширинами сразу.
	constexpr i32 RPMENU_PROGRESS_STEPS = 100;
	// Имена классов, которые ставит этот файл помимо шкалы: rp-live, hidden, paused, selected,
	// focused, shown, cp, tp, pb, wr, other — одиннадцать, плюс по одному на ступень масштаба
	// (RPMENU_SCALE_STEP_COUNT). При добавлении нового класса обновить число.
	constexpr i32 RPMENU_STATIC_CLASSES = 11 + 5;
	// Классы позиции карточки — та же сетка, что у элементов худа: проценты от центра с шагом 1
	// внутри ±100, то есть 201 имя на ось. Худший случай (игрок прогнал степпер от края до края
	// по обеим осям за один просмотр) считаем честно, хотя вживую так не двигают: сущность меню
	// создаётся заново на каждый просмотр реплея, и интерн-таблица вместе с ней обнуляется.
	constexpr i32 RPMENU_POSITION_CLASSES = 2 * 201;
	static_assert(2 * (RPMENU_PROGRESS_STEPS + 1) + RPMENU_STATIC_CLASSES + RPMENU_POSITION_CLASSES
					  <= HUD_LAYOUT_MAX_INTERNED_STRINGS,
				  "имена классов страницы не помещаются в интерн-таблицу сущности: уменьшите число шагов");

	// Отметки на шкале (спека §1.1.1 и §5): чекпоинты и телепорты автора записи. Прыжков здесь
	// пока нет: их на бхоп-ране сотни, для них нужен свой порог прореживания
	// (data-availability.md §1).
	//
	// ПОЧЕМУ 32 ПАНЕЛИ, А НЕ БОЛЬШЕ. Шкала при масштабе 100 % шириной 334 px (карточка 360 минус
	// рамка и поля), различимый зазор между штрихами — 8 px (спека §5), то есть больше ~40
	// отметок на ней физически не прочитать. 32 при зазоре 2 % дают шаг ≈ 6.7 px — уже предел.
	// Панели сверх этого не добавили бы читаемости, зато каждая — это ещё имя панели в
	// интерн-таблице сущности и до трёх пар (панель, класс) в её сетевом векторе НАВСЕГДА.
	// Поэтому набор фиксирован, а «не помещается» решается прореживанием, а не ростом числа
	// панелей (BuildReplayMarks).
	constexpr i32 RPMENU_MARK_PANELS = 32;
	// Минимальный зазор между отметками — 2 % шкалы (8 px из спеки при ширине 390). Именно
	// МИНИМАЛЬНЫЙ: на длинной записи с сотней чекпоинтов прореживание поднимает его само, пока
	// набор не влезет в панели. Обрезать «первые 32 по порядку» нельзя — так пропадал хвост.
	constexpr i32 RPMENU_MARK_MIN_GAP = 2;
	// Классы вида отметки; индекс = тип, порядок держать вместе с rpmenu.css.
	constexpr const char *RPMENU_MARK_CLASSES[] = {"cp", "tp"};
	constexpr i32 RPMARK_CP = 0;
	constexpr i32 RPMARK_TP = 1;

	// Классы расцветки бейджа типа записи (спека §3.7). Индекс кэшируется в ReplayMenuPageState,
	// чтобы снимать ровно тот класс, который стоит сейчас.
	constexpr const char *RPMENU_BADGE_CLASSES[] = {"pb", "wr", "other"};

	i32 ReplayBadgeClassIndex(const char *cls)
	{
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(RPMENU_BADGE_CLASSES); i++)
		{
			if (KZ_STREQ(cls, RPMENU_BADGE_CLASSES[i]))
			{
				return i;
			}
		}
		return 2; // other
	}

	// Позиции отметок в ШАГАХ шкалы (0..RPMENU_PROGRESS_STEPS), уже прорежённые и обрезанные по
	// числу панелей. Считаются один раз на запись: один проход по tickData (счётчики чекпоинтов
	// и телепортов лежат в КАЖДОМ тике, см. data-availability.md §1), а не каждый кадр.
	void BuildReplayMarks(std::vector<i32> &steps, std::vector<i32> &types)
	{
		using namespace KZ::replaysystem;
		steps.clear();
		types.clear();
		const auto *replay = data::GetCurrentReplay();
		if (!data::IsReplayPlaying() || !replay->tickData || replay->tickCount < 2)
		{
			return;
		}
		// Отметки обязаны лежать в ТОЙ ЖЕ шкале, что и заполнение (позиция/длительность рана,
		// menu.cpp), иначе на предстартовой части записи они разъехались бы с ручкой. Окно
		// рана — последняя пара START/END; её нет у джамп-реплея и ручной записи, тогда шкала
		// это вся запись.
		u32 winStart = 0;
		u32 winEnd = replay->tickCount - 1;
		i32 courseId = -1;
		if (!playback::RunWindowFromEvents(replay->tickData, replay->tickCount, replay->events, replay->numEvents, winStart, winEnd, courseId))
		{
			winStart = 0;
			winEnd = replay->tickCount - 1;
		}
		const u32 effStart = playback::RawTickToEffective(winStart);
		const u32 effEnd = playback::RawTickToEffective(winEnd);
		if (effEnd <= effStart)
		{
			return;
		}
		const f64 span = (f64)(effEnd - effStart);

		// ПРОХОД ПЕРВЫЙ: раскладываем отметки по КОРЗИНАМ шага шкалы (0..RPMENU_PROGRESS_STEPS).
		// Корзины, а не список: класс позиции у нас с шагом 1 %, две отметки внутри одного шага
		// всё равно нарисовались бы в одной точке. Заодно это ограничивает память проходом по
		// записи любой длины — корзин ровно 101, сколько бы чекпоинтов игрок ни поставил.
		i32 slot[RPMENU_PROGRESS_STEPS + 1];
		for (i32 i = 0; i <= RPMENU_PROGRESS_STEPS; i++)
		{
			slot[i] = -1;
		}
		const auto put = [&](u32 rawTick, i32 type)
		{
			const u32 eff = playback::RawTickToEffective(rawTick);
			if (eff < effStart || eff > effEnd)
			{
				return;
			}
			const i32 step = (i32)(((f64)(eff - effStart) / span) * RPMENU_PROGRESS_STEPS + 0.5);
			if (step < 0 || step > RPMENU_PROGRESS_STEPS)
			{
				return;
			}
			// Телепорт важнее чекпоинта: он и рисуется заметнее (выше, оранжевым), и означает
			// возврат назад, а не просто сохранение позиции.
			if (slot[step] < 0 || type == RPMARK_TP)
			{
				slot[step] = type;
			}
		};
		// Счётчики монотонно растут — момент роста и есть постановка чекпоинта / телепорт.
		// Проход идёт по ВСЕЙ записи: прерывать его, набрав RPMENU_MARK_PANELS отметок, нельзя —
		// именно так и пропадал хвост длинного рана (баг 17.09.2026: на WR kz_bhop_nothing_go
		// после 11-й минуты отметок не было вовсе, потому что панели кончались раньше).
		i32 prevCp = replay->tickData[0].checkpoint.checkpointCount;
		i32 prevTp = replay->tickData[0].checkpoint.teleportCount;
		for (u32 i = 1; i < replay->tickCount; i++)
		{
			const i32 cp = replay->tickData[i].checkpoint.checkpointCount;
			const i32 tp = replay->tickData[i].checkpoint.teleportCount;
			if (tp > prevTp)
			{
				put(i, RPMARK_TP);
			}
			else if (cp > prevCp)
			{
				put(i, RPMARK_CP);
			}
			prevCp = cp;
			prevTp = tp;
		}

		// ПРОХОД ВТОРОЙ: прореживание РАВНОМЕРНОЕ по всей шкале. Зазор растёт, пока набор не
		// влезет в число панелей, поэтому на длинном ране отметки становятся реже, но остаются
		// везде — в отличие от обрезки по счётчику, которая просто теряла конец записи.
		// Цикл конечен: при зазоре ceil((RPMENU_PROGRESS_STEPS + 1) / RPMENU_MARK_PANELS) = 4
		// отметок не может быть больше 26; верхняя граница в условии — страховка на случай
		// правки констант, а не рабочий путь.
		for (i32 gap = RPMENU_MARK_MIN_GAP; gap <= RPMENU_PROGRESS_STEPS + 1; gap++)
		{
			steps.clear();
			types.clear();
			i32 lastStep = -gap;
			for (i32 step = 0; step <= RPMENU_PROGRESS_STEPS; step++)
			{
				if (slot[step] < 0 || step - lastStep < gap)
				{
					continue;
				}
				lastStep = step;
				steps.push_back(step);
				types.push_back(slot[step]);
			}
			if ((i32)steps.size() <= RPMENU_MARK_PANELS)
			{
				break;
			}
		}
	}

	constexpr i32 RPMENU_REPEAT_DELAY_TICKS = 22;

	// Повтор создания сущности после отказа — раз в секунду, не каждый тик: меню живёт весь
	// просмотр, и отказ (схема разъехалась после апдейта CS2) иначе давал бы 64 KZ_LOG_ERROR в
	// секунду на каждого спектатора. Логируется только первый отказ.
	constexpr i32 RPMENU_RETRY_TICKS = 64;

	// «Нет данных» для слотов времени — формат из спеки (§4): та же ширина в моно, что и у
	// настоящего времени, поэтому карточка не дёргается.
	const char *const RPMENU_TIME_UNKNOWN = "--:--.---";

	// Открыто ли у слота любое cs2menus-меню (например, выбор реплея по нику): оно читает те же
	// W/S/E/A/D, и пока оно на экране, карточка ввод не трогает — иначе E из поиска реплея
	// ставил бы паузу или завершал просмотр.
	bool HasForeignMenu(KZPlayer *player)
	{
		return g_pMenus != nullptr && g_pMenus->GetActiveMenu(player->GetPlayerSlot().Get()) != kInvalidMenuHandle;
	}

	// Один класс на панель БЕЗ кэша — зовётся только в момент смены состояния (что уже
	// выставлено, помнит ReplayMenuPageState). Лог — на отказе добавить класс, как в entity.cpp.
	// Возвращает true, если класс реально переключили: вызывающий копит этот признак и один
	// раз за рендер метит слой на полный пересчёт (см. MarkFullChanged ниже).
	bool ApplyClass(KZPlayer *player, CCSCustomHudLayout *layout, const char *panelId, const char *className, bool on)
	{
		if (cyb_rpmenu_fill.Get() < 2)
		{
			return false; // диагностика: уровень 1 — только тексты, классы не трогаем
		}
		const EHudPanelClassStatus_t status = on ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass;
		if (!layout->SetHasClass(panelId, className, status))
		{
			if (on)
			{
				LogHudInternFailure(player, panelId, className);
			}
			return false;
		}
		return true;
	}

	// Классы позиции/ширины шкалы: step — те же 0.25 %, что и в сгенерированном листе позиций.
	void ApplyProgressClass(KZPlayer *player, CCSCustomHudLayout *layout, const char *panelId, const char *prefix, i32 step, bool on)
	{
		if (step < 0)
		{
			return;
		}
		if (cyb_rpmenu_fill.Get() < 3)
		{
			return; // диагностика: шкала и отметки — самый объёмный набор пар (панель,класс)
		}
		char className[16];
		V_snprintf(className, sizeof(className), "%s-p--%i", prefix, step);
		ApplyClass(player, layout, panelId, className, on);
	}

	// Удержанные кнопки наблюдательской пешки. GetPlayerPawn() у спектатора — игровая пешка
	// (мёртвая или отсутствующая), кнопки же приходят на ТЕКУЩУЮ (observer) — берём её, как
	// cs2menus (GetInputPawn). 0 — пешки/сервисов нет.
	u64 HeldButtons(KZPlayer *player)
	{
		CCSPlayerController *controller = player ? player->GetController() : nullptr;
		CBasePlayerPawn *pawn = controller ? controller->GetCurrentPawn() : nullptr;
		CPlayer_MovementServices *ms = pawn ? pawn->m_pMovementServices() : nullptr;
		if (!ms)
		{
			return 0;
		}
		return ms->m_nButtons().m_pButtonStates[0];
	}
} // namespace

// === Доступность ============================================================================

const char *KZHUDService::ReplayMenuUnavailableReason()
{
	if (g_KZPlugin.unloading)
	{
		return "unloading";
	}
	if (!KZHUDService::IsMHUDAvailable())
	{
		return "no_addon";
	}
	if (!KZ::replaysystem::data::IsReplayPlaying())
	{
		return "no_replay";
	}
	KZPlayer *target = this->player->specService->GetSpectatedPlayer();
	if (!target || !KZ::replaysystem::IsReplayBot(target))
	{
		return "not_spectating_bot";
	}
	return NULL;
}

// === Сущность ===============================================================================

CCSCustomHudLayout *KZHUDService::EnsureReplayLayout(bool &created)
{
	created = false;
	if (g_KZPlugin.unloading || !KZHUDService::IsMHUDAvailable())
	{
		return NULL;
	}
	if (CBaseEntity *cached = this->ownedReplayLayout.Get())
	{
		return (CCSCustomHudLayout *)cached;
	}
	CCSCustomHudLayout *layout = utils::CreateEntityByName<CCSCustomHudLayout>("custom_hud_layout");
	if (!layout)
	{
		return NULL;
	}
	CEntityKeyValues *pKeyValues = new CEntityKeyValues();
	const CUtlString &pageOverride = cyb_rpmenu_page.Get();
	const char *page = (pageOverride.Get() && pageOverride.Get()[0]) ? pageOverride.Get() : KZ_RPMENU_LAYOUT;
	pKeyValues->SetString("layout", page);
	char name[32];
	V_snprintf(name, sizeof(name), "kzrpmenu%i", this->player->GetPlayerSlot().Get());
	pKeyValues->SetString("targetname", name);
	layout->DispatchSpawn(pKeyValues);
	this->ownedReplayLayout = layout;
	created = true;
	// Диф-кэш — состояние ПРЕДЫДУЩЕЙ сущности, сбрасываем в момент реального создания
	// (тот же урок, что у EnsureMenuLayout).
	this->replayPage = ReplayMenuPageState();
	return layout;
}

void KZHUDService::DestroyOwnedReplayLayout()
{
	if (!this->ownedReplayLayout.IsValid())
	{
		return;
	}
	if (CBaseEntity *ent = this->ownedReplayLayout.Get())
	{
		g_pKZUtils->RemoveEntity(ent);
	}
	this->ownedReplayLayout = nullptr;
	this->replayPage = ReplayMenuPageState();
}

// === Открытие / закрытие ====================================================================

bool KZHUDService::OpenReplayMenu()
{
	if (this->replayMenuOpen)
	{
		return true;
	}
	bool created = false;
	CCSCustomHudLayout *layout = this->EnsureReplayLayout(created);
	if (!layout)
	{
		// Отказ, не выбор: сущность не создалась (схема разъехалась после апдейта CS2 /
		// MultiAddonManager нет). Лог — один раз на серию отказов (см. RPMENU_RETRY_TICKS).
		if (!this->replayMenuFailLogged)
		{
			KZ_LOG_ERROR(LogChannel::General, "[cyb] replay_menu_unavailable reason=layout_entity_failed slot=%i\n",
						 this->player->GetPlayerSlot().Get());
			this->replayMenuFailLogged = true;
		}
		this->replayMenuRetryTick = g_pKZUtils->GetServerGlobals()->tickcount;
		return false;
	}
	if (this->replayMenuFailLogged)
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb] replay_menu_recovered slot=%i\n", this->player->GetPlayerSlot().Get());
		this->replayMenuFailLogged = false;
	}
	this->replayMenuOpen = true;
	this->replayMenuRow = 0;
	// Стартовая маска = то, что уже удержано: иначе зажатая при открытии клавиша (например,
	// W из движения перед спектейтом) сработала бы как нажатие на первом же тике.
	this->replayMenuHeld = HeldButtons(this->player);
	this->replayMenuHoldTicks = 0;
	KZ_LOG_INFO(LogChannel::General, "[cyb] replay_menu_open slot=%i page=%s fill=%i\n", this->player->GetPlayerSlot().Get(),
				cyb_rpmenu_page.Get().Get() && cyb_rpmenu_page.Get().Get()[0] ? cyb_rpmenu_page.Get().Get() : KZ_RPMENU_LAYOUT, cyb_rpmenu_fill.Get());
	this->RenderReplayMenu(layout, true);
	return true;
}

void KZHUDService::CloseReplayMenu(const char *reason)
{
	if (this->replayMenuOpen)
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb] replay_menu_close reason=%s slot=%i\n", reason ? reason : "unknown",
					this->player->GetPlayerSlot().Get());
	}
	this->replayMenuOpen = false;
	this->replayMenuHeld = 0;
	this->replayMenuHoldTicks = 0;
	this->replayMenuFailLogged = false;
	this->replayMenuRetryTick = 0;
	this->DestroyOwnedReplayLayout();
}

// === Тик: доступность → открыть/закрыть, ввод, рендер ======================================

void KZHUDService::UpdateReplayMenu(KZPlayer *source)
{
	// Меню живёт ровно пока игрок наблюдает бота с идущим плейбеком и есть аддон. source —
	// то, что DrawPanels считает источником данных; для спектатора это наблюдаемый.
	const bool wanted = source && source != this->player && KZ::replaysystem::IsReplayBot(source) && this->ReplayMenuUnavailableReason() == NULL;
	if (!wanted)
	{
		if (this->replayMenuOpen)
		{
			const char *reason = this->ReplayMenuUnavailableReason();
			this->CloseReplayMenu(reason ? reason : "target_changed");
		}
		return;
	}
	if (!this->replayMenuOpen)
	{
		// После отказа сущности — повтор с бэкоффом, не каждый тик (см. RPMENU_RETRY_TICKS).
		if (this->replayMenuFailLogged && g_pKZUtils->GetServerGlobals()->tickcount - this->replayMenuRetryTick < RPMENU_RETRY_TICKS)
		{
			return;
		}
		if (!this->OpenReplayMenu())
		{
			return; // причина уже в логе (первый отказ серии)
		}
	}
	bool created = false;
	CCSCustomHudLayout *layout = this->EnsureReplayLayout(created);
	if (!layout)
	{
		this->CloseReplayMenu("layout_entity_failed");
		return;
	}

	if (HasForeignMenu(this->player))
	{
		// Чужое cs2menus-меню на экране: клавиши его. Маску держим актуальной, чтобы после его
		// закрытия удержанная клавиша не сработала ложным фронтом.
		this->replayMenuHeld = HeldButtons(this->player);
		this->replayMenuHoldTicks = 0;
	}
	else
	{
		this->ReadReplayMenuInput();
	}
	// Ввод мог остановить реплей («Завершить») — тогда меню уже закрыто и сущности нет.
	if (!this->replayMenuOpen)
	{
		return;
	}
	if (!KZ::replaysystem::data::IsReplayPlaying())
	{
		this->CloseReplayMenu("replay_stopped");
		return;
	}
	this->RenderReplayMenu(layout, created);
}

void KZHUDService::ReadReplayMenuInput()
{
	const u64 held = HeldButtons(this->player);
	const u64 pressed = held & ~this->replayMenuHeld;
	this->replayMenuHeld = held;

	if (pressed & IN_FORWARD)
	{
		this->replayMenuRow = (this->replayMenuRow + RPMENU_ROW_COUNT - 1) % RPMENU_ROW_COUNT;
	}
	if (pressed & IN_BACK)
	{
		this->replayMenuRow = (this->replayMenuRow + 1) % RPMENU_ROW_COUNT;
	}

	const ReplayMenuLine line = RPMENU_ROWS[this->replayMenuRow].line;
	if (pressed & IN_USE)
	{
		KZ::replaysystem::menu::ApplyReplayMenuInput(this->player, line, ReplayMenuInput::Select);
		if (line == ReplayMenuLine::End)
		{
			this->CloseReplayMenu("end_selected");
			return;
		}
	}

	// A/D: фронт нажатия — сразу; удержание — автоповтор после паузы (шаг по тикам и
	// перемотка на удержании — тот же жест, что в cs2menus). Нерегулируемые строки A/D не трогают.
	const u64 adjust = held & (IN_MOVELEFT | IN_MOVERIGHT);
	if (!adjust || adjust == (IN_MOVELEFT | IN_MOVERIGHT) || !KZ::replaysystem::menu::IsReplayMenuLineAdjustable(line))
	{
		this->replayMenuHoldTicks = 0;
		return;
	}
	this->replayMenuHoldTicks++;
	const bool edge = (pressed & adjust) != 0;
	const i32 period = (std::max)(kz_rpmenu_repeat_ticks.Get(), 1);
	const bool repeat =
		this->replayMenuHoldTicks > RPMENU_REPEAT_DELAY_TICKS && (this->replayMenuHoldTicks - RPMENU_REPEAT_DELAY_TICKS) % period == 0;
	if (edge || repeat)
	{
		KZ::replaysystem::menu::ApplyReplayMenuInput(this->player, line, adjust & IN_MOVERIGHT ? ReplayMenuInput::Inc : ReplayMenuInput::Dec);
	}
}

// === Запись страницы ========================================================================

void KZHUDService::SetReplayMenuVar(CCSCustomHudLayout *layout, i32 var, const char *text, bool force)
{
	std::string &cached = this->replayPage.vars[(size_t)var];
	if (!force && cached == text)
	{
		return;
	}
	const RPMenuVarDef &def = RPMENU_VARS[var];
	if (!layout->SetDialogVariableString(def.panelId, def.varName, text))
	{
		// Кэш пишем ТОЛЬКО на успехе: отказ (интерн-таблица сущности переполнена) иначе
		// защёлкнул бы слот навсегда — следующий кадр решил бы, что там уже нужный текст, и
		// на странице до конца просмотра висел бы плейсхолдер разметки.
		LogHudInternFailure(this->player, def.panelId, def.varName);
		return;
	}
	cached = text;
}

void KZHUDService::RenderReplayMenu(CCSCustomHudLayout *layout, bool force)
{
	const int fill = cyb_rpmenu_fill.Get();
	if (fill <= 0)
	{
		return; // диагностика: сущность есть, страница грузится, данных нет
	}
	ReplayMenuPageState &state = this->replayPage;
	// Свежая сущность: у страницы ещё НЕТ ни одной dialog-переменной, а кэш (пустые строки)
	// совпал бы с пустым текстом и молча ничего не отправил — первый кадр шлёт всё подряд.
	// Дальше force пробрасывается в КАЖДУЮ отправку, иначе слот, пустой на старте, не
	// отправился бы вовсе и остался бы с плейсхолдером из vxml.
	if (force || state.vars.empty())
	{
		state = ReplayMenuPageState();
		state.vars.assign((size_t)RPVar::Count, std::string());
		force = true;
	}
	const char *lang = this->player->languageService->GetLanguage();

	// Масштаб карточки — единственный преф меню реплея, который читает страница (rpmenuScale,
	// ступени RPMENU_SCALE_STEPS). Класс висит на САМОЙ карточке, а не на корне страницы:
	// корневой панели resourcecompiler запрещает иметь id (проверено компиляцией 17.09.2026), а
	// без id SetHasClass её не найдёт. Вид задан потомками — значит переключение класса метит
	// слой на полный пересчёт (см. classDirty ниже).
	const MHUDLayoutPrefs &prefs = this->GetOwnLayoutPrefs();
	const i32 scale = prefs.replayMenu.scale;
	if (force || state.scale != scale)
	{
		char scaleClass[24];
		if (!force && state.scale > 0)
		{
			V_snprintf(scaleClass, sizeof(scaleClass), "rp-scale--%i", state.scale);
			state.classDirty |= ApplyClass(this->player, layout, "replay_card", scaleClass, false);
		}
		state.scale = scale;
		V_snprintf(scaleClass, sizeof(scaleClass), "rp-scale--%i", scale);
		state.classDirty |= ApplyClass(this->player, layout, "replay_card", scaleClass, true);
	}

	// Позиция карточки — проценты от центра экрана теми же классами, что у элементов худа
	// (.x--[neg]Npct / .y--[neg]Npct из cs2kz/positions.css). SetLayoutValueClass сам снимает
	// прошлый класс, ставит новый и логирует упор в интерн-таблицу.
	//
	// classDirty здесь НЕ нужен: x/y красят саму карточку, а не её потомков — тот же довод, что
	// у классов шкалы. Это важно не из экономии: игрок двигает карточку степпером, и пометка
	// слоя на полный пересчёт шла бы на каждое нажатие «+».
	this->SetLayoutValueClass(layout, "replay_card", state.posX, prefs.replayMenu.posX, "x", true);
	this->SetLayoutValueClass(layout, "replay_card", state.posY, prefs.replayMenu.posY, "y", true);

	if (force)
	{
		// Заполнение шкалы идёт за плейбеком каждый тик — без сглаживания, иначе оно вечно
		// догоняет позицию (.seek-fill.rp-live). Класс один на сущность, кэша у ApplyClass нет,
		// поэтому ставим его ровно на создании: слать SetHasClass каждый тик — лишний сетевой
		// апдейт ради значения, которое не меняется.
		ApplyClass(this->player, layout, "seek_fill", "rp-live", true);
	}

	// Слоты с НЕИЗМЕННЫМ текстом. Идут через тот же диф-кэш, а не разово под force: отправка
	// может не удаться (интерн-таблица сущности переполнена), и разовая попытка оставила бы
	// слот с плейсхолдером «{s:...}» навсегда — кэш же пишется только на успехе и заставит
	// повторить на следующем кадре. Цена — десяток сравнений строк на тик, без сети.
	// Чипы у нас индикаторы, а не кнопки (мыши нет): показывают клавишу, которой строка
	// управляется. Знаки минуса/плюса — ASCII: типографские «−»/«±» из спеки в игровом
	// шрифте не гарантированы.
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipKeyPlay, "E", force);
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipKeyRestart, "E", force);
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipKeyExit, "E", force);
	// Стрелки макета против букв A/D — переключателем cyb_rpmenu_arrow_chips: есть ли эти
	// глифы в игровом шрифте, локально не проверить (см. ручку).
	const bool arrows = cyb_rpmenu_arrow_chips.Get();
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipFramePrev, arrows ? "\xE2\x97\x80" : "A", force);
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipFrameNext, arrows ? "\xE2\x96\xB6" : "D", force);
	char seekBack[16], seekFwd[16];
	V_snprintf(seekBack, sizeof(seekBack), "-%is", KZ::replaysystem::menu::GetReplayMenuSeekStepSeconds(false));
	V_snprintf(seekFwd, sizeof(seekFwd), "+%is", KZ::replaysystem::menu::GetReplayMenuSeekStepSeconds(false));
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipSeekBack, seekBack, force);
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipSeekFwd, seekFwd, force);
	V_snprintf(seekBack, sizeof(seekBack), "-%is", KZ::replaysystem::menu::GetReplayMenuSeekStepSeconds(true));
	V_snprintf(seekFwd, sizeof(seekFwd), "+%is", KZ::replaysystem::menu::GetReplayMenuSeekStepSeconds(true));
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipSeekFastBack, seekBack, force);
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipSeekFastFwd, seekFwd, force);
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipSpeedDec, "-", force);
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipSpeedInc, "+", force);
	// Превью времени под курсором сервер не наполняет (мышиного режима нет), но переменная
	// обязана СУЩЕСТВОВАТЬ, иначе на странице останется её плейсхолдер «{s:...}». Именно здесь
	// и нужен проброс force: на первом кадре кэш пуст, и без него пустой текст «совпал» бы с
	// кэшем и не ушёл бы вовсе. Строки тиков под шкалой на странице больше нет (решение
	// владельца 17.09: не нужна) — вместе с ней ушёл и слот.
	this->SetReplayMenuVar(layout, (i32)RPVar::SeekPreview, "", force);

	// Шапка: ник автора записи, подстрочник «карта · курс · режим» и бейдж AWR — в старой
	// карточке метка AWR стояла в начале строки состояния.
	this->SetReplayMenuVar(layout, (i32)RPVar::Nick, KZ::replaysystem::menu::GetReplayMenuAuthorName().c_str(), force);
	std::string meta = KZ::replaysystem::menu::GetReplayMenuMetaText();
	// «—» — вариант «нет данных» из спеки (§4): панель meta высоту держит в любом случае.
	this->SetReplayMenuVar(layout, (i32)RPVar::Meta, meta.empty() ? "\xE2\x80\x94" : meta.c_str(), force);
	// Бейдж типа записи. Раньше он был завязан ТОЛЬКО на AWR-режим, поэтому PB/WR не появлялись
	// никогда: тип реплея — свойство ЗАПРОСА (`!replay pb/wr/...`), в файле его нет. Теперь вид
	// запроса доезжает до плейбека (replay->badgeKind) и бейдж показывает его.
	KZ::replaysystem::menu::ReplayMenuBadge badge {};
	KZ::replaysystem::menu::GetReplayMenuBadge(badge);
	const bool badgeShown = !badge.text.empty();
	this->SetReplayMenuVar(layout, (i32)RPVar::Badge, badge.text.c_str(), force);
	const i32 badgeClass = ReplayBadgeClassIndex(badge.cls);
	if (force || state.badgeShown != (i32)badgeShown)
	{
		state.badgeShown = (i32)badgeShown;
		ApplyClass(this->player, layout, "badge_type", "hidden", !badgeShown);
	}
	if (force || state.badgeClass != badgeClass)
	{
		if (!force && state.badgeClass >= 0)
		{
			ApplyClass(this->player, layout, "badge_type", RPMENU_BADGE_CLASSES[state.badgeClass], false);
		}
		state.badgeClass = badgeClass;
		ApplyClass(this->player, layout, "badge_type", RPMENU_BADGE_CLASSES[badgeClass], true);
	}

	// Время, скорость и состояние плейбека.
	ReplayMenuStatus status {};
	KZ::replaysystem::menu::GetReplayMenuStatus(status);
	// Точность — precise (mm:ss.mmm, с часом h:mm:ss.mmm): так требует спека (§4 и
	// data-availability §5), под неё же посчитаны ширины полей. Старая карточка на разметке
	// худа показывала M:SS осознанно — там строка состояния была одной живой строкой мелким
	// кеглем. Цена решения: миллисекунды меняются каждый тик, поэтому диф-кэш на двух слотах
	// позиции не срабатывает и они уходят каждый тик (две записи в уже существующие элементы
	// вектора, полного пересчёта сущности это НЕ вызывает). Длительность при этом кэшируется.
	char position[32], total[32];
	utils::FormatTime(status.position, position, sizeof(position));
	utils::FormatTime(status.total, total, sizeof(total));
	const bool hasTotal = status.total > 0.0;
	const char *totalText = hasTotal ? total : RPMENU_TIME_UNKNOWN;
	// Подпись большого поля: у реплея рана это время рана, у записи без таймера (джамп,
	// ручная) — позиция в записи.
	const std::string caption = KZLanguageService::PrepareMessageWithLang(
		lang, KZ::replaysystem::menu::IsReplayMenuRunReplay() ? "Replay Panel - Time Caption Run" : "Replay Panel - Time Caption Position");
	this->SetReplayMenuVar(layout, (i32)RPVar::TimeCaption, caption.c_str(), force);
	this->SetReplayMenuVar(layout, (i32)RPVar::TimeCurrent, position, force);
	this->SetReplayMenuVar(layout, (i32)RPVar::PosTime, position, force);
	this->SetReplayMenuVar(layout, (i32)RPVar::DurTime, totalText, force);
	char finalText[40];
	V_snprintf(finalText, sizeof(finalText), "/ %s", totalText);
	this->SetReplayMenuVar(layout, (i32)RPVar::TimeFinal, finalText, force);
	// Итогового времени нет (длительность неизвестна) — слот прячем, а не показываем прочерк:
	// прочерк уже стоит справа от шкалы (dur_time), спека §4 велит именно скрыть.
	if (force || state.finalShown != (i32)hasTotal)
	{
		state.finalShown = (i32)hasTotal;
		ApplyClass(this->player, layout, "time_final", "hidden", !hasTotal);
	}

	char speedText[24];
	V_snprintf(speedText, sizeof(speedText), "%sx", status.speed);
	this->SetReplayMenuVar(layout, (i32)RPVar::ChipSpeedVal, speedText, force);
	// Пилюля состояния (спека §3.6): на ходу — скорость, на паузе — слово «ПАУЗА». Глиф —
	// знак воспроизведения/паузы; цвет и пульсацию даёт класс paused.
	const std::string pausedText =
		status.paused ? KZLanguageService::PrepareMessageWithLang(lang, "Replay Panel - State Paused") : std::string();
	this->SetReplayMenuVar(layout, (i32)RPVar::StateText, status.paused ? pausedText.c_str() : speedText, force);
	this->SetReplayMenuVar(layout, (i32)RPVar::StateGlyph, status.paused ? "\xE2\x9D\x99\xE2\x9D\x99" : "\xE2\x96\xB6", force);
	if (force || state.paused != (i32)status.paused)
	{
		state.paused = (i32)status.paused;
		state.classDirty |= ApplyClass(this->player, layout, "state_pill", "paused", status.paused);
	}

	// Шкала: заполнение и ручка одним шагом (0.25 %) — класс снимается у прошлого значения и
	// ставится новому, иначе на панели копились бы оба.
	i32 progress = 0;
	if (hasTotal)
	{
		const f64 ratio = status.position / status.total;
		progress = (i32)((ratio < 0.0 ? 0.0 : (ratio > 1.0 ? 1.0 : ratio)) * RPMENU_PROGRESS_STEPS + 0.5);
	}
	if (force || state.progress != progress)
	{
		if (!force)
		{
			ApplyProgressClass(this->player, layout, "seek_fill", "w", state.progress, false);
			ApplyProgressClass(this->player, layout, "seek_cursor", "x", state.progress, false);
		}
		state.progress = progress;
		ApplyProgressClass(this->player, layout, "seek_fill", "w", progress, true);
		ApplyProgressClass(this->player, layout, "seek_cursor", "x", progress, true);
	}

	// Отметки чекпоинтов и телепортов. Считаются один раз на запись (проход по tickData) и
	// дальше не трогаются: позиции отметок от плейбека не зависят. Ключ пересчёта — uuid
	// записи: плейбек один на сервер, но запись в нём может смениться, пока меню открыто.
	const std::string markUuid = KZ::replaysystem::data::IsReplayPlaying() ? KZ::replaysystem::data::GetCurrentReplay()->uuid.ToString() : "";
	if (force || state.markUuid != markUuid)
	{
		std::vector<i32> steps, types;
		BuildReplayMarks(steps, types);
		state.markStep.resize(RPMENU_MARK_PANELS, -1);
		state.markType.resize(RPMENU_MARK_PANELS, -1);
		for (i32 i = 0; i < RPMENU_MARK_PANELS; i++)
		{
			char panelId[16];
			V_snprintf(panelId, sizeof(panelId), "mark%i", i);
			const i32 newStep = i < (i32)steps.size() ? steps[i] : -1;
			const i32 newType = i < (i32)types.size() ? types[i] : -1;
			// Снимаем ровно то, что стоит сейчас: пара (панель, класс) остаётся в сетевом
			// векторе навсегда, и забытый старый класс дал бы отметку с двумя позициями.
			if (!force && state.markStep[i] >= 0)
			{
				ApplyProgressClass(this->player, layout, panelId, "x", state.markStep[i], false);
				ApplyClass(this->player, layout, panelId, RPMENU_MARK_CLASSES[state.markType[i]], false);
			}
			if (newStep < 0)
			{
				if (force || state.markStep[i] >= 0)
				{
					ApplyClass(this->player, layout, panelId, "shown", false);
				}
			}
			else
			{
				ApplyProgressClass(this->player, layout, panelId, "x", newStep, true);
				ApplyClass(this->player, layout, panelId, RPMENU_MARK_CLASSES[newType], true);
				ApplyClass(this->player, layout, panelId, "shown", true);
			}
			state.markStep[i] = newStep;
			state.markType[i] = newType;
		}
		state.markUuid = markUuid;
	}

	// Строки и подсказка.
	for (i32 i = 0; i < RPMENU_ROW_COUNT; i++)
	{
		const RPMenuRowDef &row = RPMENU_ROWS[i];
		this->SetReplayMenuVar(layout, (i32)row.label, KZ::replaysystem::menu::GetReplayMenuLineText(this->player, row.line).c_str(), force);
	}
	if (force || state.selectedRow != this->replayMenuRow)
	{
		if (!force && state.selectedRow >= 0)
		{
			const RPMenuRowDef &prev = RPMENU_ROWS[state.selectedRow];
			state.classDirty |= ApplyClass(this->player, layout, prev.panelId, "selected", false);
			if (prev.chipDec)
			{
				state.classDirty |= ApplyClass(this->player, layout, prev.chipDec, "focused", false);
				state.classDirty |= ApplyClass(this->player, layout, prev.chipInc, "focused", false);
			}
		}
		state.selectedRow = this->replayMenuRow;
		const RPMenuRowDef &cur = RPMENU_ROWS[state.selectedRow];
		state.classDirty |= ApplyClass(this->player, layout, cur.panelId, "selected", true);
		if (cur.chipDec)
		{
			state.classDirty |= ApplyClass(this->player, layout, cur.chipDec, "focused", true);
			state.classDirty |= ApplyClass(this->player, layout, cur.chipInc, "focused", true);
		}
	}
	const ReplayMenuLine selectedLine = RPMENU_ROWS[this->replayMenuRow].line;
	this->SetReplayMenuVar(layout, (i32)RPVar::Hint, KZ::replaysystem::menu::GetReplayMenuHintText(this->player, selectedLine).c_str(), force);

	// Движковый баг (sdk/entity/ccscustomhudlayout.h:179-181): SetHasClass не доезжает до
	// ДЕТЕЙ панели, а вид строки и пилюли задан именно потомками (.row.selected .row-mark,
	// .row.selected .row-label, .chip.focused .chip-key, .state-pill.paused .state-glyph).
	// Первое появление НОВОЙ пары (панель,класс) метит слой на полный пересчёт само, поэтому
	// первый проход выглядит верно, а повторное переключение уже интернированной пары — нет:
	// подсветка залипала бы на прошлой строке. Меню настроек обходит это тем же вызовом
	// (menu.cpp), но БЕЗУСЛОВНО на каждый рендер; здесь так нельзя — карточка рисуется каждый
	// тик плейбека, и это было бы 64 полных ресенда сущности в секунду. Поэтому метим только
	// когда класс реально переключили — на плейбеке это редкие кадры (шаг W/S, пауза).
	// Классы шкалы (w-p--/x-p--) сюда НЕ попадают намеренно: .seek-fill и .seek-cursor
	// красят сами себя, потомков у них нет. Сама пометка ни строк, ни классов не заводит,
	// лимита HUD_LAYOUT_MAX_INTERNED_STRINGS не касается.
	if (state.classDirty)
	{
		state.classDirty = false;
		layout->GetGlobalLayoutState()->MarkFullChanged();
	}
}
