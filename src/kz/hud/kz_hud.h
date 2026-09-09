#pragma once
#include "kz/kz.h"
#include "kz/timer/kz_timer.h"
#include "kz/hud/layout/menu.h"
#include "entityhandle.h"
#include <unordered_map>

#define KZ_HUD_TIMER_STOPPED_GRACE_TIME 3.0f
#define KZ_HUD_ON_GROUND_THRESHOLD      0.07f
class CCSCustomHudLayout;

// Дефолтные цвета — общие для HTML- и panorama-путей худа (Task 5): ключи префов
// совпадают, поэтому настройки игрока переезжают между путями сами, без миграции.
// Определения (значения) — в kz_hud.cpp (задача 12 подняла их туда вместе с
// GetMHUDColorPref из удалённого particle-пути); значения не менялись ни на бит (иначе у
// игроков поехали бы цвета худа).
extern const Color MHUD_DEF_BASE_COLOR;
extern const Color MHUD_DEF_PERF_COLOR;
extern const Color MHUD_DEF_JUMPBUG_COLOR;
extern const Color MHUD_DEF_CJ_COLOR;
extern const Color MHUD_DEF_TIMER_TP_COLOR;
extern const Color MHUD_DEF_TIMER_PRO_COLOR;
extern const Color MHUD_DEF_TIMER_PAUSED_COLOR;
extern const Color MHUD_DEF_TIMER_STOPPED_COLOR;
extern const Color MHUD_DEF_KEYS_OVERLAP_COLOR;
// Задача 5 (транш "клавиши"): дефолты те же, что в апстриме (значения не менялись).
extern const Color MHUD_DEF_KEYS_PRESSED_COLOR;
extern const Color MHUD_DEF_KEYS_OVERLAP_GLOW_COLOR;

// Элементы panorama-худа (сущность custom_hud_layout, Task 4).
enum class LayoutElement
{
	Timer,
	Speed,
	Prespeed,
	Keys,
	Checkpoint,
	Count
};

struct LayoutElementDef
{
	const char *panelId;    // id панели в mhud.vxml аддона
	const char *varName;    // имя dialog-переменной панели
	const char *enabledKey; // НАШ общий тумблер элемента (hudTimer и т.п.)
	const char *xKey;
	const char *yKey;
	const char *sizeKey;
	const char *fontKey;
	const char *outlineKey;
	const char *opacityKey;
	i32 xDefault;
	i32 yDefault;
	i32 sizeDefault;
};

extern const LayoutElementDef LAYOUT_ELEMENTS[(i32)LayoutElement::Count];

// Стиль одного текстового лейбла panorama-разметки для KZHUDService::ApplyLayoutLabel: элементы
// худа собирают его из префов игрока (UpdateLayoutElement), меню реплея (layout/rpmenu.cpp) —
// задаёт кодом (подсветка выбранной строки). x/y — проценты от центра, size — пиксели,
// fontClass — уже резолвленный css-класс (panorama::ResolveFontClass).
struct LayoutLabelStyle
{
	i32 x {};
	i32 y {};
	i32 size {};
	const char *fontClass {};
	i32 opacity {100};
	bool outline {};
	Color color {};
};

// Показания скорости — общий результат для ЛЮБОГО худа (Task 6/R3): HTML-путь и
// panorama-layout читают его из ОДНОГО метода (KZHUDService::GetSpeedInfo), а не считают
// каждый по-своему — иначе это лишняя расходящаяся ветка вдобавок к тем, которые уже
// обязаны совпадать (см. комментарий у GetDisplayVelocity).
struct SpeedInfo
{
	Vector velocity {};  // GetDisplayVelocity(src) — полная (2D берёт вызывающий сам)
	Vector prespeed {};  // takeoffVelocity; валиден, только если hasPrespeed
	bool hasPrespeed {}; // false — игрок давно на земле/на лестнице без прыжка (useTakeoff)
	bool perfing {};     // src->IsPerfing() && !possibleLadderHop && !takeoffFromLadder
	bool jumpbug {};     // fromDuckbug — красит преф в отдельный цвет
	bool crouchJump {};  // crouchJumping НА ВЗЛЁТЕ (см. hasPrespeed) — красит саму скорость
	// Ушёл с края, а не отпрыгнул (и не с лестницы) — апстримный walkedOff. Валиден только
	// вместе с hasPrespeed; читает его mhudPrespeedHideWalkOff (см. UpdatePrespeedElement).
	bool walkedOff {};
};

// Собственные cl_crosshair* значения игрока (Task 10): дефолты игры, пока не ответит клиент
// на запрос через utils/cvarquery.h (порт с апстрима 08.09 — прежний путь через внешний
// metamod-плагин ClientCvarValue на флоте не работал вовсе, плагина там нет).
struct MHUDCrosshairSettings
{
	f32 size {5.0f};
	f32 thickness {0.5f};
	f32 gap {-2.0f};
	f32 outlineThickness {1.0f};
	i32 color {1};
	i32 r {50}, g {250}, b {50};
	i32 alpha {200};
	bool useAlpha {true};
	bool drawOutline {true};
	bool dot {false};
	bool tStyle {false};
	// false — клиент ещё ни разу не ответил на cvarquery::Query: поля выше —
	// хардкод-дефолты игры, НЕ настройки этого игрока. ApplyCrosshair обязан читать их только
	// когда true, иначе крестик красится «чужими» cl_crosshair* — тот же класс бага, что
	// fail-open в CyberSkins (пустой ответ приняли за настоящее значение).
	bool confirmed {};
};

// Кэш префов layout-худа (реализация — Task 5, GetLayoutPrefs/RefreshLayoutPrefs);
// UpdateLayoutElement (Task 4) читает уже этот тип, объявление обязано быть раньше реализации.
struct MHUDLayoutPrefs
{
	struct Element
	{
		bool enabled {};
		i32 x {};
		i32 y {};
		i32 size {};
		const char *fontClass {};
		bool outline {};
		i32 opacity {};
	};

	Element elements[(i32)LayoutElement::Count] {};

	Color timerPro {};
	Color timerTp {};
	Color timerPaused {};
	Color timerStopped {};
	Color speed {};
	Color speedCj {};
	Color prespeed {};
	Color prespeedPerf {};
	Color prespeedJumpbug {};
	Color keys {};
	Color keysOverlap {};
	Color keysPressed {};
	Color keysOverlapGlow {};
	Color checkpoint {};

	bool timerDetailed {};
	bool speedPrecise {};
	// Престрейф (порт с апстрима, origin/master:src/kz/hud/layout/preferences.cpp:46-48):
	// читаются в UpdatePrespeedElement — точность, скобки и скрытие при уходе с края.
	bool prespeedPrecise {};     // %.2f вместо %.0f
	bool prespeedBrackets {};    // «(784)» вместо «784»
	bool prespeedHideWalkOff {}; // не показывать, когда игрок ушёл с края, а не отпрыгнул
	bool keysOverlapEnabled {};
	// Клавиши (Task 5, транш "клавиши"): опции апстрима, ранее не заводившиеся — см. mhud.cpp.
	bool keysOverlapAxis {}; // красить только пару клавиш конфликтующей оси, не весь контейнер
	bool keysLetters {};     // буквы WASD вместо стрелок
	bool keysSquare {};      // квадратные кнопки вместо широких
	bool keysBorder {};      // рамка кнопки
	bool keysGlow {};        // свечение нажатой кнопки
	bool keysFill {};        // заливка нажатой кнопки
	i32 keysIdle {};         // 0 show, 1 hide, 2 underscore (см. MHUDKeysIdle апстрима)

