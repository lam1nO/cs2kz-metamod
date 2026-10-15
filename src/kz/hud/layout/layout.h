#pragma once
#include "kz/hud/kz_hud.h"

// Дефолты элементов panorama-худа: X/Y — ПРОЦЕНТЫ от центра, size — пиксели.
// Не путать с particle-MHUD, где офсеты в его собственных юнитах (префы там отдельные).
#define LAYOUT_DEF_TIMER_X    0
#define LAYOUT_DEF_TIMER_Y    35
#define LAYOUT_DEF_TIMER_SIZE 24

#define LAYOUT_DEF_SPEED_X    0
// Худ v2 (14.10): скорость и клавиши — по раскладке владельца. Прежние значения ревизии 1
// живут в defaults.cpp (HUD_REV1_*): по ним ревизия 2 узнаёт, менял ли игрок элемент.
#define LAYOUT_DEF_SPEED_Y    6
#define LAYOUT_DEF_SPEED_SIZE 34

#define LAYOUT_DEF_PRESPEED_X    0
#define LAYOUT_DEF_PRESPEED_Y    12
#define LAYOUT_DEF_PRESPEED_SIZE 22

#define LAYOUT_DEF_KEYS_X    0
#define LAYOUT_DEF_KEYS_Y    21
#define LAYOUT_DEF_KEYS_SIZE 20
// Остальное в клавишах владельца: ненажатые скрыты (mhudKeysIdle 1), вплотную (mhudKeysGap 0),
// с обводкой, цвет MHUD_DEF_KEYS_COLOR (белый с ревизии 3, был #40FF40).
#define MHUD_DEF_KEYS_IDLE 1
#define MHUD_DEF_KEYS_GAP  0
// Отступ клавиш в px — с худа v2 без «авто» и с отрицательными значениями (плотнее, вплоть до
// наложения ячеек, как у particle-MHUD). Свой ключ: старый mhudKeysGap (-1 = авто, 0..24) читают
// старые версии KZ на флоте; новый при первом чтении выводится из него (авто → N/2, как keys-size.css).
#define MHUD_KEYS_GAP_KEY "mhudKeysGapPx"
#define MHUD_KEYS_GAP_MIN (-30)
#define MHUD_KEYS_GAP_MAX 24
// Дефолт обводки элемента: с худа v2 обводка по умолчанию только у клавиш (как у владельца).
inline bool LayoutOutlineDefault(LayoutElement element)
{
	return element == LayoutElement::Keys;
}

#define LAYOUT_DEF_CHECKPOINT_X    0
#define LAYOUT_DEF_CHECKPOINT_Y    30
#define LAYOUT_DEF_CHECKPOINT_SIZE 20

// «Прогресс» по маршруту `!lead` — дефолты владельца 28.09 (его раскладка: слева по центру
// высоты, Stratum2 Medium, #00AAFF). Для «Прогресса» смена дефолтов разрешена им явно — у
// остальных старых элементов дефолты не трогаем. Элемент по умолчанию включён.
// Ревизия 3 (15.10): позиция и кегль — как у владельца (было -42/-5, 15; см. defaults.cpp).
#define LAYOUT_DEF_LEADPROGRESS_X    (-41)
#define LAYOUT_DEF_LEADPROGRESS_Y    (-4)
// Кегль процента (подпись — 0.8 от него, в mhud2.css).
#define LAYOUT_DEF_LEADPROGRESS_SIZE 17

