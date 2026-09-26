// Регистрация меню настроек panorama-худа и крестика в общем реестре (kz/option/menu/model.h,
// Task 1). Раньше (Task 11) состав меню жил статическими таблицами прямо в layout/menu.cpp —
// задача 2 переносит РОВНО тот же состав (категории, пункты, ключи префов, диапазоны,
// дефолты — ничего не меняя) сюда, а рендер (layout/menu.cpp) обходит KZ::menu::GetTree().
#include "kz/hud/kz_hud.h"
#include "kz/hud/layout/layout.h"
#include "kz/hud/share/hud_share.h"
#include "kz/option/menu/model.h"
#include "kz/option/kz_option.h"
#include "kz/language/kz_language.h"

#include "tier0/memdbgon.h"

// === Тип худа: Standard / Panorama / Off ====================================================
// Не булев тумблер (три состояния) и не сырой преф — hudType читается/пишется через
// GetHudType()/SetHudType() (kz_hud.cpp), которые сами тащат миграцию легаси-значений
// (mhudMaster, удалённый particle-тип 1). Моделируем как Choice: список из трёх пунктов в
// ТОМ ЖЕ порядке, что и старый цикл NextHudType; сам цикл убран — с задачи 3 клик открывает
// попап списка (layout/menu.cpp), как у любого другого Choice, а не перебирает по кругу.
// Значения переведены через ключи (Task 15, тот же паттерн, что kBeamTypeKeys в misc_prefs.cpp) —
// до этого висели литералом (см. отчёт задачи 9, раздел "осталось непереведённым").

static_function void GetHudTypeChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
{
	const char *lang = player->languageService->GetLanguage();
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - HudType Standard"), KZHUDService::HUD_TYPE_STANDARD});
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - HudType Panorama"), KZHUDService::HUD_TYPE_PANORAMA});
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - HudType Off"), KZHUDService::HUD_TYPE_OFF});
}

static_function i64 GetHudTypeCurrent(KZPlayer *player, i64 tag)
{
	return (i64)player->hudService->GetHudType();
}

static_function void OnHudTypePick(KZPlayer *player, i64 tag, i64 id)
{
	player->hudService->SetHudType((i32)id);
}

// === mhudKeysIdle: чем показывать ненажатую клавишу (Task 5, транш "клавиши") ================
// Сырой int-преф (0 show / 1 hide / 2 underscore, см. LayoutElement::Keys/mhud.cpp) — в
// отличие от hudType это НЕ вычисляемое состояние сервиса, поэтому читаем/пишем сам преф,
// как CompareType в misc_prefs.cpp. Значения переведены (Task 15) — см. комментарий у
// GetHudTypeChoices выше.

static_function void GetKeysIdleChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
{
	const char *lang = player->languageService->GetLanguage();
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - Idle Show"), 0});
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - Idle Hide"), 1});
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - Idle Underscore"), 2});
}

static_function i64 GetKeysIdleCurrent(KZPlayer *player, i64 tag)
{
	// Дефолт 2 (Underscore) синхронизирован с текущими настройками игрока (задача hud-defaults).
	return player->optionService->GetPreferenceInt("mhudKeysIdle", 2);
}

static_function void OnKeysIdlePick(KZPlayer *player, i64 tag, i64 id)
{
	if (id < 0 || id > 2)
	{
		return;
	}
	player->optionService->SetPreferenceInt("mhudKeysIdle", id);
}

// === Обводка элемента: тумблер через колбэки, а не голый преф ================================
// Худ применяет МИГРИРОВАННОЕ значение (KZHUDService::GetElementOutlinePref, layout/prefs.cpp:
// поэлементный mhud*Outline, а при его отсутствии — старый общий hudOutline). Голый
// AddToggle(def.outlineKey, true) читал бы преф второй раз и с другим дефолтом: игроку с
// hudOutline=false меню показывало "Вкл" при выключенной обводке, а первый клик был видимым
// no-op (писал false поверх уже действующего false). tag — индекс элемента в LAYOUT_ELEMENTS.
static_function i64 OutlineGetCurrent(KZPlayer *player, i64 tag)
{
	return player->hudService->GetElementOutlinePref((LayoutElement)tag) ? 1 : 0;
}

static_function void OutlineOnActivate(KZPlayer *player, i64 tag)
{
	// Инвертируем то же эффективное значение, которое показано в меню; запись поэлементного
	// ключа заодно завершает миграцию для этого элемента (дальше probe видит сохранённый ключ).
	const bool next = !player->hudService->GetElementOutlinePref((LayoutElement)tag);
	player->optionService->SetPreferenceBool(LAYOUT_ELEMENTS[tag].outlineKey, next);
}