	// Показывать худ так, как его видит наблюдаемый игрок (порт с апстрима, mhudMimicSpec).
	// Читается ТОЛЬКО из своего набора — никогда из набора того, кого мимикрируем, иначе
	// мимикрия стала бы транзитивной. Разбор — GetLayoutPrefs (layout/prefs.cpp).
	bool mimicSpec {};

	// false — RefreshLayoutPrefs по этому игроку ещё не прошёл (префы из БД не загружены,
	// либо это бот): вся структура нулевая, т.е. элементы выключены и цвета чёрные. Своему
	// худу это безразлично (до загрузки он и не рисуется), но mimicSpec обязан такой набор
	// отвергать — иначе спектейт свежеподключившегося гасил бы худ наблюдателю целиком.
	// У апстрима флага нет: там префы ленивые (prefsDirty), GetOwnPrefs() досчитывает их
	// на первом чтении; у нас кэш наполняется событием OnPlayerPreferencesLoaded.
	bool loaded {};

	// Меню реплея спектатора (layout/rpmenu.cpp): геометрия и шрифт списка — префы rpmenu*
	// (пункт «Меню реплея» в настройках). Читаются ТОЛЬКО из своего набора (GetOwnLayoutPrefs):
	// мимикрировать чужое меню смысла нет.
	struct ReplayMenu
	{
		i32 x {};
		i32 y {};
		i32 size {};
		i32 step {}; // вертикальный шаг строк, проценты
		const char *fontClass {};
	};

	ReplayMenu replayMenu {};

	// Крестик (Task 10) — независим от элементов худа выше, но живёт в той же структуре
	// префов: ключи те же, что читает RefreshLayoutPrefs.
	bool crosshair {};
	i32 crosshairScale {100}; // единиц раскладки на девайс-пиксель, в процентах
};

class KZHUDService : public KZBaseService
{
	using KZBaseService::KZBaseService;

private:
	bool jumpedThisTick {};
	bool fromDuckbug {};
	bool crouchJumping {};
	bool showPanel {};
	// Подавка viewpunch, ПОСЛЕДНЕЕ отправленное этому клиенту состояние. Кэш нужен, чтобы
	// персональные значения конваров уходили только на смене (не каждый тик) и чтобы
	// выключение kz_suppress_viewpunch на живом сервере вернуло клиенту ванильные значения.
	bool viewpunchSuppressed {};
	// На прошлом тике в канал нижней панели уходил её текст (нужен одноразовый клир,
	// когда слать стало нечего — см. ClearBottomPanel).
	bool bottomPanelActive {};
	// В какой канал ушла последняя отправка низа (kz_hud_bottom_channel): false — centre,
	// true — alert. Гасить обязаны ИМЕННО его: alert общий (racing, античит), и слепой клир
	// стирал бы чужое сообщение этому же игроку. Смена cvar'а на живом сервере гасит прежний
	// канал одноразово (см. UpdateBottomPanel).
	bool bottomOnAlert {};
	// Последняя отправка низа ушла не в канал движка, а строкой показаний внутрь панели меню
	// (cs2menus 005, SetSlotStatus). Приёмник, как и канал, — часть состояния: при его смене
	// прежний обязан быть погашен одноразово, иначе строка остаётся висеть в двух местах.
	bool bottomOnMenuStatus {};
	f64 timerStoppedTime {};
	f64 currentTimeWhenTimerStopped {};

	// Числовой слепок содержимого нижней панели (строка CP/TP) — ключ кэша отправки: пока
	// слепок не меняется, текст не пересобирается (PrepareMessageWithLang/tfm аллоцируют —
	// не для тактового пути) и не шлётся (centre-канал — надёжный usermessage, слать 64/с
	// расточительно). Сравнение пополево (memcmp нельзя — паддинг).
	struct BottomPanelState
	{
		bool showCpTp {};
		i32 cp {}, cpCount {}, tp {};
		// Худ спектатора под открытым Html-меню (DrawPanels → UpdateBottomPanel с menuOpen):
		// время/скорость/клавиши наблюдаемого вместо CP/TP. Значения — в гранулярности
		// отображения (целые юниты, сотые секунды), иначе слепок менялся бы чаще текста.
		// menuOpen — часть слепка: закрытие меню обязано перерисовать низ, даже если остальные
		// поля совпали.
		bool menuOpen {};
		// Приёмник: true — строка показаний ВНУТРИ панели меню (cs2menus 005), false — старый
		// plain-text-канал поверх/под меню. Меняет и вёрстку (в панели всё умещается в ОДНУ
		// строку — каждая лишняя отнимает пункт у меню), поэтому это поле слепка, а не флаг
		// сбоку: смена приёмника обязана пересобрать текст, а не только переадресовать его.
		bool menuStatus {};
		// Тумблеры элементов ПОЛУЧАТЕЛЯ (hudSpeed/hudTimer/hudKeys) действуют и здесь: игрок с
		// выключенными клавишами не должен видеть их под меню. Часть слепка — это настройки,
		// их смена обязана перерисовать низ.
		bool showSpeed {}, showKeys {};
		i32 speed {}, prespeed {};
		bool showPrespeed {};
		u8 keyMask {}; // биты KeyParticleFlags (KPF_*), порядок строки A W S D C J
		// Раскладка клавиш получателя (преф hudKeysTwoRows): ВКЛ — два ряда «C W J» / «A S D»,
		// как в HTML-худе; ВЫКЛ — одна строка A W S D C J. Часть слепка — это настройка
		// ПОЛУЧАТЕЛЯ, и её смена обязана перерисовать низ.
		bool keysTwoRows {};
		// Сколько пустых строк подставить НАД содержимым. Наша HTML-панель и нижняя панель
		// позиционируются движком независимо и о высотах друг друга не знают — при высокой
		// панели она просто накрывает низ. Отступ — единственный доступный способ их развести;
		// входит в слепок, иначе изменение высоты панели не перерисовало бы низ. Считается
		// ПОСЛЕ содержимого и уступает ему: канал показывает ограниченное число строк, и отступ
		// не имеет права вытеснить сам худ (см. BottomPadLines/kz_hud_bottom_max_lines).
		// Под открытым Html-меню всегда 0: см. ComputeBottomState.
		u8 padLines {};
		bool hasTimer {}, timerRunning {}, timerPaused {};
		i32 timeCs {}; // время в сотых (гранулярность FormatTimeHudCs)
		// Язык получателя — ЧАСТЬ слепка: kz_language меняет язык НА МЕСТЕ, без реконнекта
		// (reconnect — только при смене языкового аддона, и есть явная ветка отказа от него
		// при чекпоинтах/таймере) — без языка в слепке heartbeat бессрочно гнал бы
		// сохранённый текст на старом языке.
		char lang[16] {};

		bool HasContent() const
		{
			// Под меню содержимое есть только если хоть один элемент включён (таймер — ещё и
			// показывается): иначе шлём клир, а не пустую строку.
			return showCpTp || (menuOpen && (showSpeed || hasTimer || showKeys));
		}

