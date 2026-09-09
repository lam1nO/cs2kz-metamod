// Меню управления реплеем спектатора на panorama (см. kz_hud.h, раздел «Меню управления
// реплеем»). Семантика пунктов — KZ::replaysystem::menu (replays/menu.cpp), здесь — сущность,
// чтение клавиш и рендер строк.
//
// Почему третья сущность с разметкой ХУДА, а не страница меню настроек: в чужом аддоне
// 3469155349 стили живут по-файлово, и menu.vcss не подключает positions.vcss — список
// оттуда к левому краю не сдвинуть (только #menu_root.shift на −125px). У mhud.vxml четыре
// текстовых лейбла с позицией/цветом/кеглем/прозрачностью классами — из них и собран список,
// а блок клавиш той же страницы (6 кнопок с классом pressed) подсвечивает собственные WASD
// спектатора (hide-idle: видна только нажатая).
//
// Выравнивание строк: .element центрирует лейбл, значит строки разной длины разъехались бы
// по центру. Моноширинный шрифт + добивка NBSP до одной длины даёт левый край без своего
// css. Позиции/кегль/шрифт вынесены в cvar'ы: смысл процентов panorama проверяется только
// живьём на канарейке, пересобирать плагин ради подгонки нельзя.
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

#include <algorithm>

#include "tier0/memdbgon.h"

using KZ::replaysystem::menu::ReplayMenuInput;
using KZ::replaysystem::menu::ReplayMenuLine;

// Геометрия — проценты от центра экрана (та же сетка, что у элементов худа, ±100 шаг 1).
// Дефолты «глубоко влево, по центру по вертикали» подбираются на канарейке через rcon.
static CConVar<int> kz_rpmenu_x("kz_rpmenu_x", FCVAR_NONE, "Replay menu (panorama): X of the lines, percent from screen center.", -42);
static CConVar<int> kz_rpmenu_y("kz_rpmenu_y", FCVAR_NONE, "Replay menu (panorama): Y of the first line, percent from screen center.", -8);
static CConVar<int> kz_rpmenu_step("kz_rpmenu_step", FCVAR_NONE, "Replay menu (panorama): vertical step between lines, percent.", 4);
static CConVar<int> kz_rpmenu_size("kz_rpmenu_size", FCVAR_NONE, "Replay menu (panorama): font size of a line, px (selected line is +4).", 18);
static CConVar<int> kz_rpmenu_repeat_ticks("kz_rpmenu_repeat_ticks", FCVAR_NONE,
										   "Replay menu (panorama): auto-repeat period of A/D while held, ticks (after a 22-tick delay).", 8);
static CConVar<CUtlString> kz_rpmenu_font("kz_rpmenu_font", FCVAR_NONE,
										  "Replay menu (panorama): font slug (see !hudmenu fonts). Monospace keeps the left edge aligned.",
										  "stratum2-mono");

namespace
{
	// Порядок лейблов под строки меню: сверху вниз. Keys — отдельный блок под списком.
	constexpr LayoutElement RPMENU_LINE_SLOTS[] = {LayoutElement::Timer, LayoutElement::Speed, LayoutElement::Prespeed, LayoutElement::Checkpoint};
	constexpr i32 RPMENU_LINES = (i32)KZ_ARRAYSIZE(RPMENU_LINE_SLOTS);
	static_assert(RPMENU_LINES == (i32)ReplayMenuLine::Count, "строк разметки должно хватать на все пункты меню");

	// Подсветка: выбранная строка — зелёный KZ (#24f097, есть в палитре аддона точно), крупнее;
	// остальные — белые, приглушённые прозрачностью.
	const Color RPMENU_COLOR_SELECTED(0x24, 0xF0, 0x97, 255);
	const Color RPMENU_COLOR_IDLE(255, 255, 255, 255);
	constexpr i32 RPMENU_OPACITY_IDLE = 55;

	// Автоповтор A/D на удержании (как adjust в cs2menus): пауза перед первым повтором, тики.
	// Период — cvar kz_rpmenu_repeat_ticks (подбирается живьём: перемотка ±10 с на удержании
	// не должна улетать за секунды).
	constexpr i32 RPMENU_REPEAT_DELAY_TICKS = 22;

	// Панели блока клавиш (порядок C W J A S D — тот же, что KEY_PANELS в mhud.cpp; C и J у
	// спектатора никогда не «нажаты» и под hide-idle не видны).
	const char *const RPMENU_KEY_PANELS[] = {"mhud_key_c", "mhud_key_w", "mhud_key_j", "mhud_key_a", "mhud_key_s", "mhud_key_d"};
	constexpr InputBitMask_t RPMENU_KEY_BUTTONS[] = {(InputBitMask_t)0, IN_FORWARD, (InputBitMask_t)0, IN_MOVELEFT, IN_BACK, IN_MOVERIGHT};

