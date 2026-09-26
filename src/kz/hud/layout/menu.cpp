// Окно настроек !options на своей разметке cyber/options.xml (спека 2026-09-26-hud-editor-options
// §3.2/§4.4). Обход реестра (KZOptNode/KZOptItem, KZ::menu::Add*, kz/option/menu/model.h) — как
// и раньше: состав меню регистрируют hud/prefs/hud_prefs.cpp и локальные ветки, этот файл только
// обходит KZ::menu::GetTree() и рендерит. Отличия от прежнего рендера под cs2kz/menu.xml:
//   - Категории верхнего уровня — вкладки tab0..tab5 (без hiddenFromMenu, не больше 6).
//   - Подкатегории — не второй уровень, а заголовки секций sec{i} внутри вкладки: вкладка =
//     плоский список «секция + пункты», режется на страницы по KZ_MENU_ROWS (16) строк.
//   - Каждая строка row{i} — универсальная, вид контрола задаёт класс типа t-toggle|t-seg|t-step|
//     t-btn|t-color|t-font (Choice с <= 4 вариантами — сегменты, длиннее — список-попап).
//   - Position/Vector в окне больше не показываются: расположение элементов худа правит
//     редактор !hud (layout/editor.cpp), он живёт на ЭТОЙ ЖЕ сущности (второй корень edit_root).
//   - Попапы общие для окна и редактора: цвет (пресеты + сетка оттенок×яркость, color_ok
//     применяет), список (шрифты по трём семействам Stratum2/Noto/Arial, по 8 строк),
//     подтверждение (confirm_popup). Цель попапа — указатель на пункт реестра (menuPopupTarget),
//     поэтому редактор открывает те же попапы на пунктах скрытых узлов элементов.
//   - GetPreferenceColor/SetPreferenceColor в базе нет (R2): цвет читается GetMHUDColorPref и
//     пишется симметричным SetMHUDColorPref (ниже) — преф хранит упакованный int.
//   - Позиция/размер элемента читаются через Float, прозрачность и crosshairScale — через Int
//     (KZOptItem::storage выбирает аксессор, см. GetIntPref ниже).
//
// ВАЖНО про захват ввода: SetInputCaptureEnabled(slot, true) в OpenLayoutMenu()/OpenHudEditor()
// переводит игрока в режим курсора. Симметричное false — ЯВНО, ПЕРЕД удалением сущности, во всех
// путях закрытия: CloseLayoutMenu() (закрывает и редактор — CloseHudEditor первым), CloseHudEditor()
// и DestroyOwnedMenuLayout() (дисконнект/выгрузка плагина). Не полагаемся на то, что удаление
// сущности само снимает захват (ревью: ничем не подтверждённое предположение, цена ошибки —
// игрок навсегда застрял в курсоре и сам этого не заметит).
// Путей снятия захвата ПЯТЬ: клик по m_close/ed_done/повторная команда (!options/!hud/!hm),
// смерть без цели наблюдения (kz_player.cpp), смена карты (OnRoundStart), дисконнект (Reset),
// выгрузка плагина/килл-свитч (LayoutCleanup); шестым, сеткой безопасности, идёт инвариант
// CheckMenuCaptureInvariant() ниже (захват законен только при menuOpen || editorOpen). Уход в
// спектейт другого игрока путём закрытия НЕ является (спека 2026-09-09-hud-share §6): окно в
// спектейте работает, захват держится осознанно, пока оно открыто.
//
// Проводка клика от движка: Hook_ClientSvcUserMessage на своём KZ_UM_CUSTOM_HUD_CLICKED
// (utils/hooks.cpp) резолвит игрока по СЛОТУ хука и зовёт player->hudService->OnLayoutMenuClick —
// тот сверяет присланный handle с ownedMenuLayout ИМЕННО этого hudService, так что клик одного
// игрока физически не может применить настройку другому.
//
// Бюджет интернирования (HUD_LAYOUT_MAX_INTERNED_STRINGS = 1024 на каждую таблицу сущности):
// классы/переменные на ячейки сетки g{c}_{r} НЕ ставятся никогда (2304 кнопки кликаются только
// по id), подсчёт литералов — scripts/count_interned.py; сущность с разросшимися таблицами
// пересоздаётся на следующем открытии (RecycleMenuLayoutIfFull).
#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/menu.h"
#include "kz/hud/layout/panorama_tables.h"
#include "kz/option/kz_option.h"
#include "kz/option/menu/model.h"
#include "kz/language/kz_language.h"
#include "kz/mode/kz_mode.h" // GetModeShortName — подпись режима в сайдбаре окна
#include "kz/spec/kz_spec.h" // GetSpectatedPlayer — цель мимикрии (подпись в окне) и reason инварианта
#include "sdk/entity/ccscustomhudlayout.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "utils/logging.h"
#include "cs2kz.h"

#include <cstdlib>
#include <utility>
#include <vector>

#include "tier0/memdbgon.h"

const char *const KZ_MENU_ROW_TYPE_NONE = "t-none";

// Вкладка, на которой окно показывает карточку редактора edit_open (§3.2).
#define KZ_MENU_HUD_CATEGORY "HUD - Menu Cat Hud"

KZHUDService::MenuAppliedState::MenuAppliedState()
{
	for (i32 i = 0; i < KZ_MENU_TABS; i++)
	{
		tabHidden[i] = true;
	}
	for (i32 i = 0; i < KZ_MENU_ROWS; i++)
	{
		secHidden[i] = true;
		rowHidden[i] = true;
		rowType[i] = KZ_MENU_ROW_TYPE_NONE;
	}
}

// === Поиск по реестру =========================================================================

template<typename Fn>
static_function bool WalkMenuNodes(const std::vector<KZOptNode *> &nodes, Fn &&fn)
{
	for (KZOptNode *node : nodes)
	{
		if (fn(node) || WalkMenuNodes(node->subs, fn))
		{
			return true;
		}
	}
	return false;
}

KZOptNode *KZMenuFindNodeByPref(const char *prefKey)
{
	KZOptNode *found = NULL;
	if (!prefKey)
	{
		return NULL;
	}
	WalkMenuNodes(KZ::menu::GetTree(),
				  [&](KZOptNode *node)
				  {
					  for (const KZOptItem &it : node->items)
					  {
						  if (it.prefKey && V_strcmp(it.prefKey, prefKey) == 0)
						  {
							  found = node;
							  return true;
						  }
					  }
					  return false;
				  });
	return found;
}

const KZOptItem *KZMenuFindItemByPref(const char *prefKey)
{
	KZOptNode *node = KZMenuFindNodeByPref(prefKey);
	if (!node)
	{
		return NULL;
	}
	for (const KZOptItem &it : node->items)
	{
		if (it.prefKey && V_strcmp(it.prefKey, prefKey) == 0)
		{
			return &it;
		}
	}
	return NULL;
}

const KZOptItem *KZMenuFindItemByPhrase(const char *phraseKey)
{
	const KZOptItem *found = NULL;
	WalkMenuNodes(KZ::menu::GetTree(),
				  [&](KZOptNode *node)
				  {
					  for (const KZOptItem &it : node->items)
					  {
						  if (it.phraseKey && V_strcmp(it.phraseKey, phraseKey) == 0)
						  {
							  found = &it;
							  return true;
						  }
					  }
					  return false;
				  });
	return found;
}

// === Вкладки и строки =========================================================================

// Вкладки — категории верхнего уровня без hiddenFromMenu, по порядку регистрации (cs2kz.cpp).
static_function i32 BuildMenuTabs(const KZOptNode *(&tabs)[KZ_MENU_TABS])
{
	i32 n = 0;
	for (const KZOptNode *node : KZ::menu::GetTree())
	{
		if (node->hiddenFromMenu)
		{
			continue;
		}
		if (n == KZ_MENU_TABS)
		{
			// Вкладок в разметке шесть — седьмая категория просто не видна. Отказ обязан быть
			// видимым (канон проекта); состав фиксируется на Load, спама не будет.
			KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_menu_tabs_truncated reason=slot_limit category=%s limit=%i\n",
						node->phraseKey ? node->phraseKey : "?", KZ_MENU_TABS);
			break;
		}
		tabs[n++] = node;
	}
	return n;
}