// === Сброс страницы (KZ::menu::ResetNode) ====================================================
// Апстрим держит такую кнопку на каждой странице элемента (origin/master hud_prefs.cpp:256) —
// при порте она потерялась, и ResetNode остался без единого вызова. Возвращаем её ТОЛЬКО на
// страницы, где все пункты — обычные префы: ResetNode пишет преф напрямую, и на пункте с
// колбэком (AddActionToggle сервисов в misc_prefs/local_prefs) он оставил бы кэш сервиса
// рассинхронизированным — ровно тот дефект, из-за которого сломались тумблеры до этого
// фикс-раунда. Обводка исключением не является: её кэш — layoutPrefs, а ActivateMenuItem
// зовёт RefreshLayoutPrefs сразу после onActivate кнопки. В General своей кнопки «сбросить
// страницу» нет: там лежит общий Reset All (ниже), а HudType не сбрасывается вовсе — у его
// Choice-пункта нет префа, и ResetNode такие пункты пропускает.
// Слоты: по одному на КАЖДЫЙ элемент LAYOUT_ELEMENTS (включая «Прогресс»), затем прицел
// (Count) и меню реплея (Count + 1) — см. RPMENU_RESET_SLOT ниже. Слоты живут только в памяти
// (статический массив, наполняется при регистрации реестра), в префах не хранятся — рост
// Count сдвигает номера двух последних безнаказанно.
// С ужатия раздела (план hud-editor-options, Task 8) слот элемента указывает на его узел в
// СКРЫТОЙ подкатегории (HiddenElements) — кнопки на нём нет, сброс элемента зовёт редактор !hud.
static_global KZOptNode *s_resettableNodes[(i32)LayoutElement::Count + 2] {};
static constexpr i32 RPMENU_RESET_SLOT = (i32)LayoutElement::Count + 1;

// Узлы, которые «Сбросить всё» накрывает сверх s_resettableNodes и General: после ужатия
// раздела пункты одной бывшей страницы разъехались по двум узлам (вид клавиш остался в меню,
// их цвета ушли в скрытый узел элемента; позиция карточки реплея — в скрытый узел), а новые
// тумблеры PB/WR живут на своей странице без кнопки сброса. Без этих слотов Reset All молча
// перестал бы трогать часть того, что сбрасывал до переезда.
enum HudExtraResetSlot
{
	EXTRA_RESET_KEYS_LOOK = 0,
	EXTRA_RESET_PBWR,
	EXTRA_RESET_RPMENU_POS,
	EXTRA_RESET_SPEED,
	EXTRA_RESET_COUNT
};
static_global KZOptNode *s_extraResetNodes[EXTRA_RESET_COUNT] {};

// Сброс — тоже откатываемое действие (спека обмена §4), поэтому перед ResetNode снимаем слот
// отката тем же и единственным способом, что и применение чужого худа
// (KZ::hudshare::SaveUndo). Без этого «Сбросить всё» → «Откатить последнее применение» вернуло
// бы игроку не отмену сброса, а прошлый чужой снимок — и отчиталось бы словом «Откатил».
// Подсказку печатаем только когда слот реально записан (иначе обещали бы несуществующий откат).
static_function void SaveUndoBeforeReset(KZPlayer *player)
{
	if (KZ::hudshare::SaveUndo(player))
	{
		player->languageService->PrintChat(true, false, "HUD Share - Undo Hint");
	}
}

static_function void ResetPageOnActivate(KZPlayer *player, i64 tag)
{
	if (tag < 0 || tag >= (i64)KZ_ARRAYSIZE(s_resettableNodes))
	{
		return;
	}
	SaveUndoBeforeReset(player);
	KZ::menu::ResetNode(player, s_resettableNodes[tag]);
}

// Регистрирует кнопку сброса в конце страницы и запоминает узел под её тегом.
static_function void AddResetButton(KZOptNode *node, i32 slot)
{
	s_resettableNodes[slot] = node;
	KZ::menu::AddButton(node, "HUD - Menu Label Reset", &ResetPageOnActivate, slot);
}

// === Сброс ВСЕХ настроек худа (апстримный "Reset All" на странице General) ====================
// У апстрима такая кнопка есть (origin/master hud_prefs.cpp, General) — при порте потерялась.
// Живёт в General, а не на родительской категории «Худ»: у категории с подкатегориями своих
// пунктов не бывает вовсе (ActiveMenuNode, layout/menu.cpp — родитель возвращает NULL, пока не
// выбрана подкатегория), так что пункт на ней был бы недостижим для игрока.
// Сбрасывает страницы всех элементов LAYOUT_ELEMENTS + прицел + меню реплея (s_resettableNodes),
// разъехавшиеся после ужатия узлы (s_extraResetNodes) и саму General.
// HudType при этом НЕ сбрасывается: его Choice-пункт зарегистрирован без prefKey, а ResetNode
// пункты без префа пропускает — иначе «сбросить оформление» могло бы выключить игроку худ.
static_global KZOptNode *s_hudGeneralNode {};

static_function void ResetAllOnActivate(KZPlayer *player, i64 tag)
{
	SaveUndoBeforeReset(player);
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(s_resettableNodes); i++)
	{
		KZ::menu::ResetNode(player, s_resettableNodes[i]);
	}
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(s_extraResetNodes); i++)
	{
		KZ::menu::ResetNode(player, s_extraResetNodes[i]);
	}
	KZ::menu::ResetNode(player, s_hudGeneralNode);
}

