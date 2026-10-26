#include "kz/hud/layout/quickmenu.h"
#include "kz/hud/layout/quickmenu_input.h"
#include "kz/hud/layout/gsmenu.h"
#include "kz/hud/kz_hud.h"
#include "kz/language/kz_language.h"
#include "kz/lead/kz_lead.h"
#include "kz/measure/kz_measure.h"
#include "kz/mode/kz_mode.h"
#include "kz/paint/kz_paint.h"
#include "kz/prac/kz_prac.h"
#include "kz/quiet/kz_quiet.h"
#include "kz/option/kz_option.h"
#include "kz/replays/cyb_replay_download.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "sdk/entity/cbaseplayerweapon.h"
#include "sdk/usercmd.h"
#include "utils/simplecmds.h"
#include "utils/logging.h"
#include "utils/utils.h"
#include "entitykeyvalues.h"
#include "cs2kz.h"

#include <unordered_map>
#include <string>

#include "tier0/memdbgon.h"

// До публикации разметки включается только на локальном стенде/закрытой канарейке.
static CConVar<bool> kz_quickmenu_enable("kz_quickmenu_enable", FCVAR_NONE, "Enable the KZ quick drawer (requires quickmenu addon assets).", false);

namespace
{
	enum class Mode { Closed, Drawer, Paint, Measure };
	struct State
	{
		CHandle<CBaseEntity> entity {};
		Mode mode = Mode::Closed;
		KZ::quickmenu::InputLatch input;
		KZ::quickmenu::HoldGesture hold;
		u64 actions = 0;
		bool continuous = false;
		bool ownsAutoPaint = false;
		bool pendingPaint = false;
		bool shown = false;
		f64 retryAt = 0, lastClick = -1;
		std::string feedback;
		std::unordered_map<std::string, std::string> vars;
		std::unordered_map<std::string, bool> classes;
	};
	State states[MAXPLAYERS];
	constexpr u64 kToolKeys = IN_USE | IN_RELOAD | IN_LOOK_AT_WEAPON;
	constexpr u64 kOwnedKeys = kToolKeys;
	constexpr u64 kMovementKeys = IN_FORWARD | IN_BACK | IN_MOVELEFT | IN_MOVERIGHT | IN_JUMP | IN_DUCK | IN_SPEED | IN_JOYAUTOSPRINT | IN_TURNLEFT | IN_TURNRIGHT;
	static_assert((kOwnedKeys & kMovementKeys) == 0, "quickmenu controls must preserve movement");

