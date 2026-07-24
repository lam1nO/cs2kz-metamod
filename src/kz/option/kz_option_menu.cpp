// Интерактивное меню !options (cs2menus) — корень с подменю по категориям:
// чекпоинты/старт, HUD, видимость, звуки, джампстаты, paint. Подменю HUD и
// джампстатов строят их модули (CreateHUDMenu/CreateJumpstatsMenu) — там же
// живут их per-slot хэндлы; здесь только локальные подменю и корень.
// Навигация: пункт корня → подменю (AddSubMenu); назад — бинд R движка меню
// (возврат по parent), выход — бинд F. Пунктов «← Назад» больше нет (решение 23.07).
#include "kz/option/kz_option.h"
#include "kz/language/kz_language.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "kz/quiet/kz_quiet.h"
#include "kz/jumpstats/kz_jumpstats.h"
#include "kz/hud/kz_hud.h"
#include "kz/paint/kz_paint.h"
#include "kz/timer/kz_timer.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include "tier0/memdbgon.h"

// Меню-движок cs2menus (определён в cs2kz.cpp); может быть nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

// Локальные подменю этого файла (HUD/JS — в своих модулях).
enum OptSubmenu : u8
{
	OPTSUB_CHECKPOINT = 0,
	OPTSUB_VISIBILITY,
	OPTSUB_SOUND,
	OPTSUB_PAINT,
	OPTSUB_COUNT
};

struct OptionsMenuSlotHandles
{
	MenuHandle root;
	MenuHandle sub[OPTSUB_COUNT];
};

static_global OptionsMenuSlotHandles s_optMenus[MAXPLAYERS + 1] = {};

enum class OptItemKind : u8
{
	Toggle,     // bool-преф: on/off
	Action,     // действие без состояния (текст пункта не обновляется)
	Volume,     // float-преф: цикл по пресетам громкости
	PaintColor, // цикл по именованной палитре paint
	PaintSize,  // float-преф: регулируемая строка A/D (размер краски)
};

struct OptionsMenuItem
{
	OptItemKind kind;
	const char *labelKey;      // phrase-ключ подписи
	const char *tag;           // info-тег пункта; для Toggle/Volume — имя префа
	bool defaultValue;         // для Toggle
	f32 defaultFloat;          // для Volume
	void (*apply)(KZPlayer *); // кастомное применение; для Toggle nullptr = прямой тоггл префа
};

// Тоггл-функции сервисов: держат в синхроне кэш сервиса и/или шлют апдейты.
static void ApplySetStartPos(KZPlayer *p)
{
	p->checkpointService->SetStartPosition();
}

static void ApplyClearStartPos(KZPlayer *p)
{
	p->checkpointService->ClearStartPosition();
}

static void ApplyHidePlayers(KZPlayer *p)
{
	p->quietService->ToggleHide();
}

static void ApplyHideWeapon(KZPlayer *p)
{
	p->quietService->ToggleHideWeapon();
}

static void ApplyHideLegs(KZPlayer *p)
{
	p->ToggleHideLegs();
}

static void ApplyTimerStopSound(KZPlayer *p)
{
	p->timerService->ToggleTimerStopSound();
}

static void ApplyShowAllPaint(KZPlayer *p)
{
	p->paintService->ToggleShowAllPaint();
}

// clang-format off

// Чекпоинты и старт-позиция.
static const OptionsMenuItem s_cpItems[] = {
	{OptItemKind::Action, "Options - Menu Label SetStartPos",       "action:ssp",        false, 0.0f, &ApplySetStartPos  },
	{OptItemKind::Action, "Options - Menu Label ClearStartPos",     "action:csp",        false, 0.0f, &ApplyClearStartPos},
	{OptItemKind::Toggle, "Options - Menu Label CheckpointMessage", "checkpointMessage", true,  0.0f, nullptr            },
};

// Видимость.
static const OptionsMenuItem s_visItems[] = {
	{OptItemKind::Toggle, "Options - Menu Label HidePlayers", "hideOtherPlayers", false, 0.0f, &ApplyHidePlayers},
	{OptItemKind::Toggle, "Options - Menu Label HideWeapon",  "hideWeapon",       false, 0.0f, &ApplyHideWeapon },
	{OptItemKind::Toggle, "Options - Menu Label HideLegs",    "hideLegs",         true,  0.0f, &ApplyHideLegs   },
};