// Поля редактора `!hud` (PB/WR, showpos, курс, тип рана) — дефолты владельца 28.09 (его
// сохранённая раскладка; тип рана он не двигал). Все четыре по умолчанию включены.
// Исходно (чеклист H6): Борд 1 даёт их в процентах ЭКРАНА от левого верхнего угла
// (PB/WR 1/88, showpos 1/2, курс 50/2, тип рана 47/93), а у нас X/Y — проценты от ЦЕНТРА, и
// элемент центрируется на своей точке. Прямой перевод p-50 увёл бы левые элементы
// наполовину за край экрана, поэтому они подтянуты внутрь примерно на полширины элемента.
#define LAYOUT_DEF_PBWR_X    (-41)
#define LAYOUT_DEF_PBWR_Y    (-44)
// Худ v2: таблица WR/PB × NUB/PRO; кегль времени 17 с ревизии 3 (был 15).
#define LAYOUT_DEF_PBWR_SIZE 17

#define LAYOUT_DEF_SHOWPOS_X    43
#define LAYOUT_DEF_SHOWPOS_Y    (-40)
#define LAYOUT_DEF_SHOWPOS_SIZE 13

#define LAYOUT_DEF_COURSE_X    0
#define LAYOUT_DEF_COURSE_Y    (-39)
#define LAYOUT_DEF_COURSE_SIZE 15

// Тип рана — под таймером по центру (0/40): прежние -3/43 перекрывались с «Прогрессом».
#define LAYOUT_DEF_RUNTYPE_X    0
#define LAYOUT_DEF_RUNTYPE_Y    40
// Ревизия 3: бейдж крупнее (был 12).
#define LAYOUT_DEF_RUNTYPE_SIZE 16

#define LAYOUT_SIZE_MIN 8
#define LAYOUT_SIZE_MAX 100

// Настройки меню реплея — страница «Меню реплея» в `!options` (hud/prefs/hud_prefs.cpp), ключи
// читает layout/prefs.cpp:RefreshLayoutPrefs. Их РОВНО ДВЕ, и обе — списки, а не числа: сервер
// умеет над panorama-страницей только SetHasClass, поэтому каждое значение обязано иметь свой
// готовый набор правил в vcss аддона. Числовой ползунок здесь означал бы, что большинство
// значений ничего не меняют — игрок читает это как «настройка сломана».
//
// Прежние ключи (rpmenuX/Y/Size/Step/Font/Outline/Background) удалены 17.09.2026: они остались
// от карточки на разметке ХУДА и после переезда на свою страницу не влияли ни на что. Пункты
// вводили в заблуждение (замечание владельца), а сохранённые у игроков значения теперь просто
// не читаются — миграции не нужно. Состав белого списка обмена худом от этого меняется, и
// старые снимки применяются с предупреждением о другом составе: это штатное поведение обмена
// (отпечаток считается из реестра, см. hud/share/hud_share.cpp).

// Масштаб карточки: ступени процентов, класс .rp-scale--N на панели replay_card.
// Наборы правил — rpmenu2-scale.css (генератор tools/build_rpmenu2_scale.py в аддоне).
#define RPMENU_DEF_SCALE 75
extern const i32 RPMENU_SCALE_STEPS[];
extern const i32 RPMENU_SCALE_STEP_COUNT;
i32 SnapReplayMenuScale(i32 percent);

// Позиция карточки: ПРОЦЕНТЫ ОТ ЦЕНТРА экрана по обеим осям, как у элементов худа, и теми же
// классами .x--[neg]Npct / .y--[neg]Npct из cs2kz/positions.css (файл уже едет в аддоне ради
// mhud — своего набора классов заводить не пришлось). Пункт — обычный Position со степперами
// ±1/±5, ровно как у соседей (решение владельца 17.09.2026: якоря-список заменены на шаги).
//
// Дефолт по X считается так: центр карточки = 960 + (-36 % × 1920) = 268.8, то есть левый край
// при масштабе 100 % (ширина 360) — 89 px. Спека просит 48, но держать ЛЕВЫЙ край на месте при
// центрирующем позиционировании нельзя: на 130 % карточка шириной 468 уехала бы за левый край
// экрана. -36 % держит её на экране на всех пяти масштабах (75…130 %), а точную посадку игрок
// доводит теми же ±1/±5.
#define RPMENU_DEF_POS_X (-36)
#define RPMENU_DEF_POS_Y 0

