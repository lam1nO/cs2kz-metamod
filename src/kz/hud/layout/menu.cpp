// Меню настроек panorama-худа и крестика (Task 11, обход реестра — задача 2). Урезанный
// перенос апстримного src/kz/option/menu/{kz_menu,model,tables}.cpp — расхождения с ним:
//   - Состав меню (категории/пункты) больше НЕ зашит статическими таблицами прямо тут —
//     регистрируется в hud/prefs/hud_prefs.cpp через общий реестр (KZOptNode/KZOptItem,
//     KZ::menu::Add*, kz/option/menu/model.h — Task 1), этот файл только обходит
//     KZ::menu::GetTree() и рендерит. prefs_transfer апстрима (экспорт/импорт настроек)
//     по-прежнему НЕ переносим — не нужен: своей подсистемы экспорта настроек у нас нет.
//   - НЕТ апстримного font-list-попапа (постраничный обзор ~30 семейств шрифтов): шрифт —
//     Cycle-пункт (клик перебирает короткий курированный список MENU_FONTS), поэтому список
//     панелей ("li%i"/list_popup) и есть НЕ переносим тоже.
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
// kz_player.cpp (мёртв и никого не наблюдает, а также ушёл в спектейт другого игрока) — те же
// точки, где база уже гасит persistent-состояние layout-худа (DestroyOwnedLayout). Без этого
// покрытия игрок, у которого меню было открыто в момент одного из этих событий, застревает в
// режиме курсора и не может играть вообще.
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
#include "sdk/entity/ccscustomhudlayout.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "utils/logging.h"
#include "cs2kz.h"

#include <utility>
#include <vector>

#include "tier0/memdbgon.h"

#define KZ_MENU_DEFAULT_TITLE "HUD Settings"

// === Обход реестра (KZ::menu::GetTree(), Task 1) ============================================
// Состав меню (категории/пункты) зарегистрирован в hud/prefs/hud_prefs.cpp — наше дерево без
// подкатегорий (плоский список, как и раньше: 7 категорий, максимум ~10 пунктов), поэтому
// menuCategory индексирует ПРЯМО верхний уровень GetTree(), subs не используются.

static_function const KZOptItem *GetMenuItem(i32 category, i32 itemIndex)
{
	const std::vector<KZOptNode *> &tree = KZ::menu::GetTree();
	if (category < 0 || category >= (i32)tree.size())
	{
		return NULL;
	}
	const std::vector<KZOptItem> &items = tree[category]->items;
	if (itemIndex < 0 || itemIndex >= (i32)items.size())
	{
		return NULL;
	}
	return &items[itemIndex];
}

// === Мелкие таблицы: шрифты (без апстримного полного list-попапа) ==========================

// Тип худа теперь — обычный Choice-пункт реестра (hud_prefs.cpp: getChoices/getCurrent/onPick
// вокруг GetHudType/SetHudType), цикл Standard → Panorama → Off → Standard получается сам
// перебором getChoices() по кругу (см. ActivateMenuItem/RenderMenuItems ниже) — свой
// HudTypeLabel/NextHudType здесь больше не нужен.

// Курированный список вместо апстримного постраничного обзора всех семейств (~30):
// сознательно урезано (см. комментарий вверху файла) — клик по пункту просто перебирает.
static_global const char *const MENU_FONTS[] = {
	LAYOUT_DEFAULT_FONT, "stratum2-regular-monodigit", "stratum2-bold", "stratum2-medium", "stratum2-mono-bold", "noto-sans-bold", "arial",
	"forcestratum2",
};

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
		default: // Vector/Button — не используются нашим составом меню
			return "type-toggle";
	}
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
SLOT_ID(SwPanel, "sw%i")
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

	this->SetMenuVar(layout, "menu_title", "title", KZ_MENU_DEFAULT_TITLE);
	this->SetMenuBoolClass(layout, "color_popup", "hidden", this->menuApplied.colorPopupHidden, this->menuPopup != MenuPopup::Color);
	this->SetMenuBoolClass(layout, "step_popup", "hidden", this->menuApplied.stepPopupHidden, this->menuPopup != MenuPopup::Step);

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
}

void KZHUDService::RenderMenuCategories(CCSCustomHudLayout *layout)
{
	const std::vector<KZOptNode *> &tree = KZ::menu::GetTree();
	for (i32 i = 0; i < KZ_MENU_CATS; i++)
	{
		const bool used = i < (i32)tree.size();
		if (used)
		{
			this->SetMenuVar(layout, CatLbl(i), CatVar(i), tree[i]->phraseKey);
			this->SetMenuBoolClass(layout, CatPanel(i), "selected", this->menuApplied.catSelected[i], i == this->menuCategory);
		}
		this->SetMenuBoolClass(layout, CatPanel(i), "hidden", this->menuApplied.catHidden[i], !used);
	}
}

