#include "kz/pistol/kz_pistol.h"
#include "kz/weapon/kz_weapon.h"
#include "kz/quiet/kz_quiet.h"
#include "kz/option/kz_option.h"
#include "kz/language/kz_language.h"

#include "utils/simplecmds.h"
#include "icvar.h"
#include "sdk/cskeletoninstance.h"

static_global class : public KZOptionServiceEventListener
{
	void OnPlayerPreferencesLoaded(KZPlayer *player) override
	{
		// Дефолт ПУСТАЯ строка, а не "weapon_usp_silencer": иначе игрок, ни разу не трогавший
		// настройку, получал бы USP и за T, а владелец просил там Glock. Пустую и любую
		// неопознанную строку ResolvePreference превращает в дефолт команды — раньше она
		// молча означала «нож, пистолета нет» (GetPistolIndexByName возвращал 0).
		const char *pref = player->optionService->GetPreferenceStr("preferredPistol", "");
		i16 index = (i16)KZPistolService::GetPistolIndexByName(pref);
		player->pistolService->preferredPistol = index;
		player->pistolService->UpdatePistol();
	}
} optionEventListener;

void KZPistolService::Init()
{
	KZOptionService::RegisterEventListener(&optionEventListener);
}

SCMD(kz_pistol, SCFL_PREFERENCE | SCFL_MISC | SCFL_PLAYER)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (args->ArgC() < 2)
	{
		player->languageService->PrintChat(true, false, "Pistol Command Usage");
		return MRES_SUPERCEDE;
	}
	const char *weapon = args->ArgS();
	i16 pistolIndex = (i16)KZPistolService::GetPistolIndexByName(weapon);
	if (pistolIndex == KZPistolService::PISTOL_UNKNOWN)
	{
		player->languageService->PrintChat(true, false, "Pistol Unknown", weapon);
		return MRES_SUPERCEDE;
	}
	player->pistolService->preferredPistol = pistolIndex;
	player->pistolService->UpdatePistol();
	player->optionService->SetPreferenceStr("preferredPistol", KZPistolService::pistols[pistolIndex].className);
	if (pistolIndex == 0)
	{
		player->languageService->PrintChat(true, false, "Pistol Disabled");
		return MRES_SUPERCEDE;
	}
	player->languageService->PrintChat(true, false, "Pistol Changed", KZPistolService::pistols[pistolIndex].name);
	return MRES_SUPERCEDE;
}

void KZPistolService::UpdatePistol(bool force)
{
	if (!player->IsAlive() || !player->IsInGame())
	{
		return;
	}
	// Don't swap the weapon while the player is hiding it
	// UpdatePistol() will be called when they toggle hide weapon off.
	if (player->quietService->ShouldHideWeapon() && !force)
	{
		return;
	}

	// Выданное через !ak/!he оружие обязано пережить RemoveAllItems ниже. Сначала
	// выбрасываем из списка то, чего в руках уже нет (игрок выкинул на G), — иначе
	// перевыдача вернула бы выброшенное обратно и дроп выглядел бы сломанным.
	this->player->weaponService->SyncFromHeld();
	// Явный выбор игрока либо дефолт его команды — резолвим ОДИН раз на вызов: ниже пешку
	// временно переставляют в чужую команду, и повторный резолв взял бы чужой дефолт.
	const i16 pistolIndex = this->ResolvePreferred();
	if (pistolIndex == 0)
	{
		if (this->NeedWeaponStripping())
		{
			this->player->GetPlayerPawn()->m_pItemServices()->RemoveAllItems(false);
			auto weapon = this->player->GetPlayerPawn()->m_pItemServices()->GiveNamedItem(
				this->player->GetController()->m_iTeamNum() == CS_TEAM_CT ? "weapon_knife" : "weapon_knife_t");
			this->player->weaponService->RegiveGiven();
		}
		return;
	}

	// if another plugin modifies weapons they may be in a bad state
	// just force remove everything and always regive weapons
	this->player->GetPlayerPawn()->m_pItemServices()->RemoveAllItems(false);

	const PistolInfo_t &pistol = pistols[pistolIndex];
	i32 originalTeam = player->GetController()->m_iTeamNum();
	i32 otherTeam = originalTeam == CS_TEAM_CT ? CS_TEAM_T : CS_TEAM_CT;
	bool switchTeam = false;
	if (pistol.team == CS_TEAM_CT && originalTeam == CS_TEAM_T)
	{
		switchTeam = true;
	}
	else if (pistol.team == CS_TEAM_T && originalTeam == CS_TEAM_CT)
	{
		switchTeam = true;
	}
	else if (pistol.team == CS_TEAM_NONE && !this->player->IsFakeClient())
	{
		// Check the player's inventory. If there's a skin on this current team, don't switch. Otherwise, switch team.
		bool checkOtherTeam = true;
		CCSPlayerInventory *inventory = this->player->GetController()->m_pInventoryServices()->GetInventory();
		// LOADOUT_POSITION_SECONDARY0 to LOADOUT_POSITION_SECONDARY5
		for (i32 i = 2; i <= 7; i++)
		{
			if (inventory->m_loadoutItems[originalTeam][i].definitionIndex == pistol.itemDef
				&& inventory->m_loadoutItems[originalTeam][i].itemID != 0)
			{
				checkOtherTeam = false;
				break;
			}
		}
		if (checkOtherTeam)
		{
			for (i32 i = 2; i <= 7; i++)
			{
				if (inventory->m_loadoutItems[otherTeam][i].definitionIndex == pistol.itemDef && inventory->m_loadoutItems[otherTeam][i].itemID != 0)
				{
					switchTeam = true;
					break;
				}
			}
		}
	}
	auto knife = this->player->GetPlayerPawn()->m_pItemServices()->GiveNamedItem(
		this->player->GetController()->m_iTeamNum() == CS_TEAM_CT ? "weapon_knife" : "weapon_knife_t");
	if (switchTeam)
	{
		player->GetPlayerPawn()->m_iTeamNum(otherTeam);
	}
	auto weapon = this->player->GetPlayerPawn()->m_pItemServices()->GiveNamedItem(pistol.className);

	if (switchTeam)
	{
		player->GetPlayerPawn()->m_iTeamNum(originalTeam);
	}

	this->player->weaponService->RegiveGiven();
}

i32 KZPistolService::GetTeam()
{
	auto controller = this->player->GetController();
	// Без контроллера (ещё не в игре) команды нет — дефолт считаем как для CT: на KZ вход
	// идёт в CT (KZ::misc::JoinTeam(..., CS_TEAM_CT)), и это же значение вернёт GetTeam
	// сразу после появления контроллера.
	return controller ? (i32)controller->m_iTeamNum() : (i32)CS_TEAM_CT;
}

bool KZPistolService::NeedWeaponStripping()
{
	if (!player->IsAlive() || !player->IsInGame())
	{
		return false;
	}

	auto weapons = player->GetPlayerPawn()->m_pWeaponServices()->m_hMyWeapons();
	FOR_EACH_VEC(*weapons, i)
	{
		CBaseModelEntity *weapon = (*weapons)[i].Get();
		if (weapon && (KZ_STREQI(weapon->GetClassname(), "weapon_knife") || KZ_STREQI(weapon->GetClassname(), "weapon_knife_t")))
		{
			continue;
		}
		return true;
	}
	return false;
}
