// Меню настроек panorama-худа и крестика (Task 11, обход реестра — задача 2, типы Choice/Vector/
// Button + enabledBy/solidOnly/unit/scale/subtext — задача 3). Урезанный перенос
// апстримного src/kz/option/menu/{kz_menu,model,tables}.cpp — расхождения с ним:
//   - Состав меню (категории/пункты) больше НЕ зашит статическими таблицами прямо тут —
//     регистрируется в hud/prefs/hud_prefs.cpp через общий реестр (KZOptNode/KZOptItem,
//     KZ::menu::Add*, kz/option/menu/model.h — Task 1), этот файл только обходит
//     KZ::menu::GetTree() и рендерит. prefs_transfer апстрима (экспорт/импорт настроек)
//     по-прежнему НЕ переносим — не нужен: своей подсистемы экспорта настроек у нас нет.
//   - list_popup/li%i (панель апстримного font-list-попапа, постраничный обзор ~30 семейств)
//     задача 2 сознательно не переносила — у нас Font остался Cycle-пунктом (MENU_FONTS).
//     Задача 3 использует ЭТУ ЖЕ панель под Choice (список, наполняемый getChoices) — панель
//     жила в разметке неиспользованной, теперь работает; Font по-прежнему Cycle, без попапа.
//   - Позиция/размер/прозрачность используют ПОПАП-СТЕППЕР (+-1/+-5), как у апстрима, —
//     без него эти пункты были бы нередактируемы, а спека прямо требует «для каждого элемента
//     — позиция X/Y, размер, шрифт, прозрачность».
//   - GetPreferenceColor/SetPreferenceColor в базе нет (R2, журнал решений задачи): цвет читается
//     GetMHUDColorPref (уже в кэше, kz_hud.cpp) и пишется симметричным
//     KZHUDService::SetMHUDColorPref (тут же, ниже) — преф хранит упакованный int.
//   - Позиция/размер элемента ЧИТАЮТСЯ ЧЕРЕЗ Float (см. layout/prefs.cpp — GetPreferenceFloat
//     для xKey/yKey/sizeKey), а прозрачность и crosshairScale — через Int (GetPreferenceInt):
//     KZOptItem::storage (KZOptStorage::Float/Int, переопределён для Opacity/Scale в
//     hud_prefs.cpp через SetItemPref — AddSize по умолчанию заводит Float) выбирает нужный
//     аксессор, перепутать типы значило бы читать иное значение, чем видит RefreshLayoutPrefs.
//
// ВАЖНО про захват ввода: SetInputCaptureEnabled(slot, true) в OpenLayoutMenu() переводит
// игрока в режим курсора. Симметричное false — ЯВНО, ПЕРЕД удалением сущности, в ОБОИХ путях
// закрытия: CloseLayoutMenu() (закрытие по кнопке/команде/смерти/спектейту/смене карты) и
// DestroyOwnedMenuLayout() (дисконнект/выгрузка плагина). Не полагаемся на то, что удаление
// сущности само снимает захват (ревью: ничем не подтверждённое предположение, цена ошибки —
// игрок навсегда застрял в курсоре и сам этого не заметит) — обе функции снимают его вручную.
// CloseLayoutMenu() дополнительно зовётся из kz_hud.cpp (OnRoundStart — смена карты) и
// kz_player.cpp (мёртв и никого не наблюдает) — те же точки, где база уже гасит
// persistent-состояние layout-худа (DestroyOwnedLayout). Без этого покрытия игрок, у которого
// меню было открыто в момент одного из этих событий, застревает в режиме курсора и не может
// играть вообще.
// Путей снятия захвата ПЯТЬ: клик по m_close/повторная команда (!hm/!options/!hud), смерть без
// цели наблюдения, смена карты (OnRoundStart), дисконнект (Reset), выгрузка плагина/килл-свитч
// (LayoutCleanup); шестым, сеткой безопасности, идёт инвариант CheckMenuCaptureInvariant() ниже. Уход в
// спектейт другого игрока путём закрытия БОЛЬШЕ НЕ ЯВЛЯЕТСЯ (спека 2026-09-09-hud-share §6,
// см. комментарий в kz_player.cpp): меню в спектейте работает, а захват там держится
// осознанно, пока меню открыто.
//
// Проводка клика от движка: Hook_ClientSvcUserMessage на своём KZ_UM_CUSTOM_HUD_CLICKED
// (utils/hooks.cpp; НЕ SDK-шный CS_UM_CustomHudClicked — тот идёт через symlink-proto в
// сабмодуль на чужом пине, см. protobuf/kz_customhud.proto) резолвит игрока по СЛОТУ хука
// (не по сущности из сообщения) и зовёт
// player->hudService->OnLayoutMenuClick(...) — OnLayoutMenuClick ниже сам сверяет присланный
// handle с ownedMenuLayout ИМЕННО этого hudService, так что клик одного игрока физически не
// может применить настройку другому (см. utils/hooks.cpp и журнал решений задачи).
#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/menu.h"
#include "kz/hud/layout/panorama_tables.h"
#include "kz/option/kz_option.h"
#include "kz/option/menu/model.h"
#include "kz/language/kz_language.h"
#include "kz/spec/kz_spec.h" // GetSpectatedPlayer — цель мимикрии (подпись в меню) и reason инварианта
#include "sdk/entity/ccscustomhudlayout.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "utils/logging.h"
#include "cs2kz.h"

#include <utility>
#include <vector>

#include "tier0/memdbgon.h"

// Заголовок меню: фраза (переведена, translations/cs2kz-hud.phrases.txt), а не английский
// литерал — под этим заголовком лежат уже не только настройки худа, но и Misc/Jumpstats/
// локальные ветки. На выбранной категории показываем её имя: игрок видит, где он.
#define KZ_MENU_TITLE_PHRASE "HUD - Menu Title"

// Оформление самого меню (menuFont/menuColor/menuSounds/menuPopupShift). Дефолты — ровно те
// значения, что до этой задачи были зашиты в RenderMenu классами font-family--stratum2-medium-tf
// и pal-fg-9 (= белый, panorama_tables.cpp): смена дефолта поехала бы у всех сразу.
// KZ_MENU_DEFAULT_FONT совпадает с апстримным (origin/master:src/kz/option/menu/prefs/menu_prefs.cpp:10)
// и НЕ равен LAYOUT_DEFAULT_FONT: у худа свой дефолтный шрифт (lato-bold), у меню — свой.
#define KZ_MENU_DEFAULT_FONT "stratum2-medium-tf"
static_global const Color KZ_MENU_DEFAULT_COLOR(255, 255, 255, 255);

// === Обход реестра (KZ::menu::GetTree(), Task 1) ============================================
// Состав меню собирается из ЧЕТЫРЁХ Register() (hud/prefs/hud_prefs.cpp, misc/local/jumpstats —
// порядок Init см. cs2kz.cpp, Task 15) и с задачи «дерево категорий» он ДВУХУРОВНЕВЫЙ, как у
// апстрима: верхний уровень — GetTree(), под раскрытой категорией — её KZOptNode::subs.
// Активный узел держит пара (menuCategory, menuSub), см. kz_hud.h: menuCategory == -1 — root
// (список категорий, ни одна не раскрыта, панель пунктов пуста, OpenLayoutMenu(NULL));
// menuSub == -1 — у категории подкатегорий нет и пункты показывает она сама.

// Узел, чьи пункты показывает средняя колонка (апстримный ActiveNode, kz_menu.cpp:211-228).
// У категории с подкатегориями своих пунктов не бывает — их держат subs.
static_function const KZOptNode *ActiveMenuNode(i32 category, i32 sub)
{
	const std::vector<KZOptNode *> &tree = KZ::menu::GetTree();
	if (category < 0 || category >= (i32)tree.size())
	{
		return NULL;
	}
	const KZOptNode *cat = tree[category];
	if (cat->subs.empty())
	{
		return cat;
	}
	if (sub >= 0 && sub < (i32)cat->subs.size())
	{
		return cat->subs[sub];
	}
	return NULL;
}

static_function const KZOptItem *GetMenuItem(i32 category, i32 sub, i32 itemIndex)
{
	const KZOptNode *node = ActiveMenuNode(category, sub);
	if (!node || itemIndex < 0 || itemIndex >= (i32)node->items.size())
	{
		return NULL;
	}
	return &node->items[itemIndex];
}

// Левая колонка плющится в те же 20 кнопок cat0..cat19, что и раньше (своих панелей под
// вложенность в разметке чужого аддона НЕТ — апстрим рисует подкатегории теми же кнопками с
// классами indent/cat-parent, kz_menu.cpp:230-248/336-357). Слот -> узел: категории верхнего
// уровня по порядку, а сразу под РАСКРЫТОЙ категорией (selectedCategory) — её подкатегории.
// Состав слотов зависит только от selectedCategory, поэтому обработчик клика пересобирает
// раскладку тем же вызовом и не хранит её между кадрами.
struct MenuLeftEntry
{
	const KZOptNode *node {};
	bool isSub {};
	i32 category {-1};
	i32 sub {-1};
};

