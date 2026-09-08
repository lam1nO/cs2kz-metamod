// Меню настроек panorama-худа и крестика (Task 11). Урезанный перенос апстримного
// src/kz/option/menu/{kz_menu,model,tables}.cpp — расхождения с ним:
//   - НЕТ pref-registry (KZOptNode/KZOptItem, KZ::menu::Add*) и НЕТ prefs_transfer
//     (экспорт/импорт настроек): своей подсистемы настроек с деревом категорий у нас нет,
//     это меню обслуживает ровно худ+крестик, поэтому категории и пункты — статические
//     таблицы (MenuCategories() ниже), а не общий реестр. Прямое требование спеки задачи
//     (task-11-brief.md), не вкусовщина.
//   - НЕТ апстримного font-list-попапа (постраничный обзор ~30 семейств шрифтов): шрифт —
//     Cycle-пункт (клик перебирает короткий курированный список MENU_FONTS), поэтому список
//     панелей ("li%i"/list_popup) и есть НЕ переносим тоже.
//   - Позиция/размер/прозрачность используют ПОПАП-СТЕППЕР (+-1/+-5), как у апстрима, —
//     без него эти пункты были бы нередактируемы, а спека прямо требует «для каждого элемента
//     — позиция X/Y, размер, шрифт, прозрачность».
//   - GetPreferenceColor/SetPreferenceColor в базе нет (R2, base-facts.md): цвет читается
//     GetMHUDColorPref (уже в кэше) и пишется симметричным KZHUDService::SetMHUDColorPref
//     (тут же, ниже) — преф хранит упакованный int, как у particles.cpp/SetColorPref.
//   - Позиция/размер элемента ЧИТАЮТСЯ ЧЕРЕЗ Float (см. layout/prefs.cpp — GetPreferenceFloat
//     для xKey/yKey/sizeKey), а прозрачность и crosshairScale — через Int (GetPreferenceInt):
//     MenuItem::isFloat выбирает нужный аксессор, перепутать типы значило бы читать иное
//     значение, чем видит RefreshLayoutPrefs.
//
// ВАЖНО про захват ввода: SetInputCaptureEnabled(slot, true) в OpenLayoutMenu() переводит
// игрока в режим курсора. Симметричное false — в CloseLayoutMenu() (закрытие по кнопке/команде)
// И в DestroyOwnedMenuLayout() (дисконнект/выгрузка плагина — там снимается неявно вместе с
// уничтожением сущности, которая держит capture-состояние). Дополнительно CloseLayoutMenu()
// зовётся из kz_hud.cpp (OnRoundStart — смена карты) и kz_player.cpp (мёртв и никого не
// наблюдает) — те же точки, где база уже гасит persistent-состояние layout-худа
// (DestroyOwnedLayout). Без этого покрытия игрок, у которого меню было открыто в момент одного
// из этих событий, застревает в режиме курсора и не может играть вообще.
//
// Проводка клика от движка: Hook_ClientSvcUserMessage на своём KZ_UM_CUSTOM_HUD_CLICKED
// (utils/hooks.cpp; НЕ SDK-шный CS_UM_CustomHudClicked — тот идёт через symlink-proto в
// сабмодуль на чужом пине, см. protobuf/kz_customhud.proto) резолвит игрока по СЛОТУ хука
// (не по сущности из сообщения) и зовёт
// player->hudService->OnLayoutMenuClick(...) — OnLayoutMenuClick ниже сам сверяет присланный
// handle с ownedMenuLayout ИМЕННО этого hudService, так что клик одного игрока физически не
// может применить настройку другому (см. utils/hooks.cpp и base-facts.md).
#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/menu.h"
#include "kz/hud/layout/panorama_tables.h"
#include "kz/option/kz_option.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "cs2kz.h"

#include <utility>
#include <vector>

#include "tier0/memdbgon.h"

#define KZ_MENU_DEFAULT_TITLE "HUD Settings"

// === Модель пунктов меню (локальная, НЕ реестр — см. комментарий вверху файла) =============

enum class MenuItemKind
{
	Toggle,
	HudType, // цикл MHUD -> Panorama -> Standard -> Off -> MHUD (GetHudType/SetHudType)
	Font,    // цикл по MENU_FONTS, без попапа
	Position,
	Size,
	Opacity,
	Color,
};

