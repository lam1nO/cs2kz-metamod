// Меню управления реплеем спектатора на panorama (см. kz_hud.h, раздел «Меню управления
// реплеем»). Семантика пунктов и тексты — KZ::replaysystem::menu (replays/menu.cpp), здесь —
// сущности, чтение клавиш и рендер карточки.
//
// Почему сущности с разметкой ХУДА, а не страница меню настроек: в чужом аддоне 3469155349
// стили живут по-файлово, и menu.vcss не подключает positions.vcss — список оттуда к левому
// краю не сдвинуть (только #menu_root.shift на −125px). У mhud.vxml четыре текстовых лейбла с
// позицией/цветом/кеглем/прозрачностью/шрифтом/фоном классами — из них и собрана карточка. Строк
// девять (заголовок, состояние, 6 пунктов, подсказка), у каждой текст и подложка — по две строки
// на копию, копий ПЯТЬ (RPMENU_ENTITIES): клиент держит копии одной страницы раздельно (cyb.166).
//
// Левый край (решение пользователя 09.09): .element центрирует лейбл, поэтому все строки
// добиваются NBSP до одной длины в кодовых точках и рисуются ОДНИМ шрифтом и ОДНИМ кеглем.
// Точно это работает только в моноширинном шрифте — поэтому шрифт меню ограничен таблицей
// RPMENU_MONO_FONTS (layout.h; пропорциональный давал «буквы в разнобой» и скачки при смене
// выбора, канарейка cyb.169). Выбор — только цвет и прозрачность (рост кегля сдвигал бы край);
// «< »/« >» на выбранной регулируемой строке, у остальных на их месте NBSP той же ширины —
// ширина блока — константа: считается по САМЫМ ДЛИННЫМ вариантам всех строк (все подсказки, все
// пункты со скобками, состояние с самой длинной скоростью), а не по текущему тексту, иначе смена
// подсказки меняла бы ширину и центрированный блок ездил бы влево-вправо (канарейка cyb.170).
// Заголовок отличается акцентным цветом и капсом, не шрифтом. Подложка — чёрный pal-bg с
// прозрачностью rpmenuBackground (палитра аддона непрозрачная, а opacity гасит и текст — поэтому
// отдельный лейбл). Геометрия/шрифт/обводка/фон — префы rpmenu*.
//
// Меню ВСЕГДА открыто, пока игрок наблюдает реплей-бота с идущим плейбеком: UpdateReplayMenu
// (тик из DrawPanels) сам открывает и закрывает его; команды нет, cs2menus-меню удалено.
#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/panorama_tables.h"
#include "kz/language/kz_language.h"
#include "kz/spec/kz_spec.h"
#include "kz/replays/kz_replaysystem.h"
#include "kz/replays/menu.h"
#include "kz/replays/data.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "sdk/services.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/logging.h"
#include "cs2kz.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include <algorithm>

#include "tier0/memdbgon.h"

using KZ::replaysystem::menu::ReplayMenuInput;
using KZ::replaysystem::menu::ReplayMenuLine;

// Меню-движок cs2menus (определён в cs2kz.cpp); nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

// Автоповтор A/D на удержании (как adjust в cs2menus): период в тиках после 22-тиковой паузы.
// Подбирается живьём: перемотка ±10 с на удержании не должна улетать за секунды.
static CConVar<int> kz_rpmenu_repeat_ticks("kz_rpmenu_repeat_ticks", FCVAR_NONE,
										   "Replay menu (panorama): auto-repeat period of A/D while held, ticks (after a 22-tick delay).", 8);

namespace
{
	// Раскладка копии страницы: две строки на копию. У строки два лейбла — ПОДЛОЖКА (NBSP с фоном,
	// слот, который в mhud.vxml идёт РАНЬШЕ) и ТЕКСТ (слот позже): в panorama поздний ребёнок
	// рисуется поверх раннего, так текст гарантированно над фоном без z-index. Строка i →
	// сущность i / 2, пара слотов i % 2. Keys не используется.
	constexpr LayoutElement RPMENU_PLATE_SLOTS[] = {LayoutElement::Timer, LayoutElement::Speed};
	constexpr LayoutElement RPMENU_TEXT_SLOTS[] = {LayoutElement::Prespeed, LayoutElement::Checkpoint};
	constexpr i32 RPMENU_LINES_PER_ENTITY = (i32)KZ_ARRAYSIZE(RPMENU_TEXT_SLOTS);
	constexpr i32 RPMENU_ITEMS = (i32)ReplayMenuLine::Count;