// Значение Choice-пункта (сейчас — только Hud Type) — текущий id ищем в списке getChoices(),
// не найден (незнакомое сохранённое значение) — берём первый пункт списка тем же способом,
// каким старый HudTypeLabel по умолчанию отдавал "Standard" (он первый в GetHudTypeChoices).
static_function std::string GetChoiceValueLabel(KZPlayer *player, const KZOptItem &it)
{
	std::vector<KZChoice> choices;
	if (it.getChoices)
	{
		it.getChoices(player, it.tag, choices);
	}
	if (choices.empty())
	{
		return "";
	}
	const i64 current = it.getCurrent ? it.getCurrent(player, it.tag) : choices[0].id;
	for (const KZChoice &c : choices)
	{
		if (c.id == current)
		{
			return c.label;
		}
	}
	return choices[0].label;
}

void KZHUDService::RenderMenuItems(CCSCustomHudLayout *layout)
{
	const std::vector<KZOptNode *> &tree = KZ::menu::GetTree();
	const std::vector<KZOptItem> *items = (this->menuCategory >= 0 && this->menuCategory < (i32)tree.size()) ? &tree[this->menuCategory]->items : NULL;
	const i32 count = items ? MIN((i32)items->size(), KZ_MENU_ITEMS) : 0;

	for (i32 i = 0; i < KZ_MENU_ITEMS; i++)
	{
		const bool used = i < count;
		if (used)
		{
			const KZOptItem &it = (*items)[i];
			this->SetMenuVar(layout, ItemLbl(i), ItemLblVar(i), it.phraseKey);

			const bool isFloat = it.storage == KZOptStorage::Float;
			std::string value;
			const char *swatch = NULL;
			bool on = false;
			switch (it.type)
			{
				case KZOptItemType::Toggle:
					on = this->player->optionService->GetPreferenceBool(it.prefKey, it.idef != 0);
					value = on ? "On" : "Off";
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
					V_snprintf(buf, sizeof(buf), "%i%s", GetIntPref(this->player, it.prefKey, it.idef, isFloat), it.unit ? it.unit : "");
					value = buf;
					break;
				}
				case KZOptItemType::Color:
				{
					const Color cur = this->GetMHUDColorPref(it.prefKey, it.cdef);
					swatch = panorama::GetColorEntryBgClass(panorama::FindColorEntry(cur));
					break;
				}
				default: // Vector/Button — не используются нашим составом меню
					break;
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
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuPopupItem);
	i32 curIdx = -1;
	if (it && it->type == KZOptItemType::Color)
	{
		curIdx = panorama::FindColorEntry(this->GetMHUDColorPref(it->prefKey, it->cdef));
	}
	// Градиенты в попапе РАЗРЕШЕНЫ (задача 14): ограничение до сплошных заводилось из-за
	// particle-MHUD (там же градиент был маркер-цветом с alpha==1, который particle-путь
	// понимал как реальную прозрачность — почти невидимый худ). Particle удалён в задаче 12,
	// а panorama (ResolveColorClass/FindColorEntry) и так резолвит маркер в свою CSS-палитру
	// правильно — прятать от игрока рабочую опцию незачем.
	const i32 total = panorama::GetColorEntryCount();
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
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuPopupItem);
	if (!it)
	{
		return;
	}
	const bool isFloat = it->storage == KZOptStorage::Float;
	const bool vstep = it->type == KZOptItemType::Position;
	if (this->menuApplied.stepVHidden != !vstep)
	{
		this->menuApplied.stepVHidden = !vstep;
		this->SetMenuClass(layout, "m_step_up", "hidden", !vstep);
		this->SetMenuClass(layout, "m_step_down", "hidden", !vstep);
	}

	char readout[32];
	if (vstep)
	{
		V_snprintf(readout, sizeof(readout), "%i%%, %i%%", GetIntPref(this->player, it->prefKey, it->idef, isFloat),
				   GetIntPref(this->player, it->yKey, it->iydef, isFloat));
	}
	else
	{
		V_snprintf(readout, sizeof(readout), "%i%s", GetIntPref(this->player, it->prefKey, it->idef, isFloat), it->unit ? it->unit : "");
	}
	this->SetMenuVar(layout, "step_readout", "step", readout);
	this->SetMenuVar(layout, "step_label", "steplabel", it->phraseKey);
}

// === Взаимодействие ==========================================================================

void KZHUDService::SelectMenuCategory(i32 index)
{
	const std::vector<KZOptNode *> &tree = KZ::menu::GetTree();
	if (index < 0 || index >= (i32)tree.size())
	{
		return;
	}
	if (this->menuPopup != MenuPopup::None)
	{
		this->CloseMenuPopup();
	}
	this->menuCategory = index;
	this->RenderMenu();
}

