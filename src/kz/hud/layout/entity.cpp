// Сущность custom_hud_layout (Task 4): создание/уничтожение на игрока, запись классов
// панелей и dialog-переменных. Перенесено с апстрима (src/kz/hud/layout/entity.cpp),
// расхождения с ним — см. журнал решений задачи (планинг-доки этой ветки, разделы R1/R4).
#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/panorama_tables.h"
#include "kz/spec/kz_spec.h" // GetSpectatedPlayer — источник mhudMimicSpec (см. EnsureOwnedLayout)
#include "sdk/entity/ccscustomhudlayout.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/logging.h"
#include "cs2kz.h"

#include "tier0/memdbgon.h"

// SetHasClass/SetDialogVariableString возвращают false, когда уже упёрлись в
// HUD_LAYOUT_MAX_INTERNED_STRINGS (1024, sdk/entity/ccscustomhudlayout.h) — дальше схема
// молча перестаёт меняться. Отказ обязан быть видимым (канон проекта), а не тихим фризом
// оформления.
void LogHudInternFailure(KZPlayer *player, const char *panelId, const char *className)
{
	KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n", panelId, className,
				player ? player->GetPlayerSlot().Get() : -1);
}

// clang-format off
extern const LayoutElementDef LAYOUT_ELEMENTS[(i32)LayoutElement::Count] =
{
	{"mhud_timer",      "mhud_timer_row",  "timer",      "hudTimer",      "mhudTimerX",      "mhudTimerY",      "mhudTimerSize",      "mhudTimerFont",      "mhudTimerOutline",      "mhudTimerOpacity",      LAYOUT_DEF_TIMER_X,      LAYOUT_DEF_TIMER_Y,      LAYOUT_DEF_TIMER_SIZE,      true},
	{"mhud_speed",      "mhud_speed",      "speed",      "hudSpeed",      "mhudSpeedX",      "mhudSpeedY",      "mhudSpeedSize",      "mhudSpeedFont",      "mhudSpeedOutline",      "mhudSpeedOpacity",      LAYOUT_DEF_SPEED_X,      LAYOUT_DEF_SPEED_Y,      LAYOUT_DEF_SPEED_SIZE,      true},
	{"mhud_prespeed",   "mhud_prespeed",   "prespeed",   "hudPrespeed",   "mhudPrespeedX",   "mhudPrespeedY",   "mhudPrespeedSize",   "mhudPrespeedFont",   "mhudPrespeedOutline",   "mhudPrespeedOpacity",   LAYOUT_DEF_PRESPEED_X,   LAYOUT_DEF_PRESPEED_Y,   LAYOUT_DEF_PRESPEED_SIZE,   true},
	{"mhud_keys",       "mhud_keys",       "keys",       "hudKeys",       "mhudKeysX",       "mhudKeysY",       "mhudKeysSize",       "mhudKeysFont",       "mhudKeysOutline",       "mhudKeysOpacity",       LAYOUT_DEF_KEYS_X,       LAYOUT_DEF_KEYS_Y,       LAYOUT_DEF_KEYS_SIZE,       true},
	{"mhud_checkpoint", "mhud_checkpoint", "checkpoint", "hudCpTp",       "mhudCheckpointX", "mhudCheckpointY", "mhudCheckpointSize", "mhudCheckpointFont", "mhudCheckpointOutline", "mhudCheckpointOpacity", LAYOUT_DEF_CHECKPOINT_X, LAYOUT_DEF_CHECKPOINT_Y, LAYOUT_DEF_CHECKPOINT_SIZE, true},
	// «Прогресс» — своя панель mhud_progress (подпись + процент + полоса) на ОБЩЕЙ сущности худа.
	// Раньше элемент жил на отдельной копии страницы и занимал её лейбл таймера: своей панели в
	// разметке не было. varName "progress" — переменная процента (лейбл mhud_progress_pct внутри
	// корня); подпись и ширину полосы пишет UpdateLeadProgressElement. Ключи префов прежние
	// (mhudLeadProgress*). Дефолт тумблера false: элемент включают явно.
	{"mhud_progress_pct", "mhud_progress", "progress",   "mhudLeadProgress", "mhudLeadProgressX", "mhudLeadProgressY", "mhudLeadProgressSize", "mhudLeadProgressFont", "mhudLeadProgressOutline", "mhudLeadProgressOpacity", LAYOUT_DEF_LEADPROGRESS_X, LAYOUT_DEF_LEADPROGRESS_Y, LAYOUT_DEF_LEADPROGRESS_SIZE, false},
	// Поля редактора `!hud` (§4.1 спеки). Корни — Panel (pbwr, showpos) или Label (course,
	// runtype). У pbwr varName не пишется никогда: текст идёт в переменные ячеек (mhud.cpp).
	{"mhud_pbwr",       "mhud_pbwr",       "pbwr",       "hudPbWr",       "mhudPbWrX",       "mhudPbWrY",       "mhudPbWrSize",       "mhudPbWrFont",       "mhudPbWrOutline",       "mhudPbWrOpacity",       LAYOUT_DEF_PBWR_X,       LAYOUT_DEF_PBWR_Y,       LAYOUT_DEF_PBWR_SIZE,       true},
	{"mhud_showpos",    "mhud_showpos",    "pos",        "hudShowPos",    "mhudShowPosX",    "mhudShowPosY",    "mhudShowPosSize",    "mhudShowPosFont",    "mhudShowPosOutline",    "mhudShowPosOpacity",    LAYOUT_DEF_SHOWPOS_X,    LAYOUT_DEF_SHOWPOS_Y,    LAYOUT_DEF_SHOWPOS_SIZE,    false},
	{"mhud_course",     "mhud_course",     "course",     "hudCourse",     "mhudCourseX",     "mhudCourseY",     "mhudCourseSize",     "mhudCourseFont",     "mhudCourseOutline",     "mhudCourseOpacity",     LAYOUT_DEF_COURSE_X,     LAYOUT_DEF_COURSE_Y,     LAYOUT_DEF_COURSE_SIZE,     true},
	{"mhud_runtype",    "mhud_runtype",    "runtype",    "hudRunType",    "mhudRunTypeX",    "mhudRunTypeY",    "mhudRunTypeSize",    "mhudRunTypeFont",    "mhudRunTypeOutline",    "mhudRunTypeOpacity",    LAYOUT_DEF_RUNTYPE_X,    LAYOUT_DEF_RUNTYPE_Y,    LAYOUT_DEF_RUNTYPE_SIZE,    true},
};
// clang-format on

