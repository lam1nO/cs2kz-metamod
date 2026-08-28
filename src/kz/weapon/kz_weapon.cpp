#include "kz/weapon/kz_weapon.h"
#include "kz/language/kz_language.h"
#include "kz/pistol/kz_pistol.h"

#include "utils/simplecmds.h"
#include "sdk/usercmd.h"
#include "sdk/entity/cbaseplayerweapon.h"

#include "tier0/memdbgon.h"

CConVar<bool> kz_weapon_commands("kz_weapon_commands", FCVAR_NONE,
								 "Разрешить чат-команды выдачи оружия (!ak, !m4, !he, ...). Гранаты выдаются, но бросить их нельзя.", true);

// Каталог команд. weapon_taser сюда сознательно НЕ входит: это единственное, чем можно
// достать другого игрока, а вся затея — про реквизит для себя, а не про бой.
static_global const WeaponInfo_t s_weapons[] = {
	// винтовки
	{"ak",		 "weapon_ak47",			 false},
	{ "m4",		"weapon_m4a1",			false},
	{ "m4s",	   "weapon_m4a1_silencer", false},
	{ "aug",	   "weapon_aug",			 false},
	// !sg занят таймерным kz_safeguard — берём разговорные имена SG 553.
	{ "sg553",	 "weapon_sg556",		   false},
	{ "krieg",	 "weapon_sg556",		   false},
	{ "galil",	 "weapon_galilar",		 false},
	{ "famas",	 "weapon_famas",		   false},
	{ "awp",	   "weapon_awp",			 false},
	{ "scout",	 "weapon_ssg08",		   false},
	{ "ssg",	   "weapon_ssg08",		   false},
	{ "scar",	  "weapon_scar20",		  false},
	{ "g3",		"weapon_g3sg1",		   false},
	// пистолеты-пулемёты
	{ "mp9",	   "weapon_mp9",			 false},
	{ "mac10",	 "weapon_mac10",		   false},
	{ "mp7",	   "weapon_mp7",			 false},
	{ "mp5",	   "weapon_mp5sd",		   false},
	{ "ump",	   "weapon_ump45",		   false},
	{ "p90",	   "weapon_p90",			 false},
	{ "bizon",	 "weapon_bizon",		   false},
	// тяжёлое
	{ "nova",	  "weapon_nova",			false},
	{ "xm",		"weapon_xm1014",		  false},
	{ "mag7",	  "weapon_mag7",			false},
	{ "sawedoff",  "weapon_sawedoff",		false},
	{ "m249",	  "weapon_m249",			false},
	{ "negev",	 "weapon_negev",		   false},
	// пистолеты
	{ "deagle",	"weapon_deagle",		  false},
	{ "r8",		"weapon_revolver",		false},
	{ "glock",	 "weapon_glock",		   false},
	{ "usp",	   "weapon_usp_silencer",  false},
	{ "p2000",	 "weapon_hkp2000",		 false},
	{ "p250",	  "weapon_p250",			false},
	{ "tec9",	  "weapon_tec9",			false},
	{ "fiveseven", "weapon_fiveseven",	   false},
	{ "cz",		"weapon_cz75a",		   false},
	{ "elite",	 "weapon_elite",		   false},
	// гранаты — держать можно, кинуть нельзя
	{ "he",		"weapon_hegrenade",	   true },
	{ "flash",	 "weapon_flashbang",	   true },
	{ "smoke",	 "weapon_smokegrenade",   true },
	{ "molo",	  "weapon_molotov",		 true },
	{ "inc",	   "weapon_incgrenade",	 true },
	{ "decoy",	 "weapon_decoy",		   true },
};

bool KZWeaponService::Enabled()
{
	return kz_weapon_commands.GetBool();
}

const WeaponInfo_t *KZWeaponService::FindByCommand(const char *cmd)
{
	for (u32 i = 0; i < KZ_ARRAYSIZE(s_weapons); i++)
	{
		if (KZ_STREQI(s_weapons[i].cmd, cmd))
		{
			return &s_weapons[i];
		}
	}
	return nullptr;
}

