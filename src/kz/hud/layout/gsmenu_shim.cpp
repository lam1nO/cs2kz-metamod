// Прослойка над cs2menus (спека монорепы docs/design/2026-10-13-panorama-menus.md, фаза 3).
//
// Все меню форка ходят через g_pMenus. Прослойка встаёт на его место: КАЖДЫЙ вызов построения
// уходит в настоящий cs2menus (у него своя копия меню и свой хэндл), а параллельно копия меню
// живёт здесь, в модели прослойки. Решение «кто показывает» принимается в DisplayMenu по игроку:
// когда панорама доступна (KZ::gsmenu::Available), меню ведёт прослойка — ввод с пешки (W/S/E/A/D/R/F, у
// спектатора выход — Shift) и боковой список gs panorama-окна; иначе — настоящий cs2menus,
// как раньше. Наружу видны только хэндлы прослойки: колбэки cs2menus переводятся обратно.
//
// Семантика повторяет cs2menus (ics2menus.h): disabled-пункты пропускаются курсором; E на
// подменю — вход, R — к родителю; CloseOnSelect закрывает после onSelect, а onEnd(Selected)
// идёт после него; adjustable-строка: A/D зовут onAdjust (в capture-меню — только после E,
// повторная E отпускает, W/S заморожены); CancelMenu и новый DisplayMenu — Cancelled.
#include "kz/hud/layout/gsmenu.h"
#include "kz/kz.h"
#include "sdk/datatypes.h"
#include "sdk/entity/ccsplayercontroller.h"
#include "sdk/services.h"
#include "utils/logging.h"
#include "cs2kz.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include <algorithm>
#include <unordered_map>

#include "tier0/memdbgon.h"

extern bool g_menusHasSlotStatus;
extern bool g_menusHasAdjustCapture;

namespace
{
	struct Item
	{
		std::string text, info;
		bool disabled = false;
		bool adjustable = false;
		float step = 0, minValue = 0, maxValue = 0;
		MenuHandle child = kInvalidMenuHandle;
	};

	struct Menu
	{
		MenuHandle real = kInvalidMenuHandle;
		std::string title;
		MenuItemSelectFn onSelect;
		MenuEndFn onEnd;
		MenuItemAdjustFn onAdjust;
		bool closeOnSelect = true;
		bool capture = false;
		i32 startItem = 0;
		std::vector<Item> items;
		// Родитель подменю — как childDef->parent в cs2menus: R ведёт к нему, даже когда подменю
		// показано напрямую DisplayMenu (jumptop LinkParent, zones), а не входом из родителя.
		MenuHandle parent = kInvalidMenuHandle;
		std::unordered_map<i32, std::string> groups; // заголовок группы перед пунктом (SetItemGroup)
	};

	struct Session
	{
		bool active = false;
		std::vector<MenuHandle> stack; // корень — первый, текущий — последний
		i32 cursor = 0;
		bool captured = false;
		std::string status;
		u64 held = 0;
		i32 holdTicks = 0;
		i32 closedTick = -1000; // такт закрытия — карточка реплея не должна съесть ту же E
		f64 endTime = 0; // 0 — без таймаута
		bool dirty = true;

		MenuHandle Current() const
		{
			return stack.empty() ? kInvalidMenuHandle : stack.back();
		}
	};

	constexpr i32 kRepeatDelayTicks = 22; // как у карточки реплея и cs2menus
	constexpr i32 kRepeatPeriodTicks = 6;

	u64 HeldButtons(KZPlayer *player)
	{
		CCSPlayerController *controller = player ? player->GetController() : nullptr;
		CBasePlayerPawn *pawn = controller ? controller->GetCurrentPawn() : nullptr;
		CPlayer_MovementServices *ms = pawn ? pawn->m_pMovementServices() : nullptr;
		return ms ? ms->m_nButtons().m_pButtonStates[0] : 0;
	}