// === Запись классов на сущность (глобальный слой — у нас сущность персональная, но апстримный
// API работает с глобальным layout-состоянием, per-player состояние нам не нужно) =============

void KZHUDService::SetLayoutClass(CCSCustomHudLayout *layout, const char *panelId, const char *&cache, const char *className)
{
	if (cache == className)
	{
		return;
	}
	if (cache)
	{
		layout->SetHasClass(panelId, cache, k_eHudPanelClassStatus_DoesNotHaveClass);
	}
	if (className && !layout->SetHasClass(panelId, className, k_eHudPanelClassStatus_HasClass))
	{
		LogHudInternFailure(this->player, panelId, className);
	}
	cache = className;
}

void KZHUDService::SetLayoutValueClass(CCSCustomHudLayout *layout, const char *panelId, i32 &cache, i32 value, const char *prefix, bool percent)
{
	value = percent ? panorama::SnapToStep(value, -100, 100) : panorama::SnapToStep(value, 0, 500);
	if (cache == value)
	{
		return;
	}
	const char *unit = percent ? "pct" : "px";
	char className[64];
	if (cache != INT_MIN)
	{
		V_snprintf(className, sizeof(className), "%s--%s%i%s", prefix, cache < 0 ? "neg" : "", abs(cache), unit);
		layout->SetHasClass(panelId, className, k_eHudPanelClassStatus_DoesNotHaveClass);
	}
	V_snprintf(className, sizeof(className), "%s--%s%i%s", prefix, value < 0 ? "neg" : "", abs(value), unit);
	if (!layout->SetHasClass(panelId, className, k_eHudPanelClassStatus_HasClass))
	{
		LogHudInternFailure(this->player, panelId, className);
	}
	cache = value;
}

// Переменная дочерней панели с диф-кэшем: каждый SetDialogVariableString — изменение
// сетевого состояния сущности, слать одно и то же каждый тик незачем.
void KZHUDService::SetLayoutVar(CCSCustomHudLayout *layout, const char *panelId, const char *varName, std::string &cache, const char *value)
{
	if (cache == value)
	{
		return;
	}
	cache = value;
	if (!layout->SetDialogVariableString(panelId, varName, value))
	{
		LogHudInternFailure(this->player, panelId, varName);
	}
}