// === Поля элемента, общие для всех строк LAYOUT_ELEMENTS — ключи из самой таблицы
// (entity.cpp, Task 4), один источник правды, как и раньше в menu.cpp. ======================
// С ужатия раздела (план hud-editor-options, Task 8) зовётся для узлов скрытой подкатегории
// HiddenElements: в окне !options этих пунктов нет, их правит редактор !hud, а узлы остаются
// в реестре ради ResetNode (сброс элемента в редакторе), обмена худом и белого списка префов.
static_function void AddHudElementItems(KZOptNode *node, LayoutElement e)
{
	const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)e];
	// Дефолт — поэлементный (def.enabledDefault): у «Прогресса» он false. Тот же ответ читает
	// RefreshLayoutPrefs (layout/prefs.cpp) — разойтись значило бы показать в меню «Вкл» при
	// фактически выключенном элементе.
	KZ::menu::AddToggle(node, "HUD - Menu Label Enabled", def.enabledKey, def.enabledDefault);
	KZ::menu::AddPosition(node, "HUD - Menu Label Position", def.xKey, def.yKey, def.xDefault, def.yDefault);
	KZ::menu::AddSize(node, "HUD - Menu Label Size", def.sizeKey, def.sizeDefault, LAYOUT_SIZE_MIN, LAYOUT_SIZE_MAX);
	KZ::menu::AddFont(node, "HUD - Menu Label Font", def.fontKey, LAYOUT_DEFAULT_FONT);
	// Обводка — теперь поэлементный тумблер (def.outlineKey, задача 4), а не один общий
	// пункт на всё меню: старый "Outline" в General убран, чтобы не осталось двух источников.
	// Через колбэки — из-за миграции с hudOutline, см. OutlineGetCurrent выше.
	KZ::menu::AddActionToggle(node, "HUD - Menu Label Outline", &OutlineGetCurrent, &OutlineOnActivate, (i64)e);
	// Дефолт 0 (выкл) синхронизирован с текущими настройками игрока (задача hud-defaults) —
	// тот же ответ отдаёт GetElementOutlinePref (layout/prefs.cpp) через fallback на hudOutline.
	KZ::menu::SetItemPref(node, def.outlineKey, KZOptStorage::Bool, 0);
	// Прозрачность хранится Int (0-100), а не Float, как размер/позиция того же элемента —
	// AddSize по умолчанию заводит Float (px-поле), поэтому storage переопределяем ЯВНО:
	// реальный потребитель (layout/prefs.cpp:GetLayoutPrefs) читает opacityKey через
	// GetPreferenceInt, разойтись с ним значило бы читать иное значение, чем видит худ.
	KZ::menu::AddSize(node, "HUD - Menu Label Opacity", def.opacityKey, 100, 0, 100);
	KZ::menu::SetItemUnit(node, "%");
	KZ::menu::SetItemPref(node, def.opacityKey, KZOptStorage::Int, 100);
}

// === Обмен худом: четыре действия одной страницей =============================================
// Ядро — hud/share/hud_share.cpp, транспорт кода — hud/share/hud_share_commands.cpp; здесь
// только пункты меню поверх тех же функций, что и чат-команды. Ни один из четырёх пунктов не
// хранит настройки (Button без префа), поэтому в белый список обмена они не попадают
// (AddEntry пропускает storage == None, option/pref_registry.cpp) и отпечаток состава не двигают.
static_function void ShareCodeOnActivate(KZPlayer *player, i64 tag)
{
	KZ::hudshare::IssueShareCode(player);
}

static_function void ShareTakeOnActivate(KZPlayer *player, i64 tag)
{
	// Динамического гейта у пунктов нет: enabledBy умеет только БУЛЕВ преф (IsMenuItemEnabled,
	// layout/menu.cpp), а «наблюдаю ли я сейчас за кем-то» — состояние кадра, не настройка, и
	// серым пункт стал бы только до следующей перерисовки меню. Поэтому пункт кликабелен всегда,
	// а отказ печатает TakeFromSpectated — с причиной, а не молчанием.
	KZ::hudshare::TakeFromSpectated(player);
}

// === Настройки меню реплея ====================================================================
// Оба пункта — Choice со Int-хранением (как preferredMode/preferredPistol в misc_prefs.cpp),
// id = индекс в таблице. Пункта выбора шрифта здесь нет и не будет: карточка берёт шрифты из
// своей vcss (решение владельца 17.09.2026).
//
// Масштаб карточки меню реплея — СПИСОК, а не числовой ползунок. Довод: страница умеет ровно
// те размеры, под которые в аддоне сгенерирован набор правил (RPMENU_SCALE_STEPS), и ползунок
// 75..130 с шагом 1 на 24 значениях из 56 не менял бы ничего — игрок читает это как «настройка
// не работает» (замечание владельца 17.09.2026).
static_function void RpMenuScaleGetChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
{
	for (i64 i = 0; i < RPMENU_SCALE_STEP_COUNT; i++)
	{
		char label[16];
		V_snprintf(label, sizeof(label), "%i%%", RPMENU_SCALE_STEPS[i]);
		out.push_back({label, i});
	}
}

