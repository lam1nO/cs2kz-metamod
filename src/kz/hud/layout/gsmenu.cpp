// Общее panorama-окно GYMSTRIKE — см. шапку gsmenu.h.
//
// Захват ввода (SetInputCaptureEnabled) — как у !options (layout/menu.cpp): включается на
// открытии окна и снимается ЯВНО, до удаления сущности, на всех путях закрытия — крестик,
// колбэк (действие, уводящее игрока из окна), дисконнект, смена карты, выгрузка плагина.
// Клик сверяется с хэндлом СОБСТВЕННОЙ сущности игрока, поэтому чужой клик сюда не попадёт.
//
// Диф-кэш классов и переменных живёт вместе с сущностью: свежая сущность — пустой кэш, иначе
// первое открытие после смены карты решило бы, что всё уже выставлено (урок EnsureMenuLayout).
// Смена класса на родителе не перестраивает стили детей (sdk ccscustomhudlayout.h, BUGS п.2) —
// в кадре, где класс реально сменился, слой помечается на полный пересчёт.
#include "kz/hud/layout/gsmenu.h"
#include "kz/hud/layout/layout.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/logging.h"
#include "utils/interfaces.h"
#include "cs2kz.h"

#include <cctype>
#include <unordered_map>

#include "tier0/memdbgon.h"

namespace
{
	// Подпись полосы бренда: «KZ #5» из hostname «GYMSTRIKE.NET | KZ #5»; без «|» — пусто.
	std::string ServerTag()
	{
		static CConVarRef<CUtlString> hostname("hostname");
		if (!hostname.IsValidRef() || !hostname.IsConVarDataAvailable())
		{
			return {};
		}
		const char *bar = strrchr(hostname.Get().Get(), '|');
		if (!bar)
		{
			return {};
		}
		bar++;
		while (*bar == ' ')
		{
			bar++;
		}
		std::string tag = bar;
		while (!tag.empty() && tag.back() == ' ')
		{
			tag.pop_back();
		}
		return tag;
	}

	struct WindowState
	{
		CHandle<CBaseEntity> entity {};
		bool open = false;
		bool capture = false;
		i32 openRow = -1;
		KZ::gsmenu::ClickFn onClick;
		std::unordered_map<std::string, bool> classes;
		std::unordered_map<std::string, std::string> vars;
		std::unordered_map<std::string, std::string> swaps; // панель -> текущий класс из набора (mc-*, t1..t7, avc*)
		bool classDirty = false;
		std::string owner; // кто открыл окно gw (maptop/jumptop)
	};

	WindowState g_windows[MAXPLAYERS + 1];

	// Сущность внешнего владельца (мост cyb_ui_* для CSSharp-плагинов — GG1): своя на игрока,
	// чтобы чужая разметка не делила диф-кэш и захват с окном форка.
	struct ExtState
	{
		CHandle<CBaseEntity> entity {};
		bool capture = false;
		std::string layout;
		f64 clickWindowStart = 0.0;
		i32 clicksInWindow = 0;
	};

	ExtState g_ext[MAXPLAYERS + 1];

	WindowState *StateOf(KZPlayer *player)
	{
		if (!player)
		{
			return nullptr;
		}
		const i32 slot = player->GetPlayerSlot().Get();
		return (slot >= 0 && slot <= MAXPLAYERS) ? &g_windows[slot] : nullptr;
	}

	CCSCustomHudLayout *LayoutOf(WindowState &w)
	{
		CBaseEntity *ent = w.entity.Get();
		return ent ? (CCSCustomHudLayout *)ent : nullptr;
	}

	void ResetCaches(WindowState &w)
	{
		w.classes.clear();
		w.vars.clear();
		w.swaps.clear();
		// Классы наборов, которые разметка ставит сама (gen_gsmenu_xml.py): без них Swap не снял бы
		// стартовый класс, и cols-jumptop встал бы рядом с cols-maptop.
		w.swaps["gw_cover"] = "mc-none";
		w.swaps["gw_table"] = "cols-maptop";
		w.openRow = -1;
		w.classDirty = false;
	}