	// Маркеры выбора одинаковой ширины (моно): «» » у выбранной, два NBSP у остальных.
	const char *const RPMENU_MARK_SELECTED = "\xC2\xBB\xC2\xA0"; // »NBSP
	const char *const RPMENU_MARK_IDLE = "\xC2\xA0\xC2\xA0";     // NBSP NBSP
	const char *const RPMENU_PAD = "\xC2\xA0";                   // NBSP

	// Длина в кодовых точках UTF-8 — добивка строк до одной ширины в моно-шрифте.
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

// === Доступность / открытие / закрытие ======================================================

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

bool KZHUDService::CanOpenReplayMenu()
{
	return this->ReplayMenuUnavailableReason() == NULL;
}

// Сколько тиков ждать фактического спектейта бота (64 тика/с → ~3 с). Смена команды
// применяется движком в пределах одного-двух кадров, запас — на ретрансляцию/лаг.
static constexpr i32 KZ_RPMENU_PENDING_TICKS = 192;

void KZHUDService::RequestReplayMenu()
{
	this->replayMenuPending = true;
	this->replayMenuPendingTicks = 0;
}

void KZHUDService::TickReplayMenuPending()
{
	if (!this->replayMenuPending)
	{
		return;
	}
	const char *reason = this->ReplayMenuUnavailableReason();
	if (!reason)
	{
		this->replayMenuPending = false;
		// Уже открытое cs2menus-!rpmenu гасим ДО panorama — клавиши не должны уходить в оба.
		KZ::replaysystem::menu::CancelReplayControlsMenuCs2menus(this->player);
		if (!this->OpenReplayMenu())
		{
			// Причина уже в логе OpenReplayMenu (layout_entity_failed) — отдаём cs2menus.
			KZ::replaysystem::menu::OpenReplayControlsMenuCs2menus(this->player);
		}
		return;
	}
	if (KZ_STREQ(reason, "not_spectating_bot") && ++this->replayMenuPendingTicks <= KZ_RPMENU_PENDING_TICKS)
	{
		return; // спектейт ещё применяется — ждём
	}
	this->replayMenuPending = false;
	if (KZ_STREQ(reason, "no_replay") || KZ_STREQ(reason, "unloading"))
	{
		// Реплей кончился/остановлен за окно ожидания (или плагин выгружается) — управлять
		// нечем, cs2menus-меню «из ниоткуда» спустя секунды было бы ошибкой. Только лог.
		KZ_LOG_INFO(LogChannel::General, "[cyb] replay_menu_fallback backend=none reason=%s waited_ticks=%i slot=%i\n", reason,
					this->replayMenuPendingTicks, this->player->GetPlayerSlot().Get());
		return;
	}
	// Не дождались спектейта (или аддон пропал) — фолбэк на cs2menus, причина в лог: иначе
	// «почему у меня старое меню» не отличить от бага выбора бэкенда.
	KZ_LOG_INFO(LogChannel::General, "[cyb] replay_menu_fallback backend=cs2menus reason=%s waited_ticks=%i slot=%i\n", reason,
				this->replayMenuPendingTicks, this->player->GetPlayerSlot().Get());
	KZ::replaysystem::menu::OpenReplayControlsMenuCs2menus(this->player);
}

bool KZHUDService::OpenReplayMenu()
{
	if (this->replayMenuOpen)
	{
		return true;
	}
	if (!this->CanOpenReplayMenu())
	{
		return false;
	}
	bool created = false;
	CCSCustomHudLayout *layout = this->EnsureReplayLayout(created);
	if (!layout)
	{
		// Отказ, не выбор: сущность не создалась (схема разъехалась после апдейта CS2 /
		// MultiAddonManager нет) — вызывающий уйдёт на cs2menus, причина в логе.
		KZ_LOG_ERROR(LogChannel::General, "[cyb] replay_menu_unavailable reason=layout_entity_failed slot=%i\n", this->player->GetPlayerSlot().Get());
		return false;
	}
	this->replayMenuOpen = true;
	this->replayMenuLine = 0;
	// Стартовая маска = то, что уже удержано: иначе зажатая при открытии клавиша (например,
	// W из движения перед спектейтом) сработала бы как нажатие на первом же тике.
	this->replayMenuHeld = HeldButtons(this->player);
	this->replayMenuHoldTicks = 0;
	KZ_LOG_INFO(LogChannel::General, "[cyb] replay_menu_open backend=panorama slot=%i\n", this->player->GetPlayerSlot().Get());
	this->RenderReplayMenu(layout, /* force */ true);
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
	this->DestroyOwnedReplayLayout();
}

// === Сущность ==============================================================================

CCSCustomHudLayout *KZHUDService::EnsureReplayLayout(bool &created)
{
	created = false;
	if (g_KZPlugin.unloading || !KZHUDService::IsMHUDAvailable())
	{
		return NULL;
	}
	if (CBaseEntity *cached = this->ownedReplayLayout.Get())
	{
		return (CCSCustomHudLayout *)cached;
	}
	CCSCustomHudLayout *layout = utils::CreateEntityByName<CCSCustomHudLayout>("custom_hud_layout");
	if (!layout)
	{
		return NULL;
	}
	CEntityKeyValues *pKeyValues = new CEntityKeyValues();
	// Та же разметка, что у худа (см. шапку файла) — вторая копия страницы у одного клиента.
	pKeyValues->SetString("layout", KZ_MHUD_LAYOUT);
	char name[32];
	V_snprintf(name, sizeof(name), "kzrpmenu%i", this->player->GetPlayerSlot().Get());
	pKeyValues->SetString("targetname", name);
	layout->DispatchSpawn(pKeyValues);
	this->ownedReplayLayout = layout;
	created = true;
	// Диф-кэш — состояние ПРЕДЫДУЩЕЙ сущности, сбрасываем в момент реального создания
	// (тот же урок, что у EnsureMenuLayout).
	for (i32 i = 0; i < (i32)LayoutElement::Count; i++)
	{
		this->replayLines[i] = LayoutElementState();
	}
	this->replayKeys = LayoutKeysState();
	this->replayKeysStyled = false;
	return layout;
}

void KZHUDService::DestroyOwnedReplayLayout()
{
	if (!this->ownedReplayLayout.IsValid())
	{
		return;
	}
	if (CBaseEntity *ent = this->ownedReplayLayout.Get())
	{
		g_pKZUtils->RemoveEntity(ent);
	}
	this->ownedReplayLayout = nullptr;
	for (i32 i = 0; i < (i32)LayoutElement::Count; i++)
	{
		this->replayLines[i] = LayoutElementState();
	}
	this->replayKeys = LayoutKeysState();
	this->replayKeysStyled = false;
}

// === Тик: ввод + рендер ====================================================================

void KZHUDService::UpdateReplayMenu(KZPlayer *source)
{
	if (!this->replayMenuOpen)
	{
		return;
	}
	// Меню имеет смысл только пока игрок наблюдает бота с идущим плейбеком: ушёл на другого
	// игрока / ожил / реплей кончился или остановлен — закрываем сами, без команды.
	if (!source || source == this->player || !KZ::replaysystem::IsReplayBot(source) || !KZ::replaysystem::data::IsReplayPlaying())
	{
		this->CloseReplayMenu(!KZ::replaysystem::data::IsReplayPlaying() ? "replay_stopped" : "target_changed");
		return;
	}
	bool created = false;
	CCSCustomHudLayout *layout = this->EnsureReplayLayout(created);
	if (!layout)
	{
		this->CloseReplayMenu("layout_entity_failed");
		return;
	}

	this->ReadReplayMenuInput();
	// Ввод мог остановить реплей («Завершить») — тогда меню уже закрыто и сущности нет.
	if (!this->replayMenuOpen)
	{
		return;
	}
	if (!KZ::replaysystem::data::IsReplayPlaying())
	{
		this->CloseReplayMenu("replay_stopped");
		return;
	}
	this->RenderReplayMenu(layout, created);
}

void KZHUDService::ReadReplayMenuInput()
{
	const u64 held = HeldButtons(this->player);
	const u64 pressed = held & ~this->replayMenuHeld;
	this->replayMenuHeld = held;

	if (pressed & IN_FORWARD)
	{
		this->replayMenuLine = (this->replayMenuLine + RPMENU_LINES - 1) % RPMENU_LINES;
	}
	if (pressed & IN_BACK)
	{
		this->replayMenuLine = (this->replayMenuLine + 1) % RPMENU_LINES;
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
	// перемотка на удержании — тот же жест, что в cs2menus).
	const u64 adjust = held & (IN_MOVELEFT | IN_MOVERIGHT);
	if (!adjust || adjust == (IN_MOVELEFT | IN_MOVERIGHT))
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

void KZHUDService::RenderReplayMenu(CCSCustomHudLayout *layout, bool force)
{
	const char *fontClass = panorama::ResolveFontClass(kz_rpmenu_font.Get().Get(), LAYOUT_DEFAULT_FONT);
	const i32 x = panorama::SnapToStep(kz_rpmenu_x.Get(), -100, 100);
	const i32 y0 = kz_rpmenu_y.Get();
	const i32 step = kz_rpmenu_step.Get();
	const i32 size = panorama::SnapToStep(kz_rpmenu_size.Get(), LAYOUT_SIZE_MIN, LAYOUT_SIZE_MAX);

	// Тексты — сперва все, чтобы добить до общей ширины (см. шапку файла про центрирование).
	std::string texts[RPMENU_LINES];
	size_t width = 0;
	for (i32 i = 0; i < RPMENU_LINES; i++)
	{
		texts[i] = KZ::replaysystem::menu::GetReplayMenuLineText(this->player, (ReplayMenuLine)i);
		width = (std::max)(width, Utf8Length(texts[i]));
	}

	for (i32 i = 0; i < RPMENU_LINES; i++)
	{
		const bool selected = i == this->replayMenuLine;
		std::string text = (selected ? RPMENU_MARK_SELECTED : RPMENU_MARK_IDLE) + texts[i];
		for (size_t n = Utf8Length(texts[i]); n < width; n++)
		{
			text += RPMENU_PAD;
		}

		const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)RPMENU_LINE_SLOTS[i]];
		LayoutLabelStyle style;
		style.x = x;
		style.y = panorama::SnapToStep(y0 + i * step, -100, 100);
		style.size = selected ? panorama::SnapToStep(size + 4, LAYOUT_SIZE_MIN, LAYOUT_SIZE_MAX) : size;
		style.fontClass = fontClass;
		style.opacity = selected ? 100 : RPMENU_OPACITY_IDLE;
		style.outline = true;
		style.color = selected ? RPMENU_COLOR_SELECTED : RPMENU_COLOR_IDLE;
		this->ApplyLayoutLabel(layout, def.panelId, def.varName, this->replayLines[(i32)RPMENU_LINE_SLOTS[i]], style, true, text.c_str(), force);
	}

	// Блок клавиш под списком: контейнер — через ту же машинерию (позиция/кегль/цвет), сами
	// кнопки — классом pressed по СОБСТВЕННЫМ кнопкам спектатора (не наблюдаемого, как в худе).
	const LayoutElementDef &keysDef = LAYOUT_ELEMENTS[(i32)LayoutElement::Keys];
	LayoutLabelStyle keysStyle;
	keysStyle.x = x;
	keysStyle.y = panorama::SnapToStep(y0 + RPMENU_LINES * step + 4, -100, 100);
	keysStyle.size = size;
	keysStyle.fontClass = fontClass;
	keysStyle.opacity = 100;
	keysStyle.outline = false;
	keysStyle.color = RPMENU_COLOR_SELECTED;
	this->ApplyLayoutLabel(layout, keysDef.panelId, keysDef.varName, this->replayLines[(i32)LayoutElement::Keys], keysStyle, true, NULL, force);

	if (force || !this->replayKeysStyled)
	{
		// Статика блока: буквы вместо стрелок и «видна только нажатая» (иначе C/J висели бы
		// пустыми рамками как ложная подсказка биндов).
		this->replayKeys = LayoutKeysState();
		const char *statics[] = {"keys-letters", "hide-idle"};
		for (const char *cls : statics)
		{
			if (!layout->SetHasClass(keysDef.panelId, cls, k_eHudPanelClassStatus_HasClass))
			{
				LogHudInternFailure(this->player, keysDef.panelId, cls);
			}
		}
		this->replayKeysStyled = true;
	}
	// Кегль кнопок/глифов и шрифт — тем же хелпером, что у худа: font-size не наследуется
	// детьми, класс на контейнере буквы бы не уменьшил.
	this->ApplyKeysSizing(layout, this->replayKeys, size, fontClass);
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(RPMENU_KEY_PANELS); i++)
	{
		const bool down = RPMENU_KEY_BUTTONS[i] != 0 && (this->replayMenuHeld & RPMENU_KEY_BUTTONS[i]) != 0;
		if (this->replayKeys.pressed[i] == down)
		{
			continue;
		}
		this->replayKeys.pressed[i] = down;
		if (!layout->SetHasClass(RPMENU_KEY_PANELS[i], "pressed", down ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
		{
			LogHudInternFailure(this->player, RPMENU_KEY_PANELS[i], "pressed");
		}
	}
}