static_function i32 BuildMenuLeft(i32 selectedCategory, MenuLeftEntry (&slots)[KZ_MENU_CATS])
{
	const std::vector<KZOptNode *> &tree = KZ::menu::GetTree();
	i32 n = 0;
	for (i32 ci = 0; ci < (i32)tree.size() && n < KZ_MENU_CATS; ci++)
	{
		slots[n++] = {tree[ci], false, ci, -1};
		if (ci != selectedCategory)
		{
			continue;
		}
		const std::vector<KZOptNode *> &subs = tree[ci]->subs;
		for (i32 si = 0; si < (i32)subs.size() && n < KZ_MENU_CATS; si++)
		{
			slots[n++] = {subs[si], true, ci, si};
		}
	}
	return n;
}

// === Мелкие таблицы: шрифты (без апстримного полного list-попапа) ==========================

// Тип худа теперь — обычный Choice-пункт реестра (hud_prefs.cpp: getChoices/getCurrent/onPick
// вокруг GetHudType/SetHudType) и открывается попапом списка, как остальные 13 Choice-пунктов
// (см. ActivateMenuItem/RenderMenuListPopup ниже) — свой HudTypeLabel/NextHudType и прежний
// перебор по кругу здесь больше не нужны.

// Курированный список вместо апстримного постраничного обзора всех семейств (~30):
// сознательно урезано (см. комментарий вверху файла) — клик по пункту просто перебирает.
// stratum2-bold-monodigit был дефолтом до смены LAYOUT_DEFAULT_FONT на lato-bold: без него
// в цикле игрок, у которого он сохранён, терял свой шрифт первым же кликом (NextFontSlug не
// нашёл бы текущий и вернул MENU_FONTS[0]). KZ_MENU_DEFAULT_FONT здесь по той же причине: цикл
// один на все Font-пункты, а у нового пункта menuFont дефолт СВОЙ (stratum2-medium-tf) — без
// него первый клик по «Шрифт меню» терял бы текущее значение вместо шага на следующее.
// clang-format off
// Раскладка зафиксирована руками: локальный clang-format (22) и CI-канон (18) переносят этот
// массив по-разному, и любая правка состава давала красный check-code-quality.
static_global const char *const MENU_FONTS[] = {
	LAYOUT_DEFAULT_FONT,
	"stratum2-bold-monodigit",
	"stratum2-regular-monodigit",
	"stratum2-bold",
	"stratum2-medium",
	"stratum2-mono-bold",
	"noto-sans-bold",
	"arial",
	"forcestratum2",
	KZ_MENU_DEFAULT_FONT,
};
// clang-format on

static_function const char *NextFontSlug(const char *current)
{
	const char *slug = panorama::ResolveFontSlug(current, LAYOUT_DEFAULT_FONT);
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(MENU_FONTS); i++)
	{
		if (V_strcmp(slug, MENU_FONTS[i]) == 0)
		{
			return MENU_FONTS[(i + 1) % KZ_ARRAYSIZE(MENU_FONTS)];
		}
	}
	return MENU_FONTS[0];
}

// .type-toggle/.type-color — единственные подтверждённые strings-ом классы с реальным CSS-эффектом
// (пилюля тумблера, показ свотча вместо текста). .type-position/.type-size/.type-choice уже были
// в коде до этой задачи без подтверждённого CSS-правила (harmless, класс просто не красит) —
// .type-vector/.type-button заведены по тому же прецеденту, не новый риск.
static_function const char *GetItemTypeClass(KZOptItemType type)
{
	switch (type)
	{
		case KZOptItemType::Toggle:
			return "type-toggle";
		case KZOptItemType::Font:
		case KZOptItemType::Choice:
			return "type-choice";
		case KZOptItemType::Position:
			return "type-position";
		case KZOptItemType::Size:
			return "type-size";
		case KZOptItemType::Color:
			return "type-color";
		case KZOptItemType::Vector:
			return "type-vector";
		case KZOptItemType::Button:
			return "type-button";
	}
	return "type-toggle";
}

// === Float/Int-развязка хранения (R2/prefs.cpp): позиция/размер элемента — Float,
// прозрачность/crosshairScale — Int. Перепутать значило бы разойтись с RefreshLayoutPrefs. ===

static_function i32 GetIntPref(KZPlayer *player, const char *key, i32 def, bool isFloat)
{
	return isFloat ? (i32)player->optionService->GetPreferenceFloat(key, (f64)def) : (i32)player->optionService->GetPreferenceInt(key, def);
}

static_function void SetIntPref(KZPlayer *player, const char *key, i32 value, bool isFloat)
{
	if (isFloat)
	{
		player->optionService->SetPreferenceFloat(key, (f64)value);
	}
	else
	{
		player->optionService->SetPreferenceInt(key, value);
	}
}

// === scale (model.h): "the preference stores value / scale" — на экране Size всегда целое
// (шаг 1/5), а преф при scale>1 хранит ДРОБЬ этого целого (Float, независимо от storage-флага
// самого пункта — дробь без float не сохранить). scale ставят recordVolume (local_prefs.cpp)
// и jsVolume (jumpstats_prefs.cpp) — ветка scale > 1 живая; у остальных Size-пунктов scale<=1
// (0 или 1, дефолт), и там это ровно старый GetIntPref/SetIntPref, бит в бит.
static_function i32 GetScaledDisplay(KZPlayer *player, const char *key, i32 displayDefault, i32 scale, bool isFloat)
{
	if (scale <= 1)
	{
		return GetIntPref(player, key, displayDefault, isFloat);
	}
	const f64 stored = player->optionService->GetPreferenceFloat(key, (f64)displayDefault / (f64)scale);
	// Size никогда не отрицателен (px/percent) — обычное округление до целого без <cmath>.
	return (i32)(stored * (f64)scale + 0.5);
}

static_function void SetScaledDisplay(KZPlayer *player, const char *key, i32 displayValue, i32 scale, bool isFloat)
{
	if (scale <= 1)
	{
		SetIntPref(player, key, displayValue, isFloat);
		return;
	}
	player->optionService->SetPreferenceFloat(key, (f64)displayValue / (f64)scale);
}

// === enabledBy: серый и клики игнорируются, пока хотя бы один из до двух гейт-префов выключен.
// Дефолт отсутствующего гейт-префа — true (включён): гейтующие тумблеры нашего состава сами
// заведены с дефолтом true (см. hud_prefs.cpp — Enabled-тумблеры элементов), трактовать
// отсутствие как "выключено" ложно погасило бы пункт игроку, который этот преф не трогал.
static_function bool IsMenuItemEnabled(KZPlayer *player, const KZOptItem &it)
{
	for (const char *key : it.enabledBy)
	{
		if (key && !player->optionService->GetPreferenceBool(key, true))
		{
			return false;
		}
	}
	return true;
}

// === solidOnly (Color): попап без градиентов — для потребителя, который не умеет их
// рендерить (см. model.h). Сплошные идут первыми, градиенты хвостом (panorama_tables.cpp:
// entry<PANORAMA_COLOR_COUNT — сплошной), поэтому «без градиентов» это ровно
// GetSolidColorCount() (та же граница, panorama_tables.cpp:392), а не свой обход палитры.
// Флаг ставят два цвета клавиш (hud_prefs.cpp: key-glow-N в keys.css градиентов не знает);
// при solidOnly=false это ровно GetColorEntryCount().
static_function i32 GetColorPopupTotal(const KZOptItem *it)
{
	return (it && it->solidOnly) ? panorama::GetSolidColorCount() : panorama::GetColorEntryCount();
}

// Симметрично GetMHUDColorPref (kz_hud.cpp) — тот же формат упаковки (R2: своего
// SetPreferenceColor в базе нет).
static_function i64 PackColorForPref(const Color &c)
{
	return ((i64)c.r() << 24) | ((i64)c.g() << 16) | ((i64)c.b() << 8) | (i64)c.a();
}

void KZHUDService::SetMHUDColorPref(const char *name, const Color &color)
{
	this->player->optionService->SetPreferenceInt(name, PackColorForPref(color));
}

// === Panel id / dialog var helpers (тот же приём, что у апстрима — своя static-буфер функция
// на каждый паттерн) =========================================================================

#define SLOT_ID(fn, fmt) \
	static_function const char *fn(i32 i) \
	{ \
		static_persist char buf[24]; \
		V_snprintf(buf, sizeof(buf), fmt, i); \
		return buf; \
	}

SLOT_ID(CatPanel, "cat%i")
SLOT_ID(CatLbl, "cat_lbl%i")
SLOT_ID(CatVar, "cl%i")
SLOT_ID(ItemPanel, "item%i")
SLOT_ID(ItemLbl, "item_lbl%i")
SLOT_ID(ItemLblVar, "il%i")
SLOT_ID(ItemVal, "item_val%i")
SLOT_ID(ItemValVar, "iv%i")
SLOT_ID(ItemSw, "item_sw%i")
SLOT_ID(ItemSub, "item_sub%i")
SLOT_ID(ItemSubVar, "is%i")
SLOT_ID(SwPanel, "sw%i")
SLOT_ID(LiPanel, "li%i")
SLOT_ID(LiLbl, "li_lbl%i")
SLOT_ID(LiLblVar, "ll%i")
#undef SLOT_ID

// === Сущность меню (своя, отдельная от this->ownedLayout — см. kz_hud.h/OpenLayoutMenu) =====