	void SetCapture(KZPlayer *player, WindowState &w, bool on)
	{
		CCSCustomHudLayout *layout = LayoutOf(w);
		if (layout && w.capture != on)
		{
			layout->SetInputCaptureEnabled(player->GetPlayerSlot(), on);
		}
		w.capture = on && layout;
	}

	void Destroy(KZPlayer *player, WindowState &w)
	{
		if (CBaseEntity *ent = w.entity.Get())
		{
			// Захват снимаем ЯВНО, до удаления — не полагаемся на побочный эффект RemoveEntity.
			((CCSCustomHudLayout *)ent)->SetInputCaptureEnabled(player->GetPlayerSlot(), false);
			g_pKZUtils->RemoveEntity(ent);
		}
		w.entity = nullptr;
		w.open = false;
		w.capture = false;
		w.onClick = nullptr;
		ResetCaches(w);
	}

	CCSCustomHudLayout *Ensure(KZPlayer *player, WindowState &w)
	{
		if (g_KZPlugin.unloading || !KZHUDService::IsMHUDAvailable())
		{
			return nullptr;
		}
		if (CCSCustomHudLayout *cached = LayoutOf(w))
		{
			const i32 panels = cached->m_vecPanelIds().Count();
			const i32 classes = cached->m_vecClassNames().Count();
			const i32 vars = cached->m_vecDialogVariableNames().Count();
			if (panels < KZ_GSMENU_INTERN_RECYCLE && classes < KZ_GSMENU_INTERN_RECYCLE && vars < KZ_GSMENU_INTERN_RECYCLE)
			{
				return cached;
			}
			KZ_LOG_INFO(LogChannel::General, "[cyb] gsmenu_recycled reason=intern_near_limit panels=%i classes=%i vars=%i slot=%i\n", panels,
						classes, vars, player->GetPlayerSlot().Get());
			Destroy(player, w);
		}
		CCSCustomHudLayout *layout = utils::CreateEntityByName<CCSCustomHudLayout>("custom_hud_layout");
		if (!layout)
		{
			return nullptr;
		}
		CEntityKeyValues *kv = new CEntityKeyValues();
		kv->SetString("layout", KZ_GSMENU_LAYOUT);
		char name[32];
		V_snprintf(name, sizeof(name), "kzgsmenu%i", player->GetPlayerSlot().Get());
		kv->SetString("targetname", name);
		layout->DispatchSpawn(kv);
		w.entity = layout;
		ResetCaches(w);
		KZ::gsmenu::ShimRedrawAll(); // открытые списки прослойки рисовались в прежнюю сущность
		return layout;
	}

	// Запись с диф-кэшем. Отказ по лимиту интернов обязан быть видимым — окно иначе молча
	// перестаёт обновляться.
	void Class(KZPlayer *player, WindowState &w, CCSCustomHudLayout *layout, const char *panel, const char *cls, bool on)
	{
		std::string key = std::string(panel) + '|' + cls;
		auto it = w.classes.find(key);
		if (it != w.classes.end() && it->second == on)
		{
			return;
		}
		w.classes[key] = on;
		w.classDirty = true;
		if (!layout->SetHasClass(panel, cls, on ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] gsmenu_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n", panel, cls,
						player->GetPlayerSlot().Get());
		}
	}

	// Один класс из набора на панели (обложка mc-*, тир t*, цвет avc*): снять прежний, поставить новый.
	void Swap(KZPlayer *player, WindowState &w, CCSCustomHudLayout *layout, const char *panel, const std::string &cls)
	{
		std::string &cur = w.swaps[panel];
		if (cur == cls)
		{
			return;
		}
		if (!cur.empty())
		{
			Class(player, w, layout, panel, cur.c_str(), false);
		}
		if (!cls.empty())
		{
			Class(player, w, layout, panel, cls.c_str(), true);
		}
		cur = cls;
	}

	void Var(KZPlayer *player, WindowState &w, CCSCustomHudLayout *layout, const char *panel, const char *var, const std::string &value)
	{
		std::string key = std::string(panel) + '|' + var;
		auto it = w.vars.find(key);
		if (it != w.vars.end() && it->second == value)
		{
			return;
		}
		w.vars[key] = value;
		// Panorama переводит текст, совпавший с ключом локализации целиком («Main» → «Главный проход»),
		// хвостовой NBSP делает текст не-ключом и на экране не виден.
		const std::string shown = value.empty() ? value : value + "\xC2\xA0";
		if (!layout->SetDialogVariableString(panel, var, shown.c_str()))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] gsmenu_var_dropped reason=intern_limit panel=%s var=%s slot=%i\n", panel, var,
						player->GetPlayerSlot().Get());
		}
	}

	void Flush(WindowState &w, CCSCustomHudLayout *layout)
	{
		if (w.classDirty)
		{
			w.classDirty = false;
			layout->GetGlobalLayoutState()->MarkFullChanged();
		}
	}

	void SetOpenRow(KZPlayer *player, WindowState &w, CCSCustomHudLayout *layout, i32 row)
	{
		char id[24];
		for (i32 i = 0; i < KZ_GSMENU_TABLE_ROWS; i++)
		{
			V_snprintf(id, sizeof(id), "gr%i_wrap", i);
			Class(player, w, layout, id, "open", i == row);
		}
		w.openRow = row;
	}
} // namespace

