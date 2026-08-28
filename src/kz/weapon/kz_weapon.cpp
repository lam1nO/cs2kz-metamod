#include "kz/weapon/kz_weapon.h"
#include "kz/language/kz_language.h"
#include "kz/pistol/kz_pistol.h"

#include "utils/simplecmds.h"
#include "sdk/usercmd.h"
#include "sdk/entity/cbaseplayerweapon.h"

#include "tier0/memdbgon.h"

CConVar<bool> kz_weapon_commands("kz_weapon_commands", FCVAR_NONE,
								 "Enable weapon chat commands (!ak, !m4, !he, ...). Grenades can be held but not thrown.", true);

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

GiveResult KZWeaponService::GiveWeapon(const WeaponInfo_t &info)
{
	if (!KZWeaponService::Enabled() || !this->player->IsAlive() || !this->player->IsInGame())
	{
		return GiveResult::NotAlive;
	}
	auto itemServices = this->player->GetPlayerPawn()->m_pItemServices();
	if (!itemServices)
	{
		return GiveResult::Internal;
	}

	// Синхронизация ПЕРВЫМ делом. Без неё дедуп ниже сверяется со списком, а не с
	// реальностью: игрок выбросил AK на G (а дроп мы сами и включили, и фраза про «G —
	// выбросить» это прямо обещает) — в списке AK остался, повторный !ak стал бы no-op,
	// и игрок остался бы без оружия при бодром «Оружие выдано» в чате. SyncFromHeld
	// зовётся ещё и из UpdatePistol, но тот на !r и телепорте не срабатывает. Обход
	// m_hMyWeapons дешёвый и не в горячем пути.
	this->SyncFromHeld();

	// Дедуп по ЗАПРОШЕННОМУ имени и ДО выдачи. По запрошенному — потому что движок
	// подменяет предмет по команде (CT: weapon_molotov → weapon_incgrenade), и сверка по
	// фактическому промахивалась бы каждый раз. До выдачи — потому что GiveNamedItem уже
	// имеющегося оружия плодит вторую сущность в мире, и список от этого не спасает.
	FOR_EACH_VEC(this->givenWeapons, i)
	{
		if (KZ_STREQI(this->givenWeapons[i].requested.Get(), info.className))
		{
			return GiveResult::Ok; // повтор команды — no-op
		}
	}
	// Потолок: в KZ раунд не кончается, выброшенное на G лежит до смены карты. Без капа
	// «выдал → выбросил → выдал» растит число энтити без предела.
	if (this->givenWeapons.Count() >= KZ_MAX_GIVEN_WEAPONS)
	{
		return GiveResult::LimitHit;
	}

	CBasePlayerWeapon *weapon = itemServices->GiveNamedItem(info.className);
	if (!weapon)
	{
		return GiveResult::Internal;
	}
	CBaseModelEntity *entity = weapon;
	GivenWeapon_t given;
	given.requested = info.className;
	// Фактический classname может отличаться от запрошенного — см. комментарий у
	// GivenWeapon_t. Берём его с САМОЙ сущности, а не из таблицы подмен: таблица
	// протухнет на следующем апдейте Valve, сущность — нет.
	given.actual = entity->GetClassname();
	this->givenWeapons.AddToTail(given);
	return GiveResult::Ok;
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
		itemServices->GiveNamedItem(this->givenWeapons[i].requested.Get());
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
	// Гейт — ТОЛЬКО «в руках граната». Проверять givenWeapons здесь нельзя: гранату можно
	// подобрать с земли (её выбросили на G — а дроп мы сами и включили), и у подобравшего
	// список пуст. Иначе запрет обходился бы «выбросил → поднял → кинул», в том числе
	// чужую гранату любым игроком.
	if (!KZWeaponService::Enabled() || !this->HoldingGrenade())
	{
		return;
	}
	// Гасим на уровне ВВОДА, до ProcessUsercmds: снаряд не рождается вообще. Удалять
	// уже брошенный снаряд было бы поздно — звук вырывания чеки и бросок клиент бы
	// уже показал, а другим игрокам граната успела бы помешать.
	const u64 attack = (u64)IN_ATTACK | (u64)IN_ATTACK2;
	for (i32 i = 0; i < numcmds; i++)
	{
		PlayerCommand *pc = &cmds[i];
		CBaseUserCmdPB *base = pc->mutable_base();
		CInButtonStatePB *buttons = base->mutable_buttons_pb();
		// ВСЕ ТРИ слова, а не только первое: CInButtonState::IsButtonPressed
		// (sdk/cinbuttonstate.h) считает кнопку нажатой при keyState > IN_BUTTON_DOWN_UP,
		// то есть по битам из [1]/[2] даже при нулевом [0]. Плюс зеркало в собственную
		// копию PlayerCommand::buttonstates — из неё форк читает напрямую, и туда же
		// пишет playback.cpp:556-568, когда ему нужен реальный эффект.
		buttons->set_buttonstate1(buttons->buttonstate1() & ~attack);
		buttons->set_buttonstate2(buttons->buttonstate2() & ~attack);
		buttons->set_buttonstate3(buttons->buttonstate3() & ~attack);
		for (i32 w = 0; w < 3; w++)
		{
			pc->buttonstates.m_pButtonStates[w] &= ~attack;
		}

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
	switch (player->weaponService->GiveWeapon(*info))
	{
		case GiveResult::Ok:
			break;
		case GiveResult::LimitHit:
			player->languageService->PrintChat(true, false, "Weapon Limit Reached");
			return MRES_SUPERCEDE;
		case GiveResult::Internal:
			// Отказ по нашей вине — в лог с машинно-читаемым reason (CLAUDE.md).
			KZ_LOG_ERROR(LogChannel::Misc, "[cyb] weapon_give_failed steam_id=%llu weapon=%s reason=give_internal\n", player->GetSteamId64(false),
						 info->className);
			player->languageService->PrintChat(true, false, "Weapon Give Failed");
			return MRES_SUPERCEDE;
		case GiveResult::NotAlive:
		default:
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

// Список доступного оружия. Единственная видимая в !help команда этого сервиса.
// Печатаем в КОНСОЛЬ: 42 строки в чат — это спам, который вытеснит всё остальное.
SCMD(kz_guns, SCFL_MISC | SCFL_PLAYER | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!KZWeaponService::Enabled())
	{
		return MRES_SUPERCEDE;
	}
	player->languageService->PrintChat(true, false, "Weapon List Chat");
	player->PrintConsole(false, false, "");
	std::string note = player->languageService->PrepareMessage("Weapon List Grenade Note");
	for (u32 i = 0; i < KZ_ARRAYSIZE(s_weapons); i++)
	{
		if (s_weapons[i].grenade)
		{
			player->PrintConsole(false, false, "!%s  %s", s_weapons[i].cmd, note.c_str());
		}
		else
		{
			player->PrintConsole(false, false, "!%s", s_weapons[i].cmd);
		}
	}
	return MRES_SUPERCEDE;
}

// Одна команда на каждую строку каталога. Имя callback'а обязано совпадать с именем
// команды (макрос SCMD склеивает name##_callback), поэтому пишем их списком, а не циклом.
// SCFL_HIDDEN (= 0, без категории): 42 команды в категории Misc распухли бы вчетверо
// таблицу !help. Обнаруживаются через !guns ниже — он в !help есть.
#define KZ_WEAPON_CMD(shortName) \
	SCMD(kz_##shortName, SCFL_HIDDEN) \
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