CCSCustomHudLayout *KZHUDService::EnsureMenuLayout(bool &created)
{
	created = false;
	if (g_KZPlugin.unloading || !KZHUDService::IsMHUDAvailable())
	{
		return NULL;
	}
	if (CBaseEntity *cached = this->ownedMenuLayout.Get())
	{
		return (CCSCustomHudLayout *)cached;
	}
	CCSCustomHudLayout *layout = utils::CreateEntityByName<CCSCustomHudLayout>("custom_hud_layout");
	if (!layout)
	{
		return NULL;
	}
	CEntityKeyValues *pKeyValues = new CEntityKeyValues();
	pKeyValues->SetString("layout", KZ_MENU_LAYOUT);
	char name[32];
	V_snprintf(name, sizeof(name), "kzmenu%i", this->player->GetPlayerSlot().Get());
	pKeyValues->SetString("targetname", name);
	layout->DispatchSpawn(pKeyValues);
	this->ownedMenuLayout = layout;
	created = true;
	// Диф-кэш (menuApplied/menuVars) — состояние ПРЕДЫДУЩЕЙ сущности (смена карты её не
	// переживает, см. CYBER.md/транзит), а сам кэш переживает: без сброса ЗДЕСЬ, сразу на
	// споне, первое открытие меню после смены карты решило бы, что классы/переменные уже
	// выставлены как надо, и не отправило бы их — пустая рамка до реконнекта. Сбрасываем в
	// момент реального создания сущности, а не полагаемся на `created`, дошедший до
	// вызывающего: OpenLayoutMenu зовёт Ensure и тут же RenderMenu, который зовёт Ensure
	// повторно и всегда видит created=false (сущность уже кэширована первым вызовом).
	this->menuApplied = MenuAppliedState();
	this->menuVars.clear();
	return layout;
}

void KZHUDService::DestroyOwnedMenuLayout()
{
	if (CBaseEntity *ent = this->ownedMenuLayout.Get())
	{
		// Снимаем захват ЯВНО, до удаления сущности — не полагаемся на побочный эффект
		// RemoveEntity: ничего в API/документации CCSCustomHudLayout не гарантирует, что
		// удаление сущности с активным m_bInputCaptureEnabled само возвращает игроку
		// управление (ревью: предположение "снимется само" ничем не подтверждено, а цена
		// ошибки — игрок навсегда застрял в режиме курсора и сам этого не заметит). Тот же
		// явный вызов, что и в CloseLayoutMenu().
		((CCSCustomHudLayout *)ent)->SetInputCaptureEnabled(this->player->GetPlayerSlot(), false);
		g_pKZUtils->RemoveEntity(ent);
	}
	this->ownedMenuLayout = nullptr;
	// menuOpen-флаг живёт на KZPlayer дольше сущности (сама сущность уже погашена выше) и
	// обязан сброситься явно, иначе следующий Toggle() решит, что меню открыто, и попробует
	// его "закрыть" вместо открытия.
	this->menuOpen = false;
	this->menuPopup = MenuPopup::None;
	this->menuPopupItem = -1;
	// Кэш применённых классов/переменных — та же ловушка, что у layoutElements/layoutKeys/
	// layoutCrosshair (entity.cpp/DestroyOwnedLayout): живёт ТОЛЬКО вместе с сущностью,
	// иначе следующий владелец слота унаследует чужой diff-кэш.
	this->menuApplied = MenuAppliedState();
	this->menuVars.clear();
}

// === Запись классов/переменных — diff-кэш (полная сеть на КАЖДЫЙ SetHasClass/SetDialogVariableString,
// поэтому лишние повторы дорогие; см. entity.cpp/SetLayoutClass для того же приёма) ==========

void KZHUDService::SetMenuClass(CCSCustomHudLayout *layout, const char *panelId, const char *className, bool on)
{
	// SetHasClass возвращает false, когда сущность упёрлась в HUD_LAYOUT_MAX_INTERNED_STRINGS
	// (1024) — дальше меню молча перестаёт обновляться. Отказ обязан быть видимым (канон
	// проекта), а не тихим фризом оформления.
	if (!layout->SetHasClass(panelId, className, on ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_menu_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n",
					panelId, className, this->player->GetPlayerSlot().Get());
	}
}

void KZHUDService::SetMenuBoolClass(CCSCustomHudLayout *layout, const char *panelId, const char *className, bool &cache, bool want)
{
	if (cache != want)
	{
		cache = want;
		this->SetMenuClass(layout, panelId, className, want);
	}
}

void KZHUDService::SetMenuSwapClass(CCSCustomHudLayout *layout, const char *panelId, const char *&cache, const char *want)
{
	if (cache == want)
	{
		return;
	}
	if (cache)
	{
		this->SetMenuClass(layout, panelId, cache, false);
	}
	if (want)
	{
		this->SetMenuClass(layout, panelId, want, true);
	}
	cache = want;
}

void KZHUDService::SetMenuVar(CCSCustomHudLayout *layout, const char *panelId, const char *var, const char *value)
{
	std::string &cached = this->menuVars[var];
	if (cached == value)
	{
		return;
	}
	cached = value;
	if (!layout->SetDialogVariableString(panelId, var, value))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_menu_var_dropped reason=intern_limit panel=%s var=%s slot=%i\n", panelId, var,
					this->player->GetPlayerSlot().Get());
	}
}

// Мимикрия РЕАЛЬНО действует: на экране худ наблюдаемого, а меню и читает, и пишет только
// свои настройки (this->player->optionService везде ниже) — значит правка не даёт видимого
// эффекта, и об этом надо сказать. Отсечки один-в-один с GetLayoutPrefs (layout/prefs.cpp):
// нет цели / цель — сам / цель — бот / у цели ещё не загружены префы.
static_function bool IsMenuMimicActive(KZPlayer *player)
{
	if (!player->hudService->GetOwnLayoutPrefs().mimicSpec)
	{
		return false;
	}
	KZPlayer *target = player->specService->GetSpectatedPlayer();
	if (!target || target == player || target->IsFakeClient())
	{
		return false;
	}
	return target->hudService->GetOwnLayoutPrefs().loaded;
}

// === Рендер ==================================================================================

void KZHUDService::RenderMenu()
{
	bool created = false;
	CCSCustomHudLayout *layout = this->EnsureMenuLayout(created);
	if (!layout)
	{
		return;
	}
	if (created)
	{
		this->menuApplied = MenuAppliedState();
		this->menuVars.clear();
	}

	this->SetMenuBoolClass(layout, "menu_root", "hidden", this->menuApplied.rootHidden, !this->menuOpen);
	if (!this->menuOpen)
	{
		return;
	}

	auto *opts = this->player->optionService;
	// menuSounds: класс snd на корне включает звуки наведения/клика — правила в menu.css чужого
	// аддона висят на потомках корня (".snd .cat:not(.disabled):hover { sound: ... }" и ещё шесть
	// пар), так что класс обязан лежать именно на menu_root. Апстрим: kz_menu.cpp:289.
	this->SetMenuBoolClass(layout, "menu_root", "snd", this->menuApplied.sounds, opts->GetPreferenceBool("menuSounds", true));
	// menuPopupShift: пока открыт попап, всё меню уезжает влево (#menu_root.shift { x: -125px }) —
	// на 4:3/5:4 попап иначе вылезает за край экрана. Апстрим: kz_menu.cpp:291-293.
	const bool shift = this->menuPopup != MenuPopup::None && opts->GetPreferenceBool("menuPopupShift", true);
	this->SetMenuBoolClass(layout, "menu_root", "shift", this->menuApplied.shift, shift);

	// Заголовок — имя АКТИВНОГО узла (подкатегории, если она выбрана): имя самой категории и
	// так стоит слева жирной шапкой (класс cat-parent), дублировать его в заголовке значило бы
	// не показать игроку, на какой он странице.
	const KZOptNode *titleNode = ActiveMenuNode(this->menuCategory, this->menuSub);
	const char *titleKey = (titleNode && titleNode->phraseKey) ? titleNode->phraseKey : KZ_MENU_TITLE_PHRASE;
	std::string title = KZLanguageService::PrepareMessageWithLang(this->player->languageService->GetLanguage(), titleKey);
	// Подпись про мимикрию (спека §6) — в САМ заголовок: своей панели/переменной под подзаголовок
	// в разметке чужого аддона нет (menu.vxml_c), а заводить её мы не можем. Хвост короткий
	// намеренно: заголовок узкий, длинную фразу движок обрезал бы. Полное объяснение уходит в
	// чат при открытии меню (OpenLayoutMenu).
	if (IsMenuMimicActive(this->player))
	{
		title += " ";
		title += KZLanguageService::PrepareMessageWithLang(this->player->languageService->GetLanguage(), "HUD - Menu Mimic Suffix");
	}
	this->SetMenuVar(layout, "menu_title", "title", title.c_str());
	// Шрифт и цвет корня — ПРЕФЫ игрока (порт апстримного RenderChrome, kz_menu.cpp:316-334).
	// До этой задачи оба класса были зашиты (font-family--stratum2-medium-tf + pal-fg-9); теперь
	// это ДЕФОЛТЫ пунктов menuFont/menuColor (KZMenuChromeMenu_Register ниже), и зашитая пара
	// сохраняется как поведение по умолчанию — у кого префов нет, вид не меняется ни на пиксель.
	// Текстовые панели наследуют оба класса от корня; без них движковый дефолт (мелкий шрифт,
	// красный текст) — этим и было наше «пиксельно».
	const char *menuFont = panorama::ResolveFontClass(opts->GetPreferenceStr("menuFont", KZ_MENU_DEFAULT_FONT), KZ_MENU_DEFAULT_FONT);
	// Расхождение с апстримной строкой kz_menu.cpp:321: GetPreferenceColor в нашей базе нет (R2),
	// цвет хранится упакованным int и читается GetMHUDColorPref — как у всех остальных цветов.
	const char *menuColor = panorama::ResolveColorClass(this->GetMHUDColorPref("menuColor", KZ_MENU_DEFAULT_COLOR));
	// Апстрим оборачивает эти два вызова в `if (applied.menuFont != font || applied.menuColor != color)`
	// ради MarkFullChanged (без пересчёта слоя ребёнок остаётся со старым шрифтом). У нас
	// MarkFullChanged зовётся БЕЗУСЛОВНО в конце RenderMenu (см. комментарий там), поэтому
	// условие лишнее: сам SetMenuSwapClass уже диф-кэширован и на неизменившемся классе молчит.
	this->SetMenuSwapClass(layout, "menu_root", this->menuApplied.menuFont, menuFont);
	this->SetMenuSwapClass(layout, "menu_root", this->menuApplied.menuColor, menuColor);
	this->SetMenuBoolClass(layout, "color_popup", "hidden", this->menuApplied.colorPopupHidden, this->menuPopup != MenuPopup::Color);
	this->SetMenuBoolClass(layout, "step_popup", "hidden", this->menuApplied.stepPopupHidden, this->menuPopup != MenuPopup::Step);
	this->SetMenuBoolClass(layout, "list_popup", "hidden", this->menuApplied.listPopupHidden, this->menuPopup != MenuPopup::List);

	this->RenderMenuCategories(layout);
	this->RenderMenuItems(layout);

	if (this->menuPopup == MenuPopup::Color)
	{
		this->RenderMenuColorPopup(layout);
	}
	else if (this->menuPopup == MenuPopup::Step)
	{
		this->RenderMenuStepPopup(layout);
	}
	else if (this->menuPopup == MenuPopup::List)
	{
		this->RenderMenuListPopup(layout);
	}

	// Движковый баг (sdk/entity/ccscustomhudlayout.h:179-181): SetHasClassForPlayer не
	// доезжает до ДЕТЕЙ панели — стили ребёнка (статичный фон .menu-box, зелёная плашка
	// .item.type-toggle.on .item-value, свотч .item.type-color .item-swatch, подпись
	// .item.has-sub .item-sub) применяются только при прямом изменении самого ребёнка или
	// при полном пересчёте слоя. Первое появление НОВОЙ пары (панель,класс) сама метит
	// сущность на полный пересчёт (SetHasClass, ветка AddToTail), но повторное переключение
	// уже интернированной пары — только точечно (MarkHasClassChanged), и тогда ребёнок
	// остаётся со старым стилем. Апстрим обходит это тем же вызовом, но только вслед за
	// сменой шрифта/цвета (kz_menu.cpp:330); мы зовём безусловно на каждый рендер меню —
	// сама пометка не заводит ни строку, ни класс, лимита HUD_LAYOUT_MAX_INTERNED_STRINGS
	// не касается (0 новых записей).
	layout->GetGlobalLayoutState()->MarkFullChanged();
}

