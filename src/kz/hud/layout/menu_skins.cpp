// Вкладка «Скины» окна !options (спека монорепы docs/design/2026-10-20-server-menu-addon.md, «KZ: вкладка
// «Скины»»). Скинченджер один на все режимы: страницу строят cyber-skins (SkinsGrid) и cyber-menu (MenuCore),
// как вкладку «Скины» общего меню на 35hp/dm. Форк только встраивает её в своё окно:
//   - клик по вкладке → `cyb_menu_yield <slot>` + `cyb_menu_embed <slot> skins`: cyber-menu заводит «встроенную»
//     сессию (без своей сущности и захвата) и шлёт операции командой `cyb_ui_embed <slot> <ops>` (формат
//     cyb_ui_batch);
//   - операции применяются к сущности окна (ownedMenuLayout) только на открытой вкладке и только для id сетки
//     (KZ_MENU_EMBED_PREFIX + id); шапка, крестик и футер cyber-menu отбрасываются — они у окна свои;
//   - клик по ks_* уходит обратно `css_ui_click <slot> <id>`; уход с вкладки — `cyb_menu_yield <slot>`.
// Иконки плиток — уникальные классы (sk-skin-…): за долгий просмотр интерн-таблица сущности растёт к лимиту —
// тогда окно пересоздаётся и вкладка открывается заново (cyber-menu перерисует всё).
#include "kz/hud/kz_hud.h"
#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/menu.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "utils/utils.h"
#include "utils/logging.h"
#include "utils/interfaces.h"
#include "cs2kz.h"

#include <cstring>
#include <string>

#include "tier0/memdbgon.h"

bool KZHUDService::IsMenuSkinsAvailable()
{
	return g_pCVar && g_pCVar->FindConCommand("cyb_menu_embed").IsValidRef();
}

void KZHUDService::OpenLayoutMenuSkins()
{
	if (!this->menuOpen)
	{
		this->OpenLayoutMenu();
		if (!this->menuOpen)
		{
			return;
		}
	}
	if (KZMenuSkinsTab() >= 0)
	{
		// Явный запрос (!ws) — сессия cyber-menu заводится заново, даже если вкладка уже открыта: пропавшая
		// сессия (перезагрузка плагина) иначе оставила бы замершую сетку.
		this->SelectMenuSkins(true);
	}
}

void KZHUDService::SelectMenuSkins(bool force)
{
	if (!this->menuOpen || KZMenuSkinsTab() < 0)
	{
		return;
	}
	if (this->menuPopup != MenuPopup::None)
	{
		this->CloseMenuPopup();
	}
	if (!this->menuSkins || force)
	{
		this->menuSkins = true;
		// cyber-menu начнёт с чистого листа (yield сбрасывает его диф-кэш), а сущность окна та же: снимаем всё,
		// что прошлый заход оставил включённым, — иначе у плитки остались бы две иконки.
		if (CBaseEntity *ent = this->ownedMenuLayout.Get())
		{
			for (const std::string &key : this->menuEmbedOn)
			{
				const size_t sep = key.find('\x1e');
				if (sep != std::string::npos)
				{
					this->SetMenuClass((CCSCustomHudLayout *)ent, key.substr(0, sep).c_str(), key.c_str() + sep + 1, false);
				}
			}
		}
		this->menuEmbedOn.clear();
		const i32 slot = this->player->GetPlayerSlot().Get();
		char cmd[64];
		V_snprintf(cmd, sizeof(cmd), "cyb_menu_yield %i\n", slot);
		interfaces::pEngine->ServerCommand(cmd);
		V_snprintf(cmd, sizeof(cmd), "cyb_menu_embed %i skins\n", slot);
		interfaces::pEngine->ServerCommand(cmd);
	}
	this->RenderMenu();
}

void KZHUDService::LeaveMenuSkins()
{
	if (!this->menuSkins)
	{
		return;
	}
	this->menuSkins = false;
	char cmd[48];
	V_snprintf(cmd, sizeof(cmd), "cyb_menu_yield %i\n", this->player->GetPlayerSlot().Get());
	interfaces::pEngine->ServerCommand(cmd);
}

// Id встроенной страницы → id панели окна; false — операция не наша (шапка/футер/подсказка cyber-menu).
static_function bool MapEmbedPanel(const std::string &panel, std::string &out)
{
	static const char *const exact[] = {"page_grid", "sk_back", "sk_prev", "sk_next"};
	bool ok = false;
	for (const char *e : exact)
	{
		ok = ok || panel == e;
	}
	if (!ok)
	{
		// cat0..9, t0..23, te*, ti*, tk* — префикс и только цифры за ним.
		static const char *const indexed[] = {"cat", "te", "ti", "tk", "t"};
		for (const char *pre : indexed)
		{
			const size_t n = strlen(pre);
			if (panel.size() > n && panel.size() <= n + 2 && panel.compare(0, n, pre) == 0
				&& panel.find_first_not_of("0123456789", n) == std::string::npos)
			{
				ok = true;
				break;
			}
		}
	}
	if (ok)
	{
		out = KZ_MENU_EMBED_PREFIX + panel;
	}
	return ok;
}