	// Текст пункта cs2menus несёт чат-цвета (байты 0x01..0x10) и бывает с html-тегами — в
	// панораме они видны мусором. Оставляем только печатное.
	std::string Plain(const std::string &s)
	{
		std::string out;
		bool tag = false;
		for (size_t i = 0; i < s.size(); i++)
		{
			const unsigned char c = (unsigned char)s[i];
			// Тег — только если дальше есть «>»: в нике «a<3» это просто символ.
			if (c == '<' && !tag && s.find('>', i) != std::string::npos)
			{
				tag = true;
				continue;
			}
			if (tag)
			{
				tag = c != '>';
				continue;
			}
			if (c >= 0x20)
			{
				out += (char)c;
			}
		}
		// Ведущий «#» панорама считает ключом локализации.
		if (!out.empty() && out[0] == '#')
		{
			out.insert(out.begin(), ' ');
		}
		return out;
	}
} // namespace

class GsMenus final : public ICS2Menus
{
public:
	explicit GsMenus(ICS2Menus *real) : real(real) {}

	ICS2Menus *real;
	std::unordered_map<MenuHandle, Menu> menus;
	std::unordered_map<MenuHandle, MenuHandle> fromReal;
	MenuHandle nextHandle = 1;
	Session sessions[MAXPLAYERS + 1];

	Menu *Find(MenuHandle h)
	{
		auto it = menus.find(h);
		return it == menus.end() ? nullptr : &it->second;
	}

	MenuHandle RealOf(MenuHandle h)
	{
		Menu *m = Find(h);
		return m ? m->real : kInvalidMenuHandle;
	}

	MenuHandle ShimOf(MenuHandle realHandle)
	{
		auto it = fromReal.find(realHandle);
		return it == fromReal.end() ? kInvalidMenuHandle : it->second;
	}

	Session *SessionOf(int slot)
	{
		return (slot >= 0 && slot <= MAXPLAYERS && sessions[slot].active) ? &sessions[slot] : nullptr;
	}

	static bool Selectable(const Menu &m, i32 i)
	{
		return i >= 0 && i < (i32)m.items.size() && !m.items[i].disabled;
	}

	static i32 FirstSelectable(const Menu &m, i32 from)
	{
		for (i32 i = (std::max)(from, 0); i < (i32)m.items.size(); i++)
		{
			if (Selectable(m, i))
			{
				return i;
			}
		}
		for (i32 i = 0; i < (i32)m.items.size(); i++)
		{
			if (Selectable(m, i))
			{
				return i;
			}
		}
		return 0;
	}