// Одна строка экрана: заголовок секции (подкатегория) или пункт.
struct MenuRowBinding
{
	const KZOptItem *item {};
	const KZOptNode *node {};
	bool isSection {};
};

// Пункт виден в окне, если его тип окно умеет рисовать: Position/Vector ушли в редактор (§3.2).
static_function bool IsMenuRowItem(const KZOptItem &it)
{
	return it.type != KZOptItemType::Position && it.type != KZOptItemType::Vector;
}

static_function void BuildMenuRows(const KZOptNode *cat, std::vector<MenuRowBinding> &rows)
{
	rows.clear();
	if (!cat)
	{
		return;
	}
	auto addItems = [&rows](const KZOptNode *node)
	{
		for (const KZOptItem &it : node->items)
		{
			if (IsMenuRowItem(it))
			{
				rows.push_back({&it, node, false});
			}
		}
	};
	addItems(cat);
	for (const KZOptNode *sub : cat->subs)
	{
		if (sub->hiddenFromMenu)
		{
			continue;
		}
		rows.push_back({NULL, sub, true});
		addItems(sub);
	}
}

static_function const KZOptNode *GetMenuTabNode(i32 tab)
{
	const KZOptNode *tabs[KZ_MENU_TABS] {};
	const i32 count = BuildMenuTabs(tabs);
	return (tab >= 0 && tab < count) ? tabs[tab] : NULL;
}

static_function bool IsHudTab(const KZOptNode *tab)
{
	return tab && tab->phraseKey && V_strcmp(tab->phraseKey, KZ_MENU_HUD_CATEGORY) == 0;
}

// Страницы — по ВЫСОТЕ тела окна, а не по числу строк (options.css): тело 560px, строка 52px,
// заголовок секции 37px, карточка редактора ~100px с отступами (только первая страница вкладки
// «Худ»). Заголовок секции страницу не заканчивает — уезжает на следующую вместе с первым
// пунктом. KZ_MENU_ROWS (16 слотов разметки) — только верхняя граница.
#define KZ_MENU_BODY_PX    560
#define KZ_MENU_ROW_PX     52
#define KZ_MENU_SECTION_PX 37
#define KZ_MENU_HERO_PX    100

struct MenuPage
{
	i32 first {};
	i32 count {};
};

static_function void BuildMenuPages(const std::vector<MenuRowBinding> &rows, bool heroOnFirst, std::vector<MenuPage> &pages)
{
	pages.clear();
	const i32 n = (i32)rows.size();
	i32 i = 0;
	do
	{
		MenuPage page {i, 0};
		i32 budget = KZ_MENU_BODY_PX - ((heroOnFirst && pages.empty()) ? KZ_MENU_HERO_PX : 0);
		while (i < n && page.count < KZ_MENU_ROWS)
		{
			const bool section = rows[i].isSection;
			i32 need = section ? KZ_MENU_SECTION_PX : KZ_MENU_ROW_PX;
			// Секция без своего первого пункта на этой странице — висячий заголовок.
			if (section && i + 1 < n && !rows[i + 1].isSection)
			{
				need += KZ_MENU_ROW_PX;
			}
			if (need > budget || (section && page.count + 1 >= KZ_MENU_ROWS))
			{
				break;
			}
			budget -= section ? KZ_MENU_SECTION_PX : KZ_MENU_ROW_PX;
			page.count++;
			i++;
		}
		if (page.count == 0 && i < n)
		{
			// Строка выше бюджета целиком (не бывает при текущих размерах) — берём её одну,
			// иначе цикл не сдвинулся бы.
			page.count = 1;
			i++;
		}
		pages.push_back(page);
	} while (i < n);
}

// Строка экрана i страницы page; NULL — строки нет.
static_function const MenuRowBinding *GetMenuRow(const std::vector<MenuRowBinding> &rows, const std::vector<MenuPage> &pages, i32 page, i32 i)
{
	if (page < 0 || page >= (i32)pages.size() || i < 0 || i >= pages[page].count)
	{
		return NULL;
	}
	return &rows[pages[page].first + i];
}

// Строки и страницы вкладки — один расчёт для рендера и клика.
static_function void BuildMenuTab(i32 tab, std::vector<MenuRowBinding> &rows, std::vector<MenuPage> &pages)
{
	const KZOptNode *node = GetMenuTabNode(tab);
	BuildMenuRows(node, rows);
	BuildMenuPages(rows, IsHudTab(node), pages);
}

// Разбор id вида <prefix><a>[_<b>][suffix]: "tab3", "sg2_1", "st4_dec" (suffix "_dec"). Остаток
// после разбора обязан совпасть целиком — "list_close" не станет строкой li по префиксу "li".
static_function bool ParseIndexed(const char *id, const char *prefix, i32 &a, i32 &b, const char *suffix = NULL)
{
	const size_t n = V_strlen(prefix);
	if (V_strncmp(id, prefix, n) != 0 || !V_isdigit(id[n]))
	{
		return false;
	}
	char *end = NULL;
	a = (i32)strtol(id + n, &end, 10);
	b = -1;
	if (suffix)
	{
		return V_strcmp(end, suffix) == 0;
	}
	if (*end == '_' && V_isdigit(end[1]))
	{
		b = (i32)strtol(end + 1, &end, 10);
	}
	return *end == '\0';
}

// === Шрифты: три семейства попапа (Stratum2 / Noto / Arial) ===================================
// Спека §3.3: в игре работают только Stratum2, Noto и Arial — остальные семейства таблицы
// (Lato*, Quicksand*, Trebuchet*) молча падают в Arial и в попапе не предлагаются. Сохранённый
// слаг такого семейства по-прежнему показывается в строке пункта своим именем.
static_function i32 FontGroupOf(i32 fontIndex)
{
	const char *family = panorama::GetFontFamilyAt(fontIndex);
	if (V_strncmp(family, "Stratum2", 8) == 0 || V_strncmp(family, "ForceStratum2", 13) == 0)
	{
		return 0;
	}
	if (V_strncmp(family, "Noto", 4) == 0)
	{
		return 1;
	}
	if (V_strncmp(family, "Arial", 5) == 0)
	{
		return 2;
	}
	return -1;
}

static_function const char *FontItemFallback(const KZOptItem &it)
{
	return it.sdef ? it.sdef : LAYOUT_DEFAULT_FONT;
}

// Строки попапа списка. Font — начертания выбранного семейства (id = индекс в таблице шрифтов),
// Choice — getChoices. Один источник для рендера, листания и клика.
static_function void BuildListChoices(KZPlayer *player, const KZOptItem &it, i32 family, std::vector<KZChoice> &choices, i64 &current)
{
	choices.clear();
	current = -1;
	if (it.type == KZOptItemType::Font)
	{
		const char *fallback = FontItemFallback(it);
		const char *slug = panorama::ResolveFontSlug(player->optionService->GetPreferenceStr(it.prefKey, fallback), fallback);
		for (i32 i = 0; i < panorama::GetFontCount(); i++)
		{
			if (V_strcmp(slug, panorama::GetFontSlugAt(i)) == 0)
			{
				current = i;
			}
			if (FontGroupOf(i) == family)
			{
				choices.push_back({panorama::GetFontDisplayName(panorama::GetFontSlugAt(i), fallback), (i64)i});
			}
		}
		return;
	}
	if (it.getChoices)
	{
		it.getChoices(player, it.tag, choices);
	}
	current = it.getCurrent ? it.getCurrent(player, it.tag) : (choices.empty() ? 0 : choices[0].id);
}

// Значение Choice-пункта: текущий id ищем в списке getChoices(); промах — отмеченные строки
// (мультивыбор «Стили»: getCurrent всегда -1), ничего не отмечено — пусто.
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

// === Float/Int-развязка хранения (R2/prefs.cpp) ===============================================

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