// Класс-тумблер (ставится И снимается по значению) с диф-кэшем; -1 в cache — ещё не выставляли.
void KZHUDService::SetLayoutBoolClass(CCSCustomHudLayout *layout, const char *panelId, const char *className, i32 &cache, bool want)
{
	const i32 value = want ? 1 : 0;
	if (cache == value)
	{
		return;
	}
	cache = value;
	if (!layout->SetHasClass(panelId, className, want ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
	{
		LogHudInternFailure(this->player, panelId, className);
	}
}

void KZHUDService::UpdateLayoutElement(CCSCustomHudLayout *layout, LayoutElement element, bool show, const char *text, const Color &color, bool force)
{
	const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)element];
	this->ApplyLayoutElementTo(layout, this->GetLayoutPrefs().elements[(i32)element], this->layoutElements[(i32)element], def.panelId, def.varName,
							   def.posPanelId, show, text, color, force);
}

// Запись элемента худа на произвольные id и кэш: настоящий худ (UpdateLayoutElement выше) и
// реплика редактора !hud (layout/editor.cpp — e_* несёт позицию/показ, x_mhud_* — текст/стиль)
// идут через ОДИН код, чтобы реплика выглядела ровно как худ.
void KZHUDService::ApplyLayoutElementTo(CCSCustomHudLayout *layout, const MHUDLayoutPrefs::Element &cached, LayoutElementState &state,
										const char *panelId, const char *varName, const char *posPanelId, bool show, const char *text,
										const Color &color, bool force)
{
	LayoutLabelStyle style;
	style.x = cached.x;
	style.y = cached.y;
	style.size = cached.size;
	// cached.fontClass — уже РЕЗОЛВЛЕННЫЙ css-класс (RefreshLayoutPrefs, Task 5 зовёт
	// panorama::ResolveFontClass один раз при обновлении префов). Повторный резолв здесь
	// сравнил бы готовый класс со слагом и всегда проигрывал бы fallback'у.
	style.fontClass = cached.fontClass;
	style.opacity = cached.opacity;
	// Обводка — поэлементный преф (mhud*Outline), layout/prefs.cpp наполняет cached.outline
	// из ключа outlineKey (с миграцией из старого общего hudOutline при первом чтении).
	style.outline = cached.outline;
	style.color = color;
	this->ApplyLayoutLabel(layout, panelId, varName, state, style, show, text, force, posPanelId);
}

const char *KZHUDService::ColorClassCache::Get(const Color &c)
{
	const u32 p = ((u32)c.r() << 24) | ((u32)c.g() << 16) | ((u32)c.b() << 8) | (u32)c.a();
	if (!this->valid || this->packed != p)
	{
		this->valid = true;
		this->packed = p;
		this->cls = panorama::ResolveColorClass(c);
	}
	return this->cls;
}