bool KZ::gsmenu::Available(KZPlayer *player)
{
	return player && KZHUDService::IsMHUDAvailable();
}

std::string KZ::gsmenu::Initial(const char *name)
{
	if (!name || !name[0])
	{
		return "?";
	}
	const unsigned char c = (unsigned char)name[0];
	if (c < 0x80)
	{
		return std::string(1, (char)toupper(c));
	}
	// Ведущий байт UTF-8: длина символа по старшим битам.
	const i32 len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
	std::string out;
	for (i32 i = 0; i < len && name[i]; i++)
	{
		out += name[i];
	}
	return out;
}

bool KZ::gsmenu::ShowTable(KZPlayer *player, const Table &t, ClickFn onClick, const char *owner)
{
	WindowState *w = StateOf(player);
	if (!w)
	{
		return false;
	}
	CCSCustomHudLayout *layout = Ensure(player, *w);
	if (!layout)
	{
		return false;
	}
	w->onClick = std::move(onClick);
	w->owner = owner ? owner : "";
	// Боковой список (меню cs2menus) живёт рядом и не трогается: это другой корень разметки.
	Class(player, *w, layout, "gg", "shown", false);
	Class(player, *w, layout, "gw", "shown", true);

	Var(player, *w, layout, "gw", "t", t.title);
	Var(player, *w, layout, "gw", "s", t.sub);
	Var(player, *w, layout, "gw", "srv", ServerTag());
	Swap(player, *w, layout, "gw_cover", t.cover.empty() ? "mc-none" : "mc-" + t.cover);
	Class(player, *w, layout, "gw_tier", "hidden", t.tier <= 0);
	if (t.tier > 0)
	{
		char tier[16];
		V_snprintf(tier, sizeof(tier), "Тир %i", t.tier);
		Var(player, *w, layout, "gw_tier", "t", tier);
		Swap(player, *w, layout, "gw_tier", "t" + std::to_string(t.tier));
	}

	char id[32];
	for (i32 g = 0; g < KZ_GSMENU_FILTER_GROUPS; g++)
	{
		for (i32 k = 0; k < KZ_GSMENU_FILTER_CHIPS; k++)
		{
			V_snprintf(id, sizeof(id), "gf%i_%i", g, k);
			const bool used = k < (i32)t.filters[g].size();
			Class(player, *w, layout, id, "hidden", !used);
			if (used)
			{
				Var(player, *w, layout, id, "l", t.filters[g][k].label);
				Class(player, *w, layout, id, "on", t.filters[g][k].on);
			}
		}
	}

	Swap(player, *w, layout, "gw_table", t.cols);
	Class(player, *w, layout, "gw_table", "no-tp", t.noTp);
	for (i32 j = 0; j < KZ_GSMENU_TABLE_COLS; j++)
	{
		V_snprintf(id, sizeof(id), "gh%i", j);
		Var(player, *w, layout, id, "l", t.headers[j]);
	}

	char sub[32];
	for (i32 i = 0; i < KZ_GSMENU_TABLE_ROWS; i++)
	{
		const bool used = i < (i32)t.rows.size();
		V_snprintf(id, sizeof(id), "gr%i_line", i);
		Class(player, *w, layout, id, "hidden", !used);
		if (!used)
		{
			continue;
		}
		const Row &r = t.rows[i];
		Class(player, *w, layout, id, "wr", r.wr);
		Class(player, *w, layout, id, "me", r.me);
		for (i32 j = 0; j < KZ_GSMENU_TABLE_COLS; j++)
		{
			V_snprintf(sub, sizeof(sub), "gr%i_c%i", i, j);
			Var(player, *w, layout, sub, "v", r.cells[j]);
		}
		for (i32 k = 0; k < KZ_GSMENU_ROW_ACTIONS; k++)
		{
			V_snprintf(sub, sizeof(sub), "gr%i_a%i", i, k);
			Class(player, *w, layout, sub, "hidden", r.actions[k].empty());
			if (!r.actions[k].empty())
			{
				Var(player, *w, layout, sub, "l", r.actions[k]);
			}
		}
		V_snprintf(sub, sizeof(sub), "gr%i_note", i);
		Var(player, *w, layout, sub, "v", r.note);
		V_snprintf(sub, sizeof(sub), "gr%i_go", i);
		Class(player, *w, layout, sub, "hidden", r.noGo);
	}
	// Своя строка под чертой — ваш рекорд, когда его нет на текущей странице.
	Class(player, *w, layout, "gm_sep", "hidden", !t.hasMe);
	Class(player, *w, layout, "gm_line", "hidden", !t.hasMe);
	if (t.hasMe)
	{
		for (i32 j = 0; j < KZ_GSMENU_TABLE_COLS; j++)
		{
			V_snprintf(sub, sizeof(sub), "gm_c%i", j);
			Var(player, *w, layout, sub, "v", t.me.cells[j]);
		}
		Class(player, *w, layout, "gm_go", "hidden", t.me.actions[0].empty());
	}

	Class(player, *w, layout, "gw_prev", "hidden", !t.prev);
	Class(player, *w, layout, "gw_next", "hidden", !t.next);
	Var(player, *w, layout, "gw", "pg", t.page);
	Var(player, *w, layout, "gw", "hint", t.hint);

	// Новые данные — строки свёрнуты (номер раскрытой строки относился к прежнему содержимому).
	SetOpenRow(player, *w, layout, -1);
	Flush(*w, layout);

	w->open = true;
	SetCapture(player, *w, true);
	return true;
}