bool KZWeaponService::IsGrenadeClassName(const char *className)
{
	if (!className)
	{
		return false;
	}
	for (u32 i = 0; i < KZ_ARRAYSIZE(s_weapons); i++)
	{
		if (s_weapons[i].grenade && KZ_STREQI(s_weapons[i].className, className))
		{
			return true;
		}
	}
	return false;
}

bool KZWeaponService::GiveWeapon(const WeaponInfo_t &info)
{
	if (!KZWeaponService::Enabled() || !this->player->IsAlive() || !this->player->IsInGame())
	{
		return false;
	}
	auto itemServices = this->player->GetPlayerPawn()->m_pItemServices();
	if (!itemServices)
	{
		return false;
	}
	itemServices->GiveNamedItem(info.className);

	// Дубли в списке не нужны: перевыдача одного и того же оружия дважды оставила бы
	// вторую сущность висеть без слота.
	FOR_EACH_VEC(this->givenWeapons, i)
	{
		if (KZ_STREQI(this->givenWeapons[i].Get(), info.className))
		{
			return true;
		}
	}
	this->givenWeapons.AddToTail(CUtlString(info.className));
	return true;
}

void KZWeaponService::SyncFromHeld()
{
	if (this->givenWeapons.Count() == 0 || !this->player->IsAlive() || !this->player->IsInGame())
	{
		return;
	}
	auto weaponServices = this->player->GetPlayerPawn()->m_pWeaponServices();
	if (!weaponServices)
	{
		return;
	}
	auto weapons = weaponServices->m_hMyWeapons();
	// Идём с хвоста: удаление элемента сдвигает индексы.
	for (i32 i = this->givenWeapons.Count() - 1; i >= 0; i--)
	{
		bool held = false;
		FOR_EACH_VEC(*weapons, j)
		{
			CBaseModelEntity *weapon = (*weapons)[j].Get();
			if (weapon && KZ_STREQI(weapon->GetClassname(), this->givenWeapons[i].Get()))
			{
				held = true;
				break;
			}
		}
		if (!held)
		{
			// Игрок выбросил его на G — значит и перевыдавать нечего.
			this->givenWeapons.Remove(i);
		}
	}
}

void KZWeaponService::RegiveGiven()
{
	if (!KZWeaponService::Enabled() || this->givenWeapons.Count() == 0)
	{
		return;
	}
	if (!this->player->IsAlive() || !this->player->IsInGame())
	{
		return;
	}
	auto itemServices = this->player->GetPlayerPawn()->m_pItemServices();
	if (!itemServices)
	{
		return;
	}
	FOR_EACH_VEC(this->givenWeapons, i)
	{
		itemServices->GiveNamedItem(this->givenWeapons[i].Get());
	}
}

bool KZWeaponService::HoldingGrenade()
{
	if (!this->player->IsAlive() || !this->player->IsInGame())
	{
		return false;
	}
	auto weaponServices = this->player->GetPlayerPawn()->m_pWeaponServices();
	if (!weaponServices)
	{
		return false;
	}
	CBasePlayerWeapon *active = weaponServices->m_hActiveWeapon().Get();
	if (!active)
	{
		return false;
	}
	CBaseModelEntity *entity = active;
	return KZWeaponService::IsGrenadeClassName(entity->GetClassname());
}

