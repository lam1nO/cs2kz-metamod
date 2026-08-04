#pragma once
#include "kz/kz.h"
#include "kz/timer/kz_timer.h"
#include "entityhandle.h"
#include "sdk/entity/cparticlesystem.h"

#define KZ_HUD_TIMER_STOPPED_GRACE_TIME 3.0f
#define KZ_HUD_ON_GROUND_THRESHOLD      0.07f
class IEntityResourceManifest;

class KZHUDService : public KZBaseService
{
	using KZBaseService::KZBaseService;

private:
	bool jumpedThisTick {};
	bool fromDuckbug {};
	bool crouchJumping {};
	bool showPanel {};
	bool particlesActive {};
	// На прошлом тике в centre-канал уходил текст нижней панели (нужен одноразовый клир,
	// когда слать стало нечего — см. ClearBottomPanel).
	bool bottomPanelActive {};
	f64 timerStoppedTime {};
	f64 currentTimeWhenTimerStopped {};

	// Числовой слепок содержимого нижней панели — ключ кэша отправки: пока слепок не меняется,
	// текст не пересобирается (PrepareMessageWithLang/tfm аллоцируют — не для тактового пути)
	// и не шлётся (centre-канал — надёжный usermessage, слать 128/с расточительно). Сравнение
	// пополево (memcmp нельзя — паддинг).
	struct BottomPanelState
	{
		bool showCpTp {};
		i32 cp {}, cpCount {}, tp {};
		bool showTime {};
		bool timePlaceholder {}; // «--:--.--» — prac-часы стоят (нет действующей попытки)
		i32 timeCs {};           // отображаемое время в сотых (как в FormatTimeHud)
		bool stopped {}, paused {};
		// Язык получателя — ЧАСТЬ слепка: kz_language меняет язык НА МЕСТЕ, без реконнекта
		// (reconnect — только при смене языкового аддона, и есть явная ветка отказа от него
		// при чекпоинтах/таймере) — без языка в слепке heartbeat бессрочно гнал бы
		// сохранённый текст на старом языке.
		char lang[16] {};

		bool HasContent() const
		{
			return showCpTp || showTime;
		}

		// Входы кэша готовой CP/TP-строки (bottomCpTpLine): только её слагаемые.
		bool SameCpTpInputs(const BottomPanelState &o) const
		{
			return cp == o.cp && cpCount == o.cpCount && tp == o.tp && V_strcmp(lang, o.lang) == 0;
		}