void KZHUDService::RenderMenuCategories(CCSCustomHudLayout *layout)
{
	MenuLeftEntry slots[KZ_MENU_CATS];
	const i32 count = BuildMenuLeft(this->menuCategory, slots);
	const KZOptNode *active = ActiveMenuNode(this->menuCategory, this->menuSub);
	const char *lang = this->player->languageService->GetLanguage();
	for (i32 i = 0; i < KZ_MENU_CATS; i++)
	{
		const bool used = i < count;
		if (used)
		{
			const MenuLeftEntry &e = slots[i];
			// Классы ровно апстримные (kz_menu.cpp:336-357), все четыре описаны в menu.css чужого
			// аддона: .cat.indent (отступ подкатегории), .cat.cat-parent (жирная шапка категории),
			// .cat.cat-parent.disabled (раскрытый родитель не подсвечивается наведением, клик по
			// нему гасит SelectMenuCategory), .cat.selected (плашка активной строки).
			const bool isParent = !e.isSub;
			const bool disabled = isParent && !e.node->subs.empty() && e.category == this->menuCategory;
			// PrepareMessageWithLang деградирует в сам ключ, если фразы нет (см. GetTranslatedFormat) —
			// это то же поведение, что и у остального форка (KZLanguageService), не своя логика.
			const std::string catLabel = KZLanguageService::PrepareMessageWithLang(lang, e.node->phraseKey);
			this->SetMenuVar(layout, CatLbl(i), CatVar(i), catLabel.c_str());
			this->SetMenuBoolClass(layout, CatPanel(i), "indent", this->menuApplied.catIndent[i], e.isSub);
			this->SetMenuBoolClass(layout, CatPanel(i), "cat-parent", this->menuApplied.catParent[i], isParent);
			this->SetMenuBoolClass(layout, CatPanel(i), "disabled", this->menuApplied.catDisabled[i], disabled);
			this->SetMenuBoolClass(layout, CatPanel(i), "selected", this->menuApplied.catSelected[i], e.node == active);
		}
		this->SetMenuBoolClass(layout, CatPanel(i), "hidden", this->menuApplied.catHidden[i], !used);
	}
}

// Значение Choice-пункта: текущий id ищем в списке getChoices(). Первую строку списка при
// промахе НЕ подставляем — так строка "Стили" (мультивыбор, getCurrent всегда -1) вечно
// показывала имя первого стиля как выбранное, а Mode/Language врали при сохранённом значении,
// которого в списке нет. Фолбэк — отмеченные строки (getChoices сам ставит selected), как в
// апстриме (origin/master src/kz/option/menu/kz_menu.cpp:415-444); ничего не отмечено — пусто.
static_function std::string GetChoiceValueLabel(KZPlayer *player, const KZOptItem &it)
{
	std::vector<KZChoice> choices;
	if (it.getChoices)
	{
		it.getChoices(player, it.tag, choices);
	}
	if (it.getCurrent)
	{
		const i64 current = it.getCurrent(player, it.tag);
		for (const KZChoice &c : choices)
		{
			if (c.id == current)
			{
				return c.label;
			}
		}
	}
	std::string value;
	for (const KZChoice &c : choices)
	{
		if (c.selected)
		{
			value += value.empty() ? c.label : ", " + c.label;
		}
	}
	return value;
}

void KZHUDService::RenderMenuItems(CCSCustomHudLayout *layout)
{
	const KZOptNode *node = ActiveMenuNode(this->menuCategory, this->menuSub);
	const std::vector<KZOptItem> *items = node ? &node->items : NULL;
	const i32 count = items ? MIN((i32)items->size(), KZ_MENU_ITEMS) : 0;
	// Пункты за KZ_MENU_ITEMS-м просто не существуют для игрока — отказ обязан быть видимым
	// (канон проекта), как и у интерн-лимита в SetMenuClass. Логируем один раз на кадр рендера
	// той категории, что не влезла: состав фиксируется на регистрации, спама не будет.
	if (items && (i32)items->size() > KZ_MENU_ITEMS)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_menu_items_truncated reason=slot_limit category=%s items=%i limit=%i slot=%i\n",
					node->phraseKey ? node->phraseKey : "?", (i32)items->size(), KZ_MENU_ITEMS, this->player->GetPlayerSlot().Get());
	}
	const char *lang = this->player->languageService->GetLanguage();

	for (i32 i = 0; i < KZ_MENU_ITEMS; i++)
	{
		const bool used = i < count;
		if (used)
		{
			const KZOptItem &it = (*items)[i];
			const std::string itemLabel = KZLanguageService::PrepareMessageWithLang(lang, it.phraseKey);
			this->SetMenuVar(layout, ItemLbl(i), ItemLblVar(i), itemLabel.c_str());
			// subtext (item-sub) — видимость целиком на CSS (.item.has-sub .item-sub), нам
			// достаточно переключить класс has-sub на самом пункте; var пишем всегда (пусто,
			// если subKey нет, — безвредно под collapse). subKey — ключ фразы, а не готовый
			// текст (как и phraseKey label двумя строками выше) — переводим тем же механизмом,
			// апстрим делает так же (kz_menu.cpp:450, KZMenuService::GetPhrase).
			const std::string itemSub = it.subKey ? KZLanguageService::PrepareMessageWithLang(lang, it.subKey) : "";
			this->SetMenuVar(layout, ItemSub(i), ItemSubVar(i), itemSub.c_str());
			this->SetMenuBoolClass(layout, ItemPanel(i), "has-sub", this->menuApplied.itemHasSub[i], it.subKey != NULL);
			// Разделителей под пунктами не проводим: панель item_div%i чужого аддона несёт
			// класс .item-divider, а правил ни для .item-divider.hidden, ни для безусловного
			// .hidden в menu.css нет — разделитель виден под каждым пунктом в любом случае.
			// Апстримный класс "divider" на самом пункте правила в CSS тоже не имеет.
			// enabledBy — серый и клики мимо (клик гасится в ActivateMenuItem, здесь только цвет).
			this->SetMenuBoolClass(layout, ItemPanel(i), "disabled", this->menuApplied.itemDisabled[i], !IsMenuItemEnabled(this->player, it));

			const bool isFloat = it.storage == KZOptStorage::Float;
			std::string value;
			const char *swatch = NULL;
			bool on = false;
			switch (it.type)
			{
				case KZOptItemType::Toggle:
					// getCurrent обязателен для AddActionToggle: состояние такого пункта держит
					// сервис (кэш + свой преф, иногда легаси-фолбэк), а prefKey у части из них
					// вообще NULL — чтение через преф показывало бы дефолт вместо правды. Ветка
					// ровно апстримная (origin/master src/kz/option/menu/kz_menu.cpp:381).
					on = it.getCurrent ? it.getCurrent(this->player, it.tag) != 0
									   : this->player->optionService->GetPreferenceBool(it.prefKey, it.idef != 0);
					// Ключи существуют с задачи 12 (particles.cpp), код их не читал — Task 15.
					value = KZLanguageService::PrepareMessageWithLang(lang, on ? "HUD - Menu On" : "HUD - Menu Off");
					break;
				case KZOptItemType::Choice:
					value = GetChoiceValueLabel(this->player, it);
					break;
				case KZOptItemType::Font:
				{
					const char *slug = this->player->optionService->GetPreferenceStr(it.prefKey, it.sdef ? it.sdef : LAYOUT_DEFAULT_FONT);
					value = panorama::GetFontDisplayName(slug, LAYOUT_DEFAULT_FONT);
					break;
				}
				case KZOptItemType::Position:
				{
					char buf[32];
					V_snprintf(buf, sizeof(buf), "%i%%, %i%%", GetIntPref(this->player, it.prefKey, it.idef, isFloat),
							   GetIntPref(this->player, it.yKey, it.iydef, isFloat));
					value = buf;
					break;
				}
				case KZOptItemType::Size:
				{
					char buf[24];
					V_snprintf(buf, sizeof(buf), "%i%s", GetScaledDisplay(this->player, it.prefKey, it.idef, it.scale, isFloat),
							   it.unit ? it.unit : "");
					value = buf;
					break;
				}
				case KZOptItemType::Vector:
				{
					const Vector v = this->player->optionService->GetPreferenceVector(it.prefKey, Vector((f32)it.idef, (f32)it.iydef, (f32)it.izdef));
					char buf[48];
					V_snprintf(buf, sizeof(buf), "%i, %i, %i", (i32)v.x, (i32)v.y, (i32)v.z);
					value = buf;
					break;
				}
				case KZOptItemType::Color:
				{
					const Color cur = this->GetMHUDColorPref(it.prefKey, it.cdef);
					swatch = panorama::GetColorEntryBgClass(panorama::FindColorEntry(cur));
					break;
				}
				case KZOptItemType::Button:
					break; // без значения (клик зовёт onActivate, состояния нет)
			}

			this->SetMenuVar(layout, ItemVal(i), ItemValVar(i), value.c_str());
			this->SetMenuSwapClass(layout, ItemSw(i), this->menuApplied.itemSwatch[i], swatch);
			this->SetMenuSwapClass(layout, ItemPanel(i), this->menuApplied.itemType[i], GetItemTypeClass(it.type));
			this->SetMenuBoolClass(layout, ItemPanel(i), "on", this->menuApplied.itemOn[i], on);
		}
		this->SetMenuBoolClass(layout, ItemPanel(i), "hidden", this->menuApplied.itemHidden[i], !used);
	}
}

