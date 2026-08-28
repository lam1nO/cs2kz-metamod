#include "kz/weapon/kz_weapon.h"
#include "kz/language/kz_language.h"

#include "utils/simplecmds.h"
#include "sdk/usercmd.h"
#include "sdk/entity/cbaseplayerweapon.h"

#include "tier0/memdbgon.h"

CConVar<bool> kz_weapon_commands("kz_weapon_commands", FCVAR_NONE,
								 "Enable weapon chat commands (!ak, !m4, !he, ...). Grenades can be held but not thrown.", true);

// Каталог команд. weapon_taser сюда сознательно НЕ входит: это единственное, чем можно
// достать другого игрока, а вся затея — про реквизит для себя, а не про бой.
// clang-format off
static_global const WeaponInfo_t s_weapons[] = {
	// винтовки
	{ "ak", "weapon_ak47", WeaponSlotKind::Rifle},
	{ "m4", "weapon_m4a1", WeaponSlotKind::Rifle},
	{ "m4s", "weapon_m4a1_silencer", WeaponSlotKind::Rifle},
	{ "aug", "weapon_aug", WeaponSlotKind::Rifle},
	// !sg занят таймерным kz_safeguard — берём разговорные имена SG 553.
	{ "sg553", "weapon_sg556", WeaponSlotKind::Rifle},
	{ "krieg", "weapon_sg556", WeaponSlotKind::Rifle},
	{ "galil", "weapon_galilar", WeaponSlotKind::Rifle},
	{ "famas", "weapon_famas", WeaponSlotKind::Rifle},
	{ "awp", "weapon_awp", WeaponSlotKind::Rifle},
	{ "scout", "weapon_ssg08", WeaponSlotKind::Rifle},
	{ "ssg", "weapon_ssg08", WeaponSlotKind::Rifle},
	{ "scar", "weapon_scar20", WeaponSlotKind::Rifle},
	{ "g3", "weapon_g3sg1", WeaponSlotKind::Rifle},
	// пистолеты-пулемёты
	{ "mp9", "weapon_mp9", WeaponSlotKind::Rifle},
	{ "mac10", "weapon_mac10", WeaponSlotKind::Rifle},
	{ "mp7", "weapon_mp7", WeaponSlotKind::Rifle},
	{ "mp5", "weapon_mp5sd", WeaponSlotKind::Rifle},
	{ "ump", "weapon_ump45", WeaponSlotKind::Rifle},
	{ "p90", "weapon_p90", WeaponSlotKind::Rifle},
	{ "bizon", "weapon_bizon", WeaponSlotKind::Rifle},
	// тяжёлое
	{ "nova", "weapon_nova", WeaponSlotKind::Rifle},
	{ "xm", "weapon_xm1014", WeaponSlotKind::Rifle},
	{ "mag7", "weapon_mag7", WeaponSlotKind::Rifle},
	{ "sawedoff", "weapon_sawedoff", WeaponSlotKind::Rifle},
	{ "m249", "weapon_m249", WeaponSlotKind::Rifle},
	{ "negev", "weapon_negev", WeaponSlotKind::Rifle},
	// пистолеты
	{ "deagle", "weapon_deagle", WeaponSlotKind::Pistol},
	{ "r8", "weapon_revolver", WeaponSlotKind::Pistol},
	{ "glock", "weapon_glock", WeaponSlotKind::Pistol},
	{ "usp", "weapon_usp_silencer", WeaponSlotKind::Pistol},
	{ "p2000", "weapon_hkp2000", WeaponSlotKind::Pistol},
	{ "p250", "weapon_p250", WeaponSlotKind::Pistol},
	{ "tec9", "weapon_tec9", WeaponSlotKind::Pistol},
	{ "fiveseven", "weapon_fiveseven", WeaponSlotKind::Pistol},
	{ "cz", "weapon_cz75a", WeaponSlotKind::Pistol},
	{ "elite", "weapon_elite", WeaponSlotKind::Pistol},
	// гранаты — держать можно, кинуть нельзя
	{ "he", "weapon_hegrenade", WeaponSlotKind::Grenade},
	{ "flash", "weapon_flashbang", WeaponSlotKind::Grenade},
	{ "smoke", "weapon_smokegrenade", WeaponSlotKind::Grenade},
	{ "molo", "weapon_molotov", WeaponSlotKind::Grenade},
	{ "inc", "weapon_incgrenade", WeaponSlotKind::Grenade},
	{ "decoy", "weapon_decoy", WeaponSlotKind::Grenade},
};
// clang-format on