		bool operator==(const BottomPanelState &o) const
		{
			return showCpTp == o.showCpTp && cp == o.cp && cpCount == o.cpCount && tp == o.tp && menuOpen == o.menuOpen && menuStatus == o.menuStatus
				   && showSpeed == o.showSpeed && showKeys == o.showKeys && speed == o.speed && prespeed == o.prespeed
				   && showPrespeed == o.showPrespeed && keyMask == o.keyMask && keysTwoRows == o.keysTwoRows && padLines == o.padLines
				   && hasTimer == o.hasTimer && timerRunning == o.timerRunning && timerPaused == o.timerPaused && timeCs == o.timeCs
				   && V_strcmp(lang, o.lang) == 0;
		}
	};

	// Кэш отправки нижней панели (this = получатель): последний отправленный слепок/текст и
	// время отправки для heartbeat'а (канал сам гаснет за несколько секунд — редкая
	// переотправка того же текста держит его живым без пересборки).
	BottomPanelState lastBottomState {};
	bool bottomStateValid {};
	char lastBottomText[256] {};
	f64 lastBottomSendTime {};

	// Последний текст HTML-панели, РЕАЛЬНО ушедший этому получателю из DrawPanels, и время
	// отправки. Пишется только под kz_hud_panel_trace (деф. выкл) — на тактовом пути это
	// копия строки на каждого получателя, платить за неё постоянно незачем. Нужен потому, что
	// `kz_hud panel` пересобирает панель В МОМЕНТ КОМАНДЫ: без сохранённого отправленного
	// текста нельзя отличить «собрали чисто, клиент не нарисовал» от «на проводе было другое».
	std::string lastPanelSent {};
	f64 lastPanelSentTime {};

	// Источник ДАННЫХ для MHUD (скорость/клавиши/таймер/CP-TP); nullptr → сам игрок.
	// ПОСЛЕ cyb.36 particle-путь идёт ТОЛЬКО живому владельцу (player == target в
	// DrawPanels), спектатор — всегда HTML, так что фактически mhudSource всегда nullptr:
	// механика «чужой источник данных» сохранена как generic-задел (развязка data/settings),
	// вызывающей стороной не используется. НЕ включать её обратно для спектатора —
	// particle-оверлей позиционируется по взгляду владельца и у спектатора уезжает за
	// экран (регрессия cyb.34, см. комментарий в DrawPanels).
	// ВАЖНО: это ТОЛЬКО источник данных. Источник НАСТРОЕК (тумблеры/цвета/раскладка) —
	// всегда this->player, см. MHUDSettingsSource().
	KZPlayer *mhudSource {};

	// Игрок, чьи ДАННЫЕ читает MHUD (mhudSource, если задан; после cyb.36 — всегда сам игрок).
	KZPlayer *MHUDDataSource()
	{
		return mhudSource ? mhudSource : this->player;
	}

	// Игрок, чьи НАСТРОЙКИ (тумблеры/цвета/раскладка) читает MHUD — ВСЕГДА сам игрок.
	// Та же развязка data/settings живёт в HTML-пути (BuildVersionCHud):
	// там спектатор реально рисует чужие данные своей раскладкой.
	// ВАЖНО: это источник СЫРЫХ префов (GetPreference*), и мимикрия под наблюдаемого
	// (mhudMimicSpec) его НЕ подменяет — она живёт в GetLayoutPrefs(), т.е. на уровне уже
	// собранного набора. Подмена здесь сломала бы RefreshLayoutPrefs (он наполняет СВОЙ кэш
	// и читает mimicSpec из своего же набора).
	KZPlayer *MHUDSettingsSource()
	{
		return this->player;
	}

public:
	virtual void Reset() override;
	static void Init();
	// Раунд-старт (в т.ч. первый на новой карте): сброс кэша нижней панели всем игрокам.
	// Критично для смены карты: curtime отсчитывается от её загрузки, переживший смену
	// lastBottomSendTime оказывается «в будущем» и глушил бы heartbeat (Reset() на
	// выделенном сервере при смене карты НЕ зовётся — только на дисконнекте).
	static void OnRoundStart();

	// Returns true when the panorama-layout HUD assets are available.
	// Requires MultiAddonManager to be available, unless kz_force_mhud is set.
	// ЛОЖНО также при kz_hud_layout_enabled=false (диагностический килл-свитч, kz_hud.cpp) —
	// он гасит доступность безусловно, поверх остальных условий.
	static bool IsMHUDAvailable();

	// Только для machine-readable reason в отказе открыть меню (menu.cpp/OpenLayoutMenu) —
	// отличить «выключено килл-свитчем» от прочих причин недоступности MHUD.
	static bool IsHudLayoutKillSwitchOff();

	// Тип худа (персистентный int-pref "hudType"). Цикл в меню: Standard → Panorama → Off.
	// Off — не рисуется НИЧЕГО (ни HTML-панель, ни panorama-layout), см. DrawPanels.
	enum
	{
		HUD_TYPE_STANDARD = 0, // классическая HTML-панель по центру
		// 1 — было HUD_TYPE_MHUD (particle-оверлей апстрима): удалён в задаче 12, апстрим
		// вычистил particles/* из воркшоп-аддона, путь больше не работал и не мог заработать.
		// Значение не переиспользуем — GetHudType() читает сохранённую 1 как HUD_TYPE_PANORAMA
		// и перезаписывает её, чтобы не оставить игрока с недействительным типом (пустой экран).
		HUD_TYPE_OFF      = 2, // ничего не рисуется
		HUD_TYPE_PANORAMA = 3, // custom_hud_layout: разметка mhud.vxml из аддона 3469155349
	};
	int GetHudType();
	void SetHudType(int type);

	// Draw the panel from a player to a specific target.
	static void DrawPanels(KZPlayer *player, KZPlayer *target);

	// Нижняя панель (обычный centre-канал HUD_PRINTCENTER, не HTML): строка CP/TP (гейт
	// hudCpTp), только при kz_hud_cptp_in_panel 0 — по умолчанию CP/TP рисуется последней
	// строкой САМОЙ HTML-панели (BuildVersionCHud), а этот тракт остаётся за плайн-худом
	// спектатора под меню (menuOpen) и за откатом по cvar'у.
	// this — получатель (его настройки/язык), dataSource — наблюдаемый (его данные).
	// Слепок состояния считается каждый тик (дёшево, без аллокаций); текст пересобирается
	// и шлётся только на изменении слепка либо heartbeat'ом раз в KZ_HUD_BOTTOM_HEARTBEAT
	// (см. BottomPanelState выше). menuOpen — спектатор с открытым Html-меню: вместо CP/TP
	// шлём plain-text худ наблюдаемого («время | скорость» + клавиши) тем же каналом и дедупом.
	// linesAbove — высота НАШЕЙ HTML-панели В СТРОКАХ: движок позиционирует оверлеи
	// независимо, поэтому высокая панель накрывает низ, и из linesAbove считается отступ,
	// разводящий их (см. BottomPadLines). Под открытым Html-меню (menuOpen) отступа нет
	// вовсе — там он только мельчил кегль, — и значение не используется.
	void UpdateBottomPanel(KZPlayer *dataSource, bool menuOpen = false, int linesAbove = 0);

	// Одноразово стереть нижнюю панель (пустой токен в тот канал, куда писали, — bottomOnAlert):
	// сам по себе канал гасит последний текст лишь через несколько секунд, а остаток CP/TP
	// после смены типа худа/смерти без спектейта/открытия меню выглядит как зависший худ.
	void ClearBottomPanel();

	void ResetShowPanel();
	void TogglePanel();
	void ToggleCompactPanel();

	void OnPhysicsSimulate()
	{
		jumpedThisTick = false;
	}