// Звуки (громкость/тир звуков джампстатов — в подменю джампстатов).
static const OptionsMenuItem s_sndItems[] = {
	{OptItemKind::Toggle, "Options - Menu Label CheckpointSound", "checkpointSound", true, 0.0f, nullptr              },
	{OptItemKind::Toggle, "Options - Menu Label TeleportSound",   "teleportSound",   true, 0.0f, nullptr              },
	{OptItemKind::Toggle, "Options - Menu Label TimerStopSound",  "timerStopSound",  true, 0.0f, &ApplyTimerStopSound },
	{OptItemKind::Volume, "Options - Menu Label RecordVolume",    "recordVolume",    false, 1.0f, nullptr             },
};

// Paint.
static const OptionsMenuItem s_paintItems[] = {
	{OptItemKind::Toggle,     "Options - Menu Label ShowAllPaint", "showAllPaint", false, 0.0f,                               &ApplyShowAllPaint},
	{OptItemKind::PaintColor, "Options - Menu Label PaintColor",   "paintColor",   false, 0.0f,                               nullptr           },
	{OptItemKind::PaintSize,  "Options - Menu Label PaintSize",    "paintSize",    false, KZPaintService::DEFAULT_PAINT_SIZE, nullptr           },
};

// clang-format on

// Регулируемая строка размера краски (A/D). Диапазон под команду !paintsize
// (SetSize требует value > 0; верхнего клэмпа у команды нет — в меню ограничиваем).
static_global constexpr f32 s_paintSizeStep = 1.0f;
static_global constexpr f32 s_paintSizeMin = 1.0f;
static_global constexpr f32 s_paintSizeMax = 50.0f;

// Пресеты громкости (как в меню джампстатов).
static_global constexpr f32 s_volumePresets[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};

// Палитра paint — тот же список, что понимает utils::ParseColorName.
static_global constexpr const char *s_paintColors[] = {"red", "white", "black", "blue", "brown", "green", "yellow", "purple"};

static_function const OptionsMenuItem *FindOptionsItem(const char *tag)
{
	for (const auto &it : s_cpItems)
	{
		if (KZ_STREQ(tag, it.tag))
		{
			return &it;
		}
	}
	for (const auto &it : s_visItems)
	{
		if (KZ_STREQ(tag, it.tag))
		{
			return &it;
		}
	}
	for (const auto &it : s_sndItems)
	{
		if (KZ_STREQ(tag, it.tag))
		{
			return &it;
		}
	}
	for (const auto &it : s_paintItems)
	{
		if (KZ_STREQ(tag, it.tag))
		{
			return &it;
		}
	}
	return nullptr;
}

// Текст пункта «<подпись>: <значение>» по текущему префу.
static_function std::string OptionsItemText(KZPlayer *p, const OptionsMenuItem &it, const char *lang)
{
	std::string label = KZLanguageService::PrepareMessageWithLang(lang, it.labelKey);
	char text[128];
	switch (it.kind)
	{
		case OptItemKind::Action:
			return label;
		case OptItemKind::Toggle:
		{
			bool on = p->optionService->GetPreferenceBool(it.tag, it.defaultValue);
			std::string state = KZLanguageService::PrepareMessageWithLang(lang, on ? "HUD - Menu On" : "HUD - Menu Off");
			// ВКЛ зелёным, ВЫКЛ красным: чат-цвет-байт перед значением (cs2menus ColorizeChat
			// переводит 0x04→зелёный / 0x07→красный в <font color>; текст пункта не эскейпит цвет-байты).
			const char *stateColor = on ? "\x04" : "\x07";
			V_snprintf(text, sizeof(text), "%s: %s%s", label.c_str(), stateColor, state.c_str());
			return std::string(text);
		}
		case OptItemKind::Volume:
		{
			f32 vol = (f32)p->optionService->GetPreferenceFloat(it.tag, it.defaultFloat);
			V_snprintf(text, sizeof(text), "%s: %.0f%%", label.c_str(), vol * 100.0f);
			return std::string(text);
		}
		case OptItemKind::PaintColor:
		{
			V_snprintf(text, sizeof(text), "%s: %s", label.c_str(), p->paintService->GetColorName());
			return std::string(text);
		}
		case OptItemKind::PaintSize:
		{
			V_snprintf(text, sizeof(text), "%s: %.1f", label.c_str(), p->paintService->GetSize());
			return std::string(text);
		}
	}
	return label;
}