static_function i64 RpMenuScaleGetCurrent(KZPlayer *player, i64 tag)
{
	const i32 scale = SnapReplayMenuScale((i32)player->optionService->GetPreferenceInt("rpmenuScale", RPMENU_DEF_SCALE));
	for (i64 i = 0; i < RPMENU_SCALE_STEP_COUNT; i++)
	{
		if (RPMENU_SCALE_STEPS[i] == scale)
		{
			return i;
		}
	}
	return 0;
}

static_function void RpMenuScaleOnPick(KZPlayer *player, i64 tag, i64 id)
{
	if (id < 0 || id >= RPMENU_SCALE_STEP_COUNT)
	{
		return;
	}
	// RefreshLayoutPrefs после пика зовёт сам ActivateMenuItem (layout/menu.cpp), как у любого Choice.
	player->optionService->SetPreferenceInt("rpmenuScale", RPMENU_SCALE_STEPS[id]);
}

// === hudTimerCompare: живая дельта в таймере (план hud-editor-options, Task 7/8) ==============
// Сырой int-преф, как mhudKeysIdle: 0 — выкл, 1 — к PB, 2 — к WR. Значения — те, что читает
// RefreshLayoutPrefs (timerCompare, ограничение 0..2) и разбирает UpdateTimerElement (1 → PB,
// иначе AWR). Порядок строк в списке — PB/WR/Выкл (как в спеке §4.4), а id — само значение
// префа, поэтому «Выкл» идёт последним с id 0.
static constexpr i64 HUD_TIMER_COMPARE_DEFAULT = 1;

static_function void GetTimerCompareChoices(KZPlayer *player, i64 tag, std::vector<KZChoice> &out)
{
	const char *lang = player->languageService->GetLanguage();
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label TimerCompare PB"), 1});
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label TimerCompare WR"), 2});
	out.push_back({KZLanguageService::PrepareMessageWithLang(lang, "HUD - Menu Label TimerCompare Off"), 0});
}

static_function i64 GetTimerCompareCurrent(KZPlayer *player, i64 tag)
{
	const i64 value = player->optionService->GetPreferenceInt("hudTimerCompare", HUD_TIMER_COMPARE_DEFAULT);
	// Кривое значение из БД/снимка показываем тем же, чем его прочитает худ после ограничения.
	return value < 0 ? 0 : (value > 2 ? 2 : value);
}

static_function void OnTimerComparePick(KZPlayer *player, i64 tag, i64 id)
{
	if (id < 0 || id > 2)
	{
		return;
	}
	player->optionService->SetPreferenceInt("hudTimerCompare", id);
}

// Подпись скрытого узла элемента. В окне она не видна (узел hiddenFromMenu), но нужна узлу как
// любому другому и пригодится редактору/логу. switch только по элементам, которые были до
// ужатия, — новые строки LAYOUT_ELEMENTS (PB/WR, showpos, курс, тип рана) получают подпись
// самой скрытой подкатегории: своих фраз «Cat …» у них нет, а в меню они не показываются.
static_function const char *HiddenElementPageKey(i32 index)
{
	switch ((LayoutElement)index)
	{
		case LayoutElement::Timer:
			return "HUD - Menu Cat Timer";
		case LayoutElement::Speed:
			return "HUD - Menu Cat Speed";
		case LayoutElement::Prespeed:
			return "HUD - Menu Cat Prespeed";
		case LayoutElement::Keys:
			return "HUD - Menu Cat Keys";
		case LayoutElement::Checkpoint:
			return "HUD - Menu Cat Checkpoint";
		case LayoutElement::LeadProgress:
			return "HUD - Menu Cat LeadProgress";
		default:
			return "HUD - Menu Cat HiddenElements";
	}
}