	// Строки карточки: заголовок, состояние, пункты, подсказка. Ряд (в шагах step от y0) —
	// с «воздухом» в один шаг после состояния и перед подсказкой (LineRow).
	constexpr i32 RPMENU_LINE_TITLE = 0;
	constexpr i32 RPMENU_LINE_STATUS = 1;
	constexpr i32 RPMENU_LINE_ITEM0 = 2;
	constexpr i32 RPMENU_LINE_HINT = RPMENU_LINE_ITEM0 + RPMENU_ITEMS;
	constexpr i32 RPMENU_LINES = RPMENU_LINE_HINT + 1;
	static_assert(RPMENU_LINES <= KZHUDService::RPMENU_ENTITIES * RPMENU_LINES_PER_ENTITY,
				  "копий страницы должно хватать на заголовок, состояние, пункты и подсказку (по две строки на копию)");

	i32 LineRow(i32 line)
	{
		i32 row = line;
		if (line >= RPMENU_LINE_ITEM0)
		{
			row++; // зазор после строки состояния
		}
		if (line >= RPMENU_LINE_HINT)
		{
			row++; // зазор перед подсказкой
		}
		return row;
	}

	// Палитра (решение пользователя 10.09): весь текст НЕПРОЗРАЧНЫЙ — прозрачность делала строки
	// «еле видными» рядом с чёткими цифрами худа; акцент — циан #00aaff (есть в палитре аддона
	// точно; CS2-чат «{blue}» у бренда — ближайший к нему из 16 цветов чата) на заголовке, выбранном
	// пункте и подсказке; остальные пункты и состояние — белые.
	const Color RPMENU_COLOR_ACCENT(0x00, 0xAA, 0xFF, 255);
	const Color RPMENU_COLOR_TEXT(255, 255, 255, 255);
	constexpr i32 RPMENU_OPACITY_IDLE = 100;
	constexpr i32 RPMENU_OPACITY_STATUS = 100;
	constexpr i32 RPMENU_OPACITY_HINT = 100;

	constexpr i32 RPMENU_REPEAT_DELAY_TICKS = 22;

	// Повтор создания сущностей после отказа — раз в секунду, не каждый тик: меню теперь
	// живёт весь просмотр, и отказ (схема разъехалась после апдейта CS2) иначе давал бы 64
	// KZ_LOG_ERROR в секунду на каждого спектатора. Логируется только первый отказ.
	constexpr i32 RPMENU_RETRY_TICKS = 64;

	// Открыто ли у слота любое cs2menus-меню (например, выбор реплея по нику): оно читает те же
	// W/S/E/A/D, и пока оно на экране, карточка ввод не трогает — иначе E из поиска реплея
	// ставил бы паузу или завершал просмотр.
	bool HasForeignMenu(KZPlayer *player)
	{
		return g_pMenus != nullptr && g_pMenus->GetActiveMenu(player->GetPlayerSlot().Get()) != kInvalidMenuHandle;
	}

	// Обрамление выбранной регулируемой строки — сигнал «сейчас можно A/D»; у остальных строк
	// на этих местах NBSP той же ширины, чтобы текст начинался в одной колонке.
	const char *const RPMENU_ADJ_OPEN = "<\xC2\xA0";  // <NBSP
	const char *const RPMENU_ADJ_CLOSE = "\xC2\xA0>"; // NBSP>
	const char *const RPMENU_PLAIN_PAD = "\xC2\xA0\xC2\xA0";
	const char *const RPMENU_PAD = "\xC2\xA0";

	// Длина в кодовых точках UTF-8 — добивка строк до одной ширины.
	size_t Utf8Length(const std::string &s)
	{
		size_t n = 0;
		for (unsigned char c : s)
		{
			if ((c & 0xC0) != 0x80)
			{
				n++;
			}
		}
		return n;
	}

	// Обрезка до limit кодовых точек: ширина блока — инвариант, а не ожидание. Строка длиннее
	// расчётной (произвольная !rpspeed, длинный ник) режется, а не сдвигает карточку.
	void Utf8Truncate(std::string &s, size_t limit)
	{
		size_t n = 0;
		for (size_t i = 0; i < s.size(); i++)
		{
			if (((unsigned char)s[i] & 0xC0) != 0x80)
			{
				if (n == limit)
				{
					s.resize(i);
					return;
				}
				n++;
			}
		}
	}