void KZHUDService::ForwardMenuEmbedClick(const char *id)
{
	const i32 slot = this->player->GetPlayerSlot().Get();
	const size_t len = strlen(id);
	std::string mapped;
	// Только кнопки страницы сетки: подделанный ks_gsm_close/ks_tab0 до cyber-menu не доходит.
	const bool badId = len == 0 || len > 48 || strspn(id, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != len
					   || !MapEmbedPanel(id, mapped) || !V_strncmp(id, "te", 2) || !V_strncmp(id, "ti", 2) || !V_strncmp(id, "tk", 2)
					   || !V_strcmp(id, "page_grid");
	// Не чаще 16 в секунду: cyber-menu перерисовывает страницу на каждый клик (как у моста, layout/gsmenu.cpp).
	const f64 now = g_pKZUtils->GetServerGlobals()->curtime;
	if (now - this->menuEmbedClickStart > 1.0 || now < this->menuEmbedClickStart)
	{
		this->menuEmbedClickStart = now;
		this->menuEmbedClicks = 0;
	}
	if (++this->menuEmbedClicks > 16 || badId)
	{
		if (badId || this->menuEmbedClicks == 17)
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] menu_skins_click_dropped reason=%s slot=%i\n", badId ? "bad_id" : "rate", slot);
		}
		return;
	}
	char cmd[96];
	V_snprintf(cmd, sizeof(cmd), "css_ui_click %i %s\n", slot, id);
	interfaces::pEngine->ServerCommand(cmd);
}

void KZHUDService::ApplyMenuEmbedOps(const char *ops)
{
	if (!this->menuOpen || !this->menuSkins || !ops)
	{
		return; // в пути после ухода с вкладки — не наше
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
	if (panels >= KZ_MENU_INTERN_RECYCLE || classes >= KZ_MENU_INTERN_RECYCLE || vars >= KZ_MENU_INTERN_RECYCLE)
	{
		// Пачку не применяем: новая сущность получит полный экран после cyb_menu_embed.
		KZ_LOG_INFO(LogChannel::General, "[cyb] panorama_menu_recycled reason=intern_near_limit_skins panels=%i classes=%i vars=%i slot=%i\n",
					panels, classes, vars, this->player->GetPlayerSlot().Get());
		this->DestroyOwnedMenuLayout();
		this->OpenLayoutMenuSkins();
		return;
	}
	bool classDirty = false;
	std::string fields[4];
	i32 n = 0;
	std::string mapped;
	for (const char *p = ops;; p++)
	{
		if (*p == '\x1f' || *p == '\0')
		{
			// c\x1epanel\x1eclass\x1e0|1  или  v\x1epanel\x1evar\x1evalue
			if (n == 3 && !fields[2].empty() && MapEmbedPanel(fields[1], mapped))
			{
				if (fields[0] == "c")
				{
					const bool on = fields[3] == "1";
					this->SetMenuClass(layout, mapped.c_str(), fields[2].c_str(), on);
					const std::string key = mapped + '\x1e' + fields[2];
					if (on)
					{
						this->menuEmbedOn.insert(key);
					}
					else
					{
						this->menuEmbedOn.erase(key);
					}
					classDirty = true;
				}
				else if (fields[0] == "v")
				{
					// Мимо menuVars: его кэш ключуется только именем переменной (l, t, n… есть и у окна).
					if (!layout->SetDialogVariableString(mapped.c_str(), fields[2].c_str(), fields[3].c_str()))
					{
						KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_menu_var_dropped reason=intern_limit panel=%s var=%s slot=%i\n", mapped.c_str(),
									fields[2].c_str(), this->player->GetPlayerSlot().Get());
					}
				}
			}
			for (std::string &f : fields)
			{
				f.clear();
			}
			n = 0;
			if (*p == '\0')
			{
				break;
			}
			continue;
		}
		if (*p == '\x1e' && n < 3)
		{
			n++;
			continue;
		}
		fields[n] += *p;
	}
	// Классы плиток красят их детей (.tile.sel .tn, .cat.active .cat-t) — нужен полный пересчёт слоя.
	if (classDirty)
	{
		layout->GetGlobalLayoutState()->MarkFullChanged();
	}
}

// Команда встроенной страницы (только консоль сервера — её шлёт cyber-menu): cyb_ui_embed <slot> <ops>.
CON_COMMAND_F(cyb_ui_embed, "cyb_ui_embed <slot> <op>\\x1f<op>... — страница cyber-menu во вкладке «Скины» окна !options", FCVAR_NONE)
{
	if (utils::GetController(context.GetPlayerSlot()) || args.ArgC() < 3)
	{
		return;
	}
	const i32 slot = V_StringToInt32(args.Arg(1), -1);
	if (slot < 0 || slot >= MAXPLAYERS)
	{
		return;
	}
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!player || !player->IsInGame())
	{
		return;
	}
	// Операции — всё после слота (ArgS отдаёт текст после имени команды, слот — первое слово).
	const char *rest = args.ArgS();
	while (*rest == ' ')
	{
		rest++;
	}
	while (*rest && *rest != ' ')
	{
		rest++;
	}
	if (*rest)
	{
		rest++;
	}
	player->hudService->ApplyMenuEmbedOps(rest);
}