void KZHUDService::RenderMenuColorPopup(CCSCustomHudLayout *layout)
{
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, this->menuPopupItem);
	i32 curIdx = -1;
	if (it && it->type == KZOptItemType::Color)
	{
		curIdx = panorama::FindColorEntry(this->GetMHUDColorPref(it->prefKey, it->cdef));
	}
	// Градиенты в попапе РАЗРЕШЕНЫ по умолчанию (задача 14): ограничение до сплошных заводилось
	// из-за particle-MHUD (там же градиент был маркер-цветом с alpha==1, который particle-путь
	// понимал как реальную прозрачность — почти невидимый худ). Particle удалён в задаче 12,
	// а panorama (ResolveColorClass/FindColorEntry) и так резолвит маркер в свою CSS-палитру
	// правильно — прятать от игрока рабочую опцию незачем. solidOnly (задача 3) отрезает
	// градиенты там, где их нечем нарисовать (см. GetColorPopupTotal): сейчас это два цвета
	// клавиш — SetItemSolidOnly на PressedColor и OverlapGlowColor, hud_prefs.cpp.
	const i32 total = GetColorPopupTotal(it);
	const i32 pages = MAX(1, (total + KZ_MENU_SWATCH - 1) / KZ_MENU_SWATCH);
	this->menuPopupPage = Clamp(this->menuPopupPage, 0, pages - 1);

	for (i32 i = 0; i < KZ_MENU_SWATCH; i++)
	{
		const i32 idx = this->menuPopupPage * KZ_MENU_SWATCH + i;
		const bool used = idx < total;
		if (used)
		{
			this->SetMenuSwapClass(layout, SwPanel(i), this->menuApplied.swBg[i], panorama::GetColorEntryBgClass(idx));
			this->SetMenuBoolClass(layout, SwPanel(i), "selected", this->menuApplied.swSelected[i], idx == curIdx);
		}
		this->SetMenuBoolClass(layout, SwPanel(i), "hidden", this->menuApplied.swHidden[i], !used);
	}
	char page[16];
	V_snprintf(page, sizeof(page), "%i/%i", this->menuPopupPage + 1, pages);
	this->SetMenuVar(layout, "cp_page", "cppage", page);
}

void KZHUDService::RenderMenuStepPopup(CCSCustomHudLayout *layout)
{
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, this->menuPopupItem);
	if (!it)
	{
		return;
	}
	const bool isFloat = it->storage == KZOptStorage::Float;
	// Y-ряд (m_step_up/m_step_down, класс v-step) нужен и Position (вторая ось), и Vector
	// (тоже вторая ось) — третья ось Vector отдельным рядом m_step_z (см. ниже).
	const bool vstep = it->type == KZOptItemType::Position || it->type == KZOptItemType::Vector;
	const bool zstep = it->type == KZOptItemType::Vector;
	if (this->menuApplied.stepVHidden != !vstep)
	{
		this->menuApplied.stepVHidden = !vstep;
		this->SetMenuClass(layout, "m_step_up", "hidden", !vstep);
		this->SetMenuClass(layout, "m_step_down", "hidden", !vstep);
	}
	if (this->menuApplied.stepZHidden != !zstep)
	{
		this->menuApplied.stepZHidden = !zstep;
		this->SetMenuClass(layout, "m_step_z", "hidden", !zstep);
	}

	char readout[48];
	if (it->type == KZOptItemType::Vector)
	{
		// Разметка: "the z row reuses the readout slot for an axis label; the main readout
		// shows all three" — единственный readout на все три оси, ряд m_step_z несёт только
		// статическую подпись "Z" (не var, уже в разметке).
		const Vector v = this->player->optionService->GetPreferenceVector(it->prefKey, Vector((f32)it->idef, (f32)it->iydef, (f32)it->izdef));
		V_snprintf(readout, sizeof(readout), "%i, %i, %i", (i32)v.x, (i32)v.y, (i32)v.z);
	}
	else if (vstep) // Position
	{
		V_snprintf(readout, sizeof(readout), "%i%%, %i%%", GetIntPref(this->player, it->prefKey, it->idef, isFloat),
				   GetIntPref(this->player, it->yKey, it->iydef, isFloat));
	}
	else // Size
	{
		V_snprintf(readout, sizeof(readout), "%i%s", GetScaledDisplay(this->player, it->prefKey, it->idef, it->scale, isFloat),
				   it->unit ? it->unit : "");
	}
	this->SetMenuVar(layout, "step_readout", "step", readout);
	const char *lang = this->player->languageService->GetLanguage();
	const std::string stepLabel = KZLanguageService::PrepareMessageWithLang(lang, it->phraseKey);
	this->SetMenuVar(layout, "step_label", "steplabel", stepLabel.c_str());
}

// Choice: попап списка, наполняется getChoices() КАЖДЫЙ рендер (а не только при открытии) —
// тот же приём, что и в ActivateMenuItem/GetChoiceValueLabel, чтобы рантайм-варианты не отставали
// от смены страницы/повторного открытия. colorClass строки (KZChoice) не применяется — под него
// нет подтверждённого CSS-класса на .li/.li-label (см. отчёт задачи), сама строка не теряется.
void KZHUDService::RenderMenuListPopup(CCSCustomHudLayout *layout)
{
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, this->menuPopupItem);
	if (!it)
	{
		return;
	}
	std::vector<KZChoice> choices;
	if (it->getChoices)
	{
		it->getChoices(this->player, it->tag, choices);
	}
	const i64 current = it->getCurrent ? it->getCurrent(this->player, it->tag) : (choices.empty() ? 0 : choices[0].id);
	const i32 total = (i32)choices.size();
	const i32 pages = MAX(1, (total + KZ_MENU_LIST - 1) / KZ_MENU_LIST);
	this->menuPopupPage = Clamp(this->menuPopupPage, 0, pages - 1);

	for (i32 i = 0; i < KZ_MENU_LIST; i++)
	{
		const i32 idx = this->menuPopupPage * KZ_MENU_LIST + i;
		const bool used = idx < total;
		if (used)
		{
			this->SetMenuVar(layout, LiLbl(i), LiLblVar(i), choices[idx].label.c_str());
			const bool selected = choices[idx].selected || choices[idx].id == current;
			this->SetMenuBoolClass(layout, LiPanel(i), "selected", this->menuApplied.liSelected[i], selected);
		}
		this->SetMenuBoolClass(layout, LiPanel(i), "hidden", this->menuApplied.liHidden[i], !used);
	}
	const char *lang = this->player->languageService->GetLanguage();
	const std::string lpTitle = KZLanguageService::PrepareMessageWithLang(lang, it->phraseKey);
	this->SetMenuVar(layout, "lp_title", "lptitle", lpTitle.c_str());
	char page[16];
	V_snprintf(page, sizeof(page), "%i/%i", this->menuPopupPage + 1, pages);
	this->SetMenuVar(layout, "lp_page", "lppage", page);
}