	// Удержанные кнопки наблюдательской пешки. GetPlayerPawn() у спектатора — игровая пешка
	// (мёртвая или отсутствующая), кнопки же приходят на ТЕКУЩУЮ (observer) — берём её, как
	// cs2menus (GetInputPawn). 0 — пешки/сервисов нет.
	u64 HeldButtons(KZPlayer *player)
	{
		CCSPlayerController *controller = player ? player->GetController() : nullptr;
		CBasePlayerPawn *pawn = controller ? controller->GetCurrentPawn() : nullptr;
		CPlayer_MovementServices *ms = pawn ? pawn->m_pMovementServices() : nullptr;
		if (!ms)
		{
			return 0;
		}
		return ms->m_nButtons().m_pButtonStates[0];
	}
} // namespace

static_function size_t ReplayMenuWidth(KZPlayer *player);

// === Доступность ============================================================================

const char *KZHUDService::ReplayMenuUnavailableReason()
{
	if (g_KZPlugin.unloading)
	{
		return "unloading";
	}
	if (!KZHUDService::IsMHUDAvailable())
	{
		return "no_addon";
	}
	if (!KZ::replaysystem::data::IsReplayPlaying())
	{
		return "no_replay";
	}
	KZPlayer *target = this->player->specService->GetSpectatedPlayer();
	if (!target || !KZ::replaysystem::IsReplayBot(target))
	{
		return "not_spectating_bot";
	}
	return NULL;
}

// === Сущности ==============================================================================

CCSCustomHudLayout *KZHUDService::EnsureReplayLayout(i32 index, bool &created)
{
	created = false;
	if (g_KZPlugin.unloading || !KZHUDService::IsMHUDAvailable() || index < 0 || index >= RPMENU_ENTITIES)
	{
		return NULL;
	}
	if (CBaseEntity *cached = this->ownedReplayLayouts[index].Get())
	{
		return (CCSCustomHudLayout *)cached;
	}
	CCSCustomHudLayout *layout = utils::CreateEntityByName<CCSCustomHudLayout>("custom_hud_layout");
	if (!layout)
	{
		return NULL;
	}
	CEntityKeyValues *pKeyValues = new CEntityKeyValues();
	// Та же разметка, что у худа (см. шапку файла) — очередная копия страницы у одного клиента.
	pKeyValues->SetString("layout", KZ_MHUD_LAYOUT);
	char name[32];
	V_snprintf(name, sizeof(name), "kzrpmenu%i_%i", this->player->GetPlayerSlot().Get(), index);
	pKeyValues->SetString("targetname", name);
	layout->DispatchSpawn(pKeyValues);
	this->ownedReplayLayouts[index] = layout;
	created = true;
	// Диф-кэш — состояние ПРЕДЫДУЩЕЙ сущности, сбрасываем в момент реального создания
	// (тот же урок, что у EnsureMenuLayout).
	for (i32 i = 0; i < (i32)LayoutElement::Count; i++)
	{
		this->replayLines[index][i] = LayoutElementState();
	}
	return layout;
}

void KZHUDService::DestroyOwnedReplayLayout()
{
	for (i32 index = 0; index < RPMENU_ENTITIES; index++)
	{
		if (!this->ownedReplayLayouts[index].IsValid())
		{
			continue;
		}
		if (CBaseEntity *ent = this->ownedReplayLayouts[index].Get())
		{
			g_pKZUtils->RemoveEntity(ent);
		}
		this->ownedReplayLayouts[index] = nullptr;
		for (i32 i = 0; i < (i32)LayoutElement::Count; i++)
		{
			this->replayLines[index][i] = LayoutElementState();
		}
	}
}

// === Открытие / закрытие ====================================================================