struct MenuItem
{
	const char *label;
	MenuItemKind kind;
	const char *prefKey {};
	const char *yKey {}; // Position only
	i32 lo {};           // Position/Size/Opacity range
	i32 hi {};
	i32 idef {};          // default value (bool: 0/1)
	i32 iydef {};         // Position y default
	const Color *cdef {}; // Color default — указатель на extern-глобал (MHUD_DEF_*), НЕ на временный
	const char *unit {};  // Size/Opacity суффикс
	bool isFloat {};      // true — преф хранится как float (позиция/размер элемента), см. layout/prefs.cpp
};

struct MenuCategory
{
	const char *label;
	std::vector<MenuItem> items;
};

// Пять полей элемента, общих для Timer/Speed/Prespeed/Keys/Checkpoint — читаем ключи из
// LAYOUT_ELEMENTS (entity.cpp, Task 4), а не дублируем строки: один источник правды.
static_function void AddElementItems(std::vector<MenuItem> &items, LayoutElement e)
{
	const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)e];
	items.push_back({"Enabled", MenuItemKind::Toggle, def.enabledKey, nullptr, 0, 1, 1});
	items.push_back({"Position", MenuItemKind::Position, def.xKey, def.yKey, -100, 100, def.xDefault, def.yDefault, nullptr, nullptr, true});
	items.push_back({"Size", MenuItemKind::Size, def.sizeKey, nullptr, LAYOUT_SIZE_MIN, LAYOUT_SIZE_MAX, def.sizeDefault, 0, nullptr, "px", true});
	items.push_back({"Font", MenuItemKind::Font, def.fontKey});
	items.push_back({"Opacity", MenuItemKind::Opacity, def.opacityKey, nullptr, 0, 100, 100, 0, nullptr, "%"});
}

static_function void AddColorItem(std::vector<MenuItem> &items, const char *label, const char *prefKey, const Color &def)
{
	items.push_back({label, MenuItemKind::Color, prefKey, nullptr, 0, 0, 0, 0, &def});
}

static_function std::vector<MenuCategory> BuildMenuCategories()
{
	std::vector<MenuCategory> cats;

	{
		MenuCategory cat {"General"};
		cat.items.push_back({"Hud Type", MenuItemKind::HudType});
		// hudOutline — ОБЩИЙ тумблер всех пяти элементов (LAYOUT_ELEMENTS[*].outlineKey
		// одинаков), поэтому один пункт на всё меню, а не по одному в каждой категории.
		cat.items.push_back({"Outline", MenuItemKind::Toggle, "hudOutline", nullptr, 0, 1, 1});
		cats.push_back(std::move(cat));
	}
	{
		MenuCategory cat {"Timer"};
		AddElementItems(cat.items, LayoutElement::Timer);
		AddColorItem(cat.items, "Pro Color", "mhudTimerProColor", MHUD_DEF_TIMER_PRO_COLOR);
		AddColorItem(cat.items, "TP Color", "mhudTimerTpColor", MHUD_DEF_TIMER_TP_COLOR);
		AddColorItem(cat.items, "Paused Color", "mhudTimerPausedColor", MHUD_DEF_TIMER_PAUSED_COLOR);
		AddColorItem(cat.items, "Stopped Color", "mhudTimerStoppedColor", MHUD_DEF_TIMER_STOPPED_COLOR);
		cats.push_back(std::move(cat));
	}
	{
		MenuCategory cat {"Speed"};
		AddElementItems(cat.items, LayoutElement::Speed);
		AddColorItem(cat.items, "Color", "mhudSpeedColor", MHUD_DEF_BASE_COLOR);
		AddColorItem(cat.items, "CJ Color", "mhudSpeedCjColor", MHUD_DEF_CJ_COLOR);
		cats.push_back(std::move(cat));
	}
	{
		MenuCategory cat {"Prespeed"};
		AddElementItems(cat.items, LayoutElement::Prespeed);
		AddColorItem(cat.items, "Color", "mhudPrespeedColor", MHUD_DEF_BASE_COLOR);
		AddColorItem(cat.items, "Perf Color", "mhudPrespeedPerfColor", MHUD_DEF_PERF_COLOR);
		AddColorItem(cat.items, "Jumpbug Color", "mhudPrespeedJumpbugColor", MHUD_DEF_JUMPBUG_COLOR);
		cats.push_back(std::move(cat));
	}
	{
		MenuCategory cat {"Keys"};
		AddElementItems(cat.items, LayoutElement::Keys);
		AddColorItem(cat.items, "Color", "mhudKeysColor", MHUD_DEF_BASE_COLOR);
		AddColorItem(cat.items, "Overlap Color", "mhudKeysOverlapColor", MHUD_DEF_KEYS_OVERLAP_COLOR);
		cats.push_back(std::move(cat));
	}
	{
		MenuCategory cat {"Checkpoint"};
		AddElementItems(cat.items, LayoutElement::Checkpoint);
		AddColorItem(cat.items, "Color", "mhudCheckpointColor", MHUD_DEF_BASE_COLOR);
		cats.push_back(std::move(cat));
	}
	{
		MenuCategory cat {"Crosshair"};
		cat.items.push_back({"Enabled", MenuItemKind::Toggle, "mhudCrosshair", nullptr, 0, 1, 0});
		cat.items.push_back({"Scale", MenuItemKind::Size, "mhudCrosshairScale", nullptr, 0, 500, 100, 0, nullptr, "%"});
		cats.push_back(std::move(cat));
	}

	return cats;
}