// Колбэк локальных подменю (чекпоинты/видимость/звуки/paint).
static_function void OnOptionsSubmenuSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!p)
	{
		return;
	}
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (!tag || !tag[0])
	{
		return;
	}

	const OptionsMenuItem *it = FindOptionsItem(tag);
	if (!it)
	{
		return;
	}

	switch (it->kind)
	{
		case OptItemKind::Action:
			it->apply(p);
			return;
		case OptItemKind::Toggle:
			if (it->apply)
			{
				it->apply(p);
			}
			else
			{
				p->optionService->SetPreferenceBool(it->tag, !p->optionService->GetPreferenceBool(it->tag, it->defaultValue));
			}
			break;
		case OptItemKind::Volume:
		{
			f32 cur = (f32)p->optionService->GetPreferenceFloat(it->tag, it->defaultFloat);
			f32 next = s_volumePresets[0]; // за последним пресетом — снова первый
			for (f32 v : s_volumePresets)
			{
				if (v > cur + 0.001f)
				{
					next = v;
					break;
				}
			}
			p->optionService->SetPreferenceFloat(it->tag, next);
			break;
		}
		case OptItemKind::PaintColor:
		{
			// Следующий цвет палитры; "Custom" (выставлен через kz_paintcolor RGB) → первый.
			const char *current = p->paintService->GetColorName();
			i32 idx = -1;
			for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(s_paintColors); i++)
			{
				if (KZ_STREQI(current, s_paintColors[i]))
				{
					idx = i;
					break;
				}
			}
			p->paintService->SetColor(s_paintColors[(idx + 1) % KZ_ARRAYSIZE(s_paintColors)]);
			break;
		}
		case OptItemKind::PaintSize:
			// Регулируется только A/D (см. OnOptionsSubmenuAdjust); E — no-op (лишь перерисовка ниже).
			break;
	}
	g_pMenus->SetItemText(menu, item, OptionsItemText(p, *it, p->languageService->GetLanguage()).c_str());
}

// A/D по регулируемой строке размера краски: применить ±delta, клэмп по [min,max] строки,
// сохранить (SetSize пишет преф paintSize), обновить текст. Механику рисования не трогаем.
static_function void OnOptionsSubmenuAdjust(MenuHandle menu, int slot, int item, f32 delta, f32 minValue, f32 maxValue)
{
	KZPlayer *p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!p)
	{
		return;
	}
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (!tag || !tag[0])
	{
		return;
	}
	const OptionsMenuItem *it = FindOptionsItem(tag);
	if (!it || it->kind != OptItemKind::PaintSize)
	{
		return;
	}
	f32 next = p->paintService->GetSize() + delta;
	if (next < minValue)
	{
		next = minValue;
	}
	if (next > maxValue)
	{
		next = maxValue;
	}
	p->paintService->SetSize(next);
	g_pMenus->SetItemText(menu, item, OptionsItemText(p, *it, p->languageService->GetLanguage()).c_str());
}

// Собрать локальное подменю из таблицы пунктов. «Назад» — бинд R движка меню
// (parent от AddSubMenu), отдельный пункт больше не нужен (решение 23.07).
static_function MenuHandle BuildOptionsSubmenu(KZPlayer *player, const char *titleKey, const OptionsMenuItem *items, i32 count)
{
	const char *lang = player->languageService->GetLanguage();
	std::string title = KZLanguageService::PrepareMessageWithLang(lang, titleKey);
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), &OnOptionsSubmenuSelect);
	if (m == kInvalidMenuHandle)
	{
		return m;
	}
	for (i32 i = 0; i < count; i++)
	{
		std::string itemText = OptionsItemText(player, items[i], lang);
		if (items[i].kind == OptItemKind::PaintSize)
		{
			// Регулируемая строка A/D (движок значение не хранит — см. OnOptionsSubmenuAdjust).
			g_pMenus->AddAdjustableItem(m, itemText.c_str(), items[i].tag, s_paintSizeStep, s_paintSizeMin, s_paintSizeMax);
		}
		else
		{
			g_pMenus->AddItem(m, itemText.c_str(), items[i].tag, false);
		}
	}
	// Колбэк A/D нужен только подменю с регулируемыми строками (paint); на прочих A/D по
	// нерегулируемым строкам движок игнорирует — регистрировать безвредно для всех.
	g_pMenus->SetAdjustCallback(m, &OnOptionsSubmenuAdjust);
	// Не закрываем при выборе — тумблеры обновляют текст вживую.
	g_pMenus->SetCloseOnSelect(m, false);
	return m;
}