bool KZHUDService::OpenReplayMenu()
{
	if (this->replayMenuOpen)
	{
		return true;
	}
	CCSCustomHudLayout *layouts[RPMENU_ENTITIES] {};
	bool created[RPMENU_ENTITIES] {};
	for (i32 i = 0; i < RPMENU_ENTITIES; i++)
	{
		layouts[i] = this->EnsureReplayLayout(i, created[i]);
		if (!layouts[i])
		{
			// Отказ, не выбор: сущность не создалась (схема разъехалась после апдейта CS2 /
			// MultiAddonManager нет). Лог — один раз на серию отказов (см. RPMENU_RETRY_TICKS),
			// уже созданные копии не оставляем висеть.
			if (!this->replayMenuFailLogged)
			{
				KZ_LOG_ERROR(LogChannel::General, "[cyb] replay_menu_unavailable reason=layout_entity_failed index=%i slot=%i\n", i,
							 this->player->GetPlayerSlot().Get());
				this->replayMenuFailLogged = true;
			}
			this->replayMenuRetryTick = g_pKZUtils->GetServerGlobals()->tickcount;
			this->DestroyOwnedReplayLayout();
			return false;
		}
	}
	if (this->replayMenuFailLogged)
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb] replay_menu_recovered slot=%i\n", this->player->GetPlayerSlot().Get());
		this->replayMenuFailLogged = false;
	}
	this->replayMenuOpen = true;
	this->replayMenuLine = 0;
	// Стартовая маска = то, что уже удержано: иначе зажатая при открытии клавиша (например,
	// W из движения перед спектейтом) сработала бы как нажатие на первом же тике.
	this->replayMenuHeld = HeldButtons(this->player);
	this->replayMenuHoldTicks = 0;
	// Ширина блока — один раз на открытие (язык и реплей за просмотр не меняются; смена языка
	// подхватится следующим открытием), а не ~14 форматирований на тик.
	this->replayMenuWidth = ReplayMenuWidth(this->player);
	KZ_LOG_INFO(LogChannel::General, "[cyb] replay_menu_open slot=%i\n", this->player->GetPlayerSlot().Get());
	bool forceAll[RPMENU_ENTITIES];
	for (i32 i = 0; i < RPMENU_ENTITIES; i++)
	{
		forceAll[i] = true;
	}
	this->RenderReplayMenu(layouts, forceAll);
	return true;
}

void KZHUDService::CloseReplayMenu(const char *reason)
{
	if (this->replayMenuOpen)
	{
		KZ_LOG_INFO(LogChannel::General, "[cyb] replay_menu_close reason=%s slot=%i\n", reason ? reason : "unknown",
					this->player->GetPlayerSlot().Get());
	}
	this->replayMenuOpen = false;
	this->replayMenuHeld = 0;
	this->replayMenuHoldTicks = 0;
	this->replayMenuFailLogged = false;
	this->replayMenuRetryTick = 0;
	this->DestroyOwnedReplayLayout();
}

// === Тик: доступность → открыть/закрыть, ввод, рендер ======================================

void KZHUDService::UpdateReplayMenu(KZPlayer *source)
{
	// Меню живёт ровно пока игрок наблюдает бота с идущим плейбеком и есть аддон. source —
	// то, что DrawPanels считает источником данных; для спектатора это наблюдаемый.
	const bool wanted = source && source != this->player && KZ::replaysystem::IsReplayBot(source) && this->ReplayMenuUnavailableReason() == NULL;
	if (!wanted)
	{
		if (this->replayMenuOpen)
		{
			const char *reason = this->ReplayMenuUnavailableReason();
			this->CloseReplayMenu(reason ? reason : "target_changed");
		}
		return;
	}
	if (!this->replayMenuOpen)
	{
		// После отказа сущности — повтор с бэкоффом, не каждый тик (см. RPMENU_RETRY_TICKS).
		if (this->replayMenuFailLogged && g_pKZUtils->GetServerGlobals()->tickcount - this->replayMenuRetryTick < RPMENU_RETRY_TICKS)
		{
			return;
		}
		if (!this->OpenReplayMenu())
		{
			return; // причина уже в логе (первый отказ серии)
		}
	}
	CCSCustomHudLayout *layouts[RPMENU_ENTITIES] {};
	bool created[RPMENU_ENTITIES] {};
	for (i32 i = 0; i < RPMENU_ENTITIES; i++)
	{
		layouts[i] = this->EnsureReplayLayout(i, created[i]);
		if (!layouts[i])
		{
			this->CloseReplayMenu("layout_entity_failed");
			return;
		}
	}

	if (HasForeignMenu(this->player))
	{
		// Чужое cs2menus-меню на экране: клавиши его. Маску держим актуальной, чтобы после его
		// закрытия удержанная клавиша не сработала ложным фронтом.
		this->replayMenuHeld = HeldButtons(this->player);
		this->replayMenuHoldTicks = 0;
	}
	else
	{
		this->ReadReplayMenuInput();
	}
	// Ввод мог остановить реплей («Завершить») — тогда меню уже закрыто и сущностей нет.
	if (!this->replayMenuOpen)
	{
		return;
	}
	if (!KZ::replaysystem::data::IsReplayPlaying())
	{
		this->CloseReplayMenu("replay_stopped");
		return;
	}
	this->RenderReplayMenu(layouts, created);
}