	// Подавка тряски камеры (viewpunch) — та же точка жизненного цикла, где она жила до
	// удаления particle-худа (см. c4b5e5f^). Реализация — kz_hud.cpp.
	void OnProcessMovement();

	void OnProcessMovementPost();

	void OnJump(bool modern = false)
	{
		jumpedThisTick = modern ? this->player->IsButtonPressed(IN_JUMP) : true;
	}

	void OnStopTouchGround()
	{
		if (jumpedThisTick)
		{
			fromDuckbug = player->duckBugged;
			crouchJumping = player->GetPlayerPawn()->m_fFlags() & FL_DUCKING || player->GetMoveServices()->m_bDucking();
		}
		else
		{
			fromDuckbug = false;
			crouchJumping = false;
		}
	}

	bool IsShowingPanel()
	{
		return this->showPanel;
	}

	bool IsCompactPanel();

	void OnTimerStopped(f64 currentTimeWhenTimerStopped);

	bool ShouldShowTimerAfterStop()
	{
		return g_pKZUtils->GetServerGlobals()->curtime > KZ_HUD_TIMER_STOPPED_GRACE_TIME
			   && g_pKZUtils->GetServerGlobals()->curtime - timerStoppedTime < KZ_HUD_TIMER_STOPPED_GRACE_TIME;
	}

	// Per-element enable flags (also consulted for panel suppression).
	bool IsMHUDSpeedEnabled();
	bool IsMHUDTimerEnabled();
	bool IsMHUDKeysEnabled();
	bool IsMHUDCpTpEnabled();
	bool IsMHUDKeysOverlapEnabled();
	// Только для стандартного HTML-худа:
	// раскладка клавиш в 2 ряда (иначе одна строка A W S D C J) и показ строки PB/WR.
	bool IsMHUDKeysTwoRowsEnabled();
	bool IsMHUDPbWrEnabled();

	// `kz_hud panel` / `!hud panel` — что уходит сейчас в центральную HTML-панель этому
	// получателю: шапка (цель, cvar'ы панели, ВСЕ тумблеры состава) и ДВА дампа — SENT
	// (реально отправленное, под kz_hud_panel_trace) и BUILT (пересобранное сейчас). В каждом
	// перечень строк: номер, ПЕРВЫЙ и МАКСИМАЛЬНЫЙ font-класс, байты, видимая длина, баланс
	// тегов. Оба класса печатаются намеренно: разбор 10.08 показал, что случаи различает
	// именно первый, а печать одного максимума этот механизм скрывала.
	// Источник данных — как в DrawPanels: наблюдаемый при спектейте. Ёмкость панели клиент
	// задаёт в пикселях, серверу она недоступна, и хвост за рамкой движок молча не рисует —
	// сравнение с экраном отличает обрезку от непостроенной строки. В консоль, без перевода.
	void PrintPanelDiagnostics();

	// Константы геометрии клавиш-элемента (public: нужны из LayoutKeysState ниже).
	// Клавиши: W A S D J C.
	static constexpr i32 MHUD_KEY_COUNT = 6;

	// Единственный источник скорости для ЛЮБЫХ показаний худа: velocity + baseVelocity,
	// а у реплей-бота на паузе — скорость кадра записи (у замороженного бота velocity
	// принудительно обнулена, чтобы физика его не унесла, — см. replays/playback.cpp).
	// Веток показа скорости несколько (BuildVersionCHud, ComputeBottomState, panorama-layout
	// в layout/mhud.cpp), и считать её каждая обязана одинаково — иначе «0 на паузе» чинится
	// в одном стиле худа и остаётся в других.
	static Vector GetDisplayVelocity(KZPlayer *src);

	// === Layout-худ (custom_hud_layout, Task 4) ======================================

	// Сущность создаётся ПЕРСОНАЛЬНО на игрока (не общая на сервер): каждый слот держит свои
	// dialog-переменные и классы панелей, а транзит чужим гасится KZ::quiet (OwnsLayoutEntity).
	// created=true — только что заспавнена (вызывающий обязан форсировать полный релейаут,
	// см. force в UpdateLayoutElement, иначе первый кадр уедет с дефолтными классами схемы).
	CCSCustomHudLayout *EnsureOwnedLayout(bool &created);
	// Гасит сущность и обнуляет кэши классов элементов: кэш живёт ТОЛЬКО вместе с сущностью,
	// иначе следующий владелец слота (реконнект/новый игрок) унаследует чужие классы и
	// UpdateLayoutElement решит, что менять нечего.
	void DestroyOwnedLayout();
	// Выгрузка плагина (g_KZPlugin.unloading, cs2kz.cpp) — снести layout-сущности всех слотов.
	static void LayoutCleanup();

	void SetLayoutClass(CCSCustomHudLayout *layout, const char *panelId, const char *&cache, const char *className);
	void SetLayoutValueClass(CCSCustomHudLayout *layout, const char *panelId, i32 &cache, i32 value, const char *prefix, bool percent);
	void UpdateLayoutElement(CCSCustomHudLayout *layout, LayoutElement element, bool show, const char *text, const Color &color, bool force);

	// CheckTransmit support (см. KZ::quiet::OnCheckTransmit) — свою сущность транслируем.
	bool OwnsLayoutEntity(CEntityHandle handle);

	// Кэш префов layout-худа: реализация — Task 5.
	// GetLayoutPrefs — ЭФФЕКТИВНЫЙ набор: свой, а при mhudMimicSpec — набор наблюдаемого
	// игрока (порт апстримной пары GetPrefs/GetOwnPrefs, origin/master:src/kz/hud/layout/
	// preferences.cpp:66-89). Всё, что РИСУЕТ худ, обязано звать GetLayoutPrefs.
	const MHUDLayoutPrefs &GetLayoutPrefs();
	// Свой набор без мимикрии: наполняется RefreshLayoutPrefs, читает его сама мимикрия
	// (у наблюдаемого) и всё, что остаётся выбором самого игрока даже во время мимикрии.
	const MHUDLayoutPrefs &GetOwnLayoutPrefs();
	void RefreshLayoutPrefs();
	bool IsLayoutElementEnabled(LayoutElement element);
	// Эффективное значение поэлементной обводки С УЧЁТОМ миграции с общего hudOutline
	// (реализация и разбор миграции — layout/prefs.cpp). Публичный, потому что тот же ответ
	// обязан показывать пункт меню (hud/prefs/hud_prefs.cpp): голое чтение mhud*Outline с
	// дефолтом true показывало игроку "Вкл" при фактически выключенной обводке.
	bool GetElementOutlinePref(LayoutElement element);

	// === Одноразовая перезапись настроек худа новыми дефолтами ========================
	// Ревизия набора дефолтов худа, применённая к префам игрока (преф HUD_DEFAULTS_REV_KEY).
	// Поднимать ТОЛЬКО когда снова решено перезаписать оформление худа всему флоту: каждый
	// подъём = один заход каждого игрока с потерей его персональных настроек худа.
	static constexpr i32 HUD_DEFAULTS_REV = 1;
	static constexpr const char *HUD_DEFAULTS_REV_KEY = "hudDefaultsRev";
	// Перезаписывает ключи ВИДА худа новыми дефолтами, если применённая ревизия старше
	// HUD_DEFAULTS_REV, и поднимает маркер (реализация и полный список ключей —
	// hud/layout/defaults.cpp). В набор НЕ входят mhudMimicSpec (поведение спектейта),
	// compactPanel/showPanel и hudKeysTwoRows — их выбор игрока не затираем.
	// Зовётся из OnPlayerPreferencesLoaded ДО RefreshLayoutPrefs; флаш в БД происходит
	// в setup_client.cpp сразу после открытия гейта записи (isSetUp).
	void ApplyHudDefaults();