// Ленивая инициализация на первое обращение — ПОСЛЕ загрузки всех TU (в отличие от
// namespace-scope статики, которая рискнула бы порядком статической инициализации между
// этим файлом и entity.cpp/kz_hud.h, откуда берутся LAYOUT_ELEMENTS и MHUD_DEF_* цвета).
static_function const std::vector<MenuCategory> &MenuCategories()
{
	static_persist std::vector<MenuCategory> cats = BuildMenuCategories();
	return cats;
}

static_function const MenuItem *GetMenuItem(i32 category, i32 itemIndex)
{
	const std::vector<MenuCategory> &cats = MenuCategories();
	if (category < 0 || category >= (i32)cats.size())
	{
		return NULL;
	}
	const std::vector<MenuItem> &items = cats[category].items;
	if (itemIndex < 0 || itemIndex >= (i32)items.size())
	{
		return NULL;
	}
	return &items[itemIndex];
}

// === Мелкие таблицы: тип худа и шрифты (без апстримного полного list-попапа) ===============

static_function const char *HudTypeLabel(i32 type)
{
	switch (type)
	{
		case KZHUDService::HUD_TYPE_MHUD:
			return "MHUD";
		case KZHUDService::HUD_TYPE_PANORAMA:
			return "Panorama";
		case KZHUDService::HUD_TYPE_OFF:
			return "Off";
		default:
			return "Standard";
	}
}

// Тот же цикл, что у particle-меню (particles.cpp/HudTypeNext, static-приватный там,
// поэтому не переиспользуем символ, а держим свою копию — четыре строки switch).
static_function i32 NextHudType(i32 current)
{
	switch (current)
	{
		case KZHUDService::HUD_TYPE_MHUD:
			return KZHUDService::HUD_TYPE_PANORAMA;
		case KZHUDService::HUD_TYPE_PANORAMA:
			return KZHUDService::HUD_TYPE_STANDARD;
		case KZHUDService::HUD_TYPE_STANDARD:
			return KZHUDService::HUD_TYPE_OFF;
		default:
			return KZHUDService::HUD_TYPE_MHUD;
	}
}

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

