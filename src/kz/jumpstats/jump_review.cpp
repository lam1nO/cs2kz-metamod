#include "kz/jumpstats/jump_review.h"
#include "kz/jumpstats/jump_review_model.h"
#include "kz/jumpstats/kz_jumpstats.h"
#include "kz/jumpstats/jump_direction.h"
#include "kz/hud/kz_hud.h"
#include "kz/hud/layout/gsmenu.h"
#include "kz/hud/layout/quickmenu.h"
#include "kz/hud/layout/quickmenu_input.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
#include "kz/option/kz_option.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "sdk/entity/cbaseplayerweapon.h"
#include "sdk/usercmd.h"
#include "utils/simplecmds.h"
#include "utils/utils.h"
#include "utils/logging.h"
#include "entitykeyvalues.h"
#include "cs2kz.h"
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <cstdarg>
#include <cstdio>

#include "tier0/memdbgon.h"

static CConVar<bool> kz_jumpreview_enable("kz_jumpreview_enable", FCVAR_NONE, "Enable jump review HUD (requires local/published jumpreview assets).",
										  false);

namespace KZ::jumpreview
{
	namespace
	{
		constexpr uint32 kAll = (1u << 30) - 1;
		constexpr uint32 kDefault = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 10) | (1u << 28);
		constexpr uint32 kHalf = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9)
								 | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 28);
		const char *const labels[30] = {"Type",        "Mode",
										"Styles",      "Distance",
										"Sync",        "Pre",
										"Max",         "Strafes",
										"Airtime",     "GainEff",
										"Airpath",     "Width",
										"BA",          "OL",
										"DA",          "Block",
										"Edge",        "LandEdge",
										"Miss",        "Deviation",
										"Offset",      "W",
										"Crouched",    "Height",
										"Status",      "Invalidation reason",
										"Strafe keys", "Mouse movement",
										"Trajectory",  "JumpDirection"};

		struct StrafeView
		{
			std::array<std::string, 9> cells;
			std::array<std::string, 11> details;
		};

		struct Snapshot
		{
			std::array<std::string, 30> fields;
			std::array<std::string, 4> graph;
			std::vector<StrafeView> strafes;
			std::vector<Point> points;
			std::vector<Span> spans;
			double created = 0;
			std::string miniDistance;
		};

		struct State
		{
			CHandle<CBaseEntity> entity;
			std::shared_ptr<Snapshot> snap, pending;
			quickmenu::InputLatch input;
			std::unordered_map<std::string, std::string> vars;
			std::unordered_map<std::string, bool> classes;
			bool open = false, settings = false, dirty = true, mini = false, writeFailed = false;
			int selected = 0, page = 0, miniPlotHeight = 240;
			double retry = 0, lastClick = -1;
		};

		State states[MAXPLAYERS];

		State *Of(KZPlayer *p)
		{
			int n = p ? p->GetPlayerSlot().Get() : -1;
			return n >= 0 && n < MAXPLAYERS ? &states[n] : nullptr;
		}

		double Now()
		{
			return g_pKZUtils->GetServerGlobals()->curtime;
		}

		std::string Format(const char *fmt, ...)
		{
			char b[512];
			va_list a;
			va_start(a, fmt);
			vsnprintf(b, sizeof(b), fmt, a);
			va_end(a);
			return b;
		}

		std::string F(float v, int precision = 2, const char *suffix = "")
		{
			return std::isfinite(v) ? Format("%.*f%s", precision, static_cast<double>(v), suffix) : "—";
		}

		bool Available(KZPlayer *p)
		{
			return p && !g_KZPlugin.unloading && kz_jumpreview_enable.Get() && p->IsInGame() && !p->IsFakeClient() && KZHUDService::IsMHUDAvailable();
		}

		bool OtherUi(KZPlayer *p)
		{
			return p->hudService->IsLayoutUiOpen() || p->hudService->IsReplayMenuOpen() || quickmenu::IsOpen(p) || gsmenu::HidesHud(p)
				   || gsmenu::ShimIsOpen(p);
		}

		bool Knife(KZPlayer *p)
		{
			auto pawn = p->GetPlayerPawn();
			auto ws = pawn ? pawn->m_pWeaponServices() : nullptr;
			auto w = ws ? ws->m_hActiveWeapon().Get() : nullptr;
			const char *name = w ? w->GetClassname() : "";
			return V_strncmp(name, "weapon_knife", 12) == 0 || KZ_STREQ(name, "weapon_bayonet");
		}

		uint32 Mask(KZPlayer *p)
		{
			return static_cast<uint32>(p->optionService->GetPreferenceInt("jsReviewFields", kDefault)) & kAll;
		}

		int Duration(KZPlayer *p)
		{
			return std::clamp(static_cast<int>(p->optionService->GetPreferenceInt("jsReviewDuration", 6)), 1, 30);
		}

		int Scale(KZPlayer *p)
		{
			int s = static_cast<int>(p->optionService->GetPreferenceInt("jsReviewScale", 100));
			return s <= 80 ? 80 : s >= 120 ? 120 : 100;
		}

		void Destroy(KZPlayer *p, State &s)
		{
			if (auto e = s.entity.Get())
			{
				auto l = static_cast<CCSCustomHudLayout *>(e);
				l->SetInputCaptureEnabled(p->GetPlayerSlot(), false);
				g_pKZUtils->RemoveEntity(e);
			}
			s.entity = nullptr;
			s.vars.clear();
			s.classes.clear();
			s.dirty = true;
			s.mini = false;
			s.writeFailed = false;
		}

		void WriteFailure(State &s, const std::string &id)
		{
			if (!s.writeFailed)
			{
				KZ_LOG_WARN(LogChannel::General, "[cyb] jump_review_write_failed reason=layout_state_rejected handle=%u panel=%s\n",
							static_cast<unsigned>(s.entity.ToInt()), id.c_str());
			}
			s.writeFailed = true;
		}

		bool SetClass(State &s, CCSCustomHudLayout *l, const std::string &id, const std::string &name, bool on)
		{
			std::string key = id + '|' + name;
			auto it = s.classes.find(key);
			if (it != s.classes.end() && it->second == on)
			{
				return true;
			}
			// Эти классы отсутствуют в исходном XML. Не передаём пять false
			// цветов на каждый сегмент при первом показе траектории.
			if (!on && it == s.classes.end()
				&& (name == "good" || name == "bad" || name == "overlap" || name == "dead" || name == "unknown" || name == "dim"))
			{
				return true;
			}
			if (!l->SetHasClass(id.c_str(), name.c_str(), on ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
			{
				WriteFailure(s, id);
				return false;
			}
			s.classes[key] = on;
			return true;
		}

		void Var(State &s, CCSCustomHudLayout *l, const std::string &id, const std::string &v)
		{
			auto it = s.vars.find(id);
			if (it != s.vars.end() && it->second == v)
			{
				return;
			}
			if (l->SetDialogVariableString(id.c_str(), "v", v.c_str()))
			{
				s.vars[id] = v;
			}
			else
			{
				WriteFailure(s, id);
			}
		}

		void ValueClass(State &s, CCSCustomHudLayout *l, const std::string &id, const char *prefix, int value)
		{
			std::string key = id + "|@" + prefix, newClass = std::string(prefix) + std::to_string(value);
			auto it = s.vars.find(key);
			if (it != s.vars.end() && it->second == newClass)
			{
				return;
			}
			if (it != s.vars.end())
			{
				SetClass(s, l, id, it->second, false);
			}
			if (SetClass(s, l, id, newClass, true))
			{
				s.vars[key] = newClass;
			}
		}

		CCSCustomHudLayout *Ensure(KZPlayer *p, State &s)
		{
			if (auto e = s.entity.Get())
			{
				return static_cast<CCSCustomHudLayout *>(e);
			}
			double now = Now();
			if (now < s.retry && s.retry - now < 3)
			{
				return nullptr;
			}
			s.retry = now + 2;
			auto l = utils::CreateEntityByName<CCSCustomHudLayout>("custom_hud_layout");
			if (!l)
			{
				return nullptr;
			}
			auto kv = new CEntityKeyValues();
			kv->SetString("layout", "panorama/layout/custom_game/cyber/jumpreview.vxml_c");
			kv->SetString("targetname", Format("kzjr%i", p->GetPlayerSlot().Get()).c_str());
			l->DispatchSpawn(kv);
			if (!l->GetPlayerLayoutState(p->GetPlayerSlot()))
			{
				g_pKZUtils->RemoveEntity(l);
				return nullptr;
			}
			s.entity = l;
			s.vars.clear();
			s.classes.clear();
			s.dirty = true;
			return l;
		}

		Shade Classify(const AACall &a)
		{
			if (a.wishspeed == 0)
			{
				uint64 buttons[3] = {a.buttons[0], a.buttons[1], a.buttons[2]};
				bool ol =
					(CInButtonState::IsButtonPressed(buttons, IN_FORWARD, true) && CInButtonState::IsButtonPressed(buttons, IN_BACK, true))
					|| (CInButtonState::IsButtonPressed(buttons, IN_MOVELEFT, true) && CInButtonState::IsButtonPressed(buttons, IN_MOVERIGHT, true));
				return ol ? Shade::Overlap : Shade::Dead;
			}
			if ((a.velocityPost - a.velocityPre).Length2D() <= JS_EPSILON)
			{
				return Shade::Bad;
			}
			return a.velocityPost.Length2D() - a.velocityPre.Length2D() > JS_EPSILON ? Shade::Good : Shade::Bad;
		}

		std::shared_ptr<Snapshot> Capture(KZPlayer *p, Jump *j)
		{
			auto out = std::make_shared<Snapshot>();
			out->created = Now();
			auto &v = out->fields;
			v.fill("—");
			out->miniDistance = F(j->GetDistance());
			auto type = j->GetReportJumpType();
			v[0] = type >= 0 && type < JUMPTYPE_COUNT ? jumpTypeStr[type] : "Invalid";
			if (j->IsFailstat())
			{
				v[0] += " (Failstat)";
			}
			v[1] = p->modeService->GetModeShortName();
			v[2] = "";
			FOR_EACH_VEC(p->styleServices, i)
			{
				if (!v[2].empty())
				{
					v[2] += " + ";
				}
				v[2] += p->styleServices[i]->GetStyleShortName();
			}
			if (v[2].empty())
			{
				v[2] = "—";
			}
			v[3] = F(j->GetDistance(), 4, " units");
			v[4] = F(j->GetSync() * 100, 2, "%");
			v[5] = F(j->GetTakeoffSpeed());
			v[6] = F(j->GetMaxSpeed());
			v[7] = Format("%i", j->GetStrafeCount());
			v[8] = F(j->airtime, 6, " s");
			v[9] = F(j->GetGainEfficiency() * 100, 2, "%");
			v[10] = F(j->GetAirPath(), 6);
			v[11] = F(j->GetWidth(), 2, "°");
			v[12] = F(j->GetBadAngles() * 100, 2, "%");
			v[13] = F(j->GetOverlap() * 100, 2, "%");
			v[14] = F(j->GetDeadAir() * 100, 2, "%");
			if (j->GetBlock() > 0)
			{
				v[15] = F(j->GetBlock(), 0);
			}
			if (j->GetEdge(false) >= 0)
			{
				v[16] = F(j->GetEdge(false), 4);
			}
			if (j->GetEdge(true) >= 0)
			{
				v[17] = F(j->GetEdge(true), 4);
			}
			v[18] = F(j->GetMiss(), 4);
			v[19] = F(j->GetDeviation(), 4);
			v[20] = F(j->GetOffset(), 4);
			v[21] = F(j->GetReleaseInTick(), 1, " ticks");
			v[22] = F(j->GetDuckTime(true), 2) + "/" + F(j->GetDuckTime(false), 2) + " s";
			v[23] = F(j->GetMaxHeight(), 4);
			v[24] = j->IsValid() ? "Valid" : j->IsFailstat() ? "Failstat" : "Invalid";
			if (j->invalidateReason[0])
			{
				v[25] = j->GetInvalidationReasonString(j->invalidateReason, "en");
			}
			v[29] = jumpstats::direction::Name(j->GetDirection());
			j->BuildConsoleStrafeMouseGraph(out->graph[0], out->graph[1], out->graph[2], out->graph[3]);
			FOR_EACH_VEC(j->strafes, i)
			{
				auto &a = j->strafes[i];
				StrafeView sv;
				float dur = a.GetStrafeDuration(), max = a.GetMaxGain();
				std::string avg = dur > 0 ? F(a.GetGain() / dur * ENGINE_FIXED_TICK_INTERVAL) : "—",
							ge = max > 0 ? F(a.GetGain() / max * 100, 2, "%") : "—";
				sv.cells = {Format("%i.", i + 1),
							F(a.GetSync() * 100, 0, "%"),
							F(a.GetGain()),
							"−" + F(fabsf(a.GetLoss())),
							F(a.GetStrafeMaxSpeed()),
							F(dur, 3),
							F(fabsf(a.GetWidth()), 1, "°"),
							avg,
							ge};
				sv.details = {F(a.GetGain()) + " (+" + F(fabsf(a.GetGain(true))) + ")",
							  "−" + F(fabsf(a.GetLoss())) + " (−" + F(fabsf(a.GetLoss(true))) + ")",
							  F(a.GetStrafeMaxSpeed()),
							  F(dur, 6, " s"),
							  F(a.GetBadAngleDuration() * ENGINE_FIXED_TICK_RATE, 2, " ticks"),
							  F(a.GetOverlapDuration() * ENGINE_FIXED_TICK_RATE, 2, " ticks"),
							  F(a.GetDeadAirDuration() * ENGINE_FIXED_TICK_RATE, 2, " ticks"),
							  F(fabsf(a.GetWidth()), 2, "°"),
							  avg,
							  ge,
							  a.arStats.available ? F(a.arStats.average) + " / " + F(a.arStats.median) + " / " + F(a.arStats.max) : "—"};
				out->strafes.push_back(std::move(sv));
				FOR_EACH_VEC(a.aaCalls, k)
				{
					const auto &aa = a.aaCalls[k];
					if (aa.reviewStart >= 0 && aa.duration > 0 && std::isfinite(aa.duration))
					{
						out->spans.push_back({aa.reviewStart, aa.reviewStart + aa.duration, Classify(aa), i});
					}
				}
			}
			float end = j->airtime;
			if (!(end > 0) || !std::isfinite(end))
			{
				end = .001f;
			}
			out->points.push_back({j->takeoffOrigin.x, j->takeoffOrigin.y, 0});
			for (int n = 0; n < j->poseCount; n++)
			{
				int k = (j->poseIndex - j->poseCount + 1 + n + JS_FAILSTATS_MAX_TRACKED_TICKS) % JS_FAILSTATS_MAX_TRACKED_TICKS;
				const auto &q = j->poseHistory[k];
				if (q.reviewTime > 0 && q.reviewTime < end)
				{
					out->points.push_back({q.position.x, q.position.y, q.reviewTime});
				}
			}
			out->points.push_back({j->landingOrigin.x, j->landingOrigin.y, end});
			return out;
		}

		const char *ShadeName(Shade shade)
		{
			switch (shade)
			{
				case Shade::Good:
					return "good";
				case Shade::Bad:
					return "bad";
				case Shade::Overlap:
					return "overlap";
				case Shade::Dead:
					return "dead";
				default:
					return "unknown";
			}
		}

		void DrawLine(State &s, CCSCustomHudLayout *l, bool mini)
		{
			const int cap = mini ? 96 : 192;
			auto lines = Project(s.snap->points, s.snap->spans, mini ? 128.f : 224.f, mini ? static_cast<float>(s.miniPlotHeight) : 370.f, cap);
			for (int i = 0; i < cap; i++)
			{
				std::string id = Format(mini ? "jr_m_seg%i" : "jr_seg%i", i);
				bool used = i < static_cast<int>(lines.size());
				SetClass(s, l, id, "hidden", !used);
				if (!used)
				{
					continue;
				}
				const auto &q = lines[i];
				ValueClass(s, l, id, "px", Quantize(q.x, 2, 128));
				ValueClass(s, l, id, "py", Quantize(q.y, 2, 255));
				float angle = atan2f(q.dy, q.dx) * 180.f / 3.14159265359f;
				if (angle < 0)
				{
					angle += 360;
				}
				ValueClass(s, l, id, "pa", static_cast<int>(std::lround(angle / 4)) % 90);
				ValueClass(s, l, id, "pl", Quantize(std::hypot(q.dx, q.dy), 2, 64));
				for (auto name : {"good", "bad", "overlap", "dead", "unknown"})
				{
					SetClass(s, l, id, name, KZ_STREQ(name, ShadeName(q.shade)));
				}
				SetClass(s, l, id, "dim", !mini && q.strafe != s.selected);
			}
		}

		void Render(KZPlayer *p, State &s, CCSCustomHudLayout *l)
		{
			uint32 mask = Mask(p);
			bool visible = p->optionService->GetPreferenceBool("jsReviewEnabled", true) && Now() - s.snap->created < Duration(p) && p->IsAlive()
						   && !OtherUi(p) && !s.open;
			if (!s.dirty && visible == s.mini)
			{
				return;
			}
			s.mini = visible;
			s.dirty = false;
			SetClass(s, l, "jr_root", "hidden", !s.open && !visible);
			SetClass(s, l, "jr_mini", "hidden", !visible);
			SetClass(s, l, "jr_modal", "hidden", !s.open);
			SetClass(s, l, "jr_review", "hidden", !s.open || s.settings);
			SetClass(s, l, "jr_settings", "hidden", !s.open || !s.settings);
			SetClass(s, l, "jr_tab_review", "selected", !s.settings);
			SetClass(s, l, "jr_tab_settings", "selected", s.settings);
			SetClass(s, l, "jr_m_header", "hidden", !(mask & 3u));
			bool path = (mask & (1u << 28)) != 0;
			int bodyCount = 0;
			for (int f = 4; f < 26; f++)
			{
				if (mask & (1u << f))
				{
					bodyCount++;
				}
			}
			if (mask & (1u << 29))
			{
				bodyCount++;
			}
			bool compact = path && bodyCount <= 6;
			SetClass(s, l, "jr_mini", "wide", path && !compact);
			SetClass(s, l, "jr_mini", "no-path", !path);
			SetClass(s, l, "jr_mini", "single", compact);
			for (int scale : {80, 100, 120})
			{
				SetClass(s, l, "jr_mini", Format("scale%i", scale), Scale(p) == scale);
			}
			bool left = p->optionService->GetPreferenceBool("jsReviewLeft", false);
			SetClass(s, l, "jr_mini", "left", left);
			SetClass(s, l, "jr_mini", "right", !left);
			for (int i = 0; i < 30; i++)
			{
				if (i < 26 || i == 29)
				{
					Var(s, l, Format("jr_g%i", i), s.snap->fields[i]);
				}
				SetClass(s, l, Format("jr_field%i", i), "on", (mask & (1u << i)) != 0);
			}
			const char *headNames[] = {"type", "mode", "styles", "distance"};
			for (int i = 0; i < 4; i++)
			{
				SetClass(s, l, std::string("jr_m_") + headNames[i] + "_box", "hidden", !(mask & (1u << i)));
				Var(s, l, std::string("jr_m_") + headNames[i], (i == 3 ? s.snap->miniDistance : s.snap->fields[i]));
			}
			std::vector<int> fields;
			for (int i = 4; i < 26; i++)
			{
				if (mask & (1u << i))
				{
					fields.push_back(i);
				}
			}
			if (mask & (1u << 29))
			{
				fields.push_back(29);
			}
			for (int i = 0; i < 24; i++)
			{
				bool used = i < static_cast<int>(fields.size());
				SetClass(s, l, Format("jr_m%i", i), "hidden", !used);
				if (used)
				{
					int f = fields[i];
					Var(s, l, Format("jr_m%i_label", i), labels[f]);
					Var(s, l, Format("jr_m%i_value", i), s.snap->fields[f]);
					SetClass(s, l, Format("jr_m%i_value", i), "good", f == 4);
				}
			}
			for (int i = 0; i < 12; i++)
			{
				SetClass(s, l, Format("jr_m_row%i", i), "hidden", i * 2 >= static_cast<int>(fields.size()));
			}
			SetClass(s, l, "jr_m_path_box", "hidden", !path);
			int ph = std::clamp(static_cast<int>((fields.size() + 1) / 2) * 42, 220, 480);
			s.miniPlotHeight = Quantize(ph, 4, 128) * 4;
			ValueClass(s, l, "jr_m_path", "ph", Quantize(ph, 4, 128));
			ValueClass(s, l, "jr_m_path_box", "ph", Quantize(ph, 4, 128));
			const char *graphIds[] = {"keys_l", "keys_r", "mouse_l", "mouse_r"};
			for (int i = 0; i < 4; i++)
			{
				Var(s, l, std::string("jr_") + graphIds[i], s.snap->graph[i]);
				Var(s, l, std::string("jr_m_") + graphIds[i], s.snap->graph[i]);
			}
			SetClass(s, l, "jr_m_keys_box", "hidden", !(mask & (1u << 26)));
			SetClass(s, l, "jr_m_mouse_box", "hidden", !(mask & (1u << 27)));
			int total = static_cast<int>(s.snap->strafes.size());
			s.page = std::clamp(s.page, 0, std::max(0, (total - 1) / 12));
			s.selected = std::clamp(s.selected, 0, std::max(0, total - 1));
			for (int r = 0; r < 12; r++)
			{
				int index = s.page * 12 + r;
				bool used = index < total;
				SetClass(s, l, Format("jr_row%i", r), "hidden", !used);
				SetClass(s, l, Format("jr_row%i", r), "selected", index == s.selected);
				for (int c = 0; c < 9; c++)
				{
					auto id = Format("jr_t%i_%i", r, c);
					Var(s, l, id, used ? s.snap->strafes[index].cells[c] : "");
					SetClass(s, l, id, "selected", index == s.selected);
				}
			}
			Var(s, l, "jr_selected", total ? Format("Strafe %i / %i", s.selected + 1, total) : "No strafes");
			for (int i = 0; i < 11; i++)
			{
				Var(s, l, Format("jr_i%i", i), total ? s.snap->strafes[s.selected].details[i] : "—");
			}
			Var(s, l, "jr_duration", Format("%i s", Duration(p)));
			Var(s, l, "jr_scale", Format("%i%%", Scale(p)));
			Var(s, l, "jr_side_value", left ? "Left" : "Right");
			SetClass(s, l, "jr_enabled", "on", p->optionService->GetPreferenceBool("jsReviewEnabled", true));
			DrawLine(s, l, true);
			DrawLine(s, l, false);
			l->GetGlobalLayoutState()->MarkFullChanged();
		}
	} // namespace

	void OnJump(KZPlayer *p, Jump *j)
	{
		if (!Available(p) || !j || j->GetJumpType() == JumpType_FullInvalid || j->airtime < .05f)
		{
			return;
		}
		auto s = Of(p);
		if (!s)
		{
			return;
		}
		auto snap = Capture(p, j);
		if (s->open)
		{
			s->pending = std::move(snap);
			return;
		}
		Destroy(p, *s);
		s->snap = std::move(snap);
		s->selected = 0;
		s->page = 0;
		s->retry = 0;
	}

	void Close(KZPlayer *p)
	{
		auto s = Of(p);
		if (!s)
		{
			return;
		}
		if (auto e = s->entity.Get())
		{
			static_cast<CCSCustomHudLayout *>(e)->SetInputCaptureEnabled(p->GetPlayerSlot(), false);
		}
		s->open = false;
		s->settings = false;
		s->dirty = true;
		s->input.Transition(IN_RELOAD);
		if (s->pending)
		{
			Destroy(p, *s);
			s->snap = std::move(s->pending);
			s->page = 0;
			s->selected = 0;
			s->retry = 0;
		}
	}

	void Open(KZPlayer *p)
	{
		auto s = Of(p);
		if (!s || !Available(p) || !s->snap || OtherUi(p))
		{
			return;
		}
		auto l = Ensure(p, *s);
		if (!l)
		{
			return;
		}
		if (!l->GetPlayerLayoutState(p->GetPlayerSlot()))
		{
			return;
		}
		s->open = true;
		s->settings = false;
		s->dirty = true;
		s->input.Transition(IN_RELOAD);
		l->SetInputCaptureEnabled(p->GetPlayerSlot(), true);
		Render(p, *s, l);
	}

	void Tick(KZPlayer *p)
	{
		auto s = Of(p);
		if (!s)
		{
			return;
		}
		if (!Available(p))
		{
			if (s->entity.IsValid())
			{
				Reset(p);
			}
			return;
		}
		if (s->snap && Now() < s->snap->created)
		{
			Reset(p);
			return;
		}
		if (s->open && (!p->IsAlive() || OtherUi(p)))
		{
			Close(p);
		}
		if (s->input.Take() & IN_RELOAD)
		{
			if (s->open)
			{
				Close(p);
			}
			else if (s->snap && Now() - s->snap->created < Duration(p) && Knife(p) && !OtherUi(p))
			{
				Open(p);
			}
		}
		if (!s->snap)
		{
			return;
		}
		bool enabled = p->optionService->GetPreferenceBool("jsReviewEnabled", true);
		if (!s->open && (!enabled || Now() - s->snap->created >= Duration(p)) && !s->entity.IsValid())
		{
			return;
		}
		if (auto l = Ensure(p, *s))
		{
			Render(p, *s, l);
		}
	}

	void Reset(KZPlayer *p)
	{
		auto s = Of(p);
		if (!s)
		{
			return;
		}
		Destroy(p, *s);
		*s = State();
	}

	void Cleanup()
	{
		for (int i = 0; i < MAXPLAYERS; i++)
		{
			auto p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(i));
			if (p)
			{
				Reset(p);
			}
		}
	}

	bool Owns(KZPlayer *p, CEntityHandle h)
	{
		auto s = Of(p);
		return s && s->entity.IsValid() && s->entity.ToInt() == h.ToInt();
	}

	bool IsOpen(KZPlayer *p)
	{
		auto s = Of(p);
		return s && s->open;
	}

	bool OnClick(KZPlayer *p, uint32 handle, const char *id)
	{
		auto s = Of(p);
		auto l = CCSCustomHudLayout::FromClickHandle(handle);
		if (!s || !l || static_cast<CBaseEntity *>(l) != s->entity.Get())
		{
			return false;
		}
		if (!id || !Available(p) || OtherUi(p))
		{
			return true;
		}
		if (KZ_STREQ(id, "jr_details"))
		{
			Open(p);
			return true;
		}
		if (!s->open)
		{
			return true;
		}
		if (KZ_STREQ(id, "jr_close"))
		{
			Close(p);
			return true;
		}
		double now = Now();
		if (s->lastClick >= 0 && now - s->lastClick < .08)
		{
			return true;
		}
		s->lastClick = now;
		if (KZ_STREQ(id, "jr_tab_review"))
		{
			s->settings = false;
		}
		else if (KZ_STREQ(id, "jr_tab_settings"))
		{
			s->settings = true;
		}
		else if (KZ_STREQ(id, "jr_prev"))
		{
			s->page--;
		}
		else if (KZ_STREQ(id, "jr_next"))
		{
			s->page++;
		}
		else if (s->settings)
		{
			auto opt = p->optionService;
			if (KZ_STREQ(id, "jr_enabled"))
			{
				opt->SetPreferenceBool("jsReviewEnabled", !opt->GetPreferenceBool("jsReviewEnabled", true));
			}
			else if (KZ_STREQ(id, "jr_duration_down"))
			{
				opt->SetPreferenceInt("jsReviewDuration", std::max(1, Duration(p) - 1));
			}
			else if (KZ_STREQ(id, "jr_duration_up"))
			{
				opt->SetPreferenceInt("jsReviewDuration", std::min(30, Duration(p) + 1));
			}
			else if (KZ_STREQ(id, "jr_scale_down"))
			{
				opt->SetPreferenceInt("jsReviewScale", std::max(80, Scale(p) - 20));
			}
			else if (KZ_STREQ(id, "jr_scale_up"))
			{
				opt->SetPreferenceInt("jsReviewScale", std::min(120, Scale(p) + 20));
			}
			else if (KZ_STREQ(id, "jr_side"))
			{
				opt->SetPreferenceBool("jsReviewLeft", !opt->GetPreferenceBool("jsReviewLeft", false));
			}
			else if (KZ_STREQ(id, "jr_preset_all"))
			{
				opt->SetPreferenceInt("jsReviewFields", kAll);
			}
			else if (KZ_STREQ(id, "jr_preset_half"))
			{
				opt->SetPreferenceInt("jsReviewFields", kHalf);
			}
			else if (KZ_STREQ(id, "jr_preset_min"))
			{
				opt->SetPreferenceInt("jsReviewFields", (1u << 0) | (1u << 1) | (1u << 3));
			}
			else if (KZ_STREQ(id, "jr_preset_default"))
			{
				opt->SetPreferenceInt("jsReviewFields", kDefault);
			}
			else
			{
				for (int i = 0; i < 30; i++)
				{
					if (KZ_STREQ(id, Format("jr_field%i", i).c_str()))
					{
						opt->SetPreferenceInt("jsReviewFields", Mask(p) ^ (1u << i));
						break;
					}
				}
			}
		}
		else
		{
			for (int i = 0; i < 12; i++)
			{
				if (KZ_STREQ(id, Format("jr_row%i", i).c_str()))
				{
					int n = s->page * 12 + i;
					if (n < static_cast<int>(s->snap->strafes.size()))
					{
						s->selected = n;
					}
					break;
				}
			}
		}
		s->dirty = true;
		Render(p, *s, l);
		return true;
	}

	void OnUsercmds(KZPlayer *p, PlayerCommand *cmds, i32 count)
	{
		auto s = Of(p);
		if (!s || !cmds || !Available(p))
		{
			return;
		}
		for (int i = 0; i < count; i++)
		{
			auto pc = &cmds[i];
			auto base = pc->mutable_base();
			auto buttons = base->mutable_buttons_pb();
			bool other = OtherUi(p);
			bool eligible = s->open || (s->snap && Now() - s->snap->created < Duration(p) && Knife(p) && p->IsAlive());
			s->input.ObserveCommand(pc->cmdNum, buttons->buttonstate1(), IN_RELOAD, other || !eligible);
			auto mask = s->input.SuppressionMask(IN_RELOAD, !other && eligible);
			if (!mask)
			{
				continue;
			}
			buttons->set_buttonstate1(buttons->buttonstate1() & ~mask);
			buttons->set_buttonstate2(buttons->buttonstate2() & ~mask);
			buttons->set_buttonstate3(buttons->buttonstate3() & ~mask);
			for (int k = 0; k < 3; k++)
			{
				pc->buttonstates.m_pButtonStates[k] &= ~mask;
			}
			for (int k = 0; k < base->subtick_moves_size(); k++)
			{
				auto step = base->mutable_subtick_moves(k);
				if (step->button() & mask)
				{
					auto left = step->button() & ~mask;
					if (left)
					{
						step->set_button(left);
					}
					else
					{
						step->set_pressed(false);
					}
				}
			}
		}
	}
} // namespace KZ::jumpreview

SCMD(kz_jumpinfohud, SCFL_HUD)
{
	auto p = g_pKZPlayerManager->ToPlayer(controller);
	if (KZ::jumpreview::IsOpen(p))
	{
		KZ::jumpreview::Close(p);
	}
	else
	{
		KZ::jumpreview::Open(p);
	}
	return true;
}

SCMD_LINK(kz_jreview, kz_jumpinfohud);