	State *Of(KZPlayer *player)
	{
		const i32 slot = player ? player->GetPlayerSlot().Get() : -1;
		return slot >= 0 && slot < MAXPLAYERS ? &states[slot] : nullptr;
	}
	bool Enabled(KZPlayer *player)
	{
		return player && !g_KZPlugin.unloading && kz_quickmenu_enable.Get() && player->IsInGame() && player->IsAlive()
			   && !player->IsFakeClient() && KZHUDService::IsMHUDAvailable();
	}
	bool OtherUi(KZPlayer *player)
	{
		return player->hudService->IsLayoutUiOpen() || player->hudService->IsReplayMenuOpen()
			   || KZ::gsmenu::HidesHud(player) || KZ::gsmenu::ShimIsOpen(player);
	}
	std::string Text(KZPlayer *player, const char *key)
	{
		return KZLanguageService::PrepareMessageWithLang(player->languageService->GetLanguage(), key);
	}
	void Var(KZPlayer *player, State &s, CCSCustomHudLayout *layout, const char *panel, const char *name, const std::string &value)
	{
		const std::string key = std::string(panel) + '|' + name;
		auto it = s.vars.find(key);
		if (it != s.vars.end() && it->second == value) return;
		if (!layout->SetDialogVariableString(panel, name, value.c_str()))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] quickmenu_write_failed kind=var panel=%s name=%s slot=%i\n", panel, name, player->GetPlayerSlot().Get());
			return;
		}
		s.vars[key] = value;
	}
	void Class(KZPlayer *player, State &s, CCSCustomHudLayout *layout, const char *panel, const char *name, bool value)
	{
		const std::string key = std::string(panel) + '|' + name;
		auto it = s.classes.find(key);
		if (it != s.classes.end() && it->second == value) return;
		if (!layout->SetHasClass(panel, name, value ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] quickmenu_write_failed kind=class panel=%s name=%s slot=%i\n", panel, name, player->GetPlayerSlot().Get());
			return;
		}
		s.classes[key] = value;
		layout->GetGlobalLayoutState()->MarkFullChanged();
	}
	CCSCustomHudLayout *Ensure(KZPlayer *player, State &s)
	{
		if (CBaseEntity *ent = s.entity.Get()) return (CCSCustomHudLayout *)ent;
		const f64 now = g_pKZUtils->GetServerGlobals()->curtime;
		if (now < s.retryAt && s.retryAt - now < 3.0) return nullptr;
		s.retryAt = now + 2.0;
		auto layout = utils::CreateEntityByName<CCSCustomHudLayout>("custom_hud_layout");
		if (!layout) return nullptr;
		auto kv = new CEntityKeyValues();
		kv->SetString("layout", "panorama/layout/custom_game/cyber/quickmenu.vxml_c");
		char target[32];
		V_snprintf(target, sizeof(target), "kzquick%i", player->GetPlayerSlot().Get());
		kv->SetString("targetname", target);
		layout->DispatchSpawn(kv);
		s.entity = layout;
		s.vars.clear();
		s.classes.clear();
		return layout;
	}
	void StopPaint(KZPlayer *player, State &s)
	{
		if (s.ownsAutoPaint && player->paintService->IsAutoPaintEnabled()) player->paintService->ToggleAutoPaint();
		s.ownsAutoPaint = false;
	}
	void SetMode(KZPlayer *player, State &s, Mode mode)
	{
		const bool hadInput = s.mode != Mode::Closed;
		s.pendingPaint = false;
		if (s.mode == Mode::Paint && mode != Mode::Paint) StopPaint(player, s);
		if (s.mode == Mode::Measure && mode != Mode::Measure) player->measureService->CancelMeasure(false);
		s.mode = mode;
		s.hold.Cancel((s.input.held & IN_USE) != 0);
		s.input.Transition((hadInput || mode != Mode::Closed) ? kOwnedKeys : 0);
		s.feedback.clear();
		if (auto layout = (CCSCustomHudLayout *)s.entity.Get()) layout->SetInputCaptureEnabled(player->GetPlayerSlot(), mode == Mode::Drawer);
	}
	void Execute(KZPlayer *player, const char *command)
	{
		CCommand args;
		args.Tokenize(command);
		CPlayerSlot slot = player->GetPlayerSlot();
		scmd::OnClientCommand(slot, args);
	}
	void ToolAction(KZPlayer *player, State &s)
	{
		s.actions++;
		if (s.mode == Mode::Paint)
		{
			if (s.continuous)
			{
				if (s.ownsAutoPaint) StopPaint(player, s);
				else
				{
					if (!player->paintService->IsAutoPaintEnabled()) player->paintService->ToggleAutoPaint();
					s.ownsAutoPaint = true;
				}
			}
			// DecalTrace не отправляет событие из PhysicsSimulatePost. Выполняем
			// запрос на ближайшем GameFrame вместе со штатным непрерывным Paint.
			else s.pendingPaint = true;
		}
		else if (s.mode == Mode::Measure)
		{
			auto measure = player->measureService;
			if (measure->startPos.IsValid()) measure->EndMeasure();
			else measure->StartMeasure();
			s.feedback = !measure->startPos.IsValid() && !measure->lastResult.valid ? Text(player, "Quick - No Surface") : "";
		}
	}
	void Render(KZPlayer *player, State &s, CCSCustomHudLayout *layout)
	{
		Class(player, s, layout, "qm_tab", "holding", s.hold.Active());
		Class(player, s, layout, "qm_tab", "hidden", !s.shown || s.mode != Mode::Closed);
		Class(player, s, layout, "qm_drawer", "hidden", !s.shown || s.mode != Mode::Drawer);
		const bool tool = s.mode == Mode::Paint || s.mode == Mode::Measure;
		Class(player, s, layout, "qm_tool", "hidden", !s.shown || !tool);
		if (!s.shown) return;
		if (s.mode == Mode::Drawer)
		{
			Var(player, s, layout, "qm_drawer", "mode", player->modeService->GetModeName());
			struct Toggle { const char *id; bool on; } toggles[] = {
				{"qm_lead", player->leadService->IsBeamEnabled()}, {"qm_practice", player->pracService->IsInPrac()}, {"qm_hide", player->quietService->ShouldHide()}
			};
			for (const auto &toggle : toggles)
			{
				Class(player, s, layout, toggle.id, "on", toggle.on);
				Var(player, s, layout, toggle.id, "value", Text(player, toggle.on ? "Quick - On" : "Quick - Off"));
			}
		}
		if (!tool) return;
		Var(player, s, layout, "qm_tool", "title", s.mode == Mode::Paint ? "PAINT" : "MEASURE");
		std::string action, hint, info, result;
		if (s.mode == Mode::Paint)
		{
			action = Text(player, s.continuous ? (player->paintService->IsAutoPaintEnabled() ? "Quick - Stop Painting" : "Quick - Start Painting") : "Quick - Place Paint");
			hint = Text(player, s.continuous && player->paintService->IsAutoPaintEnabled() ? "Quick - Painting" : "Quick - Aim Surface");
			char colorKey[64], buf[192];
			V_snprintf(colorKey, sizeof(colorKey), "Quick - Color %s", player->paintService->GetColorName());
			V_snprintf(buf, sizeof(buf), "%s · %s %.1f", Text(player, colorKey).c_str(), Text(player, "HUD - Menu Label Size").c_str(), player->paintService->GetSize());
			info = buf;
			Var(player, s, layout, "qt1", "label", Text(player, "Quick - Mode"));
			Var(player, s, layout, "qt1", "value", Text(player, s.continuous ? "Quick - Continuous" : "Quick - Single"));
		}
		else
		{
			auto measure = player->measureService;
			const bool first = !measure->startPos.IsValid();
			action = Text(player, first ? (measure->lastResult.valid ? "Quick - New Measure" : "Quick - Point A") : "Quick - Point B");
			hint = s.feedback.empty() ? Text(player, first ? (measure->lastResult.valid ? "Quick - Measure Result" : "Quick - Aim A") : "Quick - Aim B") : s.feedback;
			info = first ? "A  -  B" : "A  /  B";
			if (measure->lastResult.valid)
			{
				char buf[256];
				V_snprintf(buf, sizeof(buf), "%s %.1f u\n%s %+.1f u\n%s %.1f u", Text(player, "Quick - Horizontal").c_str(), measure->lastResult.horizontal,
					Text(player, "Quick - Height").c_str(), measure->lastResult.vertical, Text(player, "Quick - Effective").c_str(), measure->lastResult.effective);
				result = buf;
			}
			Var(player, s, layout, "qt1", "label", "");
			Var(player, s, layout, "qt1", "value", "");
		}
		Var(player, s, layout, "qm_tool", "hint", hint);
		Var(player, s, layout, "qm_tool", "info", info);
		Var(player, s, layout, "qm_tool", "result", result);
		Class(player, s, layout, "qm_result", "hidden", result.empty());
		Var(player, s, layout, "qt0", "label", action);
		Var(player, s, layout, "qt0", "value", "");
		for (i32 row = 0; row < 3; row++)
		{
			char id[12]; V_snprintf(id, sizeof(id), "qt%i", row);
			Class(player, s, layout, id, "hidden", row >= 2 || (s.mode == Mode::Measure && row == 1));
			Class(player, s, layout, id, "selected", false);
		}
	}
}