void KZHUDService::InitMenuPrefs()
{
	// Дерево, а не плоский список (задача «дерево категорий»): страницы худа — ПОДКАТЕГОРИИ
	// одного узла «Худ», как у апстрима (origin/master hud_prefs.cpp:135-162).
	//
	// Раздел ужат (план hud-editor-options, Task 8; спека §4.4): расположение, размер, шрифт,
	// обводка, прозрачность и цвета каждого элемента переехали в редактор !hud, где их двигают
	// по сетке и видят сразу на реплике худа. В окне остаются только настройки, у которых нет
	// «места на экране»: тип худа, сравнение таймера, ячейки PB/WR, вид клавиш, прицел, меню
	// реплея, обмен. Пункты элементов НЕ удалены из реестра — они в скрытой подкатегории
	// HiddenElements ниже (почему именно подкатегория «Худ», а не своя категория — там же).
	KZOptNode *hud = KZ::menu::AddCategory("HUD - Menu Cat Hud");

	KZOptNode *general = KZ::menu::AddSub(hud, "HUD - Menu Cat General");
	s_hudGeneralNode = general;
	KZ::menu::AddChoice(general, "HUD - Menu Label HudType", GetHudTypeChoices, GetHudTypeCurrent, OnHudTypePick);
	// Общий пункт "Outline" убран (задача 4): обводка стала поэлементной
	// (LAYOUT_ELEMENTS[*].outlineKey у каждого элемента свой, пункт — в AddHudElementItems),
	// два источника одной и той же настройки — прямой путь к рассинхрону.
	//
	// Пункта под апстримный showPanel здесь НЕТ, и это решение, а не пропуск: его тумблер
	// KZHUDService::TogglePanel() не вызывается ниоткуда (git grep — только объявление и
	// определение), панорама преф вообще не читает (layout/mhud.cpp:367), а роль «показывать
	// худ или нет» с задачи 12 у типа Off — то есть у пункта HudType строкой выше. Пункт на
	// showPanel звал бы мёртвый метод, а пункт «Панель» поверх HudType дал бы ДВА источника
	// одного состояния — тот же рассинхрон, из-за которого убран общий Outline (строкой выше).
	//
	// Сравнение таймера: к чему считать живую дельту под таймером (Task 7). Преф в General, а не
	// на странице таймера: страницы таймера в окне больше нет (она в редакторе). SetItemPref —
	// ради обмена худом и ResetNode (у Choice без префа сброс его пропускает).
	KZ::menu::AddChoice(general, "HUD - Menu Label TimerCompare", &GetTimerCompareChoices, &GetTimerCompareCurrent, &OnTimerComparePick);
	KZ::menu::SetItemPref(general, "hudTimerCompare", KZOptStorage::Int, (i32)HUD_TIMER_COMPARE_DEFAULT);
	// hudTimerDetail — сотые доли и часы в таймере (layout/prefs.cpp:108, timerDetailed); с
	// ужатия раздела заодно и точность дельты (FormatDelta). Раньше пункт жил на странице
	// таймера — переехал в General вместе с ней. Ключ НАШ (апстримный называется
	// mhudTimerDetailed) — не переименовываем, иначе у всех игроков настройка сбросилась бы на
	// дефолт. Дефолт true = тот же, что читает RefreshLayoutPrefs.
	KZ::menu::AddToggle(general, "HUD - Menu Label TimerDetail", "hudTimerDetail", true);
	// mhudMimicSpec — мимикрия под настройки наблюдаемого (layout/prefs.cpp:GetLayoutPrefs).
	// Дефолт false = тот же, что читает RefreshLayoutPrefs (layout/prefs.cpp:129).
	KZ::menu::AddToggle(general, "HUD - Menu Label MimicSpec", "mhudMimicSpec", false);
	KZ::menu::SetItemSubtext(general, "HUD - Menu Label MimicSpec Sub");
	// compactPanel — СТАНДАРТНЫЙ (HTML) худ, не MHUD: две строки вместо полной панели
	// (KZHUDService::IsCompactPanel, kz_hud.cpp). Преф жив и до этой задачи переключался только
	// командой `!panel compact` — пункта в меню у него не было. Голый AddToggle, а не
	// AddActionToggle: IsCompactPanel() кэша не держит, читает преф заново каждый раз.
	// Дефолт false = дефолт GetPreferenceBool("compactPanel") без второго аргумента.
	KZ::menu::AddToggle(general, "HUD - Menu Label CompactPanel", "compactPanel", false);
	KZ::menu::SetItemSubtext(general, "HUD - Menu Label CompactPanel Sub");
	// Сброс всех страниц худа разом — единственный пункт General со своим действием, поэтому
	// стоит последним (см. ResetAllOnActivate выше).
	KZ::menu::AddButton(general, "HUD - Menu Label ResetAll", &ResetAllOnActivate);

	// Ячейки элемента PB/WR (mhud_pbwr): какие из четырёх времён показывать. Все выключены —
	// элемент скрыт целиком (UpdatePbWrElement). Дефолты true = те, что читает
	// RefreshLayoutPrefs (defaults.cpp). Кнопки сброса страницы нет — четыре тумблера сбрасывать
	// по одному проще, а «Сбросить всё» страницу накрывает (s_extraResetNodes).
	KZOptNode *pbwr = KZ::menu::AddSub(hud, "HUD - Menu Cat PbWr");
	s_extraResetNodes[EXTRA_RESET_PBWR] = pbwr;
	KZ::menu::AddToggle(pbwr, "HUD - Menu Label PbNub", "hudPbNub", true);
	KZ::menu::AddToggle(pbwr, "HUD - Menu Label PbPro", "hudPbPro", true);
	KZ::menu::AddToggle(pbwr, "HUD - Menu Label WrNub", "hudWrNub", true);
	KZ::menu::AddToggle(pbwr, "HUD - Menu Label WrPro", "hudWrPro", true);

	// Скорость и престрейф: точность, скобки и скрытие при сходе с края — это ПОВЕДЕНИЕ элемента
	// (что и когда показывать), а не место на экране, и на реплике редактора их не поправить. При
	// ужатии раздела они ушли в скрытые узлы элементов вместе с цветами — и у игрока пропал
	// способ их включить; здесь они снова видны. Ключи и дефолты — те, что читает
	// RefreshLayoutPrefs (layout/prefs.cpp). Узел элемента их больше не держит, поэтому сброс
	// элемента в редакторе их не трогает, а «Сбросить всё» накрывает страницу своим слотом.
	KZOptNode *speedLook = KZ::menu::AddSub(hud, "HUD - Menu Cat SpeedPrespeed");
	s_extraResetNodes[EXTRA_RESET_SPEED] = speedLook;
	KZ::menu::AddToggle(speedLook, "HUD - Menu Label SpeedDecimal", "mhudSpeedPrecise", false);
	KZ::menu::AddToggle(speedLook, "HUD - Menu Label PrespeedDecimal", "mhudPrespeedPrecise", false);
	KZ::menu::AddToggle(speedLook, "HUD - Menu Label PrespeedBrackets", "mhudPrespeedBrackets", false);
	KZ::menu::AddToggle(speedLook, "HUD - Menu Label PrespeedHideWalkOff", "mhudPrespeedHideWalkOff", false);
	KZ::menu::SetItemSubtext(speedLook, "HUD - Menu Label PrespeedHideWalkOff Sub");

	// Клавиши: в окне остался только ВИД (буквы, квадрат, рамка, свечение, заливка, перекрытие,
	// ненажатая клавиша) — это стиль, а не место на экране, и на реплике редактора его не
	// поправить. Тумблер элемента, позиция, размер, шрифт, обводка, прозрачность и все цвета
	// клавиш — в скрытом узле элемента (ниже), их правит редактор.
	KZOptNode *keys = KZ::menu::AddSub(hud, "HUD - Menu Cat Keys");
	s_extraResetNodes[EXTRA_RESET_KEYS_LOOK] = keys;
	// hudKeysOverlap читался кодом (layout/prefs.cpp) ещё до этой задачи, но пункта в меню у
	// него не было — одна из шести находок транша "клавиши" (без пункта/команды у игрока).
	// Дефолт true — тот же, что уже читает GetPreferenceBool на этом ключе.
	KZ::menu::AddToggle(keys, "HUD - Menu Label Overlap", "hudKeysOverlap", true);
	// Осевой режим: тонировать только конфликтующую пару клавиш вместо всего контейнера.
	KZ::menu::AddToggle(keys, "HUD - Menu Label OverlapAxisOnly", "mhudKeysOverlapAxis", false);
	KZ::menu::SetItemEnabledBy(keys, "hudKeysOverlap");
	// Дефолты пяти тумблеров ниже синхронизированы с текущими настройками игрока (задача
	// hud-defaults) — те же значения читает layout/prefs.cpp:RefreshLayoutPrefs.
	KZ::menu::AddToggle(keys, "HUD - Menu Label Letters", "mhudKeysLetters", true);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Square", "mhudKeysSquare", true);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Border", "mhudKeysBorder", false);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Glow", "mhudKeysGlow", false);
	KZ::menu::AddToggle(keys, "HUD - Menu Label Fill", "mhudKeysFill", false);
	KZ::menu::AddChoice(keys, "HUD - Menu Label Idle", &GetKeysIdleChoices, &GetKeysIdleCurrent, &OnKeysIdlePick);
	KZ::menu::SetItemPref(keys, "mhudKeysIdle", KZOptStorage::Int, 2);

	KZOptNode *crosshair = KZ::menu::AddSub(hud, "HUD - Menu Cat Crosshair");
	// Дефолт true синхронизирован с текущими настройками игрока (задача hud-defaults).
	KZ::menu::AddToggle(crosshair, "HUD - Menu Label Enabled", "mhudCrosshair", true);
	KZ::menu::AddSize(crosshair, "HUD - Menu Label Scale", "mhudCrosshairScale", 100, 0, 500);
	KZ::menu::SetItemUnit(crosshair, "%");
	KZ::menu::SetItemPref(crosshair, "mhudCrosshairScale", KZOptStorage::Int, 100);
	AddResetButton(crosshair, (i32)LayoutElement::Count);

	// Меню реплея спектатора (layout/rpmenu.cpp) — отдельная страница настроек. Карточка живёт на
	// своей panorama-странице, а сервер умеет над ней только SetHasClass, поэтому настраивается
	// ровно то, под что в её vcss есть готовые наборы правил. Ключи читает
	// layout/prefs.cpp:RefreshLayoutPrefs, дефолты — RPMENU_DEF_* (layout/layout.h).
	//
	// Пунктов позиции в процентах, кегля, шрифта, шага строк и подложки здесь БОЛЬШЕ НЕТ: они
	// достались от карточки на разметке худа и после переезда не влияли ни на что (снято
	// 17.09.2026 по замечанию владельца). Шрифты у карточки свои, выбор шрифта не нужен.
	// Позиция карточки (степперы ±1/±5, c02af292) с ужатия раздела тоже ушла из окна — вместе с
	// позициями элементов; её пункт перенесён в скрытый узел ниже, а не удалён: rpmenuPosX/Y
	// по-прежнему читает RefreshLayoutPrefs, и без пункта в реестре ключи выпали бы из обмена
	// худом и из «Сбросить всё» — сохранённое смещение карточки стало бы неснимаемым.
	// Кнопки сброса страницы нет: пункт на ней один, «Сбросить всё» его накрывает (слот ниже).
	KZOptNode *rpmenu = KZ::menu::AddSub(hud, "HUD - Menu Cat ReplayMenu");
	s_resettableNodes[RPMENU_RESET_SLOT] = rpmenu;
	KZ::menu::AddChoice(rpmenu, "HUD - Menu Label ReplayMenuScale", &RpMenuScaleGetChoices, &RpMenuScaleGetCurrent, &RpMenuScaleOnPick);
	KZ::menu::SetItemPref(rpmenu, "rpmenuScale", KZOptStorage::Int, RPMENU_DEF_SCALE);

	// Обмен худом — СВОЯ подкатегория, а не пункты в General. Довод: в General лежит «сбросить
	// все настройки худа», и действия обмена рядом с ней читались бы как часть сброса, а
	// главное — General это страница СВОЙСТВ худа (тип, мимикрия, компактная панель), тогда как
	// обмен это ДЕЙСТВИЯ над всем худом целиком. Отдельная страница ещё и обязательна по месту:
	// пункт «забрать худ наблюдаемого» ищут в спектейте, а меню в спектейте показывает ровно
	// выбранную подкатегорию — своя страница находится по названию, не перебором General.
	// Своей кнопки «сбросить страницу» у неё нет: сбрасывать нечего, префов на странице ноль.
	KZOptNode *share = KZ::menu::AddSub(hud, "HUD Share - Menu Cat Share");
	KZ::menu::AddButton(share, "HUD Share - Menu Label ShareCode", &ShareCodeOnActivate);
	KZ::menu::SetItemSubtext(share, "HUD Share - Menu Label ShareCode Sub");
	KZ::menu::AddButton(share, "HUD Share - Menu Label Take", &ShareTakeOnActivate);
	KZ::menu::SetItemSubtext(share, "HUD Share - Menu Label Take Sub");
	// Пунктов «выгрузить в консоль» и «откатить» здесь НЕТ по решению пользователя 09.09:
	// обмен кодом оказался удобнее текстового блока, а про откат игрок узнаёт из строки в чате
	// («HUD Share - Undo Hint» печатается после каждого применения, hud_share.cpp). Команды
	// !hudexport и !hudundo остались — снят только вход из меню, чтобы страница была короткой
	// и на ней не было действий, которых игрок не искал.

	// === Скрытая подкатегория: вид и место каждого элемента (правит редактор !hud) ============
	// ПОДкатегория «Худ», а не своя категория верхнего уровня: обмен худом собирает состав ровно
	// из поддерева "HUD - Menu Cat Hud" (KZ::hudshare::HUD_CATEGORY_KEY → CollectCategory), и
	// отдельная категория молча выкинула бы из снимка позиции, размеры, шрифты и цвета — то
	// есть почти всё, ради чего обмен существует. hiddenFromMenu прячет узел от окна !options
	// (рендер его пропускает), реестр же видит его как обычный: ResetNode, белый список префов,
	// отпечаток состава обмена.
	// Раскладка для редактора: subs[i] при i < Count — узел элемента LAYOUT_ELEMENTS[i] (тот же
	// узел лежит в s_resettableNodes[i]); после них — позиция карточки реплея. Цикл по таблице,
	// а не список руками: новые строки LAYOUT_ELEMENTS (PB/WR, showpos, курс, тип рана) получают
	// свой узел с полным набором полей без правки этого файла.
	KZOptNode *hidden = KZ::menu::AddSub(hud, "HUD - Menu Cat HiddenElements");
	hidden->hiddenFromMenu = true;
	KZOptNode *elementNodes[(i32)LayoutElement::Count] {};
	for (i32 i = 0; i < (i32)LayoutElement::Count; i++)
	{
		KZOptNode *node = KZ::menu::AddSub(hidden, HiddenElementPageKey(i));
		node->hiddenFromMenu = true;
		AddHudElementItems(node, (LayoutElement)i);
		elementNodes[i] = node;
		s_resettableNodes[i] = node;
	}

	// Дальше — поэлементные пункты сверх общих полей: цвета и форматы, которые раньше стояли на
	// странице элемента. Состав, ключи и дефолты те же, что были до ужатия.
	KZOptNode *timer = elementNodes[(i32)LayoutElement::Timer];
	KZ::menu::AddColor(timer, "HUD - Menu Label ProColor", "mhudTimerProColor", MHUD_DEF_TIMER_PRO_COLOR);
	KZ::menu::AddColor(timer, "HUD - Menu Label TpColor", "mhudTimerTpColor", MHUD_DEF_TIMER_TP_COLOR);
	KZ::menu::AddColor(timer, "HUD - Menu Label PausedColor", "mhudTimerPausedColor", MHUD_DEF_TIMER_PAUSED_COLOR);
	KZ::menu::AddColor(timer, "HUD - Menu Label StoppedColor", "mhudTimerStoppedColor", MHUD_DEF_TIMER_STOPPED_COLOR);

	KZOptNode *speed = elementNodes[(i32)LayoutElement::Speed];
	// mhudSpeedPrecise — на видимой странице «Скорость и престрейф» выше.
	KZ::menu::AddColor(speed, "HUD - Menu Label Color", "mhudSpeedColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(speed, "HUD - Menu Label CjColor", "mhudSpeedCjColor", MHUD_DEF_CJ_COLOR);

	KZOptNode *prespeed = elementNodes[(i32)LayoutElement::Prespeed];
	// Три тумблера престрейфа — на видимой странице «Скорость и престрейф» выше.
	KZ::menu::AddColor(prespeed, "HUD - Menu Label Color", "mhudPrespeedColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(prespeed, "HUD - Menu Label PerfColor", "mhudPrespeedPerfColor", MHUD_DEF_PERF_COLOR);
	KZ::menu::AddColor(prespeed, "HUD - Menu Label JumpbugColor", "mhudPrespeedJumpbugColor", MHUD_DEF_JUMPBUG_COLOR);

	// Цвета клавиш — здесь, вид (тумблеры) — на видимой странице Keys выше. Гейты enabledBy на
	// hudKeysOverlap сохранены: преф один на оба узла, редактор читает их так же, как окно.
	KZOptNode *keysColors = elementNodes[(i32)LayoutElement::Keys];
	KZ::menu::AddColor(keysColors, "HUD - Menu Label Color", "mhudKeysColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(keysColors, "HUD - Menu Label OverlapColor", "mhudKeysOverlapColor", MHUD_DEF_KEYS_OVERLAP_COLOR);
	KZ::menu::SetItemEnabledBy(keysColors, "hudKeysOverlap");
	KZ::menu::AddColor(keysColors, "HUD - Menu Label PressedColor", "mhudKeysPressedColor", MHUD_DEF_KEYS_PRESSED_COLOR);
	KZ::menu::SetItemSolidOnly(keysColors); // key-glow-N (keys.css) — только сплошные, градиента там нет
	KZ::menu::AddColor(keysColors, "HUD - Menu Label OverlapGlowColor", "mhudKeysOverlapGlowColor", MHUD_DEF_KEYS_OVERLAP_GLOW_COLOR);
	KZ::menu::SetItemSolidOnly(keysColors);
	KZ::menu::SetItemEnabledBy(keysColors, "hudKeysOverlap");

	KZ::menu::AddColor(elementNodes[(i32)LayoutElement::Checkpoint], "HUD - Menu Label Color", "mhudCheckpointColor", MHUD_DEF_BASE_COLOR);
	// Цвета полей редактора !hud и дельты таймера: пункты в скрытых узлах элементов — их правит
	// попап цвета редактора (ep_color/ep_dcol_*), сбрасывает ResetNode элемента, переносит обмен
	// худом. Ключи — те же, что читает RefreshLayoutPrefs (layout/prefs.cpp).
	KZ::menu::AddColor(timer, "HUD - Menu Label DeltaAheadColor", "mhudDeltaAheadColor", MHUD_DEF_DELTA_AHEAD_COLOR);
	KZ::menu::AddColor(timer, "HUD - Menu Label DeltaBehindColor", "mhudDeltaBehindColor", MHUD_DEF_DELTA_BEHIND_COLOR);
	KZ::menu::AddColor(elementNodes[(i32)LayoutElement::LeadProgress], "HUD - Menu Label Color", "mhudLeadProgressColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(elementNodes[(i32)LayoutElement::PbWr], "HUD - Menu Label Color", "mhudPbWrColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(elementNodes[(i32)LayoutElement::ShowPos], "HUD - Menu Label Color", "mhudShowPosColor", MHUD_DEF_BASE_COLOR);
	KZ::menu::AddColor(elementNodes[(i32)LayoutElement::Course], "HUD - Menu Label Color", "mhudCourseColor", MHUD_DEF_BASE_COLOR);
	// У «Прогресса» своего цвета нет (в дизайне не просили) — только общие поля.

	// Позиция карточки меню реплея — см. комментарий у видимой страницы ReplayMenu выше. Классы
	// позиции берутся из cs2kz/positions.css, которая и так едет в аддоне (см. layout.h).
	KZOptNode *rpmenuPos = KZ::menu::AddSub(hidden, "HUD - Menu Cat ReplayMenu");
	rpmenuPos->hiddenFromMenu = true;
	s_extraResetNodes[EXTRA_RESET_RPMENU_POS] = rpmenuPos;
	KZ::menu::AddPosition(rpmenuPos, "HUD - Menu Label Position", "rpmenuPosX", "rpmenuPosY", RPMENU_DEF_POS_X, RPMENU_DEF_POS_Y);
}