bool KZ::gsmenu::ShowList(KZPlayer *player, const List &l)
{
	WindowState *w = StateOf(player);
	if (!w)
	{
		return false;
	}
	CCSCustomHudLayout *layout = Ensure(player, *w);
	if (!layout)
	{
		return false;
	}
	Class(player, *w, layout, "gs", "shown", true);
	Class(player, *w, layout, "gs", "nokey", true); // цифр-хоткеев у прослойки нет
	Class(player, *w, layout, "gs", "k-adj", l.adjHint);
	Var(player, *w, layout, "gs", "t", l.title);
	Var(player, *w, layout, "gs", "s", l.sub);
	Var(player, *w, layout, "gs", "ke", l.selectHint);
	Var(player, *w, layout, "gs", "kx", l.exitHint);
	char id[24];
	for (i32 i = 0; i < KZ_GSMENU_LIST_ROWS; i++)
	{
		V_snprintf(id, sizeof(id), "gs%i", i);
		const bool used = i < (i32)l.rows.size();
		Class(player, *w, layout, id, "hidden", !used);
		if (!used)
		{
			continue;
		}
		const ListRow &r = l.rows[i];
		Class(player, *w, layout, id, "sel", r.sel);
		Class(player, *w, layout, id, "dim", r.dim);
		Class(player, *w, layout, id, "k-adj", r.adj);
		Class(player, *w, layout, id, "cap", r.cap);
		Class(player, *w, layout, id, "k-tog", r.tog);
		Class(player, *w, layout, id, "on", r.on);
		Class(player, *w, layout, id, "grp", r.grp);
		char sub[24];
		V_snprintf(sub, sizeof(sub), "gs%i_l", i);
		Var(player, *w, layout, sub, "l", r.label);
		V_snprintf(sub, sizeof(sub), "gs%i_v", i);
		Var(player, *w, layout, sub, "v", r.value);
	}
	Flush(*w, layout);
	return true;
}