void KZ::quickmenu::Open(KZPlayer *player)
{
	State *s = Of(player);
	if (!s || !Enabled(player)) return;
	player->hudService->CloseLayoutMenu();
	KZ::gsmenu::ShimCancel(player);
	KZ::gsmenu::Close(player, "quickmenu");
	auto layout = Ensure(player, *s);
	if (!layout || !layout->GetPlayerLayoutState(player->GetPlayerSlot())) return;
	s->shown = true;
	SetMode(player, *s, Mode::Drawer);
	Render(player, *s, layout);
}

void KZ::quickmenu::Close(KZPlayer *player)
{
	if (State *s = Of(player)) SetMode(player, *s, Mode::Closed);
}

void KZ::quickmenu::Reset(KZPlayer *player)
{
	State *s = Of(player);
	if (!s) return;
	Close(player);
	if (CBaseEntity *ent = s->entity.Get()) g_pKZUtils->RemoveEntity(ent);
	*s = State();
}

void KZ::quickmenu::Cleanup()
{
	for (i32 slot = 0; slot < MAXPLAYERS; slot++)
		if (KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot))) Reset(player);
}

bool KZ::quickmenu::Owns(KZPlayer *player, CEntityHandle handle)
{
	State *s = Of(player);
	return s && s->entity.IsValid() && s->entity.ToInt() == handle.ToInt();
}

