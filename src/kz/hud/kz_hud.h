#pragma once
#include "kz/kz.h"
#include "kz/timer/kz_timer.h"
#include "entityhandle.h"
#include "sdk/entity/cparticlesystem.h"

#define KZ_HUD_TIMER_STOPPED_GRACE_TIME 3.0f
#define KZ_HUD_ON_GROUND_THRESHOLD      0.07f
class IEntityResourceManifest;
class CCSCustomHudLayout;

// Дефолтные цвета — ОБЩИЕ для particle- и panorama-путей худа (Task 5): ключи префов
// совпадают, поэтому настройки игрока переезжают между путями сами, без миграции.
// Определения (значения) остаются в particles.cpp — оттуда их подняли только объявлениями,
// значения не менялись ни на бит (иначе у игроков поехали бы цвета particle-худа).
extern const Color MHUD_DEF_BASE_COLOR;
extern const Color MHUD_DEF_PERF_COLOR;
extern const Color MHUD_DEF_JUMPBUG_COLOR;
extern const Color MHUD_DEF_CJ_COLOR;
extern const Color MHUD_DEF_TIMER_TP_COLOR;
extern const Color MHUD_DEF_TIMER_PRO_COLOR;
extern const Color MHUD_DEF_TIMER_PAUSED_COLOR;
extern const Color MHUD_DEF_TIMER_STOPPED_COLOR;
extern const Color MHUD_DEF_KEYS_OVERLAP_COLOR;

// Элементы panorama-худа (сущность custom_hud_layout, Task 4). НЕ путать с локальным
// `MHUDElement` из particles.cpp (другой состав/порядок) — тот же символ здесь сломал бы
// particle-путь переопределением, поэтому у layout-худа своё имя.
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
	Color checkpoint {};

	bool timerDetailed {};
	bool speedPrecise {};
	bool keysOverlapEnabled {};
};

class KZHUDService : public KZBaseService
{
	using KZBaseService::KZBaseService;

private:
	bool jumpedThisTick {};
	bool fromDuckbug {};
	bool crouchJumping {};
	bool showPanel {};
	bool particlesActive {};
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

	// Кэш отправки минимал-худа (this = получатель): последний отправленный текст и время
	// отправки per-канал (centre / html). В движении текст меняется почти каждый тик
	// (время/скорость), в простое стабилен — тогда каналы живут heartbeat'ом (см.
	// UpdateMinimalHud). minimalCentreActive — на прошлом тике в centre уходил текст
	// минимала (нужен одноразовый клир при уходе из стиля, см. ClearMinimalHud).
	bool minimalCentreActive {};
	char lastMinimalCentreText[192] {};
	f64 lastMinimalCentreSendTime {};
	char lastMinimalHtmlText[512] {};
	f64 lastMinimalHtmlSendTime {};

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
	// Та же развязка data/settings живёт в HTML-пути (BuildVersionCHud/GetSpeedText):
	// там спектатор реально рисует чужие данные своей раскладкой.
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

	// Returns true when the particle-based MHUD should be used.
	// Requires MultiAddonManager to be available, unless kz_force_mhud is set.
	static bool IsMHUDAvailable();

	// Тип худа (персистентный int-pref "hudType"). Цикл в меню: MHUD → Standard → Off.
	// Off — не рисуется НИЧЕГО (ни HTML-панель, ни particle-MHUD), см. DrawPanels.
	enum
	{
		HUD_TYPE_STANDARD = 0, // классическая HTML-панель по центру
		HUD_TYPE_MHUD     = 1, // particle-оверлей
		HUD_TYPE_OFF      = 2, // ничего не рисуется
	};
	int GetHudType();
	void SetHudType(int type);

	// Стиль стандартного HTML-худа целиком (персистентный int-pref "hudTimerStyle" —
	// имя префа историческое, семантика расширена со «стиля таймера» до всего худа).
	// Updated (деф.) — кибершоковская панель (BuildVersionCHud, CP/TP её нижняя строка);
	// Minimal — апстрим-композиция cs2kz: centre-канал = CP/TP + таймер, HTML-канал =
	// скорость + клавиши (см. UpdateMinimalHud).
	enum
	{
		HUD_TIMER_STYLE_UPDATED = 0,
		HUD_TIMER_STYLE_MINIMAL = 1,
	};

	int GetTimerStyle();
	void SetTimerStyle(int style);