	// Точка сборки panorama-худа (Task 6): true — сущность есть и худ обновлён (даже если
	// весь спрятан — это тоже валидное обновление); false — сущность создать не удалось,
	// вызывающий обязан уйти на HTML-путь. source — источник ДАННЫХ (наблюдаемый при
	// спектейте), настройки/язык/сама сущность — за this->player (см. MHUDSettingsSource).
	bool UpdateHudLayout(KZPlayer *source);

	// Крестик (Task 10): реплика cl_crosshair* игрока панелями xh_* той же сущности —
	// отдельной сущности не заводим (панель одна на слот, EnsureOwnedLayout). Не MHUDElement:
	// у крестика нет текста, вся текстово-шрифтовая машинерия UpdateLayoutElement не подходит.
	void ApplyCrosshair(CCSCustomHudLayout *layout, bool show, bool force);
	// Первый опрос — сразу на коннекте (см. kz_player.cpp/OnPlayerFullyConnect), дальше сам
	// себя переставляет таймером (StartCrosshairPolling), чтобы игрок, сменивший
	// cl_crosshair* посреди карты, увидел актуальную копию без реконнекта.
	void QueryCrosshairCvars();
	void OnCrosshairCvarValue(const char *name, const char *value);
	void StartCrosshairPolling();

	// === Меню настроек panorama-худа и крестика (Task 11/2 реестра) ===================
	// Живёт на СВОЕЙ сущности custom_hud_layout (menu.vxml_c), отдельной от this->ownedLayout
	// (mhud.vxml_c): худ остаётся видимым и обновляется, пока меню открыто поверх него, а общий
	// кэш классов/переменных развёл бы состояния разных разметок по одним и тем же панелям.
	// Наши !zones/!rpmenu/!prac и т.п. остаются на cs2menus (!options переехал в panorama) —
	// это меню их не трогает
	// и не проверяет g_pMenus вовсе (отдельная система захвата, см. SetInputCaptureEnabled ниже).

	// Регистрирует состав меню (категории/пункты) в общем реестре (kz/option/menu/model.h) —
	// зовётся один раз из Init(), см. kz_hud.cpp. Реализация — hud/prefs/hud_prefs.cpp.
	static void InitMenuPrefs();

	// categoryKey=NULL — корень дерева (menuCategory=-1, список категорий без предвыбранной
	// панели пунктов, RenderMenuItems/GetMenuItem уже умеют этот индекс, см. layout/menu.cpp).
	// categoryKey задан — открыть СРАЗУ эту категорию (поиск по KZOptNode::phraseKey, не по
	// числовому индексу: он едет при первой же смене порядка Register(), см. cs2kz.cpp Task 15);
	// категория не найдена (ещё не зарегистрирована/опечатка) — тот же root, не тишина и не крэш.
	void OpenLayoutMenu(const char *categoryKey = NULL);

	// Прячет корневую панель и снимает курсорный захват. Дополнительно зовётся (НЕ только по
	// клику "Закрыть") из точек, где персистентный худ-стейт уже гасится по тем же причинам:
	// Reset() (дисконнект/новый игрок в слоте), OnRoundStart (смена карты — саму сущность
	// меню сносит движок при загрузке новой карты, но menuOpen-флаг на KZPlayer её переживает
	// и обязан сброситься сам), kz_player.cpp (мёртв и никого не наблюдает — тот же кадр, что
	// гасит ownedLayout) и LayoutCleanup (выгрузка плагина). Без этого игрок с открытым меню
	// застревает в режиме курсора НАВСЕГДА в любой из этих ситуаций. Уход в спектейт другого
	// игрока меню больше НЕ закрывает (спека 2026-09-09-hud-share §6) — там оно работает, а
	// страховкой от залипшего захвата служит CheckMenuCaptureInvariant().
	void CloseLayoutMenu();

	bool IsLayoutMenuOpen() const
	{
		return this->menuOpen;
	}

	// Колбэк клика по кнопке меню — зовётся из Hook_ClientSvcUserMessage (utils/hooks.cpp,
	// свой тип сообщения KZ_UM_CUSTOM_HUD_CLICKED/CKZUsrMsg_CustomHudClicked, НЕ SDK-шный
	// CS_UM_CustomHudClicked — см. комментарий в hooks.cpp/protobuf/kz_customhud.proto)
	// уже с резолвленным по СЛОТУ хука hudService, поэтому здесь
	// достаточно сверить handle с СОБСТВЕННЫМ ownedMenuLayout: клик одного игрока физически
	// не может попасть на чужую сущность меню, даже если бы клиент прислал чужой handle.
	// packedHandle — CEntityHandle сущности МЕНЮ (CCSCustomHudLayout::FromClickHandle);
	// устаревший (сущность уже пересоздана) или чужой handle просто не совпадёт и будет
	// проигнорирован (см. layout/menu.cpp).
	void OnLayoutMenuClick(uint32 packedHandle, const char *panelId);

	// Снимает захват ввода ЯВНО (не полагаясь на удаление сущности), гасит саму сущность меню
	// и обнуляет её диф-кэш (та же ловушка, что у DestroyOwnedLayout — следующий владелец
	// слота иначе унаследует чужие классы/захват). Зовётся из Reset() (дисконнект) и
	// LayoutCleanup() (выгрузка плагина); CloseLayoutMenu() саму сущность НЕ уничтожает
	// (симметрично апстримному DropCapture — только capture/root), чтобы обычное закрытие
	// меню в течение карты не платило пересозданием сущности на следующее открытие.
	void DestroyOwnedMenuLayout();

	// Инвариант (спека 2026-09-09-hud-share §6): не бывает включённого захвата ввода при
	// закрытом меню. Зовётся каждый тик из KZPlayer::OnPhysicsSimulatePost (дросселируется
	// внутри, KZ_MENU_CAPTURE_CHECK_INTERVAL). Нарушение — warn с reason + принудительное
	// снятие захвата: класс отказа «игрок остался с курсором и не может играть до перезахода»
	// иначе не контролируется ничем. Реализация — layout/menu.cpp.
	void CheckMenuCaptureInvariant();