		bool operator==(const BottomPanelState &o) const
		{
			return showCpTp == o.showCpTp && cp == o.cp && cpCount == o.cpCount && tp == o.tp && showTime == o.showTime
				   && timePlaceholder == o.timePlaceholder && timeCs == o.timeCs && stopped == o.stopped && paused == o.paused
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

	// Кэш готовой CP/TP-строки (по входам SameCpTpInputs): при идущем minimal-таймере слепок
	// меняется каждый тик из-за timeCs, и без этого кэша единственная аллоцирующая часть
	// текста (PrepareMessageWithLang → tfm) собиралась бы ~100/с на получателя.
	char bottomCpTpLine[96] {};
	BottomPanelState bottomCpTpKey {};
	bool bottomCpTpValid {};

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

	// Стиль таймера стандартного HTML-худа (персистентный int-pref "hudTimerStyle").
	// Updated (деф.) — крупное время в строке 1 панели; Minimal — время уезжает в нижнюю
	// панель (обычный centre-канал), а строка 1 становится мелкой меткой «CKZ · PRO».
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
	// hudCpTp) и при минимал-стиле строка таймера. this — получатель (его настройки/язык),
	// dataSource — наблюдаемый (его данные). Слепок состояния считается каждый тик (дёшево,
	// без аллокаций); текст пересобирается и шлётся только на изменении слепка либо
	// heartbeat'ом раз в KZ_HUD_BOTTOM_HEARTBEAT (см. BottomPanelState выше).
	void UpdateBottomPanel(KZPlayer *dataSource);

	// Одноразово стереть нижнюю панель (пустой токен в centre-канал): сам по себе канал
	// гасит последний текст лишь через несколько секунд, а остаток CP/TP после смены
	// типа худа/смерти без спектейта/открытия меню выглядит как зависший худ.
	void ClearBottomPanel();

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

private:
	// dataSource = источник данных (наблюдаемый при спектировании); nullptr → сам игрок.
	// Настройки (цвета perf/CJ) всегда идут с this (получателя) — см. GetMHUDColorPref.
	std::string GetSpeedText(const char *language = KZ_DEFAULT_LANGUAGE, KZPlayer *dataSource = nullptr);
	std::string GetKeyText(const char *language = KZ_DEFAULT_LANGUAGE);
	std::string GetCheckpointText(const char *language = KZ_DEFAULT_LANGUAGE);
	std::string GetTimerText(const char *language = KZ_DEFAULT_LANGUAGE);

	// Числовое ядро состояния таймера (данные — this->player): время/флаги БЕЗ строк и
	// аллокаций — общий источник для верхней строки (GetTimerParts) и слепка нижней панели
	// (ComputeBottomState). outIdleZero — обычный игрок в простое: нулевой таймер, суффиксы
	// стопа/паузы не показываются. false — только реплей-бот, которому показывать нечего.
	bool GetTimerNumbers(f64 &outTime, bool &outRunning, bool &outPaused, bool &outIdleZero);

	// Разбор состояния таймера для кибершоковского худа: время (формат до сотых) и суффикс
	// паузы/стопа раздельно. outRunning — идёт ли активный забег (пауза = идёт → true); по
	// нему BuildVersionCHud красит время (зелёное) или обнуляет в белый 00:00.00 (стоп/idle).
	// Данные — this->player; суффикс-фразы — в языке получателя, БЕЗ скобок («HUD - Bottom
	// * Text»): единственный потребитель суффикса — нижняя панель (FormatBottomText делает
	// то же из слепка), BuildVersionCHud его игнорирует. У обычного игрока в простое (idle)
	// возвращает true с нулевым таймером; false — только для реплей-бота без времени.
	bool GetTimerParts(const char *language, std::string &outTime, std::string &outSuffix, bool &outRunning);

	// Слепок нижней панели: player — данные (наблюдаемый), target — настройки+язык
	// (получатель). Дёшево (интовые чтения), зовётся каждый тик из UpdateBottomPanel.
	static void ComputeBottomState(KZPlayer *player, KZPlayer *target, BottomPanelState &out);

	// Текст нижней панели — функция слепка (включая язык: текст и слепок обязаны совпадать
	// по построению). Зовётся только на изменении слепка; при идущем minimal-таймере это
	// каждый тик, поэтому горячий путь без аллокаций (CP/TP-строка из кэша bottomCpTpLine,
	// время — стек). Канал plain-text: разметки нет, перенос строки — '\n'.
	void FormatBottomText(const BottomPanelState &state, char *buf, i32 size);

	// Сброс кэша нижней панели БЕЗ bottomPanelActive (см. kz_hud.cpp): используется на
	// OnRoundStart, в т.ч. посреди карты (например, кик реплей-бота) — погасить флаг здесь
	// подавило бы следующий клир-кадр ClearBottomPanel. bottomPanelActive гасится только в
	// Reset() (дисконнект, слот реально освобождён).
	void ResetBottomPanelCache();

	// Единый HTML-center HUD в стиле кибершока: строка 1 — таймер (зелёный) + режим + стиль
	// (в минимал-стиле — мелкая метка «CKZ · PRO», время рисует нижняя панель), строка 2 —
	// крупная скорость + престрейф, строка 3 — Stage (только многостейдж), строка 4 —
	// || PB | WR ||; плюс опциональные ряд клавиш и showpos по тумблерам (CP/TP живёт в
	// нижней панели centre-канала, не здесь). Компакт (по this->IsCompactPanel()) — только
	// строки 1-2. Вызывается на hudService ПОЛУЧАТЕЛЯ (спектатора): настройки/язык — из
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
};