void KZ::quickmenu::FlushPaint(KZPlayer *player)
{
	State *s = Of(player);
	if (!s || !s->pendingPaint) return;
	s->pendingPaint = false;
	if (!Enabled(player) || OtherUi(player) || s->mode != Mode::Paint || s->continuous
		|| !s->entity.IsValid()) return;
	player->paintService->PlacePaint();
}

void KZ::quickmenu::Tick(KZPlayer *player)
{
	State *s = Of(player);
	if (!s) return;
	if (!Enabled(player)) { if (s->entity.IsValid() || s->mode != Mode::Closed) Reset(player); return; }
	if (OtherUi(player))
	{
		if (s->mode != Mode::Closed) Close(player);
		s->shown = false;
		s->hold.Cancel((s->input.held & IN_USE) != 0);
		s->input.Transition();
		if (auto layout = (CCSCustomHudLayout *)s->entity.Get()) Render(player, *s, layout);
		return;
	}
	s->shown = true;
	auto layout = Ensure(player, *s);
	if (!layout) return;
	const u64 pressed = s->input.Take();
	if (s->mode == Mode::Closed)
	{
		const bool held = (s->input.held & IN_USE) != 0 && !(s->input.consumedUntilRelease & IN_USE);
		if (s->hold.Update(held, g_pKZUtils->GetServerGlobals()->curtime, .7)) Open(player);
	}
	else if ((s->mode == Mode::Drawer && (pressed & IN_USE)) || (pressed & IN_LOOK_AT_WEAPON))
	{
		Close(player);
	}
	else if (pressed & IN_RELOAD)
	{
		if (s->mode == Mode::Paint || s->mode == Mode::Measure) SetMode(player, *s, Mode::Drawer);
		else if (s->mode == Mode::Drawer) Close(player);
	}
	else if (s->mode == Mode::Paint || s->mode == Mode::Measure)
	{
		if (pressed & IN_USE) ToolAction(player, *s);
	}
	Render(player, *s, layout);
}

bool KZ::quickmenu::OnClick(KZPlayer *player, uint32 packedHandle, const char *id)
{
	State *s = Of(player);
	auto layout = CCSCustomHudLayout::FromClickHandle(packedHandle);
	if (!s || !layout || (CBaseEntity *)layout != s->entity.Get()) return false;
	if (!id || !Enabled(player) || !s->shown) return true;
	if (KZ_STREQ(id, "qm_tab") && s->mode == Mode::Closed) { Open(player); return true; }
	if (s->mode != Mode::Drawer) return true;
	// Клиентские кнопки — строгий белый список, без исполнения присланных строк.
	if (KZ_STREQ(id, "qm_close")) { Close(player); return true; }
	const f64 now = g_pKZUtils->GetServerGlobals()->curtime;
	if (s->lastClick >= 0 && now >= s->lastClick && now - s->lastClick < .15) return true;
	s->lastClick = now;
	if (KZ_STREQ(id, "qm_paint") || KZ_STREQ(id, "qm_paint_single") || KZ_STREQ(id, "qm_paint_auto"))
	{
		if (KZ_STREQ(id, "qm_paint_single")) s->continuous = false;
		if (KZ_STREQ(id, "qm_paint_auto")) s->continuous = true;
		SetMode(player, *s, Mode::Paint);
		// Вход в инструмент не запускает рисование. Уже включённый togglepaint останавливаем
		// осознанным выбором инструмента, не меняя сохранённые параметры цвета/размера.
		if (player->paintService->IsAutoPaintEnabled()) player->paintService->ToggleAutoPaint();
	}
	else if (KZ_STREQ(id, "qm_measure")) { SetMode(player, *s, Mode::Measure); player->measureService->CancelMeasure(true); }
	else if (KZ_STREQ(id, "qm_lead")) player->leadService->Toggle(CybReplayDownload::Kind::AWR);
	else if (KZ_STREQ(id, "qm_practice")) player->pracService->TogglePrac();
	else if (KZ_STREQ(id, "qm_spec")) { Close(player); Execute(player, "kz_spec @me"); }
	else if (KZ_STREQ(id, "qm_hide")) player->quietService->ToggleHide();
	else if (KZ_STREQ(id, "qm_wr")) { Close(player); CybReplayDownload::RequestAndPlay(player, CybReplayDownload::Kind::WR, 0); }
	else if (KZ_STREQ(id, "qm_records")) { Close(player); Execute(player, "kz_maptop"); }
	else if (KZ_STREQ(id, "qm_options")) { Close(player); player->hudService->OpenLayoutMenu(); }
	Render(player, *s, layout);
	return true;
}