	// === Меню управления реплеем спектатора на panorama (layout/rpmenu.cpp) ===========
	// Третья персональная сущность custom_hud_layout с ТОЙ ЖЕ разметкой mhud.vxml_c, что и
	// худ: четыре её лейбла (таймер/скорость/преспид/чекпоинт) — строки меню у левого края,
	// блок клавиш — подсветка собственных WASD спектатора. Своей разметки у нас нет (чужой
	// аддон 3469155349), а страница меню настроек (menu.vxml_c) прибита к центру — её
	// стили не подключают лист позиций. Пункты и их семантика — KZ::replaysystem::menu
	// (replays/menu.h: ReplayMenuLine/ApplyReplayMenuInput), здесь только ввод и рендер.
	// Ввод — W/S по строкам, E выбор, A/D регулировка — читается с наблюдательской пешки
	// без курсорного захвата (в отличие от меню настроек), поэтому cs2menus-меню поверх
	// открывать нельзя: клавиши уйдут в оба.
	// Копий страницы худа под меню: 4 лейбла на копию, пунктов с подсказкой 7 — поэтому две.
	static constexpr i32 RPMENU_ENTITIES = 2;
	// Доступно, когда есть аддон и игрок СЕЙЧАС наблюдает реплей-бота с идущим плейбеком.
	bool CanOpenReplayMenu();
	// Почему недоступно: NULL — доступно; иначе машинный reason для лога/выбора фолбэка
	// (unloading | no_addon | no_replay | not_spectating_bot).
	const char *ReplayMenuUnavailableReason();
	// Отложенное открытие: перевод в наблюдатели (SpectateBot → JoinTeam) движок применяет
	// НЕ в том же вызове — сразу после `!replay` цель наблюдения ещё не бот, и CanOpen ложно.
	// Запрос ждёт до KZ_RPMENU_PENDING_TICKS тиков (TickReplayMenuPending из
	// KZPlayer::OnPhysicsSimulatePost), открывает panorama, как только спектейт бота
	// фактически включился, иначе уходит на cs2menus с reason в логе.
	void RequestReplayMenu();
	void TickReplayMenuPending();
	// Открыть (не тумблер: тумблер — KZ::replaysystem::menu::OpenReplayControlsMenu, он же
	// выбирает бэкенд cs2menus/panorama). false — открыть нечем (см. CanOpenReplayMenu).
	bool OpenReplayMenu();
	// Закрывает и сносит сущность (меню открывается редко, держать её живой незачем);
	// безопасно без сущности и без открытого меню. reason — в лог.
	void CloseReplayMenu(const char *reason);

	bool IsReplayMenuOpen() const
	{
		return this->replayMenuOpen;
	}

	// Тик меню: зовётся из DrawPanels для ПОЛУЧАТЕЛЯ (this) с источником данных source
	// (наблюдаемый или сам игрок). Закрывает меню, как только игрок перестал наблюдать
	// бота или плейбек кончился; иначе читает ввод и перерисовывает строки.
	void UpdateReplayMenu(KZPlayer *source);
	// Снести сущность меню реплея и сбросить её диф-кэш (та же ловушка, что у
	// DestroyOwnedLayout: кэш живёт только вместе с сущностью). Зовётся из Reset(),
	// LayoutCleanup() и CloseReplayMenu().
	void DestroyOwnedReplayLayout();

private:
	// Единственная точка расчёта SpeedInfo (Task 6/R3, см. комментарий у struct SpeedInfo):
	// HTML-путь (BuildVersionCHud) и panorama-layout (layout/mhud.cpp)
	// обязаны звать ИМЕННО его, копировать расчёт во вторую ветку запрещено. Данные — из
	// MHUDDataSource() (this->player, если mhudSource не задан) — та же развязка data/settings,
	// что у остального худа.
	SpeedInfo GetSpeedInfo();
	std::string GetCheckpointText(const char *language = KZ_DEFAULT_LANGUAGE);
	std::string GetTimerText(const char *language = KZ_DEFAULT_LANGUAGE);

	// Числовое ядро состояния таймера (данные — this->player): время/флаги БЕЗ строк и
	// аллокаций — источник для строки таймера кибершоковского худа (GetTimerParts).
	// outIdleZero — обычный игрок в простое: нулевой таймер, суффиксы стопа/паузы не
	// показываются. false — только реплей-бот, которому показывать нечего.
	bool GetTimerNumbers(f64 &outTime, bool &outRunning, bool &outPaused, bool &outIdleZero);

	// Разбор состояния таймера для кибершоковского худа: время в формате до сотых.
	// outRunning — идёт ли активный забег (пауза = идёт → true); по нему BuildVersionCHud
	// красит время (зелёное) или обнуляет в белый 00:00.00 (стоп/idle). Данные —
	// this->player. У обычного игрока в простое (idle) возвращает true с нулевым таймером;
	// false — только для реплей-бота без времени.
	bool GetTimerParts(std::string &outTime, bool &outRunning);

	// Слепок нижней панели: player — данные (наблюдаемый), target — настройки+язык
	// (получатель). Дёшево (интовые чтения), зовётся каждый тик из UpdateBottomPanel.
	// menuOpen — заполняет поля плайн-худа под меню (скорость/время/клавиши) вместо CP/TP.
	static void ComputeBottomState(KZPlayer *player, KZPlayer *target, BottomPanelState &out, bool menuOpen, int linesAbove);

	// Текст нижней панели — функция слепка (включая язык: текст и слепок обязаны совпадать
	// по построению). Зовётся только на изменении слепка — аллокации PrepareMessageWithLang
	// только на смене CP/TP/языка. Канал plain-text: разметки нет.
	void FormatBottomText(const BottomPanelState &state, char *buf, i32 size);

	// Сброс кэша отправки нижней панели БЕЗ *Active-флага (см. kz_hud.cpp): используется на
	// OnRoundStart, в т.ч. посреди карты (например, кик реплей-бота) — погасить флаг здесь
	// подавило бы следующий клир-кадр ClearBottomPanel.
	// Флаг гасится только в Reset() (дисконнект, слот реально освобождён).
	void ResetBottomPanelCache();

	// Единый HTML-center HUD в стиле кибершока: строка 1 — таймер (зелёный) + режим + стиль,
	// строка 2 — крупная скорость + престрейф, строка 3 — Stage (только многостейдж), строка 4 —
	// || PB | WR ||; плюс опциональные ряд клавиш и showpos по тумблерам, последней строкой —
	// CP/TP (гейт hudCpTp, при kz_hud_cptp_in_panel 0 уезжает обратно в centre-канал).
	// Компакт (по this->IsCompactPanel()) — только строки 1-2 (плюс CP/TP: у него свой
	// тумблер). Вызывается на hudService ПОЛУЧАТЕЛЯ (спектатора): настройки/язык — из
	// this->player, данные — из dataSource (наблюдаемый). suppress* — исторический параметр
	// (раньше гасил элементы, дублируемые particle-MHUD; с задачи 12 particle-путь удалён,
	// вызывающая сторона больше не передаёт true, но сигнатуру не меняем — используется
	// извне). masterMode — мастер-тумблер: показываем ТОЛЬКО включённые per-element тумблеры.
	std::string BuildVersionCHud(KZPlayer *dataSource, bool suppressSpeed, bool suppressTimer, bool suppressKeys, bool masterMode,
								 const char *language);

	// Маска клавиш (KPF_*) — общий формат для нижней панели (ComputeBottomState/
	// FormatBottomText) и HTML-худа.
	enum KeyParticleFlags : u8
	{
		KPF_Forward = 1 << 0,
		KPF_Left    = 1 << 1,
		KPF_Back    = 1 << 2,
		KPF_Right   = 1 << 3,
		KPF_Jump    = 1 << 4,
		KPF_Duck    = 1 << 5,
	};

	// Preference helpers.
	Color GetMHUDColorPref(const char *name, const Color &defaultColor);

	// === Layout-худ (custom_hud_layout, Task 4) ======================================

	struct LayoutElementState
	{
		std::string text {};
		const char *colorClass {};
		const char *fontClass {};
		i32 fontSize {-1};
		i32 x {INT_MIN};
		i32 y {INT_MIN};
		bool hidden {true};
		bool outline {false};
		i32 opacity {INT_MIN};
		// Поиск ближайшего цвета палитры — только при смене цвета, не каждый тик.
		const char *colorClassComputed {};
		u32 lastColorPacked {};
		bool colorComputed {};
	};

