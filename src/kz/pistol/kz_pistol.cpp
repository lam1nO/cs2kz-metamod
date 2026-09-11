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
	// Гард на сервисы пешки, а не только на IsAlive: тот смотрит m_lifeState и про сервисы
	// ничего не знает. До этого коммита функция звалась только после Respawn() в JoinTeam и
	// из чат-команд, то есть по заведомо доделанной пешке; теперь её зовёт и player_spawn —
	// самый ранний момент жизни пешки, и сразу на всех игроках на старте карты и на
	// mp_restartgame. Дальше пользуемся локальной переменной: повторный m_pItemServices()
	// между RemoveAllItems и GiveNamedItem читал бы то же поле ещё три раза.
	auto pawn = this->player->GetPlayerPawn();
	auto itemServices = pawn ? pawn->m_pItemServices() : nullptr;
	if (!itemServices)
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
			itemServices->RemoveAllItems(false);
			auto weapon = itemServices->GiveNamedItem(KZWeaponService::KnifeClassNameForTeam(this->GetTeam()));
			this->player->weaponService->RegiveGiven();
		}
		return;
	}

	// if another plugin modifies weapons they may be in a bad state
	// just force remove everything and always regive weapons
	itemServices->RemoveAllItems(false);

	const PistolInfo_t &pistol = pistols[pistolIndex];
	// Через GetTeam(), а не сырым GetController()->m_iTeamNum(): контроллер там проверен, а
	// путь теперь идёт и со спавна.
	i32 originalTeam = this->GetTeam();
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
		// Три разыменования подряд, и все три могут быть пустыми на раннем кадре спавна
		// (контроллер, inventory services, сам инвентарь). Нет инвентаря — нечего и
		// сверять: остаёмся в своей команде, подмену не делаем.
		auto controller = this->player->GetController();
		auto inventoryServices = controller ? controller->m_pInventoryServices() : nullptr;
		CCSPlayerInventory *inventory = inventoryServices ? inventoryServices->GetInventory() : nullptr;
		if (inventory)
		{
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
					if (inventory->m_loadoutItems[otherTeam][i].definitionIndex == pistol.itemDef
						&& inventory->m_loadoutItems[otherTeam][i].itemID != 0)
					{
						switchTeam = true;
						break;
					}
				}
			}
		}
	}
	auto knife = itemServices->GiveNamedItem(KZWeaponService::KnifeClassNameForTeam(originalTeam));
	if (switchTeam)
	{
		pawn->m_iTeamNum(otherTeam);
	}
	auto weapon = itemServices->GiveNamedItem(pistol.className);

	if (switchTeam)
	{
		pawn->m_iTeamNum(originalTeam);
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

	// Гард на weapon services по той же причине, что и на item services в UpdatePistol:
	// функция зовётся с пути player_spawn, где пешка может быть ещё недоделана.
	auto pawn = player->GetPlayerPawn();
	auto weaponServices = pawn ? pawn->m_pWeaponServices() : nullptr;
	if (!weaponServices)
	{
		return false;
	}
	auto weapons = weaponServices->m_hMyWeapons();
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

// Руки уже такие, какими их сделал бы UpdatePistol? Тогда трогать сущности не надо.
//
// Это НЕ оптимизация, а снятие гонки двух плагинов. RemoveAllItems + GiveNamedItem в кадре
// спавна встают ровно в то окно, в котором cyber-skins забирает сущность оружия и применяет
// скин отложенным вызовом (RunOnTick+N, CCSPlayerControllerExtensions.cs) — а порядок
// обработчиков player_spawn у metamod и CSSharp мы не контролируем. Класс — девять
// сегфолтов 09.09 (WriteEnterPVS: GetEntServerClass failed, память проекта
// skins-spawn-regive-server-crash). Плюс масштаб: на старте карты это N игроков ×
// (RemoveAllItems + 2×GiveNamedItem) в одном кадре, чего в дереве не было никогда. На
// KZ-профиле штатный спавн за CT уже даёт нож и mp_ct_default_secondary
// (weapon_usp_silencer), то есть ровно то, что выдали бы мы, — значит спавн за CT
// перестаёт трогать сущности вовсе, а чинятся только реально сломанные руки, ради которых
// правка и делалась. За T гейт пока НЕ срабатывает: профиль ставит тот же
// mp_t_default_secondary weapon_usp_silencer, а дефолт за T теперь Glock — это чинится
// правкой профиля (gameops/profiles/kz), не здесь.
//
// Сверка обоих стволов — точная, по classname. Опознавать нож «отрицанием» (чего нет ни в
// одном каталоге) НЕЛЬЗЯ, хотя соблазн есть: тогда за нож сошли бы weapon_taser (он вне
// каталога команд сознательно), weapon_c4 и healthshot, и «в руках тазер, ножа нет»
// читалось бы как «всё в порядке». Скин ножа classname НЕ меняет: cyber-skins подменяет
// только CEconItemView в хуке GiveNamedItem, а RegiveKnife выдаёт явные
// weapon_knife/weapon_knife_t — weapon_knife_karambit в GiveNamedItem не приходит никогда.
// Косвенное доказательство прямо в этом файле: NeedWeaponStripping и !knife сверяют нож по
// classname давно, и будь это неверно, они были бы сломаны задолго до этой правки.
bool KZPistolService::HasExpectedLoadout()
{
	auto pawn = this->player->GetPlayerPawn();
	auto weaponServices = pawn ? pawn->m_pWeaponServices() : nullptr;
	if (!weaponServices)
	{
		return false;
	}
	const i16 pistolIndex = this->ResolvePreferred();
	if (pistolIndex == 0)
	{
		// Игрок ЯВНО попросил «без пистолета». Ожидаемые руки — один только нож, и ровно
		// этот вопрос задаёт NeedWeaponStripping: всё, что не нож, — повод раздеть.
		// Безусловное «пистолет на месте» здесь было бы инверсным дефектом: движок на
		// спавне выдаёт mp_*_default_secondary, и выключивший пистолет игрок оставался бы
		// с ним после mp_restartgame/форс-пика/смены карты, а ветка снятия в UpdatePistol
		// не получала бы управления вовсе.
		return !this->NeedWeaponStripping();
	}
	const char *pistolClass = pistols[pistolIndex].className;
	const char *knifeClass = KZWeaponService::KnifeClassNameForTeam(this->GetTeam());
	bool hasPistol = false;
	bool hasKnife = false;
	auto weapons = weaponServices->m_hMyWeapons();
	FOR_EACH_VEC(*weapons, i)
	{
		CBaseModelEntity *weapon = (*weapons)[i].Get();
		if (!weapon)
		{
			continue;
		}
		const char *className = weapon->GetClassname();
		if (KZ_STREQI(className, knifeClass))
		{
			hasKnife = true;
		}
		else if (KZ_STREQI(className, pistolClass))
		{
			hasPistol = true;
		}
	}
	// Лишнее в руках (выданное !ak, подобранное) гейт не волнует: оно не делает руки
	// «сломанными», а страйп ради него — та самая лишняя подмена сущностей.
	return hasKnife && hasPistol;
}