// scale (model.h): "the preference stores value / scale" — на экране Size всегда целое, а преф
// при scale>1 хранит дробь этого целого (recordVolume/jsVolume).
static_function i32 GetScaledDisplay(KZPlayer *player, const char *key, i32 displayDefault, i32 scale, bool isFloat)
{
	if (scale <= 1)
	{
		return GetIntPref(player, key, displayDefault, isFloat);
	}
	const f64 stored = player->optionService->GetPreferenceFloat(key, (f64)displayDefault / (f64)scale);
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

// panorama::SnapToStep снапит к кратным 5 вне ±100 — шаг ±1 там стал бы no-op, подменяем на ±5.
static_function i32 StepDelta(i32 current, i32 delta)
{
	if ((current > 100 || current < -100) && (delta == 1 || delta == -1))
	{
		return delta > 0 ? 5 : -5;
	}
	return delta;
}

// enabledBy: клики игнорируются, пока хотя бы один из гейт-префов выключен. Дефолт отсутствующего
// гейт-префа — true (гейтующие тумблеры заведены с дефолтом true).
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

static_function bool GetToggleValue(KZPlayer *player, const KZOptItem &it)
{
	// getCurrent обязателен для AddActionToggle: состояние такого пункта держит сервис, prefKey
	// у части из них вообще NULL.
	return it.getCurrent ? it.getCurrent(player, it.tag) != 0 : player->optionService->GetPreferenceBool(it.prefKey, it.idef != 0);
}

// Симметрично GetMHUDColorPref (kz_hud.cpp) — тот же формат упаковки (R2).
static_function i64 PackColorForPref(const Color &c)
{
	return ((i64)c.r() << 24) | ((i64)c.g() << 16) | ((i64)c.b() << 8) | (i64)c.a();
}

void KZHUDService::SetMHUDColorPref(const char *name, const Color &color)
{
	this->player->optionService->SetPreferenceInt(name, PackColorForPref(color));
}

// Индекс палитры текущего цвета пункта; -1 (битый градиент) → запись дефолта пункта.
static_function i32 GetItemColorEntry(const Color &current, const KZOptItem &it)
{
	const i32 entry = panorama::FindColorEntry(current);
	return entry >= 0 ? entry : MAX(0, panorama::FindColorEntry(it.cdef));
}

// === Id панелей (своя static-буфер функция на каждый паттерн, как у апстрима) ================

#define SLOT_ID(fn, fmt) \
	static_function const char *fn(i32 i) \
	{ \
		static_persist char buf[24]; \
		V_snprintf(buf, sizeof(buf), fmt, i); \
		return buf; \
	}

SLOT_ID(TabPanel, "tab%i")
SLOT_ID(TabVar, "t%i")
SLOT_ID(SecPanel, "sec%i")
SLOT_ID(SecVar, "s%i")
SLOT_ID(RowPanel, "row%i")
SLOT_ID(LblPanel, "lbl%i")
SLOT_ID(LblVar, "l%i")
SLOT_ID(SubPanel, "sub%i")
SLOT_ID(SubVar, "d%i")
SLOT_ID(TgPanel, "tg%i")
SLOT_ID(StValPanel, "st%i_val")
SLOT_ID(StValVar, "v%i")
SLOT_ID(BtPanel, "bt%i")
SLOT_ID(BtVar, "b%i")
SLOT_ID(ClPanel, "cl%i")
SLOT_ID(FnPanel, "fn%i")
SLOT_ID(FnVar, "f%i")
SLOT_ID(CpPanel, "cp%i")
SLOT_ID(ChPanel, "ch%i")
SLOT_ID(CvPanel, "cv%i")
SLOT_ID(LiPanel, "li%i")
SLOT_ID(LiVar, "li%i")
SLOT_ID(LfPanel, "lf%i")
#undef SLOT_ID

static_function const char *SegPanel(i32 row, i32 k)
{
	static_persist char buf[24];
	V_snprintf(buf, sizeof(buf), "sg%i_%i", row, k);
	return buf;
}

static_function const char *SegVar(i32 row, i32 k)
{
	static_persist char buf[24];
	V_snprintf(buf, sizeof(buf), "g%i_%i", row, k);
	return buf;
}

// === Сущность меню (своя, отдельная от this->ownedLayout) ====================================

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
	// Диф-кэши — состояние ПРЕДЫДУЩЕЙ сущности (смена карты её не переживает): без сброса в
	// момент реального создания первое открытие после смены карты решило бы, что всё уже
	// выставлено, и не отправило бы ничего — пустая рамка до реконнекта.
	this->menuApplied = MenuAppliedState();
	this->menuVars.clear();
	for (i32 i = 0; i < (i32)LayoutElement::Count; i++)
	{
		this->editorElements[i] = LayoutElementState();
	}
	this->editorKeys = LayoutKeysState();
	this->editorExtra = LayoutExtraState();
	return layout;
}

void KZHUDService::RecycleMenuLayoutIfFull()
{
	if (this->menuOpen || this->editorOpen)
	{
		return;
	}
	CBaseEntity *ent = this->ownedMenuLayout.Get();
	if (!ent)
	{
		return;
	}
	CCSCustomHudLayout *layout = (CCSCustomHudLayout *)ent;
	const i32 panels = layout->m_vecPanelIds().Count();
	const i32 classes = layout->m_vecClassNames().Count();
	const i32 vars = layout->m_vecDialogVariableNames().Count();
	if (panels < KZ_MENU_INTERN_RECYCLE && classes < KZ_MENU_INTERN_RECYCLE && vars < KZ_MENU_INTERN_RECYCLE)
	{
		return;
	}
	KZ_LOG_INFO(LogChannel::General, "[cyb] panorama_menu_recycled reason=intern_near_limit panels=%i classes=%i vars=%i slot=%i\n", panels, classes,
				vars, this->player->GetPlayerSlot().Get());
	this->DestroyOwnedMenuLayout();
}

void KZHUDService::DestroyOwnedMenuLayout()
{
	if (CBaseEntity *ent = this->ownedMenuLayout.Get())
	{
		// Снимаем захват ЯВНО, до удаления сущности — не полагаемся на побочный эффект
		// RemoveEntity (см. шапку файла).
		((CCSCustomHudLayout *)ent)->SetInputCaptureEnabled(this->player->GetPlayerSlot(), false);
		g_pKZUtils->RemoveEntity(ent);
	}
	this->ownedMenuLayout = nullptr;
	// Флаги режимов живут на KZPlayer дольше сущности и обязаны сброситься явно, иначе следующая
	// команда решит, что окно/редактор открыты, и попробует их «закрыть» вместо открытия.
	// onEdit попапа здесь сознательно не зовём — досинкивать кэш уходящему игроку некому.
	this->menuOpen = false;
	this->editorOpen = false;
	this->editorSelected = -1;
	this->menuPopup = MenuPopup::None;
	this->menuPopupTarget = NULL;
	this->menuConfirmTarget = NULL;
	this->menuApplied = MenuAppliedState();
	this->menuVars.clear();
	for (i32 i = 0; i < (i32)LayoutElement::Count; i++)
	{
		this->editorElements[i] = LayoutElementState();
	}
	this->editorKeys = LayoutKeysState();
	this->editorExtra = LayoutExtraState();
}

// === Запись классов/переменных — диф-кэш =======================================================