void KZ::gsmenu::HideList(KZPlayer *player)
{
	WindowState *w = StateOf(player);
	CCSCustomHudLayout *layout = w ? LayoutOf(*w) : nullptr;
	if (layout)
	{
		Class(player, *w, layout, "gs", "shown", false);
		Flush(*w, layout);
	}
}

void KZ::gsmenu::Close(KZPlayer *player, const char *reason)
{
	WindowState *w = StateOf(player);
	if (!w || !w->open)
	{
		return;
	}
	CCSCustomHudLayout *layout = LayoutOf(*w);
	SetCapture(player, *w, false);
	if (layout)
	{
		Class(player, *w, layout, "gw", "shown", false);
		Flush(*w, layout);
	}
	w->open = false;
	w->onClick = nullptr;
	KZ_LOG_INFO(LogChannel::General, "[cyb] gsmenu_closed reason=%s slot=%i\n", reason, player->GetPlayerSlot().Get());
}

bool KZ::gsmenu::IsOpen(KZPlayer *player)
{
	WindowState *w = StateOf(player);
	return w && w->open;
}

bool KZ::gsmenu::IsOpenBy(KZPlayer *player, const char *owner)
{
	WindowState *w = StateOf(player);
	return w && w->open && owner && w->owner == owner;
}

bool KZ::gsmenu::HidesHud(KZPlayer *player)
{
	WindowState *w = StateOf(player);
	if (!w)
	{
		return false;
	}
	return w->open || (g_ext[player->GetPlayerSlot().Get()].capture && g_ext[player->GetPlayerSlot().Get()].entity.IsValid());
}

bool KZ::gsmenu::Owns(KZPlayer *player, CEntityHandle handle)
{
	WindowState *w = StateOf(player);
	if (w && w->entity.IsValid() && w->entity.ToInt() == handle.ToInt())
	{
		return true;
	}
	const i32 slot = player ? player->GetPlayerSlot().Get() : -1;
	return slot >= 0 && slot <= MAXPLAYERS && g_ext[slot].entity.IsValid() && g_ext[slot].entity.ToInt() == handle.ToInt();
}

bool KZ::gsmenu::OnClick(KZPlayer *player, uint32 packedHandle, const char *id)
{
	WindowState *w = StateOf(player);
	if (!w || !id)
	{
		return false;
	}
	CCSCustomHudLayout *layout = CCSCustomHudLayout::FromClickHandle(packedHandle);
	if (!layout)
	{
		return false;
	}
	const i32 slot = player->GetPlayerSlot().Get();
	if ((CBaseEntity *)layout == g_ext[slot].entity.Get())
	{
		// Клик по разметке внешнего владельца — ему в консоль сервера, как это делает cyber-ui.
		// Не чаще 16 в секунду: CSSharp-обработчик перерисовывает окно на каждый клик.
		ExtState &e = g_ext[slot];
		const f64 now = g_pKZUtils->GetServerGlobals()->curtime;
		// curtime с каждой карты начинается заново: окно с прошлой карты оказалось бы «в будущем»
		// и никогда не сбросилось — все клики по сетке GG1 молча отбрасывались до реконнекта.
		if (now - e.clickWindowStart > 1.0 || now < e.clickWindowStart)
		{
			e.clickWindowStart = now;
			e.clicksInWindow = 0;
		}
		const bool badId = strlen(id) > 48 || strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(id);
		if (++e.clicksInWindow > 16 || badId)
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] gsmenu_ext_click_dropped reason=%s n=%i now=%.1f start=%.1f slot=%i\n",
						badId ? "bad_id" : "rate", e.clicksInWindow, now, e.clickWindowStart, slot);
			return true;
		}
		char cmd[96];
		V_snprintf(cmd, sizeof(cmd), "css_ui_click %i %s\n", slot, id);
		interfaces::pEngine->ServerCommand(cmd);
		return true;
	}
	if ((CBaseEntity *)layout != w->entity.Get())
	{
		if (g_ext[slot].entity.IsValid())
		{
			// Клик по разметке, которой уже нет у владельца (пересоздана) — видно, если окно «не нажимается».
			KZ_LOG_WARN(LogChannel::General, "[cyb] gsmenu_ext_click_dropped reason=stale_layout id=%s slot=%i\n", id, slot);
		}
		return false;
	}
	if (!w->open)
	{
		return true; // клик в пути после закрытия — наш, но делать нечего
	}
	if (KZ_STREQ(id, "gw_close"))
	{
		Close(player, "close_button");
		return true;
	}
	// Клик по самой строке gr{i} ничего не делает (действие — кнопка ▶ gr{i}_go); остальное —
	// колбэку владельца окна.
	i32 row = -1;
	char tail = 0;
	if (sscanf(id, "gr%d%c", &row, &tail) == 1)
	{
		return true;
	}
	if (w->onClick)
	{
		// Копия: колбэк может переоткрыть окно (ShowTable заменит onClick) или закрыть его.
		ClickFn fn = w->onClick;
		fn(player, id);
	}
	return true;
}

