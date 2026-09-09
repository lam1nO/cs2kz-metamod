// Меню управления реплеем спектатора на panorama (см. kz_hud.h, раздел «Меню управления
// реплеем»). Семантика пунктов — KZ::replaysystem::menu (replays/menu.cpp), здесь — сущности,
// чтение клавиш и рендер строк.
//
// Почему сущности с разметкой ХУДА, а не страница меню настроек: в чужом аддоне 3469155349
// стили живут по-файлово, и menu.vcss не подключает positions.vcss — список оттуда к левому
// краю не сдвинуть (только #menu_root.shift на −125px). У mhud.vxml четыре текстовых лейбла с
// позицией/цветом/кеглем/прозрачностью классами — из них и собран список. Пунктов шесть плюс
// строка подсказки биндов, поэтому копий страницы ДВЕ (RPMENU_ENTITIES): клиент держит две
// сущности одной страницы раздельно (проверено на канарейке cyb.166: худ и меню жили вместе).
//
// Выравнивание строк: .element центрирует лейбл, значит строки разной длины разъехались бы
// по центру. Моноширинный шрифт + добивка NBSP до одной длины даёт левый край без своего
// css. По той же причине выбранная строка НЕ увеличивается (первый вариант на канарейке уезжал
// началом за экран) — подсветка только цветом и прозрачностью, как в cs2menus.
// Геометрия и шрифт — префы игрока rpmenu* (пункт «Меню реплея» в настройках, RefreshLayoutPrefs).
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

// Автоповтор A/D на удержании (как adjust в cs2menus): период в тиках после 22-тиковой паузы.
// Подбирается живьём: перемотка ±10 с на удержании не должна улетать за секунды.
static CConVar<int> kz_rpmenu_repeat_ticks("kz_rpmenu_repeat_ticks", FCVAR_NONE,
										   "Replay menu (panorama): auto-repeat period of A/D while held, ticks (after a 22-tick delay).", 8);

namespace
{
	// Лейблы одной страницы под строки: сверху вниз. Строка i живёт в сущности i / 4, лейбле i % 4.
	constexpr LayoutElement RPMENU_LINE_SLOTS[] = {LayoutElement::Timer, LayoutElement::Speed, LayoutElement::Prespeed, LayoutElement::Checkpoint};
	constexpr i32 RPMENU_SLOTS_PER_ENTITY = (i32)KZ_ARRAYSIZE(RPMENU_LINE_SLOTS);
	constexpr i32 RPMENU_ITEMS = (i32)ReplayMenuLine::Count;
	constexpr i32 RPMENU_LINES = RPMENU_ITEMS + 1; // + строка подсказки биндов
	static_assert(RPMENU_LINES <= KZHUDService::RPMENU_ENTITIES * RPMENU_SLOTS_PER_ENTITY,
				  "лейблов двух копий страницы должно хватать на пункты и подсказку");

	// Подсветка: выбранная строка — зелёный KZ (#24f097, есть в палитре аддона точно);
	// остальные — белые, приглушённые прозрачностью; подсказка — ещё тише и мельче.
	const Color RPMENU_COLOR_SELECTED(0x24, 0xF0, 0x97, 255);
	const Color RPMENU_COLOR_IDLE(255, 255, 255, 255);
	constexpr i32 RPMENU_OPACITY_IDLE = 55;
	constexpr i32 RPMENU_OPACITY_HINT = 40;

	constexpr i32 RPMENU_REPEAT_DELAY_TICKS = 22;

