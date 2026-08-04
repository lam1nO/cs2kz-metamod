#include "common.h"
#include "kz_goto.h"
#include "utils/simplecmds.h"
#include "utils/utils.h"

#include "../invisible/kz_invisible.h"
#include "../language/kz_language.h"
#include "../timer/kz_timer.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

// Меню-движок cs2menus (определён в cs2kz.cpp); может быть nullptr, если плагин не загружен.
extern ICS2Menus *g_pMenus;

void KZGotoService::Init() {}

void KZGotoService::Reset() {}

i32 KZGotoService::CollectGotoCandidates(const char *query, KZPlayer **candidates, i32 maxCandidates)
{
	KZPlayer *exact[MAXPLAYERS + 1] = {};
	KZPlayer *sub[MAXPLAYERS + 1] = {};
	i32 exactCount = 0;
	i32 subCount = 0;
	bool wantAll = !query || !query[0];
	for (i32 i = 0; i <= MAXPLAYERS; i++)
	{
		KZPlayer *other = g_pKZPlayerManager->ToPlayer(i);
		if (!other || other == this->player || !other->GetController())
		{
			continue;
		}
		if (other->GetController()->GetTeam() == CS_TEAM_SPECTATOR)
		{
			continue;
		}
		// Невидимку не выдаём ни в меню !goto, ни по точному имени.
		if (KZInvisibleService::ShouldHideFrom(other, this->player))
		{
			continue;
		}
		if (wantAll)
		{
			sub[subCount++] = other;
		}
		else if (KZ_STREQI(other->GetName(), query))
		{
			exact[exactCount++] = other;
		}
		else if (V_stristr(other->GetName(), query))
		{
			sub[subCount++] = other;
		}
	}
	KZPlayer **src = exactCount > 0 ? exact : sub;
	i32 total = exactCount > 0 ? exactCount : subCount;
	for (i32 i = 0; i < total && i < maxCandidates; i++)
	{
		candidates[i] = src[i];
	}
	return total;
}

bool KZGotoService::GotoPlayer(KZPlayer *targetPlayer)
{
	if (!targetPlayer || !targetPlayer->GetController())
	{
		return false;
	}

	if (this->player->timerService->GetTimerRunning())
	{
		this->player->languageService->PrintChat(true, false, "Goto - Error Message (Timer Running)");
		return false;
	}

	if (targetPlayer->GetController()->GetTeam() == CS_TEAM_SPECTATOR)
	{
		this->player->languageService->PrintChat(true, false, "Goto - Error Message (Player In Spec)", targetPlayer->GetName());
		return false;
	}

	if (this->player->GetController()->GetTeam() == CS_TEAM_SPECTATOR)
	{
		this->player->GetController()->SwitchTeam(CS_TEAM_CT);
		this->player->GetController()->Respawn();
	}

	CCSPlayer_MovementServices *ms = this->player->GetMoveServices();

	if (targetPlayer->GetMoveType() == MOVETYPE_LADDER)
	{
		ms->m_vecLadderNormal(targetPlayer->GetMoveServices()->m_vecLadderNormal());
		this->player->SetMoveType(MOVETYPE_LADDER);
	}
	else
	{
		ms->m_vecLadderNormal(vec3_origin);
	}

	Vector origin;
	QAngle angles;
	targetPlayer->GetOrigin(&origin);
	targetPlayer->GetAngles(&angles);

	this->player->Teleport(&origin, &angles, &vec3_origin);
	this->player->languageService->PrintChat(true, false, "Goto - Teleported", targetPlayer->GetName());
	if (this->player->GetPlayerPawn()->m_Collision().m_CollisionGroup() != KZ_COLLISION_GROUP_STANDARD)
	{
		this->player->GetPlayerPawn()->m_Collision().m_CollisionGroup() = KZ_COLLISION_GROUP_STANDARD;
		this->player->GetPlayerPawn()->CollisionRulesChanged();
	}
	return true;
}

