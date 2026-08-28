#pragma once
#include "kz/kz.h"

// Выдача оружия по чат-командам (!ak, !m4, !he, ...) на наших не-global KZ-серверах.
// Отдельный сервис с собственным тумблером kz_weapon_commands: так его дешевле вынести
// в самостоятельный модуль, когда команды понадобятся не только в KZ.
//
// Три вещи, которые делает сервис и которые нельзя разнести по разным местам:
//   1. выдаёт оружие (GiveNamedItem);
//   2. гасит атаку, пока в руках граната — гранату можно держать, но нельзя кинуть,
//      чтобы буст и дым не ломали игру остальным (решение владельца проекта);
//   3. переживает RemoveAllItems из KZPistolService::UpdatePistol — иначе выданное
//      слетало бы на каждой смене команды и на !hideweapon.

struct WeaponInfo_t
{
	const char *cmd;       // имя команды без префикса kz_ (в чате: !<cmd>)
	const char *className; // classname сущности для GiveNamedItem
	bool grenade;          // гранаты держим, но не даём бросить
};

class KZWeaponService : public KZBaseService
{
	using KZBaseService::KZBaseService;

public:
	static bool Enabled();
	// nullptr, если такой команды нет.
	static const WeaponInfo_t *FindByCommand(const char *cmd);
	static bool IsGrenadeClassName(const char *className);

	virtual void Reset() override
	{
		this->givenWeapons.RemoveAll();
	}

	// Выдать оружие игроку. false — отказ (мёртв, не в игре, сервис выключен);
	// причину игроку печатает вызывающая команда.
	bool GiveWeapon(const WeaponInfo_t &info);

	// Синхронизировать список с тем, что реально в руках. Зовётся ДО RemoveAllItems:
	// так выброшенное на G оружие выпадает из списка и не возвращается обратно.
	void SyncFromHeld();

	// Перевыдать всё выданное. Зовётся ПОСЛЕ RemoveAllItems из UpdatePistol.
	void RegiveGiven();

	// Гасим IN_ATTACK/IN_ATTACK2, пока активное оружие — граната.
	void OnProcessUsercmds(PlayerCommand *cmds, int numcmds);

private:
	bool HoldingGrenade();

	CUtlVector<CUtlString> givenWeapons {};
};