// Общая запись одного текстового лейбла panorama-разметки: элементы худа (UpdateLayoutElement,
// стиль из префов игрока) и строки меню реплея (layout/rpmenu.cpp, стиль задаёт код) идут
// через ЭТУ функцию — второй копии машинерии классов/переменных быть не должно.
void KZHUDService::ApplyLayoutLabel(CCSCustomHudLayout *layout, const char *panelId, const char *varName, LayoutElementState &state,
									const LayoutLabelStyle &style, bool show, const char *text, bool force, const char *posPanelId)
{
	// Позиция, прозрачность и hidden — на posPanelId: у таймера это строка mhud_timer_row, и
	// двигать/гасить надо её целиком, иначе дельта рядом осталась бы на месте и видимой. Ключи
	// префов (mhudTimerX/Y и т.д.) при этом НЕ меняются: сохранённое значение старого игрока
	// просто применяется к строке, и таймер остаётся там же, где был.
	if (!posPanelId)
	{
		posPanelId = panelId;
	}
	// force — сразу после EnsureOwnedLayout(created=true): свежая сущность у схемы уже
	// прибита к дефолтным классам разметки, а наш кэш думает, что ничего слать не надо
	// (совпал с прошлым состоянием прошлой сущности/дефолтом) — форс сбрасывает кэш, чтобы
	// первый кадр реально переслал все классы новой сущности.
	if (force)
	{
		state = LayoutElementState();
	}

	// hidden — отдельной лямбдой: гасим СРАЗУ (дальше писать нечего), а показываем ПОСЛЕДНИМ,
	// после позиции/кегля/цвета. Разметка (mhud.xml и реплика e_* в options.xml) заводит элементы
	// скрытыми, и снятие hidden до классов x/y хоть на один апдейт показало бы элемент в
	// дефолтной точке .element (центр экрана) — непозиционированный видимый элемент.
	auto applyHidden = [&](bool hidden)
	{
		if (state.hidden == hidden)
		{
			return;
		}
		state.hidden = hidden;
		if (!layout->SetHasClass(posPanelId, "hidden", hidden ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
		{
			LogHudInternFailure(this->player, posPanelId, "hidden");
		}
		// Старый VPK аддона (клиент ещё не обновился): строки mhud_timer_row в нём нет, а сам
		// mhud_timer несёт `element hidden`. Дублируем hidden на лейбл, чтобы таймер у такого
		// клиента хотя бы появлялся (без своей позиции), а не пропадал целиком. На новой
		// разметке это no-op: лейбл внутри строки и так гаснет вместе с ней. Новых интерн-строк
		// не добавляет — и id, и класс уже в таблице сущности.
		if (V_strcmp(posPanelId, panelId) != 0)
		{
			layout->SetHasClass(panelId, "hidden", hidden ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass);
		}
	};
	// Скрытый элемент не теряет значения — включить его обратно ничего не стоит.
	if (!show)
	{
		applyHidden(true);
		return;
	}

	// NULL значит вызывающему нечего показать сейчас — держим прошлый текст, а не очищаем.
	if (text && state.text != text)
	{
		state.text = text;
		if (!layout->SetDialogVariableString(panelId, varName, text))
		{
			LogHudInternFailure(this->player, panelId, varName);
		}
	}

	this->SetLayoutValueClass(layout, posPanelId, state.x, style.x, "x", true);
	this->SetLayoutValueClass(layout, posPanelId, state.y, style.y, "y", true);
	this->SetLayoutValueClass(layout, panelId, state.fontSize, style.size, "font-size", false);

	const u32 packed = ((u32)style.color.r() << 24) | ((u32)style.color.g() << 16) | ((u32)style.color.b() << 8) | (u32)style.color.a();
	if (!state.colorComputed || state.lastColorPacked != packed)
	{
		state.colorComputed = true;
		state.lastColorPacked = packed;
		state.colorClassComputed = panorama::ResolveColorClass(style.color);
	}
	this->SetLayoutClass(layout, panelId, state.colorClass, state.colorClassComputed);
	this->SetLayoutClass(layout, panelId, state.fontClass, style.fontClass);
	// Фон лейбла (pal-bg-N) — только у подложек меню реплея; у элементов худа NULL → класс снят.
	this->SetLayoutClass(layout, panelId, state.bgClass, style.bgClass);

	const i32 opacity = Clamp(style.opacity, 0, 100);
	if (state.opacity != opacity)
	{
		char className[32];
		if (state.opacity != INT_MIN)
		{
			V_snprintf(className, sizeof(className), "opacity--%ipct", state.opacity);
			layout->SetHasClass(posPanelId, className, k_eHudPanelClassStatus_DoesNotHaveClass);
		}
		V_snprintf(className, sizeof(className), "opacity--%ipct", opacity);
		if (!layout->SetHasClass(posPanelId, className, k_eHudPanelClassStatus_HasClass))
		{
			LogHudInternFailure(this->player, posPanelId, className);
		}
		state.opacity = opacity;
	}

	if (state.outline != style.outline)
	{
		state.outline = style.outline;
		if (!layout->SetHasClass(panelId, "outline", style.outline ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
		{
			LogHudInternFailure(this->player, panelId, "outline");
		}
	}
	// Показ — последним (см. applyHidden выше).
	applyHidden(false);
}

// === Сущность на игрока =============================================================

CCSCustomHudLayout *KZHUDService::EnsureOwnedLayout(bool &created)
{
	created = false;
	// Плагин выгружается или panorama-худ недоступен (нет MultiAddonManager/форс-cvar) —
	// новую сущность заводить некому её показать.
	if (g_KZPlugin.unloading || !KZHUDService::IsMHUDAvailable())
	{
		return NULL;
	}

	// mhudMimicSpec: интерн-таблицы сущности (HUD_LAYOUT_MAX_INTERNED_STRINGS,
	// sdk/entity/ccscustomhudlayout.h) копятся за весь сеанс наблюдателя и не освобождаются —
	// спектейт нескольких игроков с разной раскладкой добавляет в них ~22 новые строки на
	// каждую новую цель (x/y/font-size/opacity/key-size, см. LogHudInternFailure выше по
	// файлу). Единственный способ вернуть счётчик к нулю — новая сущность (у неё свои,
	// пустые m_vecClassNames/m_vecDialogVariableNames). Пересоздаём СТРОГО на факт смены
	// эффективного источника префов, а не каждый тик: слот ниже меняется только когда
	// реально сменилась цель наблюдения (или мимикрия включилась/выключилась), сравнение —
	// дешёвый int, тик спектейта его не чувствует.
	CPlayerSlot mimicSource(-1);
	if (this->GetOwnLayoutPrefs().mimicSpec)
	{
		// Та же тройная отсечка, что в GetLayoutPrefs (layout/prefs.cpp): нет цели / цель —
		// сам игрок / цель — бот (реплей-бот). Ни один из трёх случаев не мимикрирует —
		// эффективный источник остаётся невалидным (свои префы), пересоздавать не нужно.
		KZPlayer *target = this->player->specService->GetSpectatedPlayer();
		if (target && target != this->player && !target->IsFakeClient())
		{
			mimicSource = target->GetPlayerSlot();
		}
	}
	if (mimicSource != this->layoutMimicSource)
	{
		this->layoutMimicSource = mimicSource;
		// DestroyOwnedLayout снимает сущность и ВМЕСТЕ с ней все кэши классов (layoutElements/
		// layoutKeys/layoutCrosshair) — без этого свежая сущность ниже завелась бы, но кэш
		// решил бы, что всё уже выставлено как надо, и не переслал бы ни одного класса заново.
		// Здесь та же ловушка, что уже документирована для реконнекта в этой же функции.
		//
		// Захват ввода меню (SetInputCaptureEnabled) здесь трогать не нужно: он живёт на
		// ОТДЕЛЬНОЙ сущности this->ownedMenuLayout (layout/menu.cpp, EnsureMenuLayout/
		// DestroyOwnedMenuLayout), а не на this->ownedLayout, который пересоздаём здесь.
		// custom_hud_layout ХУДА никогда не получает SetInputCaptureEnabled(true) — это
		// свойство только окна настроек. Проверено: `git grep SetInputCaptureEnabled` внутри
		// src/kz/hud/ даёт три попадания, и все три — на layout из EnsureMenuLayout
		// (layout/menu.cpp: 380, 1294, 1330), ни одного на ownedLayout. Значит DestroyOwnedLayout
		// не может оставить игрока в режиме курсора, и седьмого пути снятия захвата не
		// требуется — уже существующие шесть (см. журнал menu.cpp) остаются исчерпывающими.
		//
		// Баг апстрима "смена наблюдаемого сливает состояния худа, а не заменяет"
		// (sdk/entity/ccscustomhudlayout.h, BUGS п.1) — про m_vecPlayerLayoutStates,
		// per-observed-slot состояния РАЗНЫХ зрителей одной ОБЩЕЙ сущности. У нас сущность
		// ПЕРСОНАЛЬНАЯ (одна на владельца-наблюдателя, см. комментарий у создания ниже), и
		// весь этот файл пишет ИСКЛЮЧИТЕЛЬНО в GetGlobalLayoutState() — per-player состояния
		// (GetPlayerLayoutState/SetHasClassForPlayer) нигде не используются. Значит сам
		// механизм, в котором живёт баг, в нашем пути записи не участвует: заменить чужой
		// слот на свой в нём нечему сливаться. Пересоздание здесь ничего не лечит (лечить
		// было нечего — баг не наш случай) и ничего не усугубляет: новая сущность получает
		// такое же единственное глобальное состояние, что и старая, просто с чистыми
		// интерн-таблицами.
		this->DestroyOwnedLayout();
	}

	if (CBaseEntity *cached = this->ownedLayout.Get())
	{
		return (CCSCustomHudLayout *)cached;
	}
	// Сущность персональная (не общая на сервер): у неё свой набор dialog-переменных/классов
	// панелей, и транзит чужим гасится KZ::quiet (OwnsLayoutEntity) — общая сущность заставила
	// бы разных игроков драться за одни и те же значения.
	CCSCustomHudLayout *layout = utils::CreateEntityByName<CCSCustomHudLayout>("custom_hud_layout");
	if (!layout)
	{
		return NULL;
	}
	CEntityKeyValues *pKeyValues = new CEntityKeyValues();
	pKeyValues->SetString("layout", KZ_MHUD_LAYOUT);
	// targetname по слоту — для опознания сущности в отладчике.
	char name[32];
	V_snprintf(name, sizeof(name), "kzmhud%i", this->player->GetPlayerSlot().Get());
	pKeyValues->SetString("targetname", name);
	layout->DispatchSpawn(pKeyValues);
	this->ownedLayout = layout;
	created = true;
	return layout;
}

void KZHUDService::DestroyOwnedLayout()
{
	if (!this->ownedLayout.IsValid())
	{
		// Уже уничтожена (или никогда не создавалась) — DrawPanels зовёт этот метод КАЖДЫЙ
		// тик для каждого НЕ-panorama игрока (то есть почти для всех); без этого раннего
		// выхода пять LayoutElementState (со std::string), LayoutKeysState и
		// LayoutCrosshairState пересобирались бы заново на каждый такой тик впустую.
		return;
	}
	if (CBaseEntity *ent = this->ownedLayout.Get())
	{
		g_pKZUtils->RemoveEntity(ent);
	}
	this->ownedLayout = nullptr;
	// Кэши классов элементов обнуляются ВМЕСТЕ с сущностью: они хранят упрощённое состояние
	// ЭТОЙ сущности (что на ней уже выставлено), а не абстрактные значения игрока — оставить
	// их живыми означало бы отдать следующему владельцу слота (реконнект/новый игрок) чужой
	// кэш, из-за которого UpdateLayoutElement решит, что менять уже нечего.
	for (i32 i = 0; i < (i32)LayoutElement::Count; i++)
	{
		this->layoutElements[i] = LayoutElementState();
	}
	// Кэш классов клавиш (Task 6) — та же ловушка: живёт вместе с сущностью, иначе следующий
	// владелец слота (реконнект/новый игрок) унаследует чужие классы кнопок и клавиши останутся
	// пустыми (кэш решит, что менять уже нечего).
	this->layoutKeys = LayoutKeysState();
	// Кэш классов крестика (Task 10) — та же ловушка: без обнуления следующий владелец слота
	// унаследует xh-* классы прошлой сущности, и ApplyCrosshair решит, что крестик уже
	// выставлен как надо, — крестик молча не появится (тот же баг, что ревью поймало для
	// клавиш в задаче 6).
	this->layoutCrosshair = LayoutCrosshairState();
	// Кэш дочерних панелей полей редактора (ячейки PB/WR, угол showpos, тип рана, дельта) — та
	// же ловушка: переживший сущность кэш оставил бы новую сущность с дефолтами разметки.
	this->layoutExtra = LayoutExtraState();
	// Сами значения cl_crosshair* (и флаг confirmed) здесь НЕ трогаем: это НАСТОЯЩИЕ данные
	// игрока (не кэш классов ЭТОЙ сущности), и без цели наблюдения/при смене типа худа они
	// обязаны пережить пересоздание сущности — иначе крестик вернётся только через следующий
	// опрос (до MHUD_XH_POLL_INTERVAL). Сброс — в Reset() (реальное освобождение слота).
}

void KZHUDService::LayoutCleanup()
{
	for (i32 i = 0; i < MAXPLAYERS; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(i));
		if (player && player->hudService)
		{
			player->hudService->DestroyOwnedLayout();
			// Меню (Task 11) — сущность ОТДЕЛЬНАЯ от ownedLayout выше, с собственным
			// курсорным захватом: без явного гашения здесь застрявший в режиме курсора
			// игрок остался бы так и после выгрузки плагина (сущность и захват на ней —
			// не наши больше, но клиент их не увидит снятыми, пока кто-то не уберёт entity).
			player->hudService->DestroyOwnedMenuLayout();
			// Страница меню реплея (layout/rpmenu.cpp) — третья персональная сущность, та же причина.
			player->hudService->DestroyOwnedReplayLayout();
		}
	}
}

bool KZHUDService::OwnsLayoutEntity(CEntityHandle handle)
{
	// Своих сущностей три: сам худ (ownedLayout), отдельное меню настроек (ownedMenuLayout,
	// Task 11) и страница меню реплея
	// спектатора (ownedReplayLayout, layout/rpmenu.cpp) — транзит (KZ::quiet) гасит всё, чего
	// нет в этом списке, и без проверки каждой сущность игроку не долетала бы вовсе
	// (пустые/невалидные хэндлы по-прежнему false).
	return (this->ownedLayout.IsValid() && this->ownedLayout.ToInt() == handle.ToInt())
		   || (this->ownedMenuLayout.IsValid() && this->ownedMenuLayout.ToInt() == handle.ToInt())
		   || (this->ownedReplayLayout.IsValid() && this->ownedReplayLayout.ToInt() == handle.ToInt());
}