	// Сущность худа ЭТОГО игрока; чужим не транслируется (KZ::quiet::OnCheckTransmit).
	CHandle<CBaseEntity> ownedLayout {};
	LayoutElementState layoutElements[(i32)LayoutElement::Count] {};

	// Слот эффективного источника mhudMimicSpec на МОМЕНТ последней проверки EnsureOwnedLayout:
	// -1 (невалидный CPlayerSlot) — мимикрия не активна, используются свои префы; иначе слот
	// наблюдаемого. Сравнение с текущим слотом (entity.cpp/EnsureOwnedLayout) решает, пересоздавать
	// ли ownedLayout — интерн-таблицы сущности (HUD_LAYOUT_MAX_INTERNED_STRINGS) растут за весь
	// сеанс наблюдателя и не освобождаются, а долгий спектейт-марафон с разными раскладками цели
	// исчерпал бы лимит (T5a-отчёт, раздел 5). Отдельно от layoutPrefs.mimicSpec: тот — сырой
	// СВОЙ переключатель, этот — какая цель фактически применена сейчас.
	CPlayerSlot layoutMimicSource {-1};

	// Единственная машинерия записи текстового лейбла (hidden/текст/позиция/кегль/цвет/
	// шрифт/прозрачность/обводка) с диф-кэшем state — см. entity.cpp.
	void ApplyLayoutLabel(CCSCustomHudLayout *layout, const char *panelId, const char *varName, LayoutElementState &state,
						  const LayoutLabelStyle &style, bool show, const char *text, bool force);

	// Кэш префов (Task 5 наполняет); объявление поля — здесь, чтобы UpdateLayoutElement (Task 4)
	// уже мог читать this->GetLayoutPrefs().
	MHUDLayoutPrefs layoutPrefs {};

	// Кэш классов клавиш (Task 6): апстрим адресует не одну панель, а 16 отдельных глифов
	// (KEY_GLYPHS в layout/mhud.cpp) поверх 6 кнопок (KEY_PANELS) — своё состояние, отдельное
	// от layoutElements[Keys] (тот кэширует общий контейнер: цвет/позицию/размер/шрифт-класс
	// панели целиком). Живёт ТОЛЬКО вместе с сущностью — обнулять вместе с layoutElements[]
	// в DestroyOwnedLayout, иначе следующий владелец слота унаследует чужие классы кнопок и
	// решит, что клавиши уже выставлены (та же ловушка, что и с layoutElements).
	// Транш "клавиши" (Task 5) довёл кэш до апстримного состава: idle-режим/рамка/свечение/
	// заливка/буквы/квадрат — тумблеры на весь контейнер (одно значение кэшируется как int/bool
	// и сравнивается по значению — тех же классов на панели несколько сразу, поэтому
	// SetLayoutClass с его one-slot заменой не подходит, см. UpdateKeysElement), а per-key
	// свечение (glow) и осевая тонировка при keysOverlapAxis — по каждой из 6 кнопок отдельно.
	struct LayoutKeysState
	{
		bool pressed[KZHUDService::MHUD_KEY_COUNT] {};
		i32 boxSize {INT_MIN};
		i32 fontSize {INT_MIN};
		const char *fontClass {};
		i32 idle {-1};      // прошлый MHUDLayoutPrefs::keysIdle, -1 — ещё не выставляли
		i32 noBorder {-1};
		i32 noGlow {-1};
		i32 noFill {-1};
		i32 letters {-1};
		i32 square {-1};
		// Индекс палитры key-glow-N (keys.css) на кнопку; -1 — класс ещё не выставлен.
		i32 glow[KZHUDService::MHUD_KEY_COUNT] {-1, -1, -1, -1, -1, -1};
		// Класс тонировки конфликтующей оси (keysOverlapAxis) — резолвленный стабильный указатель
		// (panorama::ResolveColorClass), поэтому кэшируется через SetLayoutClass как обычно.
		const char *overlapClass[KZHUDService::MHUD_KEY_COUNT] {};
	};

	LayoutKeysState layoutKeys {};
	// Кегль контейнера/кнопок и шрифт глифов блока клавиш — общий для худа и меню реплея
	// (реализация в layout/mhud.cpp).
	void ApplyKeysSizing(CCSCustomHudLayout *layout, LayoutKeysState &state, i32 size, const char *fontClass);

	// === Меню реплея (layout/rpmenu.cpp) — состояние ==================================
	// Сущности меню реплея ЭТОГО игрока (см. OpenReplayMenu); гасятся вместе с остальными.
	CHandle<CBaseEntity> ownedReplayLayouts[RPMENU_ENTITIES] {};
	bool replayMenuOpen {};
	bool replayMenuPending {};     // см. RequestReplayMenu
	i32 replayMenuPendingTicks {}; // сколько тиков запрос уже ждёт спектейта бота
	i32 replayMenuLine {};       // выбранная строка (ReplayMenuLine)
	u64 replayMenuHeld {};       // маска удержанных кнопок прошлого тика — фронт нажатия свой,
								 // а не IsButtonNewlyPressed: тот живёт внутри обработки usercmd
	i32 replayMenuHoldTicks {};  // тики удержания A/D — автоповтор регулировки, как в cs2menus
	// Диф-кэш лейблов каждой сущности — живёт ТОЛЬКО с сущностью.
	LayoutElementState replayLines[RPMENU_ENTITIES][(i32)LayoutElement::Count] {};

	CCSCustomHudLayout *EnsureReplayLayout(i32 index, bool &created);
	void ReadReplayMenuInput();
	// force — по индексу сущности: пересозданная копия требует полной перезаписи классов.
	void RenderReplayMenu(CCSCustomHudLayout *(&layouts)[RPMENU_ENTITIES], const bool (&force)[RPMENU_ENTITIES]);

	// Кэш класс-суффиксов крестика (Task 10) — та же ловушка, что у layoutElements[]/
	// layoutKeys: живёт ТОЛЬКО вместе с сущностью, обнулять в DestroyOwnedLayout, иначе
	// следующий владелец слота (реконнект/новый игрок) унаследует чужие xh-* классы, и
	// ApplyCrosshair решит, что менять уже нечего — крестик молча не появится.
	// -1 — класс ещё не выставлен ни разу (в отличие от size-полей выше INT_MIN здесь не
	// нужен: любое реальное значение крестика неотрицательно).
	struct LayoutCrosshairState
	{
		i32 shown {-1};
		i32 armLength {-1};
		i32 thickness {-1};
		i32 margin {-1};
		i32 marginFar {-1};
		i32 outline {-1};
		i32 opacity {-1};
		i32 dot {-1};
		i32 noTopArm {-1};
		const char *colorClass {};
	};

	// Собственные cl_crosshair* игрока — наполняется OnCrosshairCvarValue по ответам клиента на
	// cvarquery::Query, ApplyCrosshair читает как есть (дефолты игры, пока клиент не ответил).
	MHUDCrosshairSettings crosshair {};
	// Круги опроса, на которые клиент не ответил НИ РАЗУ (сбрасывается в StartCrosshairPolling):
	// гейт confirmed держит крестик невидимым молча, поэтому по достижении лимита пишем warn с
	// reason=no_client_response — см. QueryCrosshairCvars.
	i32 crosshairUnansweredPolls {};
	LayoutCrosshairState layoutCrosshair {};