// === Взаимодействие ==========================================================================

// index — номер СЛОТА левой колонки (cat%i), а не индекс категории: под раскрытой категорией в
// той же колонке стоят её подкатегории, см. BuildMenuLeft.
void KZHUDService::SelectMenuCategory(i32 index)
{
	MenuLeftEntry slots[KZ_MENU_CATS];
	const i32 count = BuildMenuLeft(this->menuCategory, slots);
	if (index < 0 || index >= count)
	{
		return;
	}
	const MenuLeftEntry &e = slots[index];
	if (this->menuPopup != MenuPopup::None)
	{
		// Закрываем ДО любых ранних выходов (порядок апстрима, kz_menu.cpp): клик по левой
		// колонке с открытым попапом обязан его закрыть, даже если сам клик ничего не выбирает.
		// CloseMenuPopup зовёт onEdit(begin=false) для пункта ещё СТАРОГО узла — поэтому до
		// смены menuCategory/menuSub ниже.
		this->CloseMenuPopup();
	}
	// Шапка уже раскрытой категории инертна (тот же класс disabled, что и в рендере): её
	// подкатегории и так видны, а сама она пунктов не имеет — клик по ней сбросил бы выбранную
	// подкатегорию в первую без причины.
	if (!e.isSub && !e.node->subs.empty() && e.category == this->menuCategory)
	{
		this->RenderMenu();
		return;
	}
	this->menuCategory = e.category;
	// Клик по подкатегории — показать её пункты; клик по категории — раскрыть её и показать
	// ПЕРВУЮ подкатегорию (пустая средняя колонка на раскрытии выглядела бы отказом), а у
	// листовой категории показывает её саму (menuSub = -1).
	this->menuSub = e.isSub ? e.sub : (e.node->subs.empty() ? -1 : 0);
	this->RenderMenu();
}

void KZHUDService::ActivateMenuItem(i32 slot)
{
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, slot);
	if (!it)
	{
		return;
	}
	// enabledBy: клик мимо — пункт серый (см. RenderMenuItems), но проверка тут независима от
	// рендера (клиент теоретически мог прислать клик по устаревшему кадру разметки).
	if (!IsMenuItemEnabled(this->player, *it))
	{
		return;
	}
	if (this->menuPopup != MenuPopup::None)
	{
		this->CloseMenuPopup();
	}

	auto *opts = this->player->optionService;
	switch (it->type)
	{
		case KZOptItemType::Toggle:
		{
			// AddActionToggle: щёлкает сервис (Toggle*() держит кэш и преф в синхроне), а сырая
			// запись префа для таких пунктов ЗАПРЕЩЕНА — у трёх из них prefKey == NULL, и
			// SetPreferenceBool(NULL, ...) завёл бы в префах игрока член с пустым именем и
			// сохранил его в БД. Ветка ровно апстримная (kz_menu.cpp:666-675).
			if (it->onActivate)
			{
				it->onActivate(this->player, it->tag);
			}
			else
			{
				opts->SetPreferenceBool(it->prefKey, !opts->GetPreferenceBool(it->prefKey, it->idef != 0));
			}
			// Требование задачи: без этого правка (hudTimer/mhud*Outline/mhudCrosshair/...) не
			// видна в панораме до перезахода — RefreshLayoutPrefs кэш всех этих ключей.
			this->RefreshLayoutPrefs();
			this->RenderMenu();
			break;
		}
		case KZOptItemType::Choice:
			// Разметка держит настоящий попап списка (list_popup/li%i) — прежний инлайн-цикл
			// (клик сразу перебирает getChoices) убран, включая для HudType: по явному решению
			// после задачи 2 (см. бриф задачи 3) исключения для него больше нет — открывается
			// списком, как любой другой Choice.
			this->OpenMenuPopup(MenuPopup::List, slot);
			break;
		case KZOptItemType::Font:
		{
			const char *cur = opts->GetPreferenceStr(it->prefKey, it->sdef ? it->sdef : LAYOUT_DEFAULT_FONT);
			opts->SetPreferenceStr(it->prefKey, NextFontSlug(cur));
			this->RefreshLayoutPrefs();
			this->RenderMenu();
			break;
		}
		case KZOptItemType::Position:
		case KZOptItemType::Size:
		case KZOptItemType::Vector:
			this->OpenMenuPopup(MenuPopup::Step, slot);
			break;
		case KZOptItemType::Color:
			this->OpenMenuPopup(MenuPopup::Color, slot);
			break;
		case KZOptItemType::Button:
			if (it->onActivate)
			{
				it->onActivate(this->player, it->tag);
			}
			// onActivate — произвольное действие, могло написать любой преф (см. модель) —
			// тот же инвариант, что и у Toggle/Font: RefreshLayoutPrefs после записи.
			this->RefreshLayoutPrefs();
			this->RenderMenu();
			break;
	}
}

void KZHUDService::OpenMenuPopup(MenuPopup kind, i32 itemIndex)
{
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, itemIndex);
	if (!it)
	{
		return;
	}
	this->menuPopup = kind;
	this->menuPopupItem = itemIndex;
	this->menuPopupPage = 0;
	// onEdit(begin=true/false) — контракт модели (model.h): пункт узнаёт, что его правят.
	// Единственный сегодняшний потребитель (beamOffset, misc_prefs.cpp) досинкивает кэш
	// сервиса на закрытии, поэтому важнее второй вызов, но апстрим (kz_menu.cpp:758-761,
	// 769-772) зовёт оба, и порядок держим тот же — колбэк ДО первого рендера попапа.
	if (it->onEdit)
	{
		it->onEdit(this->player, it->tag, true);
	}
	this->RenderMenu();
}

// Закрытие попапа с вызовом onEdit(begin=false). Все явные пути закрытия (кнопка *_close,
// клик по другому пункту, смена категории, CloseLayoutMenu) идут сюда; DestroyOwnedMenuLayout
// (дисконнект/выгрузка плагина) колбэк сознательно НЕ зовёт — досинкивать кэш сервиса игроку,
// который уже уходит, некому и незачем.
void KZHUDService::CloseMenuPopup()
{
	if (const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, this->menuPopupItem))
	{
		if (this->menuPopup != MenuPopup::None && it->onEdit)
		{
			it->onEdit(this->player, it->tag, false);
		}
	}
	this->menuPopup = MenuPopup::None;
	this->menuPopupItem = -1;
	this->RenderMenu();
}

void KZHUDService::MenuPopupPageStep(i32 delta)
{
	if (this->menuPopup == MenuPopup::Color)
	{
		const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, this->menuPopupItem);
		// Тот же список, что и в попапе (см. RenderMenuColorPopup/GetColorPopupTotal) —
		// сплошные (+градиенты, если не solidOnly).
		const i32 total = GetColorPopupTotal(it);
		const i32 pages = MAX(1, (total + KZ_MENU_SWATCH - 1) / KZ_MENU_SWATCH);
		this->menuPopupPage = Clamp(this->menuPopupPage + delta, 0, pages - 1);
		this->RenderMenu();
	}
	else if (this->menuPopup == MenuPopup::List)
	{
		const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, this->menuPopupItem);
		if (!it)
		{
			return;
		}
		std::vector<KZChoice> choices;
		if (it->getChoices)
		{
			it->getChoices(this->player, it->tag, choices);
		}
		const i32 pages = MAX(1, ((i32)choices.size() + KZ_MENU_LIST - 1) / KZ_MENU_LIST);
		this->menuPopupPage = Clamp(this->menuPopupPage + delta, 0, pages - 1);
		this->RenderMenu();
	}
	// Степпер (+-1/+-5) — не постраничный попап; страницы есть только у цвета и списка.
}

void KZHUDService::MenuPopupPick(i32 slot)
{
	if (this->menuPopup != MenuPopup::Color)
	{
		return;
	}
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, this->menuPopupItem);
	if (!it || it->type != KZOptItemType::Color)
	{
		return;
	}
	const i32 idx = this->menuPopupPage * KZ_MENU_SWATCH + slot;
	// Тот же список, что и в попапе (см. RenderMenuColorPopup/GetColorPopupTotal).
	if (idx < 0 || idx >= GetColorPopupTotal(it))
	{
		return;
	}
	this->SetMHUDColorPref(it->prefKey, panorama::GetColorEntryValue(idx));
	this->RefreshLayoutPrefs();
	this->RenderMenu();
}