static void DestroyExt(KZPlayer *player, ExtState &e)
{
	if (CBaseEntity *ent = e.entity.Get())
	{
		((CCSCustomHudLayout *)ent)->SetInputCaptureEnabled(player->GetPlayerSlot(), false);
		g_pKZUtils->RemoveEntity(ent);
	}
	e.entity = nullptr;
	e.capture = false;
	e.layout.clear();
	e.clickWindowStart = 0.0;
	e.clicksInWindow = 0;
}

void KZ::gsmenu::OnSlotReset(KZPlayer *player)
{
	if (WindowState *w = StateOf(player))
	{
		Destroy(player, *w);
		DestroyExt(player, g_ext[player->GetPlayerSlot().Get()]);
	}
}

void KZ::gsmenu::OnRoundStart(KZPlayer *player)
{
	// Раунд-старт бывает и посреди карты (сущность тогда жива) — сносим явно, со снятием захвата;
	// на смене карты сущность уже снесена движком, Destroy безопасен и без неё.
	if (WindowState *w = StateOf(player))
	{
		Destroy(player, *w);
		ExtState &e = g_ext[player->GetPlayerSlot().Get()];
		const bool hadExt = e.entity.IsValid();
		DestroyExt(player, e);
		// Внешний владелец (GG1) не знает, что сущности больше нет, — без сброса его голосование и
		// сетка молча перестали бы рисоваться до конца карты. css_ui_reset → он откроет разметку заново.
		if (hadExt)
		{
			char cmd[48];
			V_snprintf(cmd, sizeof(cmd), "css_ui_reset %i\n", player->GetPlayerSlot().Get());
			interfaces::pEngine->ServerCommand(cmd);
		}
	}
	ShimRedrawAll();
}

void KZ::gsmenu::Cleanup()
{
	for (i32 i = 0; i < MAXPLAYERS; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(i));
		if (player)
		{
			Destroy(player, g_windows[i]);
			DestroyExt(player, g_ext[i]);
		}
	}
}

// === Мост cyb_ui_* для CSSharp-плагинов (протокол cyber-ui) =====================================
//
// На kz рядом с форком cyber-ui не живёт (фильтр транзита KZ::quiet гасит чужие custom_hud_layout),
// поэтому тот же протокол отдаёт сам форк — GG1 рисует голосование и выбор карт одинаково на kz и
// 35hp. Команды только с консоли сервера; slot — 0-based слот игрока. Клик уходит обратно
// командой css_ui_click <slot> <id>, пересоздание сущности — css_ui_reset <slot>.

static KZPlayer *ExtPlayer(const CCommandContext &context, const CCommand &args, i32 &slot)
{
	if (utils::GetController(context.GetPlayerSlot()) || args.ArgC() < 2)
	{
		return nullptr;
	}
	slot = V_StringToInt32(args.Arg(1), -1);
	if (slot < 0 || slot >= MAXPLAYERS)
	{
		return nullptr;
	}
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	return (player && player->IsInGame()) ? player : nullptr;
}