void KZWeaponService::RemoveDroppedEntity(i32 index)
{
	// Трогаем ТОЛЬКО ничейное: у оружия, подобранного другим игроком, владелец не пуст.
	// Ошибиться здесь — значит вынуть ствол из чужих рук, поэтому гард обязателен.
	CBaseEntity *dropped = this->givenWeapons[index].entity.Get();
	if (dropped && !dropped->m_hOwnerEntity().Get())
	{
		g_pKZUtils->RemoveEntity(dropped);
	}
}

void KZWeaponService::Reset()
{
	// Дисконнект. Всё, что игрок успел выбросить и что не прошло через SyncFromHeld,
	// иначе осталось бы в мире до смены карты — а реконнекты это множат.
	for (i32 i = 0; i < this->givenWeapons.Count(); i++)
	{
		this->RemoveDroppedEntity(i);
	}
	this->givenWeapons.RemoveAll();
}

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

const WeaponInfo_t *KZWeaponService::FindByClassName(const char *className)
{
	if (!className)
	{
		return nullptr;
	}
	for (u32 i = 0; i < KZ_ARRAYSIZE(s_weapons); i++)
	{
		if (KZ_STREQI(s_weapons[i].className, className))
		{
			return &s_weapons[i];
		}
	}
	return nullptr;
}

bool KZWeaponService::IsGrenadeClassName(const char *className)
{
	const WeaponInfo_t *info = KZWeaponService::FindByClassName(className);
	return info && info->slot == WeaponSlotKind::Grenade;
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

	// Освободить слот ДО выдачи: иначе новый ствол не заменит текущий, а упадёт на землю
	// и не поднимется — слот-то занят. Симптом с канарейки: «!glock с usp в руках →
	// glock падает передо мной, остаюсь с usp».
	GiveResult cleared = this->ClearSlot(info.slot);
	if (cleared != GiveResult::Ok)
	{
		return cleared;
	}

	CBasePlayerWeapon *weapon = itemServices->GiveNamedItem(info.className);
	if (!weapon)
	{
		return GiveResult::Internal;
	}
	// Ствол мог не встать в слот и упасть на пол — например, если слот занимало оружие
	// ВНЕ нашего каталога (taser, C4, выданное чужим плагином), которое ClearSlot не
	// классифицировал и потому не снял. Дешёвый общий детектор этого класса: проверяем,
	// что сущность реально попала в m_hMyWeapons. Ловит и будущие случаи, и даёт
	// наблюдаемость на канарейке.
	bool held = false;
	auto myWeapons = this->player->GetPlayerPawn()->m_pWeaponServices()->m_hMyWeapons();
	FOR_EACH_VEC(*myWeapons, i)
	{
		if ((*myWeapons)[i].Get() == weapon)
		{
			held = true;
			break;
		}
	}
	if (!held)
	{
		KZ_LOG_WARN(LogChannel::Misc, "[cyb] weapon_given_not_held steam_id=%llu weapon=%s reason=slot_occupied\n", this->player->GetSteamId64(false),
					info.className);
		return GiveResult::SlotBusy;
	}

	CBaseModelEntity *entity = weapon;
	GivenWeapon_t given;
	given.requested = info.className;
	// Фактический classname может отличаться от запрошенного — см. комментарий у
	// GivenWeapon_t. Берём его с САМОЙ сущности, а не из таблицы подмен: таблица
	// протухнет на следующем апдейте Valve, сущность — нет.
	given.actual = entity->GetClassname();
	given.entity = weapon;
	this->givenWeapons.AddToTail(given);
	return GiveResult::Ok;
}