void KZHUDService::MenuListPick(i32 slot)
{
	if (this->menuPopup != MenuPopup::List)
	{
		return;
	}
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, this->menuPopupItem);
	if (!it || it->type != KZOptItemType::Choice)
	{
		return;
	}
	std::vector<KZChoice> choices;
	if (it->getChoices)
	{
		it->getChoices(this->player, it->tag, choices);
	}
	const i32 idx = this->menuPopupPage * KZ_MENU_LIST + slot;
	if (idx < 0 || idx >= (i32)choices.size())
	{
		return;
	}
	if (it->onPick)
	{
		it->onPick(this->player, it->tag, choices[idx].id);
	}
	// onPick — тот же контракт, что у Toggle/Font/Button: пишет произвольный преф, RefreshLayoutPrefs
	// после записи обязателен. Choice-пунктов в составе 14 (HudType/Idle/Mode/Styles/Pistol/
	// Beam/Language/CompareType и т.д.); те, что кэша в layoutPrefs не имеют, проходят этот
	// путь безвредно — лишний RefreshLayoutPrefs ничего не портит.
	this->RefreshLayoutPrefs();
	this->RenderMenu();
}

// panorama::SnapToStep снапит к кратным 5 везде вне ±100 (ограничение классов схемы) — шаг
// ±1 там становится no-op (напр. 151 снапится обратно в 150). Кнопки ±1 обязаны реально
// двигать значение, поэтому вне ±100 подменяем шаг 1 на 5 того же знака; шаг ±5 уже
// совпадает с гранулярностью снапа и не нуждается в подмене.
static_function i32 StepDelta(i32 current, i32 delta)
{
	if ((current > 100 || current < -100) && (delta == 1 || delta == -1))
	{
		return delta > 0 ? 5 : -5;
	}
	return delta;
}

void KZHUDService::MenuStep(i32 axis, i32 delta)
{
	if (this->menuPopup != MenuPopup::Step)
	{
		return;
	}
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, this->menuPopupItem);
	if (!it)
	{
		return;
	}
	const bool isFloat = it->storage == KZOptStorage::Float;
	if (it->type == KZOptItemType::Position)
	{
		const char *key = axis == 1 ? it->yKey : it->prefKey;
		const i32 def = axis == 1 ? it->iydef : it->idef;
		const i32 current = GetIntPref(this->player, key, def, isFloat);
		const i32 value = panorama::SnapToStep(current + StepDelta(current, delta), it->lo, it->hi);
		SetIntPref(this->player, key, value, isFloat);
	}
	else if (it->type == KZOptItemType::Vector)
	{
		// Одна Vector-преф хранит все три компоненты разом (GetPreferenceVector/SetPreferenceVector,
		// kz_option.h) — читаем целиком, правим одну ось по axis (0=x/1=y/2=z), пишем целиком.
		Vector v = this->player->optionService->GetPreferenceVector(it->prefKey, Vector((f32)it->idef, (f32)it->iydef, (f32)it->izdef));
		f32 &comp = axis == 2 ? v.z : (axis == 1 ? v.y : v.x);
		const i32 current = (i32)comp;
		comp = (f32)panorama::SnapToStep(current + StepDelta(current, delta), it->lo, it->hi);
		this->player->optionService->SetPreferenceVector(it->prefKey, v);
	}
	else // Size — единственный оставшийся одноосный тип попапа-степпера
	{
		const i32 current = GetScaledDisplay(this->player, it->prefKey, it->idef, it->scale, isFloat);
		const i32 value = panorama::SnapToStep(current + StepDelta(current, delta), it->lo, it->hi);
		SetScaledDisplay(this->player, it->prefKey, value, it->scale, isFloat);
	}
	this->RefreshLayoutPrefs();
	this->RenderMenu();
}

// === Клик и захват ввода ======================================================================

void KZHUDService::OnLayoutMenuClick(uint32 packedHandle, const char *panelId)
{
	if (!this->menuOpen)
	{
		return;
	}
	CCSCustomHudLayout *layout = CCSCustomHudLayout::FromClickHandle(packedHandle);
	// Сверка с this->ownedMenuLayout — не чужая/устаревшая сущность (пересоздание после
	// EnsureMenuLayout сменило бы хэндл, а клиент мог прислать клик уже в пути).
	if (!layout || (CBaseEntity *)layout != this->ownedMenuLayout.Get())
	{
		return;
	}

	if (V_strcmp(panelId, "m_close") == 0)
	{
		this->CloseLayoutMenu();
	}
	else if (V_strcmp(panelId, "color_close") == 0 || V_strcmp(panelId, "step_close") == 0 || V_strcmp(panelId, "list_close") == 0)
	{
		this->CloseMenuPopup();
	}
	else if (V_strcmp(panelId, "cp_prev") == 0 || V_strcmp(panelId, "lp_prev") == 0)
	{
		this->MenuPopupPageStep(-1);
	}
	else if (V_strcmp(panelId, "cp_next") == 0 || V_strcmp(panelId, "lp_next") == 0)
	{
		this->MenuPopupPageStep(1);
	}
	else if (V_strcmp(panelId, "m_z_n5") == 0)
	{
		this->MenuStep(2, -5);
	}
	else if (V_strcmp(panelId, "m_z_n1") == 0)
	{
		this->MenuStep(2, -1);
	}
	else if (V_strcmp(panelId, "m_z_p1") == 0)
	{
		this->MenuStep(2, 1);
	}
	else if (V_strcmp(panelId, "m_z_p5") == 0)
	{
		this->MenuStep(2, 5);
	}
	else if (V_strcmp(panelId, "m_v_n5") == 0)
	{
		this->MenuStep(1, -5);
	}
	else if (V_strcmp(panelId, "m_v_n1") == 0)
	{
		this->MenuStep(1, -1);
	}
	else if (V_strcmp(panelId, "m_v_p1") == 0)
	{
		this->MenuStep(1, 1);
	}
	else if (V_strcmp(panelId, "m_v_p5") == 0)
	{
		this->MenuStep(1, 5);
	}
	else if (V_strcmp(panelId, "m_h_n5") == 0)
	{
		this->MenuStep(0, -5);
	}
	else if (V_strcmp(panelId, "m_h_n1") == 0)
	{
		this->MenuStep(0, -1);
	}
	else if (V_strcmp(panelId, "m_h_p1") == 0)
	{
		this->MenuStep(0, 1);
	}
	else if (V_strcmp(panelId, "m_h_p5") == 0)
	{
		this->MenuStep(0, 5);
	}
	else if (V_strncmp(panelId, "cat", 3) == 0 && V_isdigit(panelId[3]))
	{
		this->SelectMenuCategory(atoi(panelId + 3));
	}
	else if (V_strncmp(panelId, "item", 4) == 0 && V_isdigit(panelId[4]))
	{
		this->ActivateMenuItem(atoi(panelId + 4));
	}
	else if (V_strncmp(panelId, "sw", 2) == 0 && V_isdigit(panelId[2]))
	{
		this->MenuPopupPick(atoi(panelId + 2));
	}
	else if (V_strncmp(panelId, "li", 2) == 0 && V_isdigit(panelId[2]))
	{
		// "list_close" отсеян точным сравнением выше (panelId[2]=='s', не цифра) — коллизий нет.
		this->MenuListPick(atoi(panelId + 2));
	}
}

// Узел по его phraseKey (Task 15: развести !options/root от !hudmenu/HUD, не завязываясь на
// числовой индекс — он плывёт при первой же смене порядка регистрации в cs2kz.cpp). Ищем и по
// верхнему уровню, и по подкатегориям: с появлением дерева часть прежних категорий стала
// подкатегориями (напр. "HUD - Menu Cat General" уехал под "HUD - Menu Cat Hud"), а ключ в
// точках входа (!hudmenu тут и kz_hud.cpp) не менялся — попадание по ключу подкатегории
// раскрывает её родителя и показывает её пункты, то есть ровно прежнее поведение команды.
// NULL или ключ не найден — root (-1,-1): RenderMenuCategories/RenderMenuItems/GetMenuItem уже
// трактуют это как «список категорий без предвыбранной».
static_function void FindMenuNode(const char *categoryKey, i32 &category, i32 &sub)
{
	category = -1;
	sub = -1;
	if (!categoryKey)
	{
		return;
	}
	const std::vector<KZOptNode *> &tree = KZ::menu::GetTree();
	for (i32 i = 0; i < (i32)tree.size(); i++)
	{
		if (tree[i]->phraseKey && V_strcmp(tree[i]->phraseKey, categoryKey) == 0)
		{
			category = i;
			// Категория с подкатегориями своих пунктов не имеет — открываем на первой из них.
			sub = tree[i]->subs.empty() ? -1 : 0;
			return;
		}
	}
	for (i32 i = 0; i < (i32)tree.size(); i++)
	{
		const std::vector<KZOptNode *> &subs = tree[i]->subs;
		for (i32 si = 0; si < (i32)subs.size(); si++)
		{
			if (subs[si]->phraseKey && V_strcmp(subs[si]->phraseKey, categoryKey) == 0)
			{
				category = i;
				sub = si;
				return;
			}
		}
	}
}