static CCSCustomHudLayout *ExtLayout(KZPlayer *player, i32 slot)
{
	ExtState &e = g_ext[slot];
	CBaseEntity *ent = e.entity.Get();
	if (!ent)
	{
		return nullptr;
	}
	CCSCustomHudLayout *layout = (CCSCustomHudLayout *)ent;
	if (layout->m_vecPanelIds().Count() < KZ_GSMENU_INTERN_RECYCLE && layout->m_vecClassNames().Count() < KZ_GSMENU_INTERN_RECYCLE
		&& layout->m_vecDialogVariableNames().Count() < KZ_GSMENU_INTERN_RECYCLE)
	{
		return layout;
	}
	// Интерны на исходе — пересоздаём и просим владельца перерисовать всё (как cyber-ui).
	const std::string page = e.layout;
	const bool capture = e.capture;
	DestroyExt(player, e);
	CCSCustomHudLayout *fresh = utils::CreateEntityByName<CCSCustomHudLayout>("custom_hud_layout");
	if (!fresh)
	{
		return nullptr;
	}
	CEntityKeyValues *kv = new CEntityKeyValues();
	kv->SetString("layout", page.c_str());
	char name[32];
	V_snprintf(name, sizeof(name), "kzgsext%i", slot);
	kv->SetString("targetname", name);
	fresh->DispatchSpawn(kv);
	e.entity = fresh;
	e.layout = page;
	if (capture)
	{
		fresh->SetInputCaptureEnabled(player->GetPlayerSlot(), true);
		e.capture = true;
	}
	KZ_LOG_INFO(LogChannel::General, "[cyb] gsmenu_ext_recycled reason=intern_near_limit slot=%i\n", slot);
	char cmd[48];
	V_snprintf(cmd, sizeof(cmd), "css_ui_reset %i\n", slot);
	interfaces::pEngine->ServerCommand(cmd);
	return fresh;
}

CON_COMMAND_F(cyb_ui_open, "cyb_ui_open <slot> <layout> — разметка внешнего владельца игроку (мост форка)", FCVAR_NONE)
{
	i32 slot = -1;
	KZPlayer *player = ExtPlayer(context, args, slot);
	if (!player || args.ArgC() < 3 || g_KZPlugin.unloading || !KZHUDService::IsMHUDAvailable())
	{
		return;
	}
	ExtState &e = g_ext[slot];
	DestroyExt(player, e);
	CCSCustomHudLayout *layout = utils::CreateEntityByName<CCSCustomHudLayout>("custom_hud_layout");
	if (!layout)
	{
		return;
	}
	CEntityKeyValues *kv = new CEntityKeyValues();
	kv->SetString("layout", args.Arg(2));
	char name[32];
	V_snprintf(name, sizeof(name), "kzgsext%i", slot);
	kv->SetString("targetname", name);
	layout->DispatchSpawn(kv);
	e.entity = layout;
	e.layout = args.Arg(2);
}

CON_COMMAND_F(cyb_ui_close, "cyb_ui_close <slot> — снять захват и убрать разметку внешнего владельца", FCVAR_NONE)
{
	i32 slot = -1;
	if (KZPlayer *player = ExtPlayer(context, args, slot))
	{
		DestroyExt(player, g_ext[slot]);
	}
}

CON_COMMAND_F(cyb_ui_capture, "cyb_ui_capture <slot> <0|1> — курсор и клики на разметке внешнего владельца", FCVAR_NONE)
{
	i32 slot = -1;
	KZPlayer *player = ExtPlayer(context, args, slot);
	if (!player || args.ArgC() < 3)
	{
		return;
	}
	ExtState &e = g_ext[slot];
	CBaseEntity *ent = e.entity.Get();
	const bool on = V_StringToInt32(args.Arg(2), 0) != 0;
	if (ent && e.capture != on)
	{
		((CCSCustomHudLayout *)ent)->SetInputCaptureEnabled(player->GetPlayerSlot(), on);
		e.capture = on;
	}
}

// Текст после слота: ArgS отдаёт всё после имени команды, слот — первое слово.
static const char *ArgsAfterSlot(const CCommand &args)
{
	const char *rest = args.ArgS();
	while (*rest == ' ')
	{
		rest++;
	}
	while (*rest && *rest != ' ')
	{
		rest++;
	}
	return *rest ? rest + 1 : rest;
}