void KZ::option::OpenOptionsMenu(KZPlayer *player)
{
	if (g_pMenus == nullptr || !player)
	{
		return;
	}

	int slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return;
	}

	// Пересоздаём весь куст (корень + локальные подменю) при каждом открытии.
	OptionsMenuSlotHandles &handles = s_optMenus[slot];
	if (handles.root != kInvalidMenuHandle)
	{
		g_pMenus->DestroyMenu(handles.root);
		handles.root = kInvalidMenuHandle;
	}
	for (auto &h : handles.sub)
	{
		if (h != kInvalidMenuHandle)
		{
			g_pMenus->DestroyMenu(h);
			h = kInvalidMenuHandle;
		}
	}

	const char *lang = player->languageService->GetLanguage();
	std::string title = KZLanguageService::PrepareMessageWithLang(lang, "Options - Menu Title");
	// У корня нет собственных select-пунктов — только submenu-навигация, колбэк не нужен.
	MenuHandle root = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), nullptr);
	if (root == kInvalidMenuHandle)
	{
		return;
	}
	// Хэндл корня сохраняем ДО постройки детей: AddSubMenu ниже свяжет их parent с ним.
	handles.root = root;

	handles.sub[OPTSUB_CHECKPOINT] = BuildOptionsSubmenu(player, "Options - Menu Cat Checkpoint", s_cpItems, (i32)KZ_ARRAYSIZE(s_cpItems));
	handles.sub[OPTSUB_VISIBILITY] = BuildOptionsSubmenu(player, "Options - Menu Cat Visibility", s_visItems, (i32)KZ_ARRAYSIZE(s_visItems));
	handles.sub[OPTSUB_SOUND] = BuildOptionsSubmenu(player, "Options - Menu Cat Sound", s_sndItems, (i32)KZ_ARRAYSIZE(s_sndItems));
	handles.sub[OPTSUB_PAINT] = BuildOptionsSubmenu(player, "Options - Menu Cat Paint", s_paintItems, (i32)KZ_ARRAYSIZE(s_paintItems));
	// HUD/JS-подменю строят их модули (свои per-slot хэндлы, пункт «Назад» внутри).
	MenuHandle hudMenu = (MenuHandle)player->hudService->CreateHUDMenu();
	MenuHandle jsMenu = (MenuHandle)player->jumpstatsService->CreateJumpstatsMenu();

	// Порядок корня — по частоте использования.
	auto addCat = [&](const char *catKey, MenuHandle child)
	{
		if (child == kInvalidMenuHandle)
		{
			return;
		}
		std::string label = KZLanguageService::PrepareMessageWithLang(lang, catKey);
		g_pMenus->AddSubMenu(root, label.c_str(), child, "");
	};
	addCat("Options - Menu Cat Checkpoint", handles.sub[OPTSUB_CHECKPOINT]);
	addCat("Options - Menu Cat HUD", hudMenu);
	addCat("Options - Menu Cat Visibility", handles.sub[OPTSUB_VISIBILITY]);
	addCat("Options - Menu Cat Sound", handles.sub[OPTSUB_SOUND]);
	addCat("Options - Menu Cat Jumpstats", jsMenu);
	addCat("Options - Menu Cat Paint", handles.sub[OPTSUB_PAINT]);

	g_pMenus->DisplayMenu(root, slot, 0);
}

// Старт забега закрывает любое открытое cs2menus-меню игрока: NavSelect на серверах —
// E (см. gameops core.cfg), и оставленное открытым незакрывающееся меню превращало
// каждый E (кнопки/двери карты) в тычок по пункту меню посреди рана.
static_global class KZTimerServiceEventListener_OptionsMenu : public KZTimerServiceEventListener
{
	virtual void OnTimerStartPost(KZPlayer *player, u32 courseGUID) override
	{
		if (g_pMenus == nullptr || !player)
		{
			return;
		}
		int slot = player->GetPlayerSlot().Get();
		if (slot >= 0 && slot <= MAXPLAYERS && g_pMenus->HasMenu(slot))
		{
			g_pMenus->CancelMenu(slot);
		}
	}
} s_optionsMenuTimerListener;

void KZ::option::InitOptionsMenu()
{
	KZTimerService::RegisterEventListener(&s_optionsMenuTimerListener);
}

SCMD(kz_options, SCFL_PLAYER | SCFL_PREFERENCE | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	KZ::option::OpenOptionsMenu(player);
	return MRES_SUPERCEDE;
}