void KZHUDService::SetMenuClass(CCSCustomHudLayout *layout, const char *panelId, const char *className, bool on)
{
	// SetHasClass возвращает false, когда сущность упёрлась в HUD_LAYOUT_MAX_INTERNED_STRINGS —
	// дальше окно молча перестаёт обновляться. Отказ обязан быть видимым.
	if (!layout->SetHasClass(panelId, className, on ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_menu_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n", panelId, className,
					this->player->GetPlayerSlot().Get());
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

// Мимикрия РЕАЛЬНО действует: на экране худ наблюдаемого, а окно читает и пишет только свои
// настройки — правка не даёт видимого эффекта, и об этом надо сказать.
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

bool KZHUDService::IsDuplicateMenuAction(i32 key)
{
	const i32 tick = g_pKZUtils->GetServerGlobals()->tickcount;
	if (this->menuToggleKey == key && this->menuToggleTick == tick)
	{
		return true;
	}
	this->menuToggleKey = key;
	this->menuToggleTick = tick;
	return false;
}

// === Рендер ===================================================================================

void KZHUDService::RenderLayoutUi()
{
	if (this->editorOpen)
	{
		this->RenderEditor();
	}
	else if (this->menuOpen)
	{
		this->RenderMenu();
	}
}

void KZHUDService::RenderMenu()
{
	bool created = false;
	CCSCustomHudLayout *layout = this->EnsureMenuLayout(created);
	if (!layout)
	{
		return;
	}
	this->SetMenuBoolClass(layout, "opt_root", "hidden", this->menuApplied.optHidden, !this->menuOpen);
	if (!this->menuOpen)
	{
		return;
	}
	this->SetMenuBoolClass(layout, "edit_root", "hidden", this->menuApplied.editHidden, true);

	const char *lang = this->player->languageService->GetLanguage();
	const KZOptNode *tab = GetMenuTabNode(this->menuCategory);
	// Шапка: имя вкладки и подзаголовок; мимикрия — хвостом подзаголовка (полное объяснение
	// уходит в чат при открытии, OpenLayoutMenu).
	const std::string title = KZLanguageService::PrepareMessageWithLang(lang, tab && tab->phraseKey ? tab->phraseKey : "HUD - Menu Title");
	this->SetMenuVar(layout, "opt_root", "title", title.c_str());
	std::string sub = KZLanguageService::PrepareMessageWithLang(lang, "Options - Subtitle");
	if (IsMenuMimicActive(this->player))
	{
		sub += " ";
		sub += KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Mimic Suffix");
	}
	this->SetMenuVar(layout, "opt_root", "sub", sub.c_str());
	this->SetMenuVar(layout, "opt_root", "brand_sub", KZLanguageService::PrepareMessageWithLang(lang, "Options - Brand Sub").c_str());
	this->SetMenuVar(layout, "opt_root", "who", this->player->GetName());
	this->SetMenuVar(layout, "opt_root", "mode", this->player->modeService ? this->player->modeService->GetModeShortName() : "");
	this->SetMenuVar(layout, "opt_root", "hint", KZLanguageService::PrepareMessageWithLang(lang, "Options - Hint").c_str());

	// Карточка редактора — только на вкладке «Худ» (§3.2), на её первой странице (бюджет высоты
	// страниц считает BuildMenuPages с тем же условием).
	std::vector<MenuRowBinding> rows;
	std::vector<MenuPage> pages;
	BuildMenuTab(this->menuCategory, rows, pages);
	this->menuPage = Clamp(this->menuPage, 0, (i32)pages.size() - 1);
	const bool heroShown = IsHudTab(tab) && this->menuPage == 0;
	if (heroShown)
	{
		this->SetMenuVar(layout, "edit_open", "hero_t", KZLanguageService::PrepareMessageWithLang(lang, "Options - Hero Title").c_str());
		this->SetMenuVar(layout, "edit_open", "hero_s", KZLanguageService::PrepareMessageWithLang(lang, "Options - Hero Sub").c_str());
		this->SetMenuVar(layout, "edit_open", "hero_b", KZLanguageService::PrepareMessageWithLang(lang, "Options - Hero Button").c_str());
	}
	this->SetMenuBoolClass(layout, "edit_open", "hidden", this->menuApplied.editOpenHidden, !heroShown);

	this->RenderMenuTabs(layout);
	this->RenderMenuRows(layout);
	this->RenderMenuPopups(layout);

	// Движковый баг (sdk/entity/ccscustomhudlayout.h, BUGS п.2): смена класса панели не доезжает
	// до стилей её ДЕТЕЙ (.row.t-toggle .tg, .tg.on .tg-knob, .el.sel .tag …) без полного
	// пересчёта слоя. Пометка не заводит ни строку, ни класс — лимита интернирования не касается.
	layout->GetGlobalLayoutState()->MarkFullChanged();
}

void KZHUDService::RenderMenuTabs(CCSCustomHudLayout *layout)
{
	const KZOptNode *tabs[KZ_MENU_TABS] {};
	const i32 count = BuildMenuTabs(tabs);
	const char *lang = this->player->languageService->GetLanguage();
	for (i32 i = 0; i < KZ_MENU_TABS; i++)
	{
		const bool used = i < count;
		if (used)
		{
			const std::string label = KZLanguageService::PrepareMessageWithLang(lang, tabs[i]->phraseKey);
			this->SetMenuVar(layout, TabPanel(i), TabVar(i), label.c_str());
			this->SetMenuBoolClass(layout, TabPanel(i), "on", this->menuApplied.tabOn[i], i == this->menuCategory);
		}
		this->SetMenuBoolClass(layout, TabPanel(i), "hidden", this->menuApplied.tabHidden[i], !used);
	}
}

// Тип строки по пункту: Choice — сегменты при <= KZ_MENU_SEGS вариантах, иначе строка-значение
// со списком-попапом (тот же вид, что у шрифта).
static_function const char *GetRowTypeClass(const KZOptItem &it, i32 choiceCount)
{
	switch (it.type)
	{
		case KZOptItemType::Toggle:
			return "t-toggle";
		case KZOptItemType::Choice:
			return choiceCount <= KZ_MENU_SEGS ? "t-seg" : "t-font";
		case KZOptItemType::Size:
			return "t-step";
		case KZOptItemType::Button:
			return "t-btn";
		case KZOptItemType::Color:
			return "t-color";
		case KZOptItemType::Font:
			return "t-font";
		default:
			return KZ_MENU_ROW_TYPE_NONE;
	}
}

void KZHUDService::RenderMenuRows(CCSCustomHudLayout *layout)
{
	std::vector<MenuRowBinding> rows;
	std::vector<MenuPage> pageTable;
	BuildMenuTab(this->menuCategory, rows, pageTable);
	const i32 pages = (i32)pageTable.size();
	this->menuPage = Clamp(this->menuPage, 0, pages - 1);
	const char *lang = this->player->languageService->GetLanguage();

	for (i32 i = 0; i < KZ_MENU_ROWS; i++)
	{
		const MenuRowBinding *row = GetMenuRow(rows, pageTable, this->menuPage, i);
		const bool isSection = row && row->isSection;
		const bool isItem = row && !row->isSection;
		if (isSection)
		{
			const std::string label = KZLanguageService::PrepareMessageWithLang(lang, row->node->phraseKey);
			this->SetMenuVar(layout, SecPanel(i), SecVar(i), label.c_str());
		}
		if (isItem)
		{
			const KZOptItem &it = *row->item;
			this->SetMenuVar(layout, LblPanel(i), LblVar(i), KZLanguageService::PrepareMessageWithLang(lang, it.phraseKey).c_str());
			const std::string subText = it.subKey ? KZLanguageService::PrepareMessageWithLang(lang, it.subKey) : "";
			this->SetMenuVar(layout, SubPanel(i), SubVar(i), subText.c_str());

			std::vector<KZChoice> choices;
			i64 current = -1;
			if (it.type == KZOptItemType::Choice)
			{
				BuildListChoices(this->player, it, 0, choices, current);
			}
			const char *rowType = GetRowTypeClass(it, (i32)choices.size());
			const char *swatch = NULL;
			bool on = false;
			switch (it.type)
			{
				case KZOptItemType::Toggle:
					on = GetToggleValue(this->player, it);
					break;
				case KZOptItemType::Choice:
					if ((i32)choices.size() <= KZ_MENU_SEGS)
					{
						for (i32 k = 0; k < KZ_MENU_SEGS; k++)
						{
							const bool used = k < (i32)choices.size();
							if (used)
							{
								this->SetMenuVar(layout, SegPanel(i, k), SegVar(i, k), choices[k].label.c_str());
								this->SetMenuBoolClass(layout, SegPanel(i, k), "on", this->menuApplied.segOn[i][k],
													   choices[k].selected || choices[k].id == current);
							}
							this->SetMenuBoolClass(layout, SegPanel(i, k), "hidden", this->menuApplied.segHidden[i][k], !used);
						}
					}
					else
					{
						this->SetMenuVar(layout, FnPanel(i), FnVar(i), GetChoiceValueLabel(this->player, it).c_str());
					}
					break;
				case KZOptItemType::Font:
				{
					const char *slug = this->player->optionService->GetPreferenceStr(it.prefKey, FontItemFallback(it));
					this->SetMenuVar(layout, FnPanel(i), FnVar(i), panorama::GetFontDisplayName(slug, FontItemFallback(it)));
					break;
				}
				case KZOptItemType::Size:
				{
					char buf[24];
					V_snprintf(buf, sizeof(buf), "%i%s", GetScaledDisplay(this->player, it.prefKey, it.idef, it.scale, it.storage == KZOptStorage::Float),
							   it.unit ? it.unit : "");
					this->SetMenuVar(layout, StValPanel(i), StValVar(i), buf);
					break;
				}
				case KZOptItemType::Button:
				{
					const bool isResetAll = it.phraseKey && V_strcmp(it.phraseKey, KZ_MENU_RESET_ALL_PHRASE) == 0;
					this->SetMenuVar(layout, BtPanel(i), BtVar(i),
									 KZLanguageService::PrepareMessageWithLang(lang, isResetAll ? "Options - Button Reset" : "Options - Button Go").c_str());
					break;
				}
				case KZOptItemType::Color:
					swatch = panorama::GetColorEntryBgClass(GetItemColorEntry(this->GetMHUDColorPref(it.prefKey, it.cdef), it));
					break;
				default:
					break;
			}
			this->SetMenuSwapClass(layout, RowPanel(i), this->menuApplied.rowType[i], rowType);
			this->SetMenuBoolClass(layout, TgPanel(i), "on", this->menuApplied.tgOn[i], on);
			this->SetMenuSwapClass(layout, ClPanel(i), this->menuApplied.clBg[i], swatch);
		}
		this->SetMenuBoolClass(layout, SecPanel(i), "hidden", this->menuApplied.secHidden[i], !isSection);
		this->SetMenuBoolClass(layout, RowPanel(i), "hidden", this->menuApplied.rowHidden[i], !isItem);
	}

	char page[16];
	V_snprintf(page, sizeof(page), "%i / %i", this->menuPage + 1, pages);
	this->SetMenuVar(layout, "opt_root", "pg", page);
	this->SetMenuBoolClass(layout, "pg_prev", "hidden", this->menuApplied.pgPrevHidden, pages <= 1);
	this->SetMenuBoolClass(layout, "pg_next", "hidden", this->menuApplied.pgNextHidden, pages <= 1);
}

void KZHUDService::RenderMenuPopups(CCSCustomHudLayout *layout)
{
	this->SetMenuBoolClass(layout, "color_popup", "hidden", this->menuApplied.colorPopupHidden, this->menuPopup != MenuPopup::Color);
	this->SetMenuBoolClass(layout, "list_popup", "hidden", this->menuApplied.listPopupHidden, this->menuPopup != MenuPopup::List);
	this->SetMenuBoolClass(layout, "confirm_popup", "hidden", this->menuApplied.confirmPopupHidden, this->menuPopup != MenuPopup::Confirm);
	switch (this->menuPopup)
	{
		case MenuPopup::Color:
			this->RenderMenuColorPopup(layout);
			break;
		case MenuPopup::List:
			this->RenderMenuListPopup(layout);
			break;
		case MenuPopup::Confirm:
			this->RenderMenuConfirmPopup(layout);
			break;
		default:
			break;
	}
}

void KZHUDService::RenderMenuColorPopup(CCSCustomHudLayout *layout)
{
	const KZOptItem *it = this->menuPopupTarget;
	if (!it || it->type != KZOptItemType::Color)
	{
		return;
	}
	const char *lang = this->player->languageService->GetLanguage();
	this->SetMenuVar(layout, "color_popup", "cp_title", KZLanguageService::PrepareMessageWithLang(lang, it->phraseKey).c_str());
	this->SetMenuVar(layout, "color_popup", "cp_reset", KZLanguageService::PrepareMessageWithLang(lang, "Options - Color Reset").c_str());

	const i32 pending = this->menuColorPending >= 0 ? this->menuColorPending : GetItemColorEntry(this->GetMHUDColorPref(it->prefKey, it->cdef), *it);
	for (i32 i = 0; i < KZ_MENU_PRESETS; i++)
	{
		const i32 entry = panorama::GetPresetEntry(i);
		this->SetMenuSwapClass(layout, CpPanel(i), this->menuApplied.cpBg[i], panorama::GetColorEntryBgClass(entry));
		this->SetMenuBoolClass(layout, CpPanel(i), "on", this->menuApplied.cpOn[i], entry == pending);
	}
	// Оттенок — средняя яркость строки сетки (cv3); яркости — по выбранному оттенку (или первому).
	const i32 hue = Clamp(this->menuColorHue, 0, KZ_MENU_HUES - 1);
	for (i32 h = 0; h < KZ_MENU_HUES; h++)
	{
		this->SetMenuSwapClass(layout, ChPanel(h), this->menuApplied.chBg[h], panorama::GetColorEntryBgClass(panorama::GetHueLumEntry(h, 3)));
		this->SetMenuBoolClass(layout, ChPanel(h), "on", this->menuApplied.chOn[h], h == this->menuColorHue);
	}
	for (i32 l = 0; l < KZ_MENU_LUMS; l++)
	{
		const i32 entry = panorama::GetHueLumEntry(hue, l);
		this->SetMenuSwapClass(layout, CvPanel(l), this->menuApplied.cvBg[l], panorama::GetColorEntryBgClass(entry));
		this->SetMenuBoolClass(layout, CvPanel(l), "on", this->menuApplied.cvOn[l], this->menuColorHue >= 0 && entry == pending);
	}
	this->SetMenuSwapClass(layout, "color_cur", this->menuApplied.colorCur, panorama::GetColorEntryBgClass(pending));
	char hex[24];
	if (pending < panorama::GetSolidColorCount())
	{
		const Color c = panorama::GetColorEntryValue(pending);
		V_snprintf(hex, sizeof(hex), "#%02X%02X%02X", c.r(), c.g(), c.b());
	}
	else
	{
		V_snprintf(hex, sizeof(hex), "GRAD %i", pending - panorama::GetSolidColorCount());
	}
	this->SetMenuVar(layout, "color_popup", "chex", hex);
}

// Срез страницы попапа списка — ОДИН расчёт для рендера, листания и клика.
static_function void GetListPopupSlice(i32 total, i32 &page, i32 &first, i32 &count, i32 &pages)
{
	pages = MAX(1, (total + KZ_MENU_LIST_ROWS - 1) / KZ_MENU_LIST_ROWS);
	page = Clamp(page, 0, pages - 1);
	first = page * KZ_MENU_LIST_ROWS;
	count = Clamp(total - first, 0, KZ_MENU_LIST_ROWS);
}

void KZHUDService::RenderMenuListPopup(CCSCustomHudLayout *layout)
{
	const KZOptItem *it = this->menuPopupTarget;
	if (!it)
	{
		return;
	}
	const bool isFont = it->type == KZOptItemType::Font;
	std::vector<KZChoice> choices;
	i64 current = -1;
	BuildListChoices(this->player, *it, this->menuListFamily, choices, current);
	i32 first = 0;
	i32 count = 0;
	i32 pages = 1;
	GetListPopupSlice((i32)choices.size(), this->menuPopupPage, first, count, pages);
	for (i32 i = 0; i < KZ_MENU_LIST_ROWS; i++)
	{
		const bool used = i < count;
		if (used)
		{
			const KZChoice &c = choices[first + i];
			this->SetMenuVar(layout, LiPanel(i), LiVar(i), c.label.c_str());
			this->SetMenuBoolClass(layout, LiPanel(i), "on", this->menuApplied.liOn[i], c.selected || c.id == current);
			// Строка шрифта нарисована СВОИМ начертанием: выбирать шрифт по одному названию — гадание.
			this->SetMenuSwapClass(layout, LiPanel(i), this->menuApplied.liFont[i], isFont ? panorama::GetFontClassAt((i32)c.id) : NULL);
		}
		this->SetMenuBoolClass(layout, LiPanel(i), "hidden", this->menuApplied.liHidden[i], !used);
	}
	for (i32 f = 0; f < KZ_MENU_FAMILIES; f++)
	{
		this->SetMenuBoolClass(layout, LfPanel(f), "hidden", this->menuApplied.lfHidden[f], !isFont);
		this->SetMenuBoolClass(layout, LfPanel(f), "on", this->menuApplied.lfOn[f], isFont && f == this->menuListFamily);
	}
	const char *lang = this->player->languageService->GetLanguage();
	this->SetMenuVar(layout, "list_popup", "lp_title", KZLanguageService::PrepareMessageWithLang(lang, it->phraseKey).c_str());
	char page[16];
	V_snprintf(page, sizeof(page), "%i / %i", this->menuPopupPage + 1, pages);
	this->SetMenuVar(layout, "list_popup", "lpg", page);
}

void KZHUDService::RenderMenuConfirmPopup(CCSCustomHudLayout *layout)
{
	const char *lang = this->player->languageService->GetLanguage();
	this->SetMenuVar(layout, "confirm_popup", "ct", KZLanguageService::PrepareMessageWithLang(lang, "Confirm - ResetAll Title").c_str());
	this->SetMenuVar(layout, "confirm_popup", "cb", KZLanguageService::PrepareMessageWithLang(lang, "Confirm - ResetAll Body").c_str());
	this->SetMenuVar(layout, "confirm_popup", "c_no", KZLanguageService::PrepareMessageWithLang(lang, "Confirm - ResetAll No").c_str());
	this->SetMenuVar(layout, "confirm_popup", "c_yes", KZLanguageService::PrepareMessageWithLang(lang, "Confirm - ResetAll Yes").c_str());
}

// === Взаимодействие ============================================================================

void KZHUDService::SelectMenuTab(i32 tab)
{
	if (!GetMenuTabNode(tab))
	{
		return;
	}
	if (this->menuPopup != MenuPopup::None)
	{
		this->CloseMenuPopup();
	}
	this->menuCategory = tab;
	this->menuPage = 0;
	this->RenderMenu();
}

// control: 0 — сама строка/тумблер/кнопка/свотч/значение (главное действие по типу),
// 1 — сегмент arg, 2 — степпер на arg (±1).
void KZHUDService::MenuRowAction(i32 rowIndex, i32 control, i32 arg)
{
	std::vector<MenuRowBinding> rows;
	std::vector<MenuPage> pages;
	BuildMenuTab(this->menuCategory, rows, pages);
	const MenuRowBinding *row = GetMenuRow(rows, pages, this->menuPage, rowIndex);
	if (!row || row->isSection)
	{
		return;
	}
	const KZOptItem &it = *row->item;
	// enabledBy: клик мимо (клиент мог прислать клик по устаревшему кадру разметки).
	if (!IsMenuItemEnabled(this->player, it))
	{
		return;
	}
	// Вложенные кнопки (tg{i} внутри row{i}) могли прийти обе — второе действие по строке в том же
	// тике гасим, иначе тумблер щёлкнул бы дважды и остался на месте.
	if (this->IsDuplicateMenuAction(1000 + rowIndex))
	{
		return;
	}
	if (this->menuPopup != MenuPopup::None)
	{
		this->CloseMenuPopup();
	}
	auto *opts = this->player->optionService;
	switch (it.type)
	{
		case KZOptItemType::Toggle:
			if (control != 0)
			{
				return;
			}
			// AddActionToggle: щёлкает сервис, сырая запись префа запрещена (prefKey может быть NULL).
			if (it.onActivate)
			{
				it.onActivate(this->player, it.tag);
			}
			else
			{
				opts->SetPreferenceBool(it.prefKey, !opts->GetPreferenceBool(it.prefKey, it.idef != 0));
			}
			break;
		case KZOptItemType::Choice:
		{
			std::vector<KZChoice> choices;
			i64 current = -1;
			BuildListChoices(this->player, it, 0, choices, current);
			if ((i32)choices.size() > KZ_MENU_SEGS)
			{
				if (control == 0)
				{
					this->OpenMenuPopup(MenuPopup::List, &it);
				}
				return;
			}
			if (control != 1 || arg < 0 || arg >= (i32)choices.size() || !it.onPick)
			{
				return;
			}
			it.onPick(this->player, it.tag, choices[arg].id);
			break;
		}
		case KZOptItemType::Font:
			if (control == 0)
			{
				this->OpenMenuPopup(MenuPopup::List, &it);
			}
			return;
		case KZOptItemType::Color:
			if (control == 0)
			{
				this->OpenMenuPopup(MenuPopup::Color, &it);
			}
			return;
		case KZOptItemType::Size:
		{
			if (control != 2)
			{
				return;
			}
			const bool isFloat = it.storage == KZOptStorage::Float;
			const i32 cur = GetScaledDisplay(this->player, it.prefKey, it.idef, it.scale, isFloat);
			SetScaledDisplay(this->player, it.prefKey, panorama::SnapToStep(cur + StepDelta(cur, arg), it.lo, it.hi), it.scale, isFloat);
			break;
		}
		case KZOptItemType::Button:
			if (control != 0)
			{
				return;
			}
			if (it.phraseKey && V_strcmp(it.phraseKey, KZ_MENU_RESET_ALL_PHRASE) == 0)
			{
				this->OpenMenuConfirm(&it);
				return;
			}
			if (it.onActivate)
			{
				it.onActivate(this->player, it.tag);
			}
			break;
		default:
			return;
	}
	// Любая запись префа видна в panorama только после RefreshLayoutPrefs (кэш префов худа).
	this->RefreshLayoutPrefs();
	this->RenderLayoutUi();
}

void KZHUDService::OpenMenuPopup(MenuPopup kind, const KZOptItem *item)
{
	if (!item)
	{
		return;
	}
	if (this->menuPopup != MenuPopup::None)
	{
		this->CloseMenuPopup();
	}
	this->menuPopup = kind;
	this->menuPopupTarget = item;
	this->menuPopupPage = 0;
	this->menuColorPending = -1;
	this->menuColorHue = -1;
	if (kind == MenuPopup::List && item->type == KZOptItemType::Font)
	{
		// Список шрифтов открывается на семействе и странице ТЕКУЩЕГО значения.
		std::vector<KZChoice> all;
		i64 current = -1;
		BuildListChoices(this->player, *item, 0, all, current);
		const i32 group = current >= 0 ? FontGroupOf((i32)current) : -1;
		this->menuListFamily = group >= 0 ? group : 0;
		std::vector<KZChoice> rows;
		BuildListChoices(this->player, *item, this->menuListFamily, rows, current);
		for (i32 i = 0; i < (i32)rows.size(); i++)
		{
			if (rows[i].id == current)
			{
				this->menuPopupPage = i / KZ_MENU_LIST_ROWS;
			}
		}
	}
	else if (kind == MenuPopup::List)
	{
		std::vector<KZChoice> rows;
		i64 current = -1;
		BuildListChoices(this->player, *item, 0, rows, current);
		for (i32 i = 0; i < (i32)rows.size(); i++)
		{
			if (rows[i].id == current || rows[i].selected)
			{
				this->menuPopupPage = i / KZ_MENU_LIST_ROWS;
				break;
			}
		}
	}
	// onEdit(begin=true/false) — контракт модели: пункт узнаёт, что его правят.
	if (item->onEdit)
	{
		item->onEdit(this->player, item->tag, true);
	}
	this->RenderLayoutUi();
}

// Все явные пути закрытия попапа (кнопка *_close, клик по другой строке, смена вкладки, закрытие
// окна/редактора) идут сюда; DestroyOwnedMenuLayout колбэк сознательно не зовёт.
void KZHUDService::CloseMenuPopup()
{
	const KZOptItem *it = this->menuPopupTarget;
	const bool hadPopup = this->menuPopup != MenuPopup::None && this->menuPopup != MenuPopup::Confirm;
	this->menuPopup = MenuPopup::None;
	this->menuPopupTarget = NULL;
	this->menuConfirmTarget = NULL;
	this->menuColorPending = -1;
	this->menuColorHue = -1;
	if (hadPopup && it && it->onEdit)
	{
		it->onEdit(this->player, it->tag, false);
	}
	this->RenderLayoutUi();
}

void KZHUDService::MenuPopupPageStep(i32 delta)
{
	if (this->menuPopup != MenuPopup::List || !this->menuPopupTarget)
	{
		return;
	}
	std::vector<KZChoice> choices;
	i64 current = -1;
	BuildListChoices(this->player, *this->menuPopupTarget, this->menuListFamily, choices, current);
	i32 first = 0;
	i32 count = 0;
	i32 pages = 1;
	i32 page = this->menuPopupPage + delta;
	GetListPopupSlice((i32)choices.size(), page, first, count, pages);
	this->menuPopupPage = page;
	this->RenderLayoutUi();
}

void KZHUDService::MenuListPick(i32 slot)
{
	const KZOptItem *it = this->menuPopupTarget;
	if (this->menuPopup != MenuPopup::List || !it || (it->type != KZOptItemType::Choice && it->type != KZOptItemType::Font))
	{
		return;
	}
	std::vector<KZChoice> choices;
	i64 current = -1;
	BuildListChoices(this->player, *it, this->menuListFamily, choices, current);
	i32 first = 0;
	i32 count = 0;
	i32 pages = 1;
	GetListPopupSlice((i32)choices.size(), this->menuPopupPage, first, count, pages);
	if (slot < 0 || slot >= count)
	{
		return;
	}
	const KZChoice &c = choices[first + slot];
	if (it->type == KZOptItemType::Font)
	{
		// id = индекс в таблице шрифтов (BuildListChoices); пишем слаг напрямую — у Font-пункта нет onPick.
		this->player->optionService->SetPreferenceStr(it->prefKey, panorama::GetFontSlugAt((i32)c.id));
	}
	else if (it->onPick)
	{
		it->onPick(this->player, it->tag, c.id);
	}
	this->RefreshLayoutPrefs();
	this->RenderLayoutUi();
}

void KZHUDService::MenuColorApply()
{
	const KZOptItem *it = this->menuPopupTarget;
	if (this->menuPopup != MenuPopup::Color || !it || it->type != KZOptItemType::Color)
	{
		return;
	}
	if (this->menuColorPending >= 0)
	{
		this->SetMHUDColorPref(it->prefKey, panorama::GetColorEntryValue(this->menuColorPending));
		this->RefreshLayoutPrefs();
	}
	this->CloseMenuPopup();
}

void KZHUDService::OpenMenuConfirm(const KZOptItem *item)
{
	if (!item)
	{
		return;
	}
	if (this->menuPopup != MenuPopup::None)
	{
		this->CloseMenuPopup();
	}
	this->menuPopup = MenuPopup::Confirm;
	this->menuConfirmTarget = item;
	this->RenderLayoutUi();
}

// Клики попапов — общие для окна и редактора. true — клик принадлежал попапу.
bool KZHUDService::HandleMenuPopupClick(const char *id)
{
	i32 a = -1;
	i32 b = -1;
	if (!V_strcmp(id, "color_close") || !V_strcmp(id, "list_close") || !V_strcmp(id, "confirm_no"))
	{
		this->CloseMenuPopup();
		return true;
	}
	if (!V_strcmp(id, "confirm_yes"))
	{
		const KZOptItem *it = this->menuConfirmTarget;
		if (this->menuPopup == MenuPopup::Confirm && it && it->onActivate)
		{
			it->onActivate(this->player, it->tag);
			this->RefreshLayoutPrefs();
		}
		this->CloseMenuPopup();
		return true;
	}
	if (!V_strcmp(id, "color_ok"))
	{
		this->MenuColorApply();
		return true;
	}
	if (!V_strcmp(id, "color_reset"))
	{
		// Сброс ставит дефолт пункта в «выбранный» — применяет, как и любой выбор, color_ok.
		if (this->menuPopup == MenuPopup::Color && this->menuPopupTarget)
		{
			this->menuColorPending = MAX(0, panorama::FindColorEntry(this->menuPopupTarget->cdef));
			this->RenderLayoutUi();
		}
		return true;
	}
	if (!V_strcmp(id, "lp_prev") || !V_strcmp(id, "lp_next"))
	{
		this->MenuPopupPageStep(id[3] == 'p' ? -1 : 1);
		return true;
	}
	if (this->menuPopup == MenuPopup::Color)
	{
		if (ParseIndexed(id, "cp", a, b) && b < 0 && a < KZ_MENU_PRESETS)
		{
			this->menuColorPending = panorama::GetPresetEntry(a);
			this->menuColorHue = -1;
			this->RenderLayoutUi();
			return true;
		}
		if (ParseIndexed(id, "ch", a, b) && b < 0 && a < KZ_MENU_HUES)
		{
			this->menuColorHue = a;
			this->menuColorPending = panorama::GetHueLumEntry(a, 3);
			this->RenderLayoutUi();
			return true;
		}
		if (ParseIndexed(id, "cv", a, b) && b < 0 && a < KZ_MENU_LUMS)
		{
			this->menuColorHue = MAX(this->menuColorHue, 0);
			this->menuColorPending = panorama::GetHueLumEntry(this->menuColorHue, a);
			this->RenderLayoutUi();
			return true;
		}
	}
	if (this->menuPopup == MenuPopup::List)
	{
		if (ParseIndexed(id, "li", a, b) && b < 0)
		{
			this->MenuListPick(a);
			return true;
		}
		if (ParseIndexed(id, "lf", a, b) && b < 0 && a < KZ_MENU_FAMILIES)
		{
			if (this->menuPopupTarget && this->menuPopupTarget->type == KZOptItemType::Font)
			{
				this->menuListFamily = a;
				this->menuPopupPage = 0;
				this->RenderLayoutUi();
			}
			return true;
		}
	}
	return false;
}

bool KZHUDService::HandleMenuWindowClick(const char *id)
{
	i32 a = -1;
	i32 b = -1;
	if (!V_strcmp(id, "m_close"))
	{
		this->CloseLayoutMenu();
		return true;
	}
	if (!V_strcmp(id, "edit_open"))
	{
		this->OpenHudEditor();
		return true;
	}
	if (!V_strcmp(id, "pg_prev") || !V_strcmp(id, "pg_next"))
	{
		if (this->menuPopup != MenuPopup::None)
		{
			this->CloseMenuPopup();
		}
		this->menuPage += id[3] == 'p' ? -1 : 1; // рендер клампит страницу
		this->RenderMenu();
		return true;
	}
	if (ParseIndexed(id, "tab", a, b) && b < 0)
	{
		this->SelectMenuTab(a);
		return true;
	}
	if (ParseIndexed(id, "st", a, b, "_dec"))
	{
		this->MenuRowAction(a, 2, -1);
		return true;
	}
	if (ParseIndexed(id, "st", a, b, "_inc"))
	{
		this->MenuRowAction(a, 2, 1);
		return true;
	}
	if (ParseIndexed(id, "sg", a, b) && b >= 0)
	{
		this->MenuRowAction(a, 1, b);
		return true;
	}
	if ((ParseIndexed(id, "row", a, b) || ParseIndexed(id, "tg", a, b) || ParseIndexed(id, "bt", a, b) || ParseIndexed(id, "cl", a, b)
		 || ParseIndexed(id, "fn", a, b))
		&& b < 0)
	{
		this->MenuRowAction(a, 0, 0);
		return true;
	}
	return false;
}

// === Клик и захват ввода =======================================================================

void KZHUDService::OnLayoutMenuClick(uint32 packedHandle, const char *panelId)
{
	if (!this->menuOpen && !this->editorOpen)
	{
		return;
	}
	CCSCustomHudLayout *layout = CCSCustomHudLayout::FromClickHandle(packedHandle);
	// Сверка с this->ownedMenuLayout — не чужая/устаревшая сущность (пересоздание сменило бы
	// хэндл, а клиент мог прислать клик уже в пути).
	if (!layout || (CBaseEntity *)layout != this->ownedMenuLayout.Get() || !panelId)
	{
		return;
	}
	if (this->editorOpen)
	{
		// Редактор первым (у него свой m_close/ed_done), попапы — общие.
		if (!this->HandleEditorClick(panelId))
		{
			this->HandleMenuPopupClick(panelId);
		}
		return;
	}
	if (!this->HandleMenuPopupClick(panelId))
	{
		this->HandleMenuWindowClick(panelId);
	}
}

// Вкладка по ключу категории (или подкатегории — тогда страница, на которой стоит её секция).
// Ключ не найден/NULL — первая вкладка.
static_function void FindMenuTab(const char *categoryKey, i32 &tab, i32 &page)
{
	tab = 0;
	page = 0;
	if (!categoryKey)
	{
		return;
	}
	const KZOptNode *tabs[KZ_MENU_TABS] {};
	const i32 count = BuildMenuTabs(tabs);
	for (i32 i = 0; i < count; i++)
	{
		if (tabs[i]->phraseKey && V_strcmp(tabs[i]->phraseKey, categoryKey) == 0)
		{
			tab = i;
			return;
		}
		std::vector<MenuRowBinding> rows;
		std::vector<MenuPage> pages;
		BuildMenuTab(i, rows, pages);
		for (i32 p = 0; p < (i32)pages.size(); p++)
		{
			for (i32 r = pages[p].first; r < pages[p].first + pages[p].count; r++)
			{
				if (rows[r].isSection && rows[r].node->phraseKey && V_strcmp(rows[r].node->phraseKey, categoryKey) == 0)
				{
					tab = i;
					page = p;
					return;
				}
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
	// Одна сущность — один режим: открытое окно закрывает редактор (§4.5).
	if (this->editorOpen)
	{
		this->CloseHudEditor();
	}
	this->RecycleMenuLayoutIfFull();
	bool created = false;
	CCSCustomHudLayout *layout = this->EnsureMenuLayout(created);
	if (!layout)
	{
		// Отказ игроку — фразой, reason различает килл-свитч и прочую недоступность MHUD.
		KZ_LOG_WARN(LogChannel::General, "[cyb] hud_menu_open_denied reason=%s slot=%i\n",
					KZHUDService::IsHudLayoutKillSwitchOff() ? "hud_layout_disabled" : "mhud_unavailable", this->player->GetPlayerSlot().Get());
		this->player->languageService->PrintChat(true, false, "MHUD - Unavailable");
		return;
	}
	// Без per-player состояния SetInputCaptureEnabled тихо ничего не делает — игрок получил бы
	// нарисованное, но некликабельное окно. Отказ обязан быть виден.
	if (!layout->GetPlayerLayoutState(this->player->GetPlayerSlot()))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] hud_menu_open_denied reason=no_player_layout_state slot=%i\n", this->player->GetPlayerSlot().Get());
		this->player->languageService->PrintChat(true, false, "MHUD - Unavailable");
		return;
	}
	this->menuOpen = true;
	FindMenuTab(categoryKey, this->menuCategory, this->menuPage);
	this->menuPopup = MenuPopup::None;
	this->menuPopupTarget = NULL;
	this->menuConfirmTarget = NULL;
	// Переводит игрока в режим курсора — симметричное false обязано случиться на КАЖДОМ пути
	// закрытия (см. шапку файла).
	layout->SetInputCaptureEnabled(this->player->GetPlayerSlot(), true);
	if (IsMenuMimicActive(this->player))
	{
		this->player->languageService->PrintChat(true, false, "HUD - Menu Mimic Note");
	}
	this->RenderMenu();
}

void KZHUDService::CloseLayoutMenu()
{
	// Редактор живёт на той же сущности и под тем же захватом — все пути закрытия окна (смерть,
	// смена карты, команда) обязаны гасить и его.
	if (this->editorOpen)
	{
		this->CloseHudEditor();
	}
	if (!this->menuOpen)
	{
		return;
	}
	// onEdit закрытия обязан прийти и на этом пути (правка Vector-пункта осела бы только в БД).
	const KZOptItem *it = this->menuPopupTarget;
	if (this->menuPopup != MenuPopup::None && this->menuPopup != MenuPopup::Confirm && it && it->onEdit)
	{
		it->onEdit(this->player, it->tag, false);
	}
	this->menuOpen = false;
	this->menuPopup = MenuPopup::None;
	this->menuPopupTarget = NULL;
	this->menuConfirmTarget = NULL;
	// Сущность могла быть уже погашена ДО закрытия (DestroyOwnedMenuLayout/смена карты) — тогда
	// снимать нечего, это не отказ.
	if (CBaseEntity *ent = this->ownedMenuLayout.Get())
	{
		CCSCustomHudLayout *layout = (CCSCustomHudLayout *)ent;
		this->SetMenuBoolClass(layout, "opt_root", "hidden", this->menuApplied.optHidden, true);
		this->SetMenuBoolClass(layout, "color_popup", "hidden", this->menuApplied.colorPopupHidden, true);
		this->SetMenuBoolClass(layout, "list_popup", "hidden", this->menuApplied.listPopupHidden, true);
		this->SetMenuBoolClass(layout, "confirm_popup", "hidden", this->menuApplied.confirmPopupHidden, true);
		// Самая важная строка файла: без неё игрок остаётся в режиме курсора навсегда — движок
		// возвращает управление только когда ВСЕ layout-сущности с capture его сняли.
		layout->SetInputCaptureEnabled(this->player->GetPlayerSlot(), false);
	}
}

// Инвариант (спека 2026-09-09-hud-share §6): НЕ БЫВАЕТ включённого захвата ввода, когда ни окно,
// ни редактор не открыты. Нарушение — error с машинно-читаемым reason и принудительное снятие.
void KZHUDService::CheckMenuCaptureInvariant()
{
	if (this->menuOpen || this->editorOpen)
	{
		return;
	}
	const f64 now = g_pKZUtils->GetServerGlobals()->curtime;
	// curtime отсчитывается от загрузки карты: переживший смену карты дедлайн окажется «в
	// будущем» — дедлайн дальше одного окна считаем просроченным.
	if (now < this->menuCaptureCheckTime && this->menuCaptureCheckTime - now <= KZ_MENU_CAPTURE_CHECK_INTERVAL)
	{
		return;
	}
	this->menuCaptureCheckTime = now + KZ_MENU_CAPTURE_CHECK_INTERVAL;
	CBaseEntity *ent = this->ownedMenuLayout.Get();
	if (!ent)
	{
		return;
	}
	CCSCustomHudLayout *layout = (CCSCustomHudLayout *)ent;
	const CPlayerSlot slot = this->player->GetPlayerSlot();
	if (!layout->IsInputCaptureEnabled(slot))
	{
		return;
	}
	KZ_LOG_ERROR(LogChannel::General, "[cyb] hud_menu_capture_leak reason=capture_without_open_menu slot=%i alive=%s spectating=%s\n", slot.Get(),
				 this->player->IsAlive() ? "true" : "false", this->player->specService->GetSpectatedPlayer() ? "true" : "false");
	layout->SetInputCaptureEnabled(slot, false);
}

// !hudmenu/!hm — с 26.09 открывает редактор худа (§4.5): настройки вида элементов уехали туда.
SCMD(kz_hudmenu, SCFL_HUD | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	if (player->hudService->IsHudEditorOpen())
	{
		player->hudService->CloseHudEditor();
	}
	else
	{
		player->hudService->OpenHudEditor();
	}
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_hm, kz_hudmenu);

// === Оформление самого меню: регистрация пунктов ==============================================
// Четыре ключа (menuFont/menuColor/menuSounds/menuPopupShift) остаются в реестре — их переносит
// обмен худом и белый список префов, — но у окна на cyber/options.xml своё оформление по дизайну,
// и ни один из них на него не действует. Показывать в окне пункты без эффекта хуже, чем не
// показывать, поэтому категория скрыта (hiddenFromMenu).
void KZMenuChromeMenu_Register()
{
	KZOptNode *cat = KZ::menu::AddCategory("HUD - Menu Cat MenuChrome");
	cat->hiddenFromMenu = true;
	KZ::menu::AddFont(cat, "HUD - Menu Label MenuFont", "menuFont", "stratum2-medium-tf");
	KZ::menu::AddColor(cat, "HUD - Menu Label MenuColor", "menuColor", Color(255, 255, 255, 255));
	KZ::menu::AddToggle(cat, "HUD - Menu Label MenuSounds", "menuSounds", true);
	KZ::menu::AddToggle(cat, "HUD - Menu Label MenuPopupShift", "menuPopupShift", true);
	KZ::menu::SetItemSubtext(cat, "HUD - Menu Label MenuPopupShift Sub");
}