void KZHUDService::ReadReplayMenuInput()
{
	const u64 held = HeldButtons(this->player);
	const u64 pressed = held & ~this->replayMenuHeld;
	this->replayMenuHeld = held;

	if (pressed & IN_FORWARD)
	{
		this->replayMenuLine = (this->replayMenuLine + RPMENU_ITEMS - 1) % RPMENU_ITEMS;
	}
	if (pressed & IN_BACK)
	{
		this->replayMenuLine = (this->replayMenuLine + 1) % RPMENU_ITEMS;
	}

	const ReplayMenuLine line = (ReplayMenuLine)this->replayMenuLine;
	if (pressed & IN_USE)
	{
		KZ::replaysystem::menu::ApplyReplayMenuInput(this->player, line, ReplayMenuInput::Select);
		if (line == ReplayMenuLine::End)
		{
			this->CloseReplayMenu("end_selected");
			return;
		}
	}

	// A/D: фронт нажатия — сразу; удержание — автоповтор после паузы (шаг по тикам и
	// перемотка на удержании — тот же жест, что в cs2menus). Нерегулируемые строки A/D не трогают.
	const u64 adjust = held & (IN_MOVELEFT | IN_MOVERIGHT);
	if (!adjust || adjust == (IN_MOVELEFT | IN_MOVERIGHT) || !KZ::replaysystem::menu::IsReplayMenuLineAdjustable(line))
	{
		this->replayMenuHoldTicks = 0;
		return;
	}
	this->replayMenuHoldTicks++;
	const bool edge = (pressed & adjust) != 0;
	const i32 period = (std::max)(kz_rpmenu_repeat_ticks.Get(), 1);
	const bool repeat =
		this->replayMenuHoldTicks > RPMENU_REPEAT_DELAY_TICKS && (this->replayMenuHoldTicks - RPMENU_REPEAT_DELAY_TICKS) % period == 0;
	if (edge || repeat)
	{
		KZ::replaysystem::menu::ApplyReplayMenuInput(this->player, line, adjust & IN_MOVERIGHT ? ReplayMenuInput::Inc : ReplayMenuInput::Dec);
	}
}

// Максимальная длина строки карточки в кодовых точках по ВСЕМ вариантам текстов (см. шапку файла):
// подсказки всех пунктов, пункты со скобками, заголовок, состояние с самой длинной скоростью и
// суффиксом паузы. Зависит от языка и реплея, но не от выбранного пункта и тика.
static_function size_t ReplayMenuWidth(KZPlayer *player)
{
	using namespace KZ::replaysystem::menu;
	size_t width = Utf8Length(RPMENU_PLAIN_PAD + GetReplayMenuTitleText(player) + RPMENU_PLAIN_PAD);
	width = (std::max)(width, Utf8Length(RPMENU_PLAIN_PAD + GetReplayMenuStatusMaxText(player) + RPMENU_PLAIN_PAD));
	for (i32 i = 0; i < RPMENU_ITEMS; i++)
	{
		const ReplayMenuLine line = (ReplayMenuLine)i;
		width = (std::max)(width, Utf8Length(RPMENU_ADJ_OPEN + GetReplayMenuLineText(player, line) + RPMENU_ADJ_CLOSE));
		width = (std::max)(width, Utf8Length(RPMENU_PLAIN_PAD + GetReplayMenuHintText(player, line) + RPMENU_PLAIN_PAD));
	}
	return width;
}