void KZWeaponService::OnProcessUsercmds(PlayerCommand *cmds, int numcmds)
{
	if (!KZWeaponService::Enabled() || this->givenWeapons.Count() == 0)
	{
		return;
	}
	if (!this->HoldingGrenade())
	{
		return;
	}
	// Гасим на уровне ВВОДА, до ProcessUsercmds: снаряд не рождается вообще. Удалять
	// уже брошенный снаряд было бы поздно — звук вырывания чеки и бросок клиент бы
	// уже показал, а другим игрокам граната успела бы помешать.
	for (i32 i = 0; i < numcmds; i++)
	{
		PlayerCommand *pc = &cmds[i];
		CBaseUserCmdPB *base = pc->mutable_base();
		CInButtonStatePB *buttons = base->mutable_buttons_pb();
		buttons->set_buttonstate1(buttons->buttonstate1() & ~((u64)IN_ATTACK | (u64)IN_ATTACK2));

		// Атака приходит и субтиковыми шагами — там бит в buttonstate1 может быть уже
		// снят, а нажатие всё равно отработает. Превращаем нажатие в отпускание.
		for (i32 j = 0; j < base->subtick_moves_size(); j++)
		{
			CSubtickMoveStep *step = base->mutable_subtick_moves(j);
			if (step->button() == IN_ATTACK || step->button() == IN_ATTACK2)
			{
				step->set_pressed(false);
			}
		}
	}
}

static_function META_RES GiveByCommand(CCSPlayerController *controller, const char *cmdName)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!KZWeaponService::Enabled())
	{
		return MRES_SUPERCEDE;
	}
	const WeaponInfo_t *info = KZWeaponService::FindByCommand(cmdName);
	if (!info)
	{
		return MRES_SUPERCEDE;
	}
	if (!player->weaponService->GiveWeapon(*info))
	{
		player->languageService->PrintChat(true, false, "Weapon Give Failed");
		return MRES_SUPERCEDE;
	}
	if (info->grenade)
	{
		player->languageService->PrintChat(true, false, "Weapon Given Grenade");
	}
	else
	{
		player->languageService->PrintChat(true, false, "Weapon Given");
	}
	return MRES_SUPERCEDE;
}

// Одна команда на каждую строку каталога. Имя callback'а обязано совпадать с именем
// команды (макрос SCMD склеивает name##_callback), поэтому пишем их списком, а не циклом.
#define KZ_WEAPON_CMD(shortName) \
	SCMD(kz_##shortName, SCFL_MISC | SCFL_PLAYER) \
	{ \
		return GiveByCommand(controller, #shortName); \
	}

KZ_WEAPON_CMD(ak)
KZ_WEAPON_CMD(m4)
KZ_WEAPON_CMD(m4s)
KZ_WEAPON_CMD(aug)
KZ_WEAPON_CMD(sg553)
KZ_WEAPON_CMD(krieg)
KZ_WEAPON_CMD(galil)
KZ_WEAPON_CMD(famas)
KZ_WEAPON_CMD(awp)
KZ_WEAPON_CMD(scout)
KZ_WEAPON_CMD(ssg)
KZ_WEAPON_CMD(scar)
KZ_WEAPON_CMD(g3)
KZ_WEAPON_CMD(mp9)
KZ_WEAPON_CMD(mac10)
KZ_WEAPON_CMD(mp7)
KZ_WEAPON_CMD(mp5)
KZ_WEAPON_CMD(ump)
KZ_WEAPON_CMD(p90)
KZ_WEAPON_CMD(bizon)
KZ_WEAPON_CMD(nova)
KZ_WEAPON_CMD(xm)
KZ_WEAPON_CMD(mag7)
KZ_WEAPON_CMD(sawedoff)
KZ_WEAPON_CMD(m249)
KZ_WEAPON_CMD(negev)
KZ_WEAPON_CMD(deagle)
KZ_WEAPON_CMD(r8)
KZ_WEAPON_CMD(glock)
KZ_WEAPON_CMD(usp)
KZ_WEAPON_CMD(p2000)
KZ_WEAPON_CMD(p250)
KZ_WEAPON_CMD(tec9)
KZ_WEAPON_CMD(fiveseven)
KZ_WEAPON_CMD(cz)
KZ_WEAPON_CMD(elite)
KZ_WEAPON_CMD(he)
KZ_WEAPON_CMD(flash)
KZ_WEAPON_CMD(smoke)
KZ_WEAPON_CMD(molo)
KZ_WEAPON_CMD(inc)
KZ_WEAPON_CMD(decoy)

#undef KZ_WEAPON_CMD