static_function const char *GetItemTypeClass(MenuItemKind kind)
{
	switch (kind)
	{
		case MenuItemKind::Toggle:
			return "type-toggle";
		case MenuItemKind::HudType:
		case MenuItemKind::Font:
			return "type-choice";
		case MenuItemKind::Position:
			return "type-position";
		case MenuItemKind::Size:
		case MenuItemKind::Opacity:
			return "type-size";
		case MenuItemKind::Color:
			return "type-color";
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

// Симметрично GetMHUDColorPref (particles.cpp) — тот же формат упаковки (R2: своего
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
	return layout;
}

void KZHUDService::DestroyOwnedMenuLayout()
{
	if (CBaseEntity *ent = this->ownedMenuLayout.Get())
	{
		g_pKZUtils->RemoveEntity(ent);
	}
	this->ownedMenuLayout = nullptr;
	// Уничтожение сущности снимает захват ввода вместе с ней (движок возвращает управление,
	// когда capture держит только погашенная сущность) — но наш menuOpen-флаг живёт на
	// KZPlayer дольше сущности и обязан сброситься явно, иначе следующий Toggle() решит,
	// что меню открыто, и попробует его "закрыть" вместо открытия.
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
	layout->SetHasClass(panelId, className, on ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass);
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
	layout->SetDialogVariableString(panelId, var, value);
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
	const std::vector<MenuCategory> &cats = MenuCategories();
	for (i32 i = 0; i < KZ_MENU_CATS; i++)
	{
		const bool used = i < (i32)cats.size();
		if (used)
		{
			this->SetMenuVar(layout, CatLbl(i), CatVar(i), cats[i].label);
			this->SetMenuBoolClass(layout, CatPanel(i), "selected", this->menuApplied.catSelected[i], i == this->menuCategory);
		}
		this->SetMenuBoolClass(layout, CatPanel(i), "hidden", this->menuApplied.catHidden[i], !used);
	}
}

void KZHUDService::RenderMenuItems(CCSCustomHudLayout *layout)
{
	const std::vector<MenuCategory> &cats = MenuCategories();
	const std::vector<MenuItem> *items = (this->menuCategory >= 0 && this->menuCategory < (i32)cats.size()) ? &cats[this->menuCategory].items : NULL;
	const i32 count = items ? MIN((i32)items->size(), KZ_MENU_ITEMS) : 0;

	for (i32 i = 0; i < KZ_MENU_ITEMS; i++)
	{
		const bool used = i < count;
		if (used)
		{
			const MenuItem &it = (*items)[i];
			this->SetMenuVar(layout, ItemLbl(i), ItemLblVar(i), it.label);

			std::string value;
			const char *swatch = NULL;
			bool on = false;
			switch (it.kind)
			{
				case MenuItemKind::Toggle:
					on = this->player->optionService->GetPreferenceBool(it.prefKey, it.idef != 0);
					value = on ? "On" : "Off";
					break;
				case MenuItemKind::HudType:
					value = HudTypeLabel(this->GetHudType());
					break;
				case MenuItemKind::Font:
				{
					const char *slug = this->player->optionService->GetPreferenceStr(it.prefKey, LAYOUT_DEFAULT_FONT);
					value = panorama::GetFontDisplayName(slug, LAYOUT_DEFAULT_FONT);
					break;
				}
				case MenuItemKind::Position:
				{
					char buf[32];
					V_snprintf(buf, sizeof(buf), "%i%%, %i%%", GetIntPref(this->player, it.prefKey, it.idef, it.isFloat),
							   GetIntPref(this->player, it.yKey, it.iydef, it.isFloat));
					value = buf;
					break;
				}
				case MenuItemKind::Size:
				case MenuItemKind::Opacity:
				{
					char buf[24];
					V_snprintf(buf, sizeof(buf), "%i%s", GetIntPref(this->player, it.prefKey, it.idef, it.isFloat), it.unit ? it.unit : "");
					value = buf;
					break;
				}
				case MenuItemKind::Color:
				{
					const Color cur = this->GetMHUDColorPref(it.prefKey, *it.cdef);
					swatch = panorama::GetColorEntryBgClass(panorama::FindColorEntry(cur));
					break;
				}
			}

			this->SetMenuVar(layout, ItemVal(i), ItemValVar(i), value.c_str());
			this->SetMenuSwapClass(layout, ItemSw(i), this->menuApplied.itemSwatch[i], swatch);
			this->SetMenuSwapClass(layout, ItemPanel(i), this->menuApplied.itemType[i], GetItemTypeClass(it.kind));
			this->SetMenuBoolClass(layout, ItemPanel(i), "on", this->menuApplied.itemOn[i], on);
		}
		this->SetMenuBoolClass(layout, ItemPanel(i), "hidden", this->menuApplied.itemHidden[i], !used);
	}
}

void KZHUDService::RenderMenuColorPopup(CCSCustomHudLayout *layout)
{
	const MenuItem *it = GetMenuItem(this->menuCategory, this->menuPopupItem);
	i32 curIdx = -1;
	if (it && it->kind == MenuItemKind::Color)
	{
		curIdx = panorama::FindColorEntry(this->GetMHUDColorPref(it->prefKey, *it->cdef));
	}
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
	const MenuItem *it = GetMenuItem(this->menuCategory, this->menuPopupItem);
	if (!it)
	{
		return;
	}
	const bool vstep = it->kind == MenuItemKind::Position;
	if (this->menuApplied.stepVHidden != !vstep)
	{
		this->menuApplied.stepVHidden = !vstep;
		this->SetMenuClass(layout, "m_step_up", "hidden", !vstep);
		this->SetMenuClass(layout, "m_step_down", "hidden", !vstep);
	}

	char readout[32];
	if (vstep)
	{
		V_snprintf(readout, sizeof(readout), "%i%%, %i%%", GetIntPref(this->player, it->prefKey, it->idef, it->isFloat),
				   GetIntPref(this->player, it->yKey, it->iydef, it->isFloat));
	}
	else
	{
		V_snprintf(readout, sizeof(readout), "%i%s", GetIntPref(this->player, it->prefKey, it->idef, it->isFloat), it->unit ? it->unit : "");
	}
	this->SetMenuVar(layout, "step_readout", "step", readout);
	this->SetMenuVar(layout, "step_label", "steplabel", it->label);
}

// === Взаимодействие ==========================================================================

void KZHUDService::SelectMenuCategory(i32 index)
{
	const std::vector<MenuCategory> &cats = MenuCategories();
	if (index < 0 || index >= (i32)cats.size())
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
	const MenuItem *it = GetMenuItem(this->menuCategory, slot);
	if (!it)
	{
		return;
	}
	if (this->menuPopup != MenuPopup::None)
	{
		this->CloseMenuPopup();
	}

	auto *opts = this->player->optionService;
	switch (it->kind)
	{
		case MenuItemKind::Toggle:
		{
			const bool next = !opts->GetPreferenceBool(it->prefKey, it->idef != 0);
			opts->SetPreferenceBool(it->prefKey, next);
			// Требование задачи: без этого правка (hudTimer/hudOutline/mhudCrosshair/...) не
			// видна в панораме до перезахода — RefreshLayoutPrefs кэш всех этих ключей.
			this->RefreshLayoutPrefs();
			if (V_strcmp(it->prefKey, "hudOutline") == 0)
			{
				// hudOutline — ОБЩИЙ преф с particle-путём (particles.cpp/s_hudToggles): если
				// сейчас активен HUD_TYPE_MHUD, там outline — отдельный vpcf-ассет
				// (plain/glow), и без пересоздания частиц смена не подхватится до конца
				// карты/реконнекта — та же причина, по которой particle-меню зовёт
				// DestroyAllParticles() на этом же тумблере.
				this->DestroyAllParticles();
			}
			this->RenderMenu();
			break;
		}
		case MenuItemKind::HudType:
		{
			const i32 next = NextHudType(this->GetHudType());
			this->SetHudType(next);
			if (next == KZHUDService::HUD_TYPE_MHUD && !KZHUDService::IsMHUDAvailable())
			{
				this->player->languageService->PrintChat(true, false, "MHUD - Unavailable");
			}
			this->RenderMenu();
			break;
		}
		case MenuItemKind::Font:
		{
			const char *cur = opts->GetPreferenceStr(it->prefKey, LAYOUT_DEFAULT_FONT);
			opts->SetPreferenceStr(it->prefKey, NextFontSlug(cur));
			this->RefreshLayoutPrefs();
			this->RenderMenu();
			break;
		}
		case MenuItemKind::Position:
		case MenuItemKind::Size:
		case MenuItemKind::Opacity:
			this->OpenMenuPopup(MenuPopup::Step, slot);
			break;
		case MenuItemKind::Color:
			this->OpenMenuPopup(MenuPopup::Color, slot);
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
	const MenuItem *it = GetMenuItem(this->menuCategory, this->menuPopupItem);
	if (!it || it->kind != MenuItemKind::Color)
	{
		return;
	}
	const i32 idx = this->menuPopupPage * KZ_MENU_SWATCH + slot;
	if (idx < 0 || idx >= panorama::GetColorEntryCount())
	{
		return;
	}
	this->SetMHUDColorPref(it->prefKey, panorama::GetColorEntryValue(idx));
	this->RefreshLayoutPrefs();
	this->RenderMenu();
}

void KZHUDService::MenuStep(i32 axis, i32 delta)
{
	if (this->menuPopup != MenuPopup::Step)
	{
		return;
	}
	const MenuItem *it = GetMenuItem(this->menuCategory, this->menuPopupItem);
	if (!it)
	{
		return;
	}
	if (it->kind == MenuItemKind::Position)
	{
		const char *key = axis == 1 ? it->yKey : it->prefKey;
		const i32 def = axis == 1 ? it->iydef : it->idef;
		const i32 value = panorama::SnapToStep(GetIntPref(this->player, key, def, it->isFloat) + delta, it->lo, it->hi);
		SetIntPref(this->player, key, value, it->isFloat);
	}
	else
	{
		const i32 value = panorama::SnapToStep(GetIntPref(this->player, it->prefKey, it->idef, it->isFloat) + delta, it->lo, it->hi);
		SetIntPref(this->player, it->prefKey, value, it->isFloat);
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