// Дефолт синхронизирован с текущими настройками игрока (задача hud-defaults): было
// stratum2-bold-monodigit, стало lato-bold (слаг panorama_tables.cpp, "Lato Bold*").
#define LAYOUT_DEFAULT_FONT "lato-bold"
// Разметка живёт в нашем аддоне GYMSTRIKE-KZ (см. KZ_WORKSHOP_ADDON_ID); путь намеренно
// совпадает с апстримным — аддон собран из тех же исходников, и при откате на чужой
// айтем код менять не придётся.
// Худ v2 (спека h-spec-v2, 14.10) — ОТДЕЛЬНЫЙ файл: старые версии KZ на флоте грузят mhud.vxml_c
// из того же аддона и должны видеть его неизменным. mhud.xml и общие css аддона только
// дописываются; всё новое в mhud2.css под корневым классом .hud2.
#define KZ_MHUD_LAYOUT "panorama/layout/custom_game/cs2kz/mhud2.vxml_c"
// Шрифт новых элементов худа v2 (PB/WR, прогресс, курс, тип рана) — Stratum2 Bold TF, как у
// Кибершока и родного худа CS2: жирнее Noto и цифры одной ширины (решение владельца 14.10).
#define LAYOUT_V2_FONT "stratum2-bold-tf"
// Своя страница меню реплея спектатора (layout/rpmenu.cpp): карточка плеера целиком — шапка,
// время, шкала перемотки с отметками, семь пунктов и подсказка. Живёт ТОЛЬКО в нашем аддоне
// (custom_game/cyber/…), апстримного аналога у неё нет; исходники разметки — gymstrike-kz/content/panorama
// (rpmenu2.xml + rpmenu2.css + rpmenu2-seek.css + rpmenu2-scale.css + rpmenu-positions.css; xml и
// seek — генератор tools/gen_rpmenu2_xml.py), спека SPEC-rpmenu-gs2.md (вид gs2, вариант B, 15.10).
// Новый вид — ОТДЕЛЬНЫЙ файл, как mhud2: старые версии KZ на флоте грузят rpmenu.vxml_c из того же
// аддона, и он остаётся неизменным.
#define KZ_RPMENU_LAYOUT "panorama/layout/custom_game/cyber/rpmenu2.vxml_c"

// Отказ SetHasClass/SetDialogVariableString по упору в HUD_LAYOUT_MAX_INTERNED_STRINGS —
// общий лог для entity.cpp и rpmenu.cpp (реализация в entity.cpp).
void LogHudInternFailure(KZPlayer *player, const char *panelId, const char *className);

// id панели с префиксом: "" — как есть (настоящий худ), "x_" — реплика в редакторе !hud
// (cyber/options.xml повторяет mhud.xml под теми же id с префиксом). buf — буфер вызывающего:
// SetHasClass/SetDialogVariableString копируют строку в свою интерн-таблицу.
inline const char *PrefixLayoutId(char *buf, size_t len, const char *prefix, const char *id)
{
	if (!prefix || !prefix[0])
	{
		return id;
	}
	V_snprintf(buf, len, "%s%s", prefix, id);
	return buf;
}

// Курс для PB/WR и строки курса: активный, а до старт-зоны — главный курс карты (layout/mhud.cpp).
const KZCourseDescriptor *GetHudDisplayCourse(KZPlayer *source);
// Строки showpos «x y z» / «pitch yaw» игрока source (layout/mhud.cpp). false — нет пешки.
bool FormatShowPos(KZPlayer *source, char *pos, u32 posLen, char *ang, u32 angLen);
// Преф основного цвета элемента (ep_color редактора) и его дефолт; NULL — у элемента своего цвета
// нет (тип рана — бейдж красят классы rt-*). Реализация — layout/prefs.cpp.
const char *GetElementColorKey(LayoutElement element, Color &def);