void KZHUDService::OpenLayoutMenu(const char *categoryKey)
{
	if (this->menuOpen)
	{
		this->CloseLayoutMenu();
		return;
	}
	bool created = false;
	CCSCustomHudLayout *layout = this->EnsureMenuLayout(created);
	if (!layout)
	{
		// Отказ игроку — существующей фразой (она про «меню настроек недоступно»), но
		// молчаливым он быть не должен: reason различает диагностический килл-свитч
		// (kz_hud_layout_enabled 0) от прочей недоступности MHUD (нет MultiAddonManager).
		KZ_LOG_WARN(LogChannel::General, "[cyb] hud_menu_open_denied reason=%s slot=%i\n",
					KZHUDService::IsHudLayoutKillSwitchOff() ? "hud_layout_disabled" : "mhud_unavailable",
					this->player->GetPlayerSlot().Get());
		this->player->languageService->PrintChat(true, false, "MHUD - Unavailable");
		return;
	}
	// Порт апстримного гейта (origin/master:src/kz/option/menu/kz_menu.cpp:1017-1020): без
	// per-player состояния SetInputCaptureEnabled тихо ничего не делает (см.
	// sdk/entity/ccscustomhudlayout.h) — игрок получил бы нарисованное, но некликабельное меню
	// и никакого объяснения. Отказ обязан быть виден; фраза та же — она про «меню настроек
	// недоступно», reason различает причины.
	if (!layout->GetPlayerLayoutState(this->player->GetPlayerSlot()))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] hud_menu_open_denied reason=no_player_layout_state slot=%i\n",
					this->player->GetPlayerSlot().Get());
		this->player->languageService->PrintChat(true, false, "MHUD - Unavailable");
		return;
	}
	this->menuOpen = true;
	FindMenuNode(categoryKey, this->menuCategory, this->menuSub);
	this->menuPopup = MenuPopup::None;
	this->menuPopupItem = -1;
	// Переводит игрока в режим курсора — симметричное false обязано случиться на КАЖДОМ пути
	// закрытия (см. CloseLayoutMenu/DestroyOwnedMenuLayout и комментарий вверху файла).
	layout->SetInputCaptureEnabled(this->player->GetPlayerSlot(), true);
	// Меню в спектейте с включённой мимикрией: на экране чужой худ, правки идут в свои
	// значения. Молчаливое «правка без эффекта» — худший из вариантов, поэтому кроме хвоста
	// в заголовке меню (RenderMenu) объясняем это одной строкой в чат при открытии.
	if (IsMenuMimicActive(this->player))
	{
		this->player->languageService->PrintChat(true, false, "HUD - Menu Mimic Note");
	}
	this->RenderMenu();
}

void KZHUDService::CloseLayoutMenu()
{
	if (!this->menuOpen)
	{
		return;
	}
	// Меню закрывают и с открытым попапом (клавиша/смерть без цели наблюдения/смена карты) — onEdit
	// закрытия обязан прийти и на этом пути, иначе правка Vector-пункта (beamOffset) осядет
	// в БД, а кэш сервиса останется старым до реконнекта.
	if (const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuSub, this->menuPopupItem))
	{
		if (this->menuPopup != MenuPopup::None && it->onEdit)
		{
			it->onEdit(this->player, it->tag, false);
		}
	}
	this->menuOpen = false;
	this->menuPopup = MenuPopup::None;
	this->menuPopupItem = -1;
	// Сущность могла быть уже погашена ДО закрытия (DestroyOwnedMenuLayout из Reset/
	// LayoutCleanup/OnRoundStart-развязки) — тогда снимать нечего, это не отказ.
	if (CBaseEntity *ent = this->ownedMenuLayout.Get())
	{
		CCSCustomHudLayout *layout = (CCSCustomHudLayout *)ent;
		this->SetMenuClass(layout, "menu_root", "hidden", true);
		this->menuApplied.rootHidden = true;
		// Сдвиг под попап снимаем здесь же (апстрим — DropCapture, kz_menu.cpp:986-988): иначе
		// следующее открытие приезжает анимацией слева, из положения закрытого попапа.
		this->SetMenuClass(layout, "menu_root", "shift", false);
		this->menuApplied.shift = false;
		// Самая важная строка файла: без неё игрок остаётся в режиме курсора навсегда —
		// движок возвращает управление только когда ВСЕ layout-сущности с capture его сняли.
		layout->SetInputCaptureEnabled(this->player->GetPlayerSlot(), false);
	}
}

// Инвариант (спека 2026-09-09-hud-share §6, канон проекта «закрытие инцидента = проверка»):
// НЕ БЫВАЕТ включённого захвата ввода при закрытом меню. Класс отказа «игрок остался с
// курсором и не может играть до перезахода» до этой задачи не контролировался ничем, а после
// снятия закрытия меню при спектейте (kz_player.cpp) путей снятия захвата стало на один
// меньше. Нарушение не глушим и не игнорируем: пишем warn с машинно-читаемым reason (по нему
// заводится алерт/инвариант флота) и снимаем захват принудительно — игрок возвращается в игру
// сам, без перезахода, а сам факт остаётся в логах для разбора.
void KZHUDService::CheckMenuCaptureInvariant()
{
	if (this->menuOpen)
	{
		return;
	}
	// Проверка живёт в игровом такте, поэтому дросселируется: чтение схемы на каждого игрока
	// каждый тик не нужно — залипший захват не самолечится и будет виден на следующем окне.
	const f64 now = g_pKZUtils->GetServerGlobals()->curtime;
	// curtime отсчитывается от загрузки карты: переживший смену карты дедлайн окажется «в
	// будущем» и заглушил бы проверку до конца карты (та же ловушка, что у lastBottomSendTime
	// в kz_hud.cpp) — дедлайн дальше одного окна считаем просроченным.
	if (now < this->menuCaptureCheckTime && this->menuCaptureCheckTime - now <= KZ_MENU_CAPTURE_CHECK_INTERVAL)
	{
		return;
	}
	this->menuCaptureCheckTime = now + KZ_MENU_CAPTURE_CHECK_INTERVAL;
	CBaseEntity *ent = this->ownedMenuLayout.Get();
	if (!ent)
	{
		// Сущности нет — захвату негде жить: клиент возвращает управление, когда ни одна
		// layout-сущность его не держит, а удалённая сущность его и не держит.
		return;
	}
	CCSCustomHudLayout *layout = (CCSCustomHudLayout *)ent;
	const CPlayerSlot slot = this->player->GetPlayerSlot();
	if (!layout->IsInputCaptureEnabled(slot))
	{
		return;
	}
	// error, а не warn: по конвенции проекта warn — «отказали пользователю», а это сломалось
	// У НАС (захват держится при закрытом меню — состояние, которого быть не должно).
	KZ_LOG_ERROR(LogChannel::General, "[cyb] hud_menu_capture_leak reason=capture_without_open_menu slot=%i alive=%s spectating=%s\n",
				 slot.Get(), this->player->IsAlive() ? "true" : "false",
				 this->player->specService->GetSpectatedPlayer() ? "true" : "false");
	layout->SetInputCaptureEnabled(slot, false);
}

// === Точка входа: чат-команда (проводка клика от движка — отдельная задача, см. шапку файла) ==

// R8: !hudmenu годами открывал настройки худа напрямую — регресс был бы лишний клик через
// список категорий. Ключ ("HUD - Menu Cat General") — первая категория, которую регистрирует
// KZHUDService::InitMenuPrefs() (hud/prefs/hud_prefs.cpp), а не жёсткий индекс 0: индекс сам
// поехал бы при первой же смене порядка Register() в cs2kz.cpp (Task 15), ключ — нет.
SCMD(kz_hudmenu, SCFL_HUD | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	if (player->hudService->IsLayoutMenuOpen())
	{
		player->hudService->CloseLayoutMenu();
	}
	else
	{
		player->hudService->OpenLayoutMenu("HUD - Menu Cat General");
	}
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_hm, kz_hudmenu);

// === Оформление самого меню: регистрация пунктов ============================================
// Порт апстримного KZMenuService::RegisterChromePrefs (origin/master:src/kz/option/menu/prefs/
// menu_prefs.cpp:12-21) — до этой задачи четыре ключа (menuFont/menuColor/menuSounds/
// menuPopupShift) не читались вообще ни одной строкой форка, а шрифт и цвет меню были зашиты
// классами в RenderMenu. Читатель заведён выше (RenderMenu), здесь — сами пункты.
// Отличия от апстримной функции:
//   - Разделитель пункта/KZ::prefs::RegisterMenu не переносим: подсистемы экспорта настроек
//     (prefs_transfer) у нас нет, а класс divider на пункте не имеет правила в menu.css чужого
//     аддона (тот же вывод, что в RenderMenuItems) — применялся бы молча и без эффекта.
//   - Категория — ЛИСТОВАЯ (без AddSub), как Misc и Jumpstats: четыре пункта на подкатегории
//     делить нечего.
// Вызов — cs2kz.cpp::Load, последним из Register(): порядок вызовов = порядок категорий в левой
// колонке, оформление меню трогают реже всего остального.
void KZMenuChromeMenu_Register()
{
	KZOptNode *cat = KZ::menu::AddCategory("HUD - Menu Cat MenuChrome");
	// Дефолты обязаны совпасть с тем, что читает RenderMenu (KZ_MENU_DEFAULT_FONT/
	// KZ_MENU_DEFAULT_COLOR и `true` у обоих тумблеров) — разойтись значило бы показывать в
	// меню не то значение, с которым оно нарисовано.
	KZ::menu::AddFont(cat, "HUD - Menu Label MenuFont", "menuFont", KZ_MENU_DEFAULT_FONT);
	KZ::menu::AddColor(cat, "HUD - Menu Label MenuColor", "menuColor", KZ_MENU_DEFAULT_COLOR);
	KZ::menu::AddToggle(cat, "HUD - Menu Label MenuSounds", "menuSounds", true);
	KZ::menu::AddToggle(cat, "HUD - Menu Label MenuPopupShift", "menuPopupShift", true);
	KZ::menu::SetItemSubtext(cat, "HUD - Menu Label MenuPopupShift Sub");
}
