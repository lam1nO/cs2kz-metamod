#pragma once
#include "../kz.h"

struct PistolInfo_t
{
	i16 itemDef;
	i8 team;
	const char *name;
	const char *className;
	std::vector<const char *> aliases;
};

class KZPistolService : public KZBaseService
{
public:
	// clang-format off
	static inline const std::vector<PistolInfo_t> pistols = {
		{0, CS_TEAM_NONE, "Knife", "weapon_knife", {"knife", "disable", "disabled", "none", "off", "0"}},
		{1, CS_TEAM_NONE, "Desert Eagle", "weapon_deagle", {"deagle", "deag", "desert_eagle", "deserteagle", "desert-eagle", "de"}},
		{2, CS_TEAM_NONE, "Dual Berettas", "weapon_elite", {"elite", "dualies", "dual_elites", "dual_berettas", "dual-elites", "dual-berettas", "dualelites", "dualberettas", "berettas", "dual berettas"}},
		{3, CS_TEAM_CT, "Five-SeveN", "weapon_fiveseven", {"fiveseven", "five_seven", "five7", "five_7", "five-7", "5seven", "5_seven", "5-seven", "57", "5-7", "5_7"}},
		{4, CS_TEAM_T, "Glock-18", "weapon_glock", {"glock", "glock18", "glock_18", "glock-18"}},
		{30, CS_TEAM_T, "Tec-9", "weapon_tec9", {"tec9", "tec-9", "tec_9"}},
		{32, CS_TEAM_CT, "P2000", "weapon_hkp2000", {"hkp2000", "p2k", "p2000"}},
		{36, CS_TEAM_NONE, "P250", "weapon_p250", {"p250"}},
		{61, CS_TEAM_CT, "USP-S", "weapon_usp_silencer", {"usps", "usp-s", "usp"}},
		{63, CS_TEAM_NONE, "CZ75-Auto", "weapon_cz75a", {"cz75-auto", "cz75a", "cz75", "cz-75", "cz_75", "cz", "cz75_auto", "cz-75_auto", "cz_75_auto"}},
		{64, CS_TEAM_NONE, "R8 Revolver", "weapon_revolver", {"r8 revolver", "r8revolver", "revolver", "r8", "r8_revolver"}}};
	// clang-format on

	// Индекс НЕИЗВЕСТНОГО/пустого имени. Раньше здесь возвращался 0 — а нулевая строка
	// таблицы это "Knife", то есть «пистолет выключен». Из-за этого любая мусорная строка в
	// настройке preferredPistol (пустое значение из БД префов, старый формат, опечатка в
	// !pistol) МОЛЧА оставляла игрока без пистолета навсегда, и единственная проверка на
	// такой случай — `pistolIndex == -1` в SCMD(kz_pistol) — не срабатывала никогда.
	// Отдельное значение «не выбран» нужно ещё и дефолту по команде (см. ResolvePreference).
	static constexpr i16 PISTOL_UNKNOWN = -1;

	static int GetPistolIndexByName(const char *name)
	{
		if (!name || !name[0])
		{
			return PISTOL_UNKNOWN;
		}
		for (i16 i = 0; i < pistols.size(); i++)
		{
			if (KZ_STREQI(pistols[i].name, name) || KZ_STREQI(pistols[i].className, name))
			{
				return i;
			}
			for (const char *alias : pistols[i].aliases)
			{
				if (KZ_STREQI(alias, name))
				{
					return i;
				}
			}
		}
		return PISTOL_UNKNOWN;
	}

	// Дефолт по команде: USP-S за CT, Glock-18 за T (запрос владельца 10.09 — «usp за кт и
	// glock за т должны быть дефолтными»). Раньше дефолт был один на всех (индекс 8, USP-S),
	// и за T он выдавался через подмену m_iTeamNum пешки вокруг GiveNamedItem (UpdatePistol).
	// С правильным дефолтом эта живая подмена команды на обычном игроке не нужна вовсе —
	// она остаётся только для того, кто ЯВНО выбрал ствол чужой команды.
	// Ищем по className, а не по индексу: индексы таблицы сдвинутся при первой же правке.
	static i16 GetDefaultPistolIndexForTeam(i32 team)
	{
		return (i16)GetPistolIndexByName(team == CS_TEAM_T ? "weapon_glock" : "weapon_usp_silencer");
	}

	// Строка настройки → индекс. Неизвестная/пустая — НЕ «нож», а дефолт команды.
	static i16 ResolvePreference(const char *pref, i32 team)
	{
		const i16 index = (i16)GetPistolIndexByName(pref);
		return index == PISTOL_UNKNOWN ? GetDefaultPistolIndexForTeam(team) : index;
	}

	static int GetPistolIndexByItemDef(i16 itemDef)
	{
		for (i16 i = 0; i < pistols.size(); i++)
		{
			if (pistols[i].itemDef == itemDef)
			{
				return i;
			}
		}
		return 0;
	}

	using KZBaseService::KZBaseService;

	static void Init();

	virtual void Reset() override
	{
		this->preferredPistol = PISTOL_UNKNOWN; // «не выбран» — берём дефолт команды
	}

	// Команда, по которой считается дефолт. Контроллер, а не пешка: пешку UpdatePistol сам
	// временно переставляет в чужую команду ради GiveNamedItem.
	i32 GetTeam();

	// Что выдавать прямо сейчас: явный выбор игрока или дефолт его команды.
	i16 ResolvePreferred()
	{
		if (this->preferredPistol >= 0 && this->preferredPistol < (i16)pistols.size())
		{
			return this->preferredPistol;
		}
		return GetDefaultPistolIndexForTeam(this->GetTeam());
	}

	void OnPlayerJoinTeam()
	{
		this->UpdatePistol(true);
	}

	void UpdatePistol(bool force = false);
	// Return true if the player has a weapon that isn't a knife.
	bool NeedWeaponStripping();
	// PISTOL_UNKNOWN = «игрок не выбирал» → дефолт команды (USP-S за CT, Glock-18 за T).
	i16 preferredPistol = PISTOL_UNKNOWN;
};