	// Закрыть сессию слота и позвать onEnd текущего меню. Колбэк может открыть новое меню или
	// уничтожить это — поэтому всё состояние сбрасывается ДО вызова.
	void EndSession(int slot, MenuEndReason reason)
	{
		Session &s = sessions[slot];
		if (!s.active)
		{
			return;
		}
		const MenuHandle h = s.Current();
		s = Session();
		s.closedTick = g_pKZUtils->GetServerGlobals()->tickcount;
		if (KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot)))
		{
			KZ::gsmenu::HideList(player);
		}
		Menu *m = Find(h);
		if (m && m->onEnd)
		{
			MenuEndFn fn = m->onEnd;
			fn(h, slot, reason);
		}
	}

	// «Подпись: значение» пункта cs2menus → подпись слева, значение справа; вкл/выкл — переключатель.
	static void SplitItem(const std::string &text, KZ::gsmenu::ListRow &row)
	{
		const size_t colon = text.rfind(": ");
		if (colon == std::string::npos || colon == 0)
		{
			row.label = text;
			return;
		}
		row.label = text.substr(0, colon);
		row.value = text.substr(colon + 2);
		// «Main: 01:02.77» (меню !courses) — своё время: галочка «пройден» перед значением.
		row.done = !row.value.empty() && isdigit((unsigned char)row.value[0]) && row.value.find(':') != std::string::npos;
		static const char *kOn[] = {"вкл", "Вкл", "ВКЛ", "on", "On", "ON"};
		static const char *kOff[] = {"выкл", "Выкл", "ВЫКЛ", "off", "Off", "OFF"};
		for (const char *w : kOn)
		{
			if (row.value == w)
			{
				row.tog = row.on = true;
			}
		}
		for (const char *w : kOff)
		{
			if (row.value == w)
			{
				row.tog = true;
			}
		}
	}

	// false — панель не создалась (сущность/аддон): вызывающий откатывается на cs2menus.
	bool Render(KZPlayer *player, Session &s)
	{
		Menu *m = Find(s.Current());
		if (!m)
		{
			return false;
		}
		KZ::gsmenu::List list;
		list.title = Plain(m->title);
		const i32 count = (i32)m->items.size();
		// Строки списка: заголовки групп + пункты; окно из 10 строк вокруг курсора (курсор — на 4-й).
		struct Line
		{
			i32 item; // -1 — заголовок группы
			std::string group;
		};
		std::vector<Line> lines;
		i32 cursorLine = 0;
		for (i32 i = 0; i < count; i++)
		{
			auto g = m->groups.find(i);
			if (g != m->groups.end())
			{
				lines.push_back({-1, g->second});
			}
			if (i == s.cursor)
			{
				cursorLine = (i32)lines.size();
			}
			lines.push_back({i, {}});
		}
		const i32 total = (i32)lines.size();
		const i32 first = (std::max)(0, (std::min)(cursorLine - 4, total - KZ_GSMENU_LIST_ROWS));
		const i32 last = (std::min)(total, first + KZ_GSMENU_LIST_ROWS);
		std::string sub;
		if (count > KZ_GSMENU_LIST_ROWS)
		{
			sub = std::to_string(s.cursor + 1) + " / " + std::to_string(count);
		}
		if (!s.status.empty())
		{
			sub = sub.empty() ? Plain(s.status) : Plain(s.status) + " · " + sub;
		}
		list.sub = sub;
		for (i32 li = first; li < last; li++)
		{
			KZ::gsmenu::ListRow row;
			if (lines[li].item < 0)
			{
				row.label = lines[li].group;
				row.grp = true;
				list.rows.push_back(row);
				continue;
			}
			const i32 i = lines[li].item;
			const Item &it = m->items[i];
			SplitItem(Plain(it.text), row);
			row.sel = i == s.cursor;
			row.dim = it.disabled;
			row.adj = it.adjustable && (!m->capture || (row.sel && s.captured));
			row.cap = row.sel && s.captured;
			if (it.child != kInvalidMenuHandle)
			{
				row.value = "›";
			}
			list.rows.push_back(row);
		}
		const Item *cur = (s.cursor >= 0 && s.cursor < count) ? &m->items[s.cursor] : nullptr;
		list.adjHint = cur && cur->adjustable && (!m->capture || s.captured);
		list.selectHint = (cur && cur->adjustable && m->capture) ? (s.captured ? "отпустить" : "настроить") : "выбрать";
		list.exitHint = player->IsAlive() ? "F" : "Shift";
		return KZ::gsmenu::ShowList(player, list);
	}

	void Tick(KZPlayer *player)
	{
		const int slot = player->GetPlayerSlot().Get();
		Session *s = SessionOf(slot);
		if (!s)
		{
			return;
		}
		if (s->endTime > 0 && g_pKZUtils->GetServerGlobals()->curtime >= s->endTime)
		{
			EndSession(slot, MenuEndReason::Timeout);
			return;
		}
		// Слот отдан внешней системе меню или настоящий cs2menus показал своё — уступаем.
		if (real->GetExternalBusy(slot) || real->GetActiveMenu(slot) != kInvalidMenuHandle)
		{
			EndSession(slot, MenuEndReason::Cancelled);
			return;
		}
		const u64 held = HeldButtons(player);
		const u64 pressed = held & ~s->held;
		s->held = held;
		Menu *m = Find(s->Current());
		if (!m)
		{
			EndSession(slot, MenuEndReason::Destroyed);
			return;
		}
		const i32 count = (i32)m->items.size();
		const bool spectator = !player->IsAlive();
		const u64 exitKey = spectator ? IN_SPEED : IN_LOOK_AT_WEAPON;

		if (pressed & exitKey)
		{
			EndSession(slot, MenuEndReason::Exit);
			return;
		}
		if ((pressed & IN_RELOAD) && (s->stack.size() > 1 || Find(m->parent)))
		{
			const MenuHandle child = s->stack.back();
			if (s->stack.size() > 1)
			{
				s->stack.pop_back();
			}
			else
			{
				s->stack = {m->parent};
			}
			Menu *parent = Find(s->Current());
			s->cursor = parent ? FirstSelectable(*parent, parent->startItem) : 0;
			if (parent)
			{
				for (i32 i = 0; i < (i32)parent->items.size(); i++)
				{
					if (parent->items[i].child == child)
					{
						s->cursor = i;
					}
				}
			}
			s->captured = false;
			// Остальные нажатия этого такта относились к прежнему меню — не применяем их к новому.
			this->Render(player, *s);
			return;
		}
		if (!s->captured && (pressed & (IN_FORWARD | IN_BACK)) && count > 0)
		{
			const i32 dir = (pressed & IN_FORWARD) ? -1 : 1;
			for (i32 step = 1; step <= count; step++)
			{
				const i32 i = ((s->cursor + dir * step) % count + count) % count;
				if (Selectable(*m, i))
				{
					s->cursor = i;
					break;
				}
			}
			s->dirty = true;
		}
		if ((pressed & IN_USE) && Selectable(*m, s->cursor))
		{
			const Item &it = m->items[s->cursor];
			if (it.child != kInvalidMenuHandle && Find(it.child))
			{
				s->stack.push_back(it.child);
				Menu *child = Find(it.child);
				s->cursor = FirstSelectable(*child, child->startItem);
				s->captured = false;
				this->Render(player, *s);
				return;
			}
			else if (it.adjustable && m->capture)
			{
				s->captured = !s->captured;
				s->dirty = true;
			}
			else
			{
				const MenuHandle h = s->Current();
				const i32 item = s->cursor;
				const bool close = m->closeOnSelect;
				MenuItemSelectFn fn = m->onSelect;
				if (close)
				{
					// Как в cs2menus (menu_manager.cpp NavSelect): показ закрывается ДО onSelect,
					// onEnd(Selected) — после него и только если меню ещё живо.
					sessions[slot] = Session();
					sessions[slot].closedTick = g_pKZUtils->GetServerGlobals()->tickcount;
					if (fn)
					{
						fn(h, slot, item);
					}
					if (!SessionOf(slot))
					{
						KZ::gsmenu::HideList(player);
					}
					if (Menu *mm = Find(h); mm && mm->onEnd)
					{
						MenuEndFn end = mm->onEnd;
						end(h, slot, MenuEndReason::Selected);
					}
					return;
				}
				if (fn)
				{
					fn(h, slot, item); // может переоткрыть/закрыть меню — дальше перечитываем сессию
				}
				s = SessionOf(slot);
				m = s ? Find(s->Current()) : nullptr;
				if (!s || !m)
				{
					return;
				}
				s->dirty = true;
			}
		}
		// A/D: фронт — сразу, удержание — автоповтор после паузы (как у cs2menus и карточки реплея).
		const Item *cur = Selectable(*m, s->cursor) ? &m->items[s->cursor] : nullptr;
		const u64 adjust = held & (IN_MOVELEFT | IN_MOVERIGHT);
		if (!cur || !cur->adjustable || (m->capture && !s->captured) || !adjust || adjust == (IN_MOVELEFT | IN_MOVERIGHT))
		{
			s->holdTicks = 0;
		}
		else
		{
			s->holdTicks++;
			const bool edge = (pressed & adjust) != 0;
			const bool repeat = s->holdTicks > kRepeatDelayTicks && (s->holdTicks - kRepeatDelayTicks) % kRepeatPeriodTicks == 0;
			if ((edge || repeat) && m->onAdjust)
			{
				const MenuHandle h = s->Current();
				const float delta = (adjust & IN_MOVERIGHT) ? cur->step : -cur->step;
				MenuItemAdjustFn fn = m->onAdjust;
				fn(h, slot, s->cursor, delta, cur->minValue, cur->maxValue);
				if ((s = SessionOf(slot)))
				{
					s->dirty = true;
				}
			}
		}
		if (s && s->active && s->dirty)
		{
			s->dirty = false;
			this->Render(player, *s);
		}
	}

	void MarkDirty(MenuHandle h)
	{
		for (Session &s : sessions)
		{
			if (s.active && std::find(s.stack.begin(), s.stack.end(), h) != s.stack.end())
			{
				s.dirty = true;
			}
		}
	}

	// --- ICS2Menus ---

	MenuHandle CreateMenu(MenuType type, const char *title, MenuItemSelectFn onSelect) override
	{
		const MenuHandle h = nextHandle++;
		Menu &m = menus[h];
		m.title = title ? title : "";
		m.onSelect = onSelect;
		MenuItemSelectFn wrapped;
		if (onSelect)
		{
			wrapped = [this, h](MenuHandle, int slot, int item)
			{
				if (Menu *mm = Find(h); mm && mm->onSelect)
				{
					MenuItemSelectFn fn = mm->onSelect;
					fn(h, slot, item);
				}
			};
		}
		m.real = real->CreateMenu(type, title, wrapped);
		if (m.real == kInvalidMenuHandle)
		{
			menus.erase(h);
			return kInvalidMenuHandle;
		}
		fromReal[m.real] = h;
		return h;
	}

	int AddItem(MenuHandle menu, const char *text, const char *info, bool disabled) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return -1;
		}
		const int idx = real->AddItem(m->real, text, info, disabled);
		if (idx >= 0)
		{
			Item it;
			it.text = text ? text : "";
			it.info = info ? info : "";
			it.disabled = disabled;
			m->items.insert(m->items.begin() + (std::min)((size_t)idx, m->items.size()), it);
			MarkDirty(menu);
		}
		return idx;
	}

	void SetTitle(MenuHandle menu, const char *title) override
	{
		if (Menu *m = Find(menu))
		{
			m->title = title ? title : "";
			real->SetTitle(m->real, title);
			MarkDirty(menu);
		}
	}

	void SetExitButton(MenuHandle menu, bool enabled) override
	{
		real->SetExitButton(RealOf(menu), enabled);
	}

	void SetCloseOnSelect(MenuHandle menu, bool enabled) override
	{
		if (Menu *m = Find(menu))
		{
			m->closeOnSelect = enabled;
			real->SetCloseOnSelect(m->real, enabled);
		}
	}

	void SetMenuEndCallback(MenuHandle menu, MenuEndFn onEnd) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return;
		}
		m->onEnd = onEnd;
		MenuEndFn wrapped;
		if (onEnd)
		{
			wrapped = [this, menu](MenuHandle, int slot, MenuEndReason reason)
			{
				if (Menu *mm = Find(menu); mm && mm->onEnd)
				{
					MenuEndFn fn = mm->onEnd;
					fn(menu, slot, reason);
				}
			};
		}
		real->SetMenuEndCallback(m->real, wrapped);
	}

	void SetExitItem(MenuHandle menu, bool enabled) override
	{
		real->SetExitItem(RealOf(menu), enabled);
	}

	void SetMenuKey(MenuHandle menu, MenuNavAction action, MenuButton button) override
	{
		real->SetMenuKey(RealOf(menu), action, button);
	}

	bool DisplayMenu(MenuHandle menu, int slot, float duration) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return false;
		}
		KZPlayer *player = (slot >= 0 && slot < MAXPLAYERS) ? g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot)) : nullptr;
		if (!player || !KZ::gsmenu::Available(player) || real->GetExternalBusy(slot))
		{
			if (SessionOf(slot))
			{
				EndSession(slot, MenuEndReason::Cancelled);
			}
			return real->DisplayMenu(m->real, slot, duration);
		}
		// Панорама: настоящее меню на слоте (если было) закрываем — показывает прослойка.
		if (real->GetActiveMenu(slot) != kInvalidMenuHandle)
		{
			real->CancelMenu(slot);
		}
		if (SessionOf(slot))
		{
			EndSession(slot, MenuEndReason::Cancelled);
		}
		m = Find(menu); // колбэк выше мог уничтожить меню
		if (!m)
		{
			return false;
		}
		Session &s = sessions[slot];
		s = Session();
		s.active = true;
		s.stack.push_back(menu);
		s.cursor = FirstSelectable(*m, m->startItem);
		s.held = HeldButtons(player); // клавиша, открывшая меню, не должна сразу сработать в нём
		s.endTime = duration > 0 ? g_pKZUtils->GetServerGlobals()->curtime + duration : 0;
		s.dirty = false;
		if (!this->Render(player, s))
		{
			// Панель не создалась — показывает настоящий cs2menus (меню у игрока не пропадает).
			s = Session();
			return real->DisplayMenu(m->real, slot, duration);
		}
		return true;
	}

	void CancelMenu(int slot) override
	{
		if (SessionOf(slot))
		{
			EndSession(slot, MenuEndReason::Cancelled);
			return;
		}
		real->CancelMenu(slot);
	}

	bool HasMenu(int slot) override
	{
		return SessionOf(slot) != nullptr || real->HasMenu(slot);
	}

	MenuHandle GetActiveMenu(int slot) override
	{
		if (Session *s = SessionOf(slot))
		{
			return s->Current();
		}
		return ShimOf(real->GetActiveMenu(slot));
	}

	MenuType GetActiveMenuType(int slot) override
	{
		// Меню прослойки центр-канал не занимает (оно слева, в panorama) — худ уступать не должен.
		if (SessionOf(slot))
		{
			return MenuType::Chat;
		}
		return real->GetActiveMenuType(slot);
	}

	void DestroyMenu(MenuHandle menu) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return;
		}
		for (int slot = 0; slot <= MAXPLAYERS; slot++)
		{
			Session *s = SessionOf(slot);
			if (s && std::find(s->stack.begin(), s->stack.end(), menu) != s->stack.end())
			{
				EndSession(slot, MenuEndReason::Destroyed);
			}
		}
		m = Find(menu);
		if (!m)
		{
			return;
		}
		const MenuHandle r = m->real;
		fromReal.erase(r);
		menus.erase(menu);
		real->DestroyMenu(r);
	}

	int GetItemCount(MenuHandle menu) override
	{
		Menu *m = Find(menu);
		return m ? (int)m->items.size() : 0;
	}

	const char *GetItemText(MenuHandle menu, int item) override
	{
		Menu *m = Find(menu);
		return (m && item >= 0 && item < (int)m->items.size()) ? m->items[item].text.c_str() : "";
	}

	const char *GetItemInfo(MenuHandle menu, int item) override
	{
		Menu *m = Find(menu);
		return (m && item >= 0 && item < (int)m->items.size()) ? m->items[item].info.c_str() : "";
	}

	void SetItemText(MenuHandle menu, int item, const char *text) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return;
		}
		real->SetItemText(m->real, item, text);
		if (item >= 0 && item < (int)m->items.size())
		{
			m->items[item].text = text ? text : "";
			MarkDirty(menu);
		}
	}

	void SetItemDisabled(MenuHandle menu, int item, bool disabled) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return;
		}
		real->SetItemDisabled(m->real, item, disabled);
		if (item >= 0 && item < (int)m->items.size())
		{
			m->items[item].disabled = disabled;
			MarkDirty(menu);
		}
	}

	void RemoveItem(MenuHandle menu, int item) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return;
		}
		real->RemoveItem(m->real, item);
		if (item >= 0 && item < (int)m->items.size())
		{
			m->items.erase(m->items.begin() + item);
			std::unordered_map<i32, std::string> shifted;
			for (auto &[i, title] : m->groups)
			{
				if (i != item)
				{
					shifted[i > item ? i - 1 : i] = title;
				}
			}
			m->groups.swap(shifted);
			for (Session &s : sessions)
			{
				if (s.active && s.Current() == menu)
				{
					s.cursor = FirstSelectable(*m, (std::min)(s.cursor, (i32)m->items.size() - 1));
					s.captured = false;
				}
			}
			MarkDirty(menu);
		}
	}

	void RemoveAllItems(MenuHandle menu) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return;
		}
		real->RemoveAllItems(m->real);
		m->items.clear();
		m->groups.clear();
		for (Session &s : sessions)
		{
			if (s.active && s.Current() == menu)
			{
				s.cursor = 0;
				s.captured = false;
			}
		}
		MarkDirty(menu);
	}

	void SetStartItem(MenuHandle menu, int item) override
	{
		if (Menu *m = Find(menu))
		{
			m->startItem = item;
			real->SetStartItem(m->real, item);
		}
	}

	void DisplayMenuToAll(MenuHandle menu, float duration) override
	{
		real->DisplayMenuToAll(RealOf(menu), duration);
	}

	int GetSelectedItem(int slot) override
	{
		if (Session *s = SessionOf(slot))
		{
			Menu *m = Find(s->Current());
			return (m && Selectable(*m, s->cursor)) ? s->cursor : -1;
		}
		return real->GetSelectedItem(slot);
	}

	void SetItemInfo(MenuHandle menu, int item, const char *info) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return;
		}
		real->SetItemInfo(m->real, item, info);
		if (item >= 0 && item < (int)m->items.size())
		{
			m->items[item].info = info ? info : "";
		}
	}

	bool GetItemDisabled(MenuHandle menu, int item) override
	{
		Menu *m = Find(menu);
		return m && item >= 0 && item < (int)m->items.size() && m->items[item].disabled;
	}

	int GetStartItem(MenuHandle menu) override
	{
		Menu *m = Find(menu);
		return m ? m->startItem : 0;
	}

	int AddSubMenu(MenuHandle parent, const char *text, MenuHandle child, const char *info) override
	{
		Menu *p = Find(parent);
		Menu *c = Find(child);
		if (!p || !c || parent == child)
		{
			return -1;
		}
		const int idx = real->AddSubMenu(p->real, text, c->real, info);
		if (idx >= 0)
		{
			Item it;
			it.text = text ? text : "";
			it.info = info ? info : "";
			it.child = child;
			p->items.insert(p->items.begin() + (std::min)((size_t)idx, p->items.size()), it);
			c->parent = parent;
			MarkDirty(parent);
		}
		return idx;
	}

	void SetExternalBusy(int slot, bool busy) override
	{
		if (busy && SessionOf(slot))
		{
			EndSession(slot, MenuEndReason::Cancelled);
		}
		real->SetExternalBusy(slot, busy);
	}

	bool GetExternalBusy(int slot) override
	{
		return real->GetExternalBusy(slot);
	}

	void SetMenuLabel(MenuHandle menu, MenuLabel label, const char *text) override
	{
		real->SetMenuLabel(RealOf(menu), label, text);
	}

	const char *GetMenuLabel(MenuHandle menu, MenuLabel label) override
	{
		return real->GetMenuLabel(RealOf(menu), label);
	}

	int AddAdjustableItem(MenuHandle menu, const char *text, const char *info, float step, float minValue, float maxValue) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return -1;
		}
		const int idx = real->AddAdjustableItem(m->real, text, info, step, minValue, maxValue);
		if (idx >= 0)
		{
			Item it;
			it.text = text ? text : "";
			it.info = info ? info : "";
			it.adjustable = true;
			it.step = step;
			it.minValue = minValue;
			it.maxValue = maxValue;
			m->items.insert(m->items.begin() + (std::min)((size_t)idx, m->items.size()), it);
			MarkDirty(menu);
		}
		return idx;
	}

	void SetAdjustCallback(MenuHandle menu, MenuItemAdjustFn onAdjust) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return;
		}
		m->onAdjust = onAdjust;
		MenuItemAdjustFn wrapped;
		if (onAdjust)
		{
			wrapped = [this, menu](MenuHandle, int slot, int item, float delta, float minValue, float maxValue)
			{
				if (Menu *mm = Find(menu); mm && mm->onAdjust)
				{
					MenuItemAdjustFn fn = mm->onAdjust;
					fn(menu, slot, item, delta, minValue, maxValue);
				}
			};
		}
		real->SetAdjustCallback(m->real, wrapped);
	}

	void SetSlotStatus(int slot, const char *text) override
	{
		if (Session *s = SessionOf(slot))
		{
			const std::string t = text ? text : "";
			if (s->status != t)
			{
				s->status = t;
				s->dirty = true;
			}
			return;
		}
		// 005 есть не у каждой сборки cs2menus: вызывающие сверяют флаг, прослойка — тоже.
		if (g_menusHasSlotStatus)
		{
			real->SetSlotStatus(slot, text);
		}
	}

	void SetAdjustCapture(MenuHandle menu, bool enabled) override
	{
		Menu *m = Find(menu);
		if (!m)
		{
			return;
		}
		m->capture = enabled;
		if (g_menusHasAdjustCapture)
		{
			real->SetAdjustCapture(m->real, enabled);
		}
	}
};