// Колбэк меню !goto: info — userID кандидата строкой; ревалидация обязательна —
// цель могла выйти или уйти в спеки, пока меню висело.
static_function void OnGotoMenuSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *p = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!p)
	{
		return;
	}
	const char *info = g_pMenus->GetItemInfo(menu, item);
	if (!info || !info[0])
	{
		return;
	}
	// Ревалидация невидимости: цель могли добавить в список (RCON), пока меню висело.
	KZPlayer *target = g_pKZPlayerManager->ToPlayer(CPlayerUserId(V_StringToInt32(info, -1)));
	if (!target || !target->GetController() || target->GetController()->GetTeam() == CS_TEAM_SPECTATOR
		|| KZInvisibleService::ShouldHideFrom(target, p))
	{
		p->languageService->PrintChat(true, false, "Goto - Player Unavailable");
		return;
	}
	p->gotoService->GotoPlayer(target);
}

// Меню выбора цели !goto (cs2menus, одноразовый выбор — паттерн kz_spec_menu).
// Вызывается только при загруженном g_pMenus (фолбэки — в GotoPlayer(const char*)).
static_function void OpenGotoMenu(KZPlayer *player, KZPlayer **candidates, i32 count)
{
	if (!player || count <= 0)
	{
		return;
	}

	int slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return;
	}

	// Один хэндл на слот — пересоздаём при повторном вызове.
	static MenuHandle s_gotoMenu[MAXPLAYERS + 1] = {};
	if (s_gotoMenu[slot] != kInvalidMenuHandle)
	{
		g_pMenus->DestroyMenu(s_gotoMenu[slot]);
		s_gotoMenu[slot] = kInvalidMenuHandle;
	}

	const char *lang = player->languageService->GetLanguage();
	std::string title = KZLanguageService::PrepareMessageWithLang(lang, "Goto Menu - Title");
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), &OnGotoMenuSelect);
	if (m == kInvalidMenuHandle)
	{
		return;
	}

	for (i32 i = 0; i < count; i++)
	{
		char info[16];
		V_snprintf(info, sizeof(info), "%d", candidates[i]->GetClient()->GetUserID().Get());
		g_pMenus->AddItem(m, candidates[i]->GetName(), info, false);
	}

	// Одноразовый выбор — меню закрывается по клику.
	g_pMenus->SetCloseOnSelect(m, true);

	s_gotoMenu[slot] = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

bool KZGotoService::GotoPlayer(const char *playerNamePart)
{
	// Чек таймера до резолва — как раньше, чтобы не дразнить меню при беге.
	if (this->player->timerService->GetTimerRunning())
	{
		this->player->languageService->PrintChat(true, false, "Goto - Error Message (Timer Running)");
		return false;
	}

	bool hasQuery = playerNamePart && playerNamePart[0];
	KZPlayer *candidates[MAXPLAYERS + 1] = {};
	i32 total = this->CollectGotoCandidates(playerNamePart, candidates, MAXPLAYERS + 1);
	if (total == 0)
	{
		if (hasQuery)
		{
			this->player->languageService->PrintChat(true, false, "Error Message (Player Not Found)", playerNamePart);
		}
		else
		{
			this->player->languageService->PrintChat(true, false, "Goto - No Players Available");
		}
		return false;
	}
	if (total == 1 && hasQuery)
	{
		return this->GotoPlayer(candidates[0]);
	}
	// Без движка меню — прежнее поведение: подстрока → первый кандидат, пусто → подсказка.
	if (g_pMenus == nullptr)
	{
		if (hasQuery)
		{
			return this->GotoPlayer(candidates[0]);
		}
		this->player->languageService->PrintChat(true, false, "Goto - Command Usage");
		return false;
	}
	// Без аргумента или неоднозначная подстрока — меню выбора.
	OpenGotoMenu(this->player, candidates, total);
	return true;
}

SCMD(kz_goto, SCFL_PLAYER | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->gotoService->GotoPlayer(args->ArgS());
	return MRES_SUPERCEDE;
}