static void ApplyExtOp(KZPlayer *player, CCSCustomHudLayout *layout, const char *op, bool &classDirty)
{
	// c\x1epanel\x1eclass\x1e0|1  или  v\x1epanel\x1evar\x1evalue
	std::string fields[4];
	i32 n = 0;
	for (const char *p = op; *p && n < 4; p++)
	{
		if (*p == '\x1e' && n < 3)
		{
			n++;
			continue;
		}
		fields[n] += *p;
	}
	if (n != 3 || fields[1].empty() || fields[2].empty())
	{
		return;
	}
	if (fields[0] == "c")
	{
		const bool on = fields[3] == "1";
		if (!layout->SetHasClass(fields[1].c_str(), fields[2].c_str(), on ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] gsmenu_ext_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n", fields[1].c_str(),
						fields[2].c_str(), player->GetPlayerSlot().Get());
		}
		classDirty = true;
	}
	else if (fields[0] == "v")
	{
		if (!layout->SetDialogVariableString(fields[1].c_str(), fields[2].c_str(), fields[3].c_str()))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] gsmenu_ext_var_dropped reason=intern_limit panel=%s var=%s slot=%i\n", fields[1].c_str(),
						fields[2].c_str(), player->GetPlayerSlot().Get());
		}
	}
}

CON_COMMAND_F(cyb_ui_batch, "cyb_ui_batch <slot> <op>\\x1f<op>... ; op = c\\x1epanel\\x1eclass\\x1e0|1 или v\\x1epanel\\x1evar\\x1evalue", FCVAR_NONE)
{
	i32 slot = -1;
	KZPlayer *player = ExtPlayer(context, args, slot);
	if (!player)
	{
		return;
	}
	CCSCustomHudLayout *layout = ExtLayout(player, slot);
	if (!layout)
	{
		return;
	}
	bool classDirty = false;
	std::string op;
	for (const char *p = ArgsAfterSlot(args);; p++)
	{
		if (*p == '\x1f' || *p == '\0')
		{
			if (!op.empty())
			{
				ApplyExtOp(player, layout, op.c_str(), classDirty);
			}
			op.clear();
			if (*p == '\0')
			{
				break;
			}
			continue;
		}
		op += *p;
	}
	if (classDirty)
	{
		layout->GetGlobalLayoutState()->MarkFullChanged();
	}
}

CON_COMMAND_F(cyb_ui_class, "cyb_ui_class <slot> <panel> <class> <0|1>", FCVAR_NONE)
{
	i32 slot = -1;
	KZPlayer *player = ExtPlayer(context, args, slot);
	CCSCustomHudLayout *layout = player && args.ArgC() >= 5 ? ExtLayout(player, slot) : nullptr;
	if (layout)
	{
		bool dirty = false;
		std::string op = std::string("c\x1e") + args.Arg(2) + "\x1e" + args.Arg(3) + "\x1e" + args.Arg(4);
		ApplyExtOp(player, layout, op.c_str(), dirty);
		layout->GetGlobalLayoutState()->MarkFullChanged();
	}
}

CON_COMMAND_F(cyb_ui_var, "cyb_ui_var <slot> <panel> <var> <значение до конца строки>", FCVAR_NONE)
{
	i32 slot = -1;
	KZPlayer *player = ExtPlayer(context, args, slot);
	CCSCustomHudLayout *layout = player && args.ArgC() >= 4 ? ExtLayout(player, slot) : nullptr;
	if (!layout)
	{
		return;
	}
	// Значение — всё после третьего слова (может содержать пробелы).
	const char *rest = ArgsAfterSlot(args);
	for (i32 skip = 0; skip < 2 && *rest; skip++)
	{
		while (*rest && *rest != ' ')
		{
			rest++;
		}
		while (*rest == ' ')
		{
			rest++;
		}
	}
	layout->SetDialogVariableString(args.Arg(2), args.Arg(3), rest);
}

// Версия протокола — плагины проверяют её, чтобы понять, что мост есть (как у cyber-ui).
static CConVar<CUtlString> cyb_ui_version("cyb_ui_version", FCVAR_NONE, "Протокол моста cyb_ui_* (форк cs2kz)", "kz-1");