	static void PrecacheParticles(IEntityResourceManifest *pResourceManifest);
	// Draw the panel from a player to a specific target.
	static void DrawPanels(KZPlayer *player, KZPlayer *target);

	// Нижняя панель (обычный centre-канал HUD_PRINTCENTER, не HTML): строка CP/TP (гейт
	// hudCpTp). Только обновлённый стиль и только при kz_hud_cptp_in_panel 0 — по умолчанию
	// CP/TP рисуется последней строкой САМОЙ HTML-панели (BuildVersionCHud), а этот тракт
	// остаётся за плайн-худом спектатора под меню (menuOpen) и за откатом по cvar'у.
	// В минимале CP/TP рисует апстрим-композиция.
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

	// Минималистичный стиль худа целиком — апстрим-композиция cs2kz на живых строителях
	// Get*Text: centre-канал = CP/TP + таймер («HUD - Center Text»), HTML-канал = скорость
	// + клавиши («HUD - HTML Center Text»); компакт — только html (таймер<br>скорость).
	// this — получатель (настройки/язык/цвета), dataSource — наблюдаемый (данные) — тот же
	// контракт data/settings, что у BuildVersionCHud. Отправка дедуплицируется слепком
	// последнего отправленного текста per-канал + heartbeat (см. поля кэша выше).
	void UpdateMinimalHud(KZPlayer *dataSource);

	// Одноразово стереть минимал-худ при уходе из него (смена стиля/типа, particle-путь,
	// меню, смерть без спектейта): centre гасится пустым токеном (тот же приём, что
	// ClearBottomPanel), html не трогаем — его либо тут же перерисовывает новый владелец
	// (обновлённый худ/меню), либо он сам гаснет за duration=1s (utils::PrintHTMLCentre).
	void ClearMinimalHud();

	void ResetShowPanel();
	void TogglePanel();
	void ToggleCompactPanel();

	void OnPhysicsSimulate()
	{
		jumpedThisTick = false;
	}

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

	// source = игрок-источник данных (наблюдаемый при спектировании); nullptr → сам игрок.
	void UpdateParticles(KZPlayer *source = nullptr);

	// Destroy all active MHUD particles (e.g. on death or disconnect).
	void DestroyAllParticles();

	// Погасить particle-MHUD немедленно при уходе в спеки: OnProcessMovement у
	// обсервера не тикает, штатный транзишен не сработает — партикли зависают.
	void OnJoinSpectator();

	void OnClientDisconnect();

	// CheckTransmit support (see kz_quiet.cpp).
	bool OwnsParticle(const CEntityHandle &handle) const;

	// Per-element enable flags (also consulted for panel suppression).
	bool IsMHUDSpeedEnabled();
	bool IsMHUDPrespeedEnabled();
	bool IsMHUDTimerEnabled();
	bool IsMHUDKeysEnabled();
	bool IsMHUDCpTpEnabled();
	bool IsMHUDTimerDetailed();
	bool IsMHUDKeysOverlapEnabled();
	bool IsMHUDOutlineEnabled();
	// Только для стандартного HTML-худа (particle-MHUD их не смотрит):
	// раскладка клавиш в 2 ряда (иначе одна строка A W S D C J) и показ строки PB/WR.
	bool IsMHUDKeysTwoRowsEnabled();
	bool IsMHUDPbWrEnabled();

	// kz_hud / kz_mhud — печатает сводку текущего конфига.
	void PrintHUDSummary();

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

	// kz_hud (без аргументов) — интерактивное меню через cs2menus.
	// Фолбэк на PrintHUDSummary(), если меню-движок недоступен.
	void OpenHUDMenu();

	// Построить HUD-меню без показа (возвращает MenuHandle == u32; 0 = не построено).
	// «Назад» — бинд R движка меню (parent от AddSubMenu в !options).
	u32 CreateHUDMenu();

	// kz_mhud без аргументов — алиас OpenHUDMenu (обратная совместимость).
	void OpenMHUDMenu();

	// Константы геометрии particle-MHUD (public: нужны из файловых функций particles.cpp).
	// Клавиши: W A S D J C.
	static constexpr i32 MHUD_KEY_COUNT = 6;