GiveResult KZWeaponService::ClearSlot(WeaponSlotKind slot)
{
	// Гранаты не трогаем: слотов под них несколько, he+flash+smoke держатся вместе.
	if (slot == WeaponSlotKind::Grenade || !this->player->IsAlive() || !this->player->IsInGame())
	{
		return GiveResult::Ok;
	}
	auto pawn = this->player->GetPlayerPawn();
	auto weaponServices = pawn->m_pWeaponServices();
	auto itemServices = pawn->m_pItemServices();
	if (!weaponServices || !itemServices)
	{
		// Слот ни при чём: сломалась пешка. Игроку «выброси оружие на G» тут было бы
		// враньём — повтор не поможет.
		KZ_LOG_ERROR(LogChannel::Misc, "[cyb] weapon_slot_clear_failed steam_id=%llu reason=no_services\n", this->player->GetSteamId64(false));
		return GiveResult::Internal;
	}
	// Сначала СОБИРАЕМ, потом снимаем: удаление во время обхода m_hMyWeapons ломает
	// итерацию (тот же приём, что в KZPistolService::NeedWeaponStripping).
	CUtlVector<CBasePlayerWeapon *> victims;
	auto weapons = weaponServices->m_hMyWeapons();
	FOR_EACH_VEC(*weapons, i)
	{
		CBasePlayerWeapon *weapon = (*weapons)[i].Get();
		if (!weapon)
		{
			continue;
		}
		CBaseModelEntity *entity = weapon;
		// Классифицируем по НАШЕМУ каталогу. Чего в нём нет — не наше дело: нож
		// (weapon_knife/_t) в каталог не входит и потому никогда сюда не попадёт.
		const WeaponInfo_t *held = KZWeaponService::FindByClassName(entity->GetClassname());
		if (held && held->slot == slot)
		{
			victims.AddToTail(weapon);
		}
	}

	GiveResult result = GiveResult::Ok;
	FOR_EACH_VEC(victims, i)
	{
		CBasePlayerWeapon *weapon = victims[i];
		// DropActiveWeapon бросает АКТИВНОЕ оружие, а параметр реального выбора не делает.
		// Замерено владельцем на канарейке: с AK в руках команда !r8 выбрасывала AK, а не
		// usp, и снятие пистолета «не удавалось». Поэтому жертву сперва делаем активной —
		// ровно так это делает проверенный прод-код cyber-35hp (Cyber35hp.cs:809).
		weaponServices->m_hActiveWeapon(weapon);
		itemServices->DropActiveWeapon(weapon);

		// ТРЕТИЙ ШАГ, без которого этот код — тот самый краш 21.08. DropActiveWeapon
		// может оказаться пустышкой по пока не известной нам причине, и тогда
		// RemoveEntity убьёт сущность, на которую ещё смотрят m_hMyWeapons и
		// m_hActiveWeapon клиента. Поэтому УДАЛЯЕМ ТОЛЬКО ОТЦЕПЛЁННОЕ, а если
		// отцепить не вышло — честно отказываем в выдаче, а не роняем игрока.
		bool stillHeld = false;
		auto held = weaponServices->m_hMyWeapons();
		FOR_EACH_VEC(*held, j)
		{
			if ((*held)[j].Get() == weapon)
			{
				stillHeld = true;
				break;
			}
		}
		if (stillHeld)
		{
			KZ_LOG_WARN(LogChannel::Misc, "[cyb] weapon_slot_not_freed steam_id=%llu weapon=%s reason=drop_no_op\n",
						this->player->GetSteamId64(false), ((CBaseModelEntity *)weapon)->GetClassname());
			result = GiveResult::SlotBusy;
			continue;
		}

		// Своя запись о нём больше не нужна — иначе RegiveGiven вернёт снятое обратно.
		for (i32 j = this->givenWeapons.Count() - 1; j >= 0; j--)
		{
			if (this->givenWeapons[j].entity.Get() == (CBaseEntity *)weapon)
			{
				this->givenWeapons.Remove(j);
			}
		}
		g_pKZUtils->RemoveEntity(weapon);
	}
	// Активное оружие после дропа НЕ восстанавливаем сырой записью в m_hActiveWeapon:
	// она не проходит через Deploy/Holster, и у движка осталась бы вьюмодель прошлого
	// ствола (в этом же форке playback.cpp для реального переключения идёт через
	// set_weaponselect, а сырой записью пользуется только чтобы обнулить). Движок сам
	// выберет следующее оружие штатным путём — это корректнее и не наша забота.
	return result;
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
		CBaseEntity *tracked = this->givenWeapons[i].entity.Get();
		FOR_EACH_VEC(*weapons, j)
		{
			CBaseModelEntity *weapon = (*weapons)[j].Get();
			if (!weapon)
			{
				continue;
			}
			// Сверка по ХЭНДЛУ, а не по имени: имена не уникальны. Игрок мог выбросить
			// свой AK и поднять чужой — по classname запись считалась бы «в руках», её
			// сущность никогда не убралась бы, и поле entity перестало бы работать.
			// Хэндл всегда актуален: RegiveGiven переписывает его при каждой перевыдаче.
			if (tracked && weapon == tracked)
			{
				held = true;
				break;
			}
			// Фолбэк по фактическому имени — на случай, если хэндл протух, а оружие с тем
			// же classname в руках есть (перевыдача чужим кодом). Лучше не убрать лишнего.
			if (!tracked && KZ_STREQI(weapon->GetClassname(), this->givenWeapons[i].actual.Get()))
			{
				held = true;
				break;
			}
		}
		if (!held)
		{
			// Игрок выбросил его на G — значит и перевыдавать нечего. Заодно убираем
			// сущность из мира: в KZ раунд не кончается, посмертной уборки нет, и без
			// этого спам «выдал → выбросил» копил бы энтити без предела. Трогаем ТОЛЬКО
			// ничейное: у подобранного другим игроком владелец не пуст.
			this->RemoveDroppedEntity(i);
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
	// Слоты освобождаем ОДНИМ проходом ДО выдачи. Два повода: UpdatePistol только что
	// выдал предпочтительный пистолет (строка 131), и перевыдача нашего glock'а ушла бы
	// в занятый слот — тот же «падает передо мной, остаюсь с usp», только молча, без
	// команды в чате. И второй: ClearSlot мутирует givenWeapons, поэтому звать его
	// внутри цикла по этому же вектору нельзя — собьётся индексация.
	bool needRifle = false;
	bool needPistol = false;
	FOR_EACH_VEC(this->givenWeapons, i)
	{
		const WeaponInfo_t *info = KZWeaponService::FindByClassName(this->givenWeapons[i].requested.Get());
		if (!info)
		{
			continue;
		}
		if (info->slot == WeaponSlotKind::Rifle)
		{
			needRifle = true;
		}
		else if (info->slot == WeaponSlotKind::Pistol)
		{
			needPistol = true;
		}
	}
	bool rifleFree = !needRifle || this->ClearSlot(WeaponSlotKind::Rifle) == GiveResult::Ok;
	bool pistolFree = !needPistol || this->ClearSlot(WeaponSlotKind::Pistol) == GiveResult::Ok;

	// С хвоста: неудачную выдачу удаляем на месте, а Remove сдвигает индексы.
	for (i32 i = this->givenWeapons.Count() - 1; i >= 0; i--)
	{
		// Слот не освободился — выдавать нельзя: ствол упал бы на пол молча, без команды
		// в чате и без шанса объяснить игроку, что произошло. Запись сохраняем: следующая
		// перевыдача попробует снова.
		const WeaponInfo_t *info = KZWeaponService::FindByClassName(this->givenWeapons[i].requested.Get());
		if (info && ((info->slot == WeaponSlotKind::Rifle && !rifleFree) || (info->slot == WeaponSlotKind::Pistol && !pistolFree)))
		{
			continue;
		}
		CBasePlayerWeapon *weapon = itemServices->GiveNamedItem(this->givenWeapons[i].requested.Get());
		if (!weapon)
		{
			// Отказ — с машинно-читаемым reason (CLAUDE.md). Мёртвую запись убираем сразу:
			// иначе она висела бы до следующего sync и обещала несуществующее оружие.
			KZ_LOG_ERROR(LogChannel::Misc, "[cyb] weapon_regive_failed steam_id=%llu weapon=%s reason=give_returned_null\n",
						 this->player->GetSteamId64(false), this->givenWeapons[i].requested.Get());
			this->givenWeapons.Remove(i);
			continue;
		}
		// ОБЯЗАТЕЛЬНО обновлять: движок подменяет предмет по КОМАНДЕ игрока, а команда с
		// прошлой выдачи могла смениться (за T просили weapon_molotov и получали его же,
		// после jointeam CT тот же запрос отдаёт weapon_incgrenade). Не обновив actual,
		// мы бы разошлись с реальностью: ближайший SyncFromHeld выбросил бы запись, дедуп
		// перестал бы её узнавать, а следующий страйп потерял бы гранату молча.
		CBaseModelEntity *entity = weapon;
		this->givenWeapons[i].actual = entity->GetClassname();
		this->givenWeapons[i].entity = weapon;
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
		case GiveResult::SlotBusy:
			player->languageService->PrintChat(true, false, "Weapon Slot Busy");
			return MRES_SUPERCEDE;
		case GiveResult::LimitHit:
			player->languageService->PrintChat(true, false, "Weapon Limit Reached");
			return MRES_SUPERCEDE;
		case GiveResult::Internal:
			// Отказ по нашей вине — в лог с машинно-читаемым reason (CLAUDE.md), и игроку
			// не «ты мёртв»: он жив, сломалось у нас.
			KZ_LOG_ERROR(LogChannel::Misc, "[cyb] weapon_give_failed steam_id=%llu weapon=%s reason=give_internal\n", player->GetSteamId64(false),
						 info->className);
			player->languageService->PrintChat(true, false, "Weapon Give Internal Error");
			return MRES_SUPERCEDE;
		case GiveResult::NotAlive:
		default:
			player->languageService->PrintChat(true, false, "Weapon Give Failed");
			return MRES_SUPERCEDE;
	}
	if (info->slot == WeaponSlotKind::Grenade)
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
		if (s_weapons[i].slot == WeaponSlotKind::Grenade)
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