	// Обрамление: регулируемые строки — «< … >», остальные — NBSP той же ширины, чтобы
	// текст всех строк начинался в одной колонке.
	const char *const RPMENU_ADJ_OPEN = "<\xC2\xA0";  // <NBSP
	const char *const RPMENU_ADJ_CLOSE = "\xC2\xA0>"; // NBSP>
	const char *const RPMENU_PLAIN_PAD = "\xC2\xA0\xC2\xA0";
	const char *const RPMENU_PAD = "\xC2\xA0";

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
	CCSCustomHudLayout *layouts[RPMENU_ENTITIES] {};
	bool created[RPMENU_ENTITIES] {};
	for (i32 i = 0; i < RPMENU_ENTITIES; i++)
	{
		layouts[i] = this->EnsureReplayLayout(i, created[i]);
		if (!layouts[i])
		{
			// Отказ, не выбор: сущность не создалась (схема разъехалась после апдейта CS2 /
			// MultiAddonManager нет) — вызывающий уйдёт на cs2menus, причина в логе. Уже
			// созданную копию не оставляем висеть.
			KZ_LOG_ERROR(LogChannel::General, "[cyb] replay_menu_unavailable reason=layout_entity_failed index=%i slot=%i\n", i,
						 this->player->GetPlayerSlot().Get());
			this->DestroyOwnedReplayLayout();
			return false;
		}
	}
	this->replayMenuOpen = true;
	this->replayMenuLine = 0;
	// Стартовая маска = то, что уже удержано: иначе зажатая при открытии клавиша (например,
	// W из движения перед спектейтом) сработала бы как нажатие на первом же тике.
	this->replayMenuHeld = HeldButtons(this->player);
	this->replayMenuHoldTicks = 0;
	KZ_LOG_INFO(LogChannel::General, "[cyb] replay_menu_open backend=panorama slot=%i\n", this->player->GetPlayerSlot().Get());
	const bool forceAll[RPMENU_ENTITIES] = {true, true};
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
	this->DestroyOwnedReplayLayout();
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

	this->ReadReplayMenuInput();
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

void KZHUDService::RenderReplayMenu(CCSCustomHudLayout *(&layouts)[RPMENU_ENTITIES], const bool (&force)[RPMENU_ENTITIES])
{
	// Свои префы, не мимикрия: GetOwnLayoutPrefs (см. MHUDLayoutPrefs::ReplayMenu).
	const MHUDLayoutPrefs::ReplayMenu &prefs = this->GetOwnLayoutPrefs().replayMenu;

	// Тексты — сперва все, чтобы добить до общей ширины (см. шапку файла про центрирование).
	std::string texts[RPMENU_LINES];
	size_t width = 0;
	for (i32 i = 0; i < RPMENU_ITEMS; i++)
	{
		const ReplayMenuLine line = (ReplayMenuLine)i;
		const std::string label = KZ::replaysystem::menu::GetReplayMenuLineText(this->player, line);
		texts[i] = KZ::replaysystem::menu::IsReplayMenuLineAdjustable(line) ? RPMENU_ADJ_OPEN + label + RPMENU_ADJ_CLOSE
																			: RPMENU_PLAIN_PAD + label + RPMENU_PLAIN_PAD;
		width = (std::max)(width, Utf8Length(texts[i]));
	}
	// Подсказка — тем же кеглем и в той же добивке: только так моно-ячейки совпадают и левый
	// край подсказки встаёт ровно под пунктами (она длиннее любого пункта и задаёт ширину).
	texts[RPMENU_ITEMS] = RPMENU_PLAIN_PAD + KZ::replaysystem::menu::GetReplayMenuHintText(this->player) + RPMENU_PLAIN_PAD;
	width = (std::max)(width, Utf8Length(texts[RPMENU_ITEMS]));
	for (i32 i = 0; i < RPMENU_LINES; i++)
	{
		for (size_t n = Utf8Length(texts[i]); n < width; n++)
		{
			texts[i] += RPMENU_PAD;
		}
	}

	for (i32 i = 0; i < RPMENU_LINES; i++)
	{
		const i32 entity = i / RPMENU_SLOTS_PER_ENTITY;
		const LayoutElement slot = RPMENU_LINE_SLOTS[i % RPMENU_SLOTS_PER_ENTITY];
		const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)slot];
		const bool hint = i == RPMENU_ITEMS;
		const bool selected = !hint && i == this->replayMenuLine;

		LayoutLabelStyle style;
		style.x = prefs.x;
		// Подсказка — с дополнительным зазором в один шаг от списка.
		style.y = panorama::SnapToStep(prefs.y + i * prefs.step + (hint ? prefs.step : 0), -100, 100);
		style.size = prefs.size;
		style.fontClass = prefs.fontClass;
		style.opacity = hint ? RPMENU_OPACITY_HINT : (selected ? 100 : RPMENU_OPACITY_IDLE);
		style.outline = true;
		style.color = selected ? RPMENU_COLOR_SELECTED : RPMENU_COLOR_IDLE;
		this->ApplyLayoutLabel(layouts[entity], def.panelId, def.varName, this->replayLines[entity][(i32)slot], style, true, texts[i].c_str(),
							   force[entity]);
	}
}