void KZ::quickmenu::OnUsercmds(KZPlayer *player, PlayerCommand *cmds, i32 count)
{
	State *s = Of(player);
	if (!s || !cmds || !Enabled(player)) return;
	for (i32 i = 0; i < count; i++)
	{
		auto pc = &cmds[i];
		auto base = pc->mutable_base();
		auto buttons = base->mutable_buttons_pb();
		const u64 held = buttons->buttonstate1();
		const bool otherOwner = OtherUi(player);
		s->input.ObserveCommand(pc->cmdNum, held, kToolKeys, otherOwner);
		const bool active = s->mode != Mode::Closed && !otherOwner;
		// Помощник владеет только E/R/F; мышь и обычное оружие остаются игре.
		// После F удержанные E/R/F погашены вместе с командой отпускания.
		// Чужое меню получает новые нажатия, но не хвост прежнего действия помощника.
		const u64 mask = s->input.SuppressionMask(kOwnedKeys, active);
		if (!mask) continue;
		buttons->set_buttonstate1(buttons->buttonstate1() & ~mask);
		buttons->set_buttonstate2(buttons->buttonstate2() & ~mask);
		buttons->set_buttonstate3(buttons->buttonstate3() & ~mask);
		for (i32 word = 0; word < 3; word++) pc->buttonstates.m_pButtonStates[word] &= ~mask;
		for (i32 j = 0; j < base->subtick_moves_size(); j++)
		{
			auto step = base->mutable_subtick_moves(j);
			if (step->button() & mask)
			{
				const u64 remaining = step->button() & ~mask;
				if (remaining) step->set_button(remaining);
				else step->set_pressed(false);
			}
		}
	}
}

SCMD(kz_quick, SCFL_HUD)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	State *s = Of(player);
	if (s && s->mode != Mode::Closed) KZ::quickmenu::Close(player); else KZ::quickmenu::Open(player);
	return true;
}
SCMD_LINK(kz_qmenu, kz_quick);

// Только собственная консоль: acceptance-проверки не читают чужое состояние и не спамят чат.
SCMD(kz_quickstatus, SCFL_HIDDEN)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	State *s = Of(player);
	if (!player || !s) return true;
	const char *mode = s->mode == Mode::Drawer ? "drawer" : s->mode == Mode::Paint ? "paint" : s->mode == Mode::Measure ? "measure" : "closed";
	auto layout = (CCSCustomHudLayout *)s->entity.Get();
	const bool capture = layout && layout->IsInputCaptureEnabled(player->GetPlayerSlot());
	Vector position {};
	if (player->GetPlayerPawn()) player->GetOrigin(&position);
	auto pawn = player->GetPlayerPawn();
	auto weapons = pawn ? pawn->m_pWeaponServices() : nullptr;
	auto weapon = weapons ? weapons->m_hActiveWeapon().Get() : nullptr;
	const i32 clip = weapon ? weapon->m_iClip1() : -1;
	const Color paint = player->paintService->GetColor();
	utils::PrintConsole(controller,
		"[kz_quickstatus] mode=%s capture=%d continuous=%d autopaint=%d point_a=%d result=%d actions=%llu pos=%.2f/%.2f/%.2f weapon=%s clip=%d paint_rgba=%d/%d/%d/%d\n",
		mode, capture ? 1 : 0, s->continuous ? 1 : 0, player->paintService->IsAutoPaintEnabled() ? 1 : 0,
		player->measureService->startPos.IsValid() ? 1 : 0, player->measureService->lastResult.valid ? 1 : 0,
		(unsigned long long)s->actions, position.x, position.y, position.z, weapon ? weapon->GetClassname() : "none", clip,
		paint.r(), paint.g(), paint.b(), paint.a());
	return true;
}