void KZHUDService::ActivateMenuItem(i32 slot)
{
	const KZOptItem *it = GetMenuItem(this->menuCategory, slot);
	if (!it)
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
			const bool next = !opts->GetPreferenceBool(it->prefKey, it->idef != 0);
			opts->SetPreferenceBool(it->prefKey, next);
			// Требование задачи: без этого правка (hudTimer/hudOutline/mhudCrosshair/...) не
			// видна в панораме до перезахода — RefreshLayoutPrefs кэш всех этих ключей.
			this->RefreshLayoutPrefs();
			this->RenderMenu();
			break;
		}
		case KZOptItemType::Choice:
		{
			// Тип худа (сейчас единственный Choice в нашем составе) — клик перебирает
			// getChoices() по кругу, без попапа (см. HudTypeLabel/NextHudType — было раньше,
			// поведение сохранено byte-в-byte). RefreshLayoutPrefs() тут НЕ нужен — GetHudType
			// не кэшируется в layoutPrefs, читается напрямую при каждой отрисовке.
			std::vector<KZChoice> choices;
			if (it->getChoices)
			{
				it->getChoices(this->player, it->tag, choices);
			}
			if (!choices.empty())
			{
				const i64 current = it->getCurrent ? it->getCurrent(this->player, it->tag) : choices[0].id;
				i32 idx = 0;
				for (i32 i = 0; i < (i32)choices.size(); i++)
				{
					if (choices[i].id == current)
					{
						idx = i;
						break;
					}
				}
				const i64 next = choices[(idx + 1) % choices.size()].id;
				if (it->onPick)
				{
					it->onPick(this->player, it->tag, next);
				}
			}
			this->RenderMenu();
			break;
		}
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
			this->OpenMenuPopup(MenuPopup::Step, slot);
			break;
		case KZOptItemType::Color:
			this->OpenMenuPopup(MenuPopup::Color, slot);
			break;
		default: // Vector/Button — не используются нашим составом меню
			break;
	}
}

void KZHUDService::OpenMenuPopup(MenuPopup kind, i32 itemIndex)
{
	if (!GetMenuItem(this->menuCategory, itemIndex))
	{
		return;
	}
	this->menuPopup = kind;
	this->menuPopupItem = itemIndex;
	this->menuPopupPage = 0;
	this->RenderMenu();
}

void KZHUDService::CloseMenuPopup()
{
	this->menuPopup = MenuPopup::None;
	this->menuPopupItem = -1;
	this->RenderMenu();
}

void KZHUDService::MenuPopupPageStep(i32 delta)
{
	if (this->menuPopup != MenuPopup::Color)
	{
		// Степпер (+-1/+-5) — не постраничный попап; страницы есть только у выбора цвета.
		return;
	}
	// Тот же список, что и в попапе (см. RenderMenuColorPopup) — сплошные + градиенты.
	const i32 total = panorama::GetColorEntryCount();
	const i32 pages = MAX(1, (total + KZ_MENU_SWATCH - 1) / KZ_MENU_SWATCH);
	this->menuPopupPage = Clamp(this->menuPopupPage + delta, 0, pages - 1);
	this->RenderMenu();
}

void KZHUDService::MenuPopupPick(i32 slot)
{
	if (this->menuPopup != MenuPopup::Color)
	{
		return;
	}
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuPopupItem);
	if (!it || it->type != KZOptItemType::Color)
	{
		return;
	}
	const i32 idx = this->menuPopupPage * KZ_MENU_SWATCH + slot;
	// Тот же список, что и в попапе (см. RenderMenuColorPopup) — сплошные + градиенты.
	if (idx < 0 || idx >= panorama::GetColorEntryCount())
	{
		return;
	}
	this->SetMHUDColorPref(it->prefKey, panorama::GetColorEntryValue(idx));
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
	const KZOptItem *it = GetMenuItem(this->menuCategory, this->menuPopupItem);
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
	else
	{
		const i32 current = GetIntPref(this->player, it->prefKey, it->idef, isFloat);
		const i32 value = panorama::SnapToStep(current + StepDelta(current, delta), it->lo, it->hi);
		SetIntPref(this->player, it->prefKey, value, isFloat);
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
	else if (V_strcmp(panelId, "color_close") == 0 || V_strcmp(panelId, "step_close") == 0)
	{
		this->CloseMenuPopup();
	}
	else if (V_strcmp(panelId, "cp_prev") == 0)
	{
		this->MenuPopupPageStep(-1);
	}
	else if (V_strcmp(panelId, "cp_next") == 0)
	{
		this->MenuPopupPageStep(1);
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
}

void KZHUDService::OpenLayoutMenu()
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
		this->player->languageService->PrintChat(true, false, "MHUD - Unavailable");
		return;
	}
	this->menuOpen = true;
	this->menuCategory = 0;
	this->menuPopup = MenuPopup::None;
	this->menuPopupItem = -1;
	// Переводит игрока в режим курсора — симметричное false обязано случиться на КАЖДОМ пути
	// закрытия (см. CloseLayoutMenu/DestroyOwnedMenuLayout и комментарий вверху файла).
	layout->SetInputCaptureEnabled(this->player->GetPlayerSlot(), true);
	this->RenderMenu();
}

void KZHUDService::CloseLayoutMenu()
{
	if (!this->menuOpen)
	{
		return;
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
		// Самая важная строка файла: без неё игрок остаётся в режиме курсора навсегда —
		// движок возвращает управление только когда ВСЕ layout-сущности с capture его сняли.
		layout->SetInputCaptureEnabled(this->player->GetPlayerSlot(), false);
	}
}

// === Точка входа: чат-команда (проводка клика от движка — отдельная задача, см. шапку файла) ==

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
		player->hudService->OpenLayoutMenu();
	}
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_hm, kz_hudmenu);