void KZHUDService::RenderReplayMenu(CCSCustomHudLayout *(&layouts)[RPMENU_ENTITIES], const bool (&force)[RPMENU_ENTITIES])
{
	// Свои префы, не мимикрия: GetOwnLayoutPrefs (см. MHUDLayoutPrefs::ReplayMenu).
	const MHUDLayoutPrefs::ReplayMenu &prefs = this->GetOwnLayoutPrefs().replayMenu;
	const ReplayMenuLine selectedLine = (ReplayMenuLine)this->replayMenuLine;
	const size_t width = this->replayMenuWidth;

	// Подложка: тот же моно-шрифт, кегль под шаг строк (1 % экрана = 10.8 px panorama-высоты), чтобы
	// полосы соседних строк смыкались; число NBSP — та же ширина в пикселях, что у текста (моно:
	// ширина ∝ кеглю), без запаса по краям (решение пользователя 10.09: фон не должен торчать вправо).
	const i32 plateSize = panorama::SnapToStep((i32)(prefs.step * 10.8f + 0.5f), LAYOUT_SIZE_MIN, LAYOUT_SIZE_MAX);
	const size_t plateChars = (size_t)((f32)width * (f32)prefs.size / (f32)plateSize + 0.5f);
	std::string plate;
	for (size_t n = 0; n < plateChars; n++)
	{
		plate += RPMENU_PAD;
	}
	const bool plateShown = prefs.background > 0;
	const char *const plateBg = panorama::GetColorEntryBgClass(panorama::FindColorEntry(Color(0, 0, 0, 255)));

	for (i32 i = 0; i < RPMENU_LINES; i++)
	{
		const i32 entity = i / RPMENU_LINES_PER_ENTITY;
		const LayoutElement textSlot = RPMENU_TEXT_SLOTS[i % RPMENU_LINES_PER_ENTITY];
		const LayoutElement plateSlot = RPMENU_PLATE_SLOTS[i % RPMENU_LINES_PER_ENTITY];
		const i32 y = panorama::SnapToStep(prefs.y + LineRow(i) * prefs.step, -100, 100);

		std::string body;
		bool adjustable = false;
		LayoutLabelStyle style;
		style.x = prefs.x;
		style.y = y;
		style.size = prefs.size;
		style.fontClass = prefs.fontClass;
		style.outline = prefs.outline;
		if (i == RPMENU_LINE_TITLE)
		{
			body = KZ::replaysystem::menu::GetReplayMenuTitleText(this->player);
			style.opacity = 100;
			style.color = RPMENU_COLOR_ACCENT;
		}
		else if (i == RPMENU_LINE_STATUS)
		{
			body = KZ::replaysystem::menu::GetReplayMenuStatusText(this->player);
			style.opacity = RPMENU_OPACITY_STATUS;
			style.color = RPMENU_COLOR_TEXT;
		}
		else if (i == RPMENU_LINE_HINT)
		{
			body = KZ::replaysystem::menu::GetReplayMenuHintText(this->player, selectedLine);
			style.opacity = RPMENU_OPACITY_HINT;
			style.color = RPMENU_COLOR_ACCENT;
		}
		else
		{
			const ReplayMenuLine line = (ReplayMenuLine)(i - RPMENU_LINE_ITEM0);
			const bool selected = line == selectedLine;
			body = KZ::replaysystem::menu::GetReplayMenuLineText(this->player, line);
			adjustable = selected && KZ::replaysystem::menu::IsReplayMenuLineAdjustable(line);
			style.opacity = selected ? 100 : RPMENU_OPACITY_IDLE;
			style.color = selected ? RPMENU_COLOR_ACCENT : RPMENU_COLOR_TEXT;
		}
		std::string text = adjustable ? RPMENU_ADJ_OPEN + body + RPMENU_ADJ_CLOSE : RPMENU_PLAIN_PAD + body + RPMENU_PLAIN_PAD;
		Utf8Truncate(text, width);
		for (size_t n = Utf8Length(text); n < width; n++)
		{
			text += RPMENU_PAD;
		}

		// Подложка — ПЕРВОЙ (её слот в разметке раньше, порядок вызова роли не играет, но так
		// читается): чёрный фон, прозрачность из префа, без обводки.
		LayoutLabelStyle plateStyle;
		plateStyle.x = prefs.x;
		plateStyle.y = y;
		plateStyle.size = plateSize;
		plateStyle.fontClass = prefs.fontClass;
		plateStyle.bgClass = plateBg;
		plateStyle.opacity = prefs.background;
		plateStyle.outline = false;
		plateStyle.color = Color(0, 0, 0, 255);
		const LayoutElementDef &plateDef = LAYOUT_ELEMENTS[(i32)plateSlot];
		this->ApplyLayoutLabel(layouts[entity], plateDef.panelId, plateDef.varName, this->replayLines[entity][(i32)plateSlot], plateStyle, plateShown,
							   plate.c_str(), force[entity]);

		const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)textSlot];
		this->ApplyLayoutLabel(layouts[entity], def.panelId, def.varName, this->replayLines[entity][(i32)textSlot], style, true, text.c_str(),
							   force[entity]);
	}
}