static GsMenus *g_shim = nullptr;

void KZ::gsmenu::SetItemGroup(MenuHandle menu, int item, const char *title)
{
	if (!g_shim)
	{
		return;
	}
	if (auto it = g_shim->menus.find(menu); it != g_shim->menus.end())
	{
		it->second.groups[item] = title ? title : "";
	}
}

void KZ::gsmenu::ShimRedrawAll()
{
	if (!g_shim)
	{
		return;
	}
	for (Session &s : g_shim->sessions)
	{
		if (s.active)
		{
			s.dirty = true;
		}
	}
}

ICS2Menus *KZ::gsmenu::WrapMenus(ICS2Menus *realMenus)
{
	if (!realMenus)
	{
		return nullptr;
	}
	if (!g_shim)
	{
		g_shim = new GsMenus(realMenus);
	}
	else if (g_shim->real != realMenus)
	{
		// cs2menus перезагружен: его хэндлы и колбэки умерли вместе с ним — модель с нуля, но
		// нумерацию хэндлов прослойки продолжаем (в статиках потребителей лежат старые номера).
		for (Session &s : g_shim->sessions)
		{
			s = Session();
		}
		g_shim->menus.clear();
		g_shim->fromReal.clear();
		g_shim->real = realMenus;
	}
	return g_shim;
}

void KZ::gsmenu::ShimTick(KZPlayer *player)
{
	if (g_shim && player)
	{
		g_shim->Tick(player);
	}
}

bool KZ::gsmenu::ShimClosedRecently(KZPlayer *player)
{
	if (!g_shim || !player)
	{
		return false;
	}
	const int slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return false;
	}
	// Счётчик тактов начинается заново на новой карте — отрицательная разница означает «давно».
	const i32 d = g_pKZUtils->GetServerGlobals()->tickcount - g_shim->sessions[slot].closedTick;
	return d >= 0 && d <= 1;
}

void KZ::gsmenu::ShimCancel(KZPlayer *player)
{
	if (!g_shim || !player)
	{
		return;
	}
	const int slot = player->GetPlayerSlot().Get();
	if (g_shim->SessionOf(slot))
	{
		g_shim->EndSession(slot, MenuEndReason::Cancelled);
	}
}

void KZ::gsmenu::ShimDisconnect(KZPlayer *player)
{
	if (!g_shim || !player)
	{
		return;
	}
	const int slot = player->GetPlayerSlot().Get();
	if (g_shim->SessionOf(slot))
	{
		g_shim->EndSession(slot, MenuEndReason::Disconnect);
	}
}
