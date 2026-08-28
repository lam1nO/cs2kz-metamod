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

// Потолок одновременно отслеживаемых выданных стволов на игрока. Не «сколько влезет в
// слоты», а страховка от роста числа энтити в бесконечном KZ-раунде: выброшенное на G
// лежит до смены карты, а команда выдаёт снова.
#define KZ_MAX_GIVEN_WEAPONS 8

struct WeaponInfo_t
{
	const char *cmd;       // имя команды без префикса kz_ (в чате: !<cmd>)
	const char *className; // classname сущности для GiveNamedItem
	bool grenade;          // гранаты держим, но не даём бросить
};

// Результат выдачи. Не bool: причины отказа игроку показываются разными строками, а
// «сломался движок» обязан ещё и попасть в лог с reason.
enum class GiveResult
{
	Ok,
	NotAlive,   // мёртв, не в игре или сервис выключен
	LimitHit,   // упёрся в KZ_MAX_GIVEN_WEAPONS
	Internal,   // нет itemServices или GiveNamedItem вернул nullptr — should-never-happen
};

// Одна выданная запись. Хранятся ДВА имени, и это не избыточность: движок подменяет
// предмет по команде игрока (CT просит weapon_molotov — получает weapon_incgrenade, за T
// зеркально с !inc). Дедуп обязан сверяться с ЗАПРОШЕННЫМ именем, иначе повторная команда
// его не узнает; сверка «ещё в руках?» — с ФАКТИЧЕСКИМ, иначе запись вычищается каждый
// раз, дедуп промахивается, и потолок не достигается никогда.
struct GivenWeapon_t
{
	CUtlString requested;
	CUtlString actual;
};

class KZWeaponService : public KZBaseService
{
	using KZBaseService::KZBaseService;

public:
	static bool Enabled();
	// nullptr, если такой команды нет.
	static const WeaponInfo_t *FindByCommand(const char *cmd);
	static bool IsGrenadeClassName(const char *className);

	// На выделенном сервере KZPlayer::Reset() зовётся с ДИСКОННЕКТА, а не со смены карты
	// (player_manager.cpp; ветка hooks.cpp — listen-server). Смену карты список переживает,
	// и фактически чистится на первом же спавне через SyncFromHeld: в руках после спавна
	// выданного нет, значит из списка оно выпадает.
	virtual void Reset() override
	{
		this->givenWeapons.RemoveAll();
	}

	// Выдать оружие игроку. Отказы: мёртв/не в игре/сервис выключен, упёрся в потолок
	// KZ_MAX_GIVEN_WEAPONS, внутренняя ошибка движка. Причину печатает вызывающая команда.
	GiveResult GiveWeapon(const WeaponInfo_t &info);

	// Синхронизировать список с тем, что реально в руках. Зовётся ДО RemoveAllItems:
	// так выброшенное на G оружие выпадает из списка и не возвращается обратно.
	void SyncFromHeld();

	// Перевыдать всё выданное. Зовётся ПОСЛЕ RemoveAllItems из UpdatePistol.
	void RegiveGiven();

	// Гасим IN_ATTACK/IN_ATTACK2, пока активное оружие — граната. Гейт только по
	// активному оружию: подобранная с земли граната тоже не должна бросаться.
	void OnProcessUsercmds(PlayerCommand *cmds, int numcmds);

private:
	bool HoldingGrenade();

	CUtlVector<GivenWeapon_t> givenWeapons {};
};