	// Единственный источник скорости для ЛЮБЫХ показаний худа: velocity + baseVelocity,
	// а у реплей-бота на паузе — скорость кадра записи (у замороженного бота velocity
	// принудительно обнулена, чтобы физика его не унесла, — см. replays/playback.cpp).
	// Веток показа скорости четыре (BuildVersionCHud, GetSpeedText, ComputeBottomState,
	// particle-MHUD), и считать её каждая обязана одинаково — иначе «0 на паузе»
	// чинится в одном стиле худа и остаётся в трёх других.
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
	const MHUDLayoutPrefs &GetLayoutPrefs();
	void RefreshLayoutPrefs();
	bool IsLayoutElementEnabled(LayoutElement element);

private:
	// dataSource = источник данных (наблюдаемый при спектировании); nullptr → сам игрок.
	// Настройки (цвета perf/CJ) всегда идут с this (получателя) — см. GetMHUDColorPref.
	std::string GetSpeedText(const char *language = KZ_DEFAULT_LANGUAGE, KZPlayer *dataSource = nullptr);
	std::string GetKeyText(const char *language = KZ_DEFAULT_LANGUAGE);
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

	// Сброс кэшей отправки нижней панели и минимал-худа БЕЗ *Active-флагов (см. kz_hud.cpp):
	// используется на OnRoundStart, в т.ч. посреди карты (например, кик реплей-бота) —
	// погасить флаги здесь подавило бы следующий клир-кадр ClearBottomPanel/ClearMinimalHud.
	// Флаги гасятся только в Reset() (дисконнект, слот реально освобождён).
	void ResetBottomPanelCache();

	// Единый HTML-center HUD в стиле кибершока (только обновлённый стиль; минимал идёт
	// через UpdateMinimalHud): строка 1 — таймер (зелёный) + режим + стиль, строка 2 —
	// крупная скорость + престрейф, строка 3 — Stage (только многостейдж), строка 4 —
	// || PB | WR ||; плюс опциональные ряд клавиш и showpos по тумблерам, последней строкой —
	// CP/TP (гейт hudCpTp, при kz_hud_cptp_in_panel 0 уезжает обратно в centre-канал).
	// Компакт (по this->IsCompactPanel()) — только строки 1-2 (плюс CP/TP: у него свой
	// тумблер). Вызывается на hudService ПОЛУЧАТЕЛЯ (спектатора): настройки/язык — из
	// this->player, данные — из dataSource (наблюдаемый). suppress* — элементы, дублируемые
	// particle-MHUD, чтобы не рисовать их дважды. masterMode — мастер-тумблер: показываем
	// ТОЛЬКО включённые per-element тумблеры.
	std::string BuildVersionCHud(KZPlayer *dataSource, bool suppressSpeed, bool suppressTimer, bool suppressKeys, bool masterMode,
								 const char *language);

	// Control point mapping (общий контракт particle-ассетов апстрима):
	// CP16       = RGB tint (0..255)
	// CP17.x     = sequence (кадр)
	// CP17.y     = size (масштаб)
	// CP17.z     = self-illum / init field1 (1.0 = вкл.)
	// CP18.x/y   = screen-space offset

	// Таймер: до 4 разрядов (каждый = двузначное значение) + до 3 разделителей.
	CHandle<CParticleSystem> timerTextParticles[4];
	CHandle<CParticleSystem> timerDelimiterParticles[3];

	// === MHUD (апстрим cs2kz particle-пути: velo/inputs/timer_delimiter) =================
	// Скорость: два particle'а на пары-разрядов (апстрим-схема hi/lo).
	CHandle<CParticleSystem> upstreamSpeedParticles[2];
	CHandle<CParticleSystem> upstreamPrespeedParticles[2];

	// Клавиши: один particle, sequence = 6-битная маска кнопок.
	enum KeyParticleFlags : u8
	{
		KPF_Forward = 1 << 0,
		KPF_Left    = 1 << 1,
		KPF_Back    = 1 << 2,
		KPF_Right   = 1 << 3,
		KPF_Jump    = 1 << 4,
		KPF_Duck    = 1 << 5,
	};
	CHandle<CParticleSystem> keysParticle;

	void UpdateMHUDSpeed();
	void SetMHUDSpeedParticleVelocity(const Vector &speed, const Vector *prespeed);

	void CheckMHUDTimerParticles();
	void UpdateMHUDTimer();

	void CheckMHUDKeyParticle();
	void UpdateMHUDKeys();

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

	// Кэш префов (Task 5 наполняет); объявление поля — здесь, чтобы UpdateLayoutElement (Task 4)
	// уже мог читать this->GetLayoutPrefs().
	MHUDLayoutPrefs layoutPrefs {};
};