	// === Пять элементов panorama-худа (Task 6) — перенесены с апстрима, адаптации: наши
	// геттеры текста (GetTimerText/GetCheckpointText), this->GetLayoutPrefs() вместо GetPrefs(),
	// this->IsLayoutElementEnabled() вместо IsMHUDElementEnabled(). source — источник ДАННЫХ
	// (наблюдаемый при спектейте), this->player — настройки/язык/цвета (тот же контракт, что у
	// BuildVersionCHud/UpdateBottomPanel).
	void UpdateTimerElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force);
	void UpdateSpeedElement(CCSCustomHudLayout *layout, const SpeedInfo &info, bool force);
	void UpdatePrespeedElement(CCSCustomHudLayout *layout, const SpeedInfo &info, bool force);
	void UpdateKeysElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force);
	void UpdateCheckpointElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force);

	// === Меню настроек (Task 11) — состояние и рендер ==================================

	// Сущность меню ЭТОГО игрока — своя, отдельная от this->ownedLayout (см. комментарий у
	// OpenLayoutMenu). Гасится вместе с ownedLayout везде, где гасится персистентный худ-стейт.
	CHandle<CBaseEntity> ownedMenuLayout {};
	bool menuOpen {};
	// Дедлайн следующей проверки инварианта захвата (curtime), см. CheckMenuCaptureInvariant.
	f64 menuCaptureCheckTime {};
	// Активный узел реестра — ДВА уровня, как у апстрима: индекс категории верхнего уровня
	// (KZ::menu::GetTree()) и индекс её подкатегории. -1 в menuCategory — корень (ни одна не
	// выбрана, панель пунктов пуста); -1 в menuSub — у категории подкатегорий нет и пункты
	// показывает она сама. Раскладку левой колонки по 20 кнопкам cat%i считает BuildMenuLeft
	// (layout/menu.cpp) из этой пары, отдельного состояния «что раскрыто» нет: раскрыта всегда
	// menuCategory.
	i32 menuCategory {-1};
	i32 menuSub {-1};

	enum class MenuPopup
	{
		None,
		Color, // попап выбора цвета (перенесено с апстрима почти без изменений)
		Step,  // попап +-1/+-5 для позиции/размера/прозрачности/Vector (апстримный "Step popup")
		List,  // попап списка (Choice) — строки li%i, наполняется getChoices при каждом рендере
	};

	MenuPopup menuPopup {MenuPopup::None};
	i32 menuPopupItem {-1}; // индекс пункта в активной категории, на который открыт попап
	i32 menuPopupPage {};   // страница попапа (свотчи цвета постранично, как в апстриме)

	// Диф-кэш применённых классов/переменных сущности меню — та же ловушка, что у
	// layoutElements/layoutKeys/layoutCrosshair: живёт ТОЛЬКО вместе со своей сущностью,
	// обнуляется в EnsureMenuLayout при created=true (см. force-паттерн entity.cpp).
	struct MenuAppliedState
	{
		bool rootHidden {true};
		// Шрифт и цвет корня — теперь ПРЕФЫ игрока (menuFont/menuColor, задача «оформление меню»),
		// а не два зашитых класса: держим последний применённый класс, как у апстрима
		// (KZMenuService::Applied::menuFont/menuColor, origin/master:src/kz/option/menu/kz_menu.h:127-128).
		const char *menuFont {};  // font-family--* на menu_root, наследуется текстовыми панелями
		const char *menuColor {}; // pal-fg-*/grad-* на menu_root, туда же
		bool sounds {};           // menu_root "snd" — звуки наведения/клика (menuSounds)
		bool shift {};            // menu_root "shift" — сдвиг меню влево под открытый попап (menuPopupShift)
		bool colorPopupHidden {true};
		bool stepPopupHidden {true};
		bool listPopupHidden {true};
		bool stepVHidden {true}; // вертикальный ряд степпера (Position и Y-ось Vector)
		bool stepZHidden {true}; // ряд m_step_z (только Vector, третья ось)

		bool catHidden[KZ_MENU_CATS] {};
		bool catSelected[KZ_MENU_CATS] {};
		// Классы вложенности левой колонки (menu.css чужого аддона: .cat.indent — отступ 26px,
		// .cat.cat-parent — жирная шапка, .cat.cat-parent.disabled — раскрытый родитель без
		// подсветки наведения).
		bool catIndent[KZ_MENU_CATS] {};
		bool catParent[KZ_MENU_CATS] {};
		bool catDisabled[KZ_MENU_CATS] {};

		bool itemHidden[KZ_MENU_ITEMS] {};
		const char *itemType[KZ_MENU_ITEMS] {};
		bool itemOn[KZ_MENU_ITEMS] {};
		const char *itemSwatch[KZ_MENU_ITEMS] {};
		bool itemHasSub[KZ_MENU_ITEMS] {};
		bool itemDisabled[KZ_MENU_ITEMS] {};

		const char *swBg[KZ_MENU_SWATCH] {};
		bool swSelected[KZ_MENU_SWATCH] {};
		bool swHidden[KZ_MENU_SWATCH] {};

		bool liHidden[KZ_MENU_LIST] {};
		bool liSelected[KZ_MENU_LIST] {};

		MenuAppliedState()
		{
			for (i32 i = 0; i < KZ_MENU_CATS; i++)
			{
				catHidden[i] = true;
			}
			for (i32 i = 0; i < KZ_MENU_ITEMS; i++)
			{
				itemHidden[i] = true;
			}
			for (i32 i = 0; i < KZ_MENU_SWATCH; i++)
			{
				swHidden[i] = true;
			}
			for (i32 i = 0; i < KZ_MENU_LIST; i++)
			{
				liHidden[i] = true;
			}
		}
	};

	MenuAppliedState menuApplied {};
	// Последнее записанное значение каждой (panelId,var)-пары — как у апстрима: SetDialogVariableString
	// метит ВСЮ сущность на полную пересылку, повторная запись того же значения того не стоит.
	std::unordered_map<std::string, std::string> menuVars;

	CCSCustomHudLayout *EnsureMenuLayout(bool &created);
	void RenderMenu();
	void RenderMenuCategories(CCSCustomHudLayout *layout);
	void RenderMenuItems(CCSCustomHudLayout *layout);
	void RenderMenuColorPopup(CCSCustomHudLayout *layout);
	void RenderMenuStepPopup(CCSCustomHudLayout *layout);
	void RenderMenuListPopup(CCSCustomHudLayout *layout);

	void SelectMenuCategory(i32 index);
	void ActivateMenuItem(i32 slot);
	void OpenMenuPopup(MenuPopup kind, i32 itemIndex);
	void CloseMenuPopup();
	void MenuPopupPageStep(i32 delta);
	void MenuPopupPick(i32 slot);
	void MenuListPick(i32 slot);
	// axis: 0 — x/размер/прозрачность (тот же, что раньше), 1 — y (Position и Y-ось Vector),
	// 2 — z (только Vector).
	void MenuStep(i32 axis, i32 delta);

	void SetMenuClass(CCSCustomHudLayout *layout, const char *panelId, const char *className, bool on);
	void SetMenuBoolClass(CCSCustomHudLayout *layout, const char *panelId, const char *className, bool &cache, bool want);
	void SetMenuSwapClass(CCSCustomHudLayout *layout, const char *panelId, const char *&cache, const char *want);
	void SetMenuVar(CCSCustomHudLayout *layout, const char *panelId, const char *var, const char *value);

	// Симметрично GetMHUDColorPref — цвет хранится упакованным int (R2), своего
	// GetPreferenceColor/SetPreferenceColor в базе нет.
	void SetMHUDColorPref(const char *name, const Color &color);
};
