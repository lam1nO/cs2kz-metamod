#pragma once
#include "../kz.h"
#include "utils/utils.h"
#include "KeyValues.h"
#include "interfaces/interfaces.h"
#include "filesystem.h"
#include "keyvalues3.h"
#include "utils/eventlisteners.h"

class KZOptionServiceEventListener
{
public:
	virtual void OnPlayerPreferencesLoaded(KZPlayer *player) {};
	virtual void OnPlayerPreferenceChanged(KZPlayer *player, const char *optionName) {};
};

class KZOptionService : public KZBaseService
{
	using KZBaseService::KZBaseService;

	DECLARE_CLASS_EVENT_LISTENER(KZOptionServiceEventListener);

public:
	static void InitOptions();
	static void Cleanup();
	static const char *GetOptionStr(const char *optionName, const char *defaultValue = "");
	static f64 GetOptionFloat(const char *optionName, f64 defaultValue = 0.0);
	static i64 GetOptionInt(const char *optionName, i64 defaultValue = 0);
	static KeyValues *GetOptionKV(const char *optionName);

private:
	static void LoadDefaultOptions();

private:
	enum
	{
		NONE = 0,
		LOCAL,
		GLOBAL
	} dataState, currentState;

	KeyValues3 prefKV = KeyValues3(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
	CUtlVector<CUtlString> userSetPrefs; // Track user-modified preferences
	// Пакетная запись: пока > 0, SetPreference* не флашит в БД (флаш делает BatchScope на
	// выходе). Нужно сбросу страницы меню — иначе один клик по «Сбросить страницу» с 18
	// пунктами даёт 18 полных сериализаций prefKV и 18 записей в БД.
	i32 saveBatchDepth = 0;

public:
	void Reset()
	{
		dataState = NONE;
		currentState = NONE;
		prefKV.SetToEmptyTable();
		userSetPrefs.Purge();
	}

	void InitializeLocalPrefs(CUtlString text);
	void InitializeGlobalPrefs(std::string json);

	// Локальные префы загружены из БД (хотя бы раз, InitializeLocalPrefs прошла успешно).
	// Fail-closed гейт для путей записи/чтения, которым нужны реальные данные, а не
	// пустая таблица по умолчанию (см. SaveLocalPrefs, GetHudType, !ssp/!csp).
	bool IsLoaded()
	{
		return this->dataState >= LOCAL;
	}

	void SaveLocalPrefs();

	// RAII-обёртка пакетной записи: копит правки без флаша и пишет один раз на выходе.
	struct BatchScope
	{
		KZOptionService *svc;

		BatchScope(KZOptionService *service) : svc(service)
		{
			this->svc->saveBatchDepth++;
		}

		~BatchScope()
		{
			if (--this->svc->saveBatchDepth == 0)
			{
				this->svc->SaveLocalPrefs();
			}
		}
	};

	void SaveGlobalPrefs() {}

	void OnPlayerActive();

	void OnClientDisconnect()
	{
		SaveLocalPrefs();
		SaveGlobalPrefs();
	}

	void GetPreferencesAsJSON(CUtlString *error, CUtlString *output)
	{
		SaveKV3AsJSON(&this->prefKV, error, output);
	}

	// True once the player has a value for this preference, as opposed to falling back to a
	// default. Порт с апстрима (origin/master:src/kz/option/kz_option.h:104-107): нужен
	// обобщённому чтению по реестру (KZ::prefs::ReadValue) — без него «нет ключа» и
	// «сохранён ноль» неразличимы, и снимок худа увозил бы нули вместо дефолтов.
	bool HasPreference(const char *optionName)
	{
		return prefKV.FindMember(optionName) != NULL;
	}

	// Due to the way keyvalues3.h is written, we can't template these functions.
	void SetPreferenceBool(const char *optionName, bool value)
	{
		if (userSetPrefs.Find(optionName) == userSetPrefs.InvalidIndex())
		{
			userSetPrefs.AddToTail(optionName); // Mark as user-set
		}
		prefKV.FindOrCreateMember(optionName)->SetBool(value);
		CALL_FORWARD(eventListeners, OnPlayerPreferenceChanged, this->player, optionName);
		// Флаш в БД сразу: сохранение только на дисконнекте теряло настройки при
		// рестарте/краше сервера (жалоба «paint/таймер слетают после реконнекта»).
		SaveLocalPrefs();
	}

	bool GetPreferenceBool(const char *optionName, bool defaultValue = false)
	{
		KeyValues3 *option = prefKV.FindMember(optionName);
		if (!option)
		{
			return defaultValue;
		}
		return option->GetBool(defaultValue);
	}

	void SetPreferenceFloat(const char *optionName, f64 value)
	{
		if (userSetPrefs.Find(optionName) == userSetPrefs.InvalidIndex())
		{
			userSetPrefs.AddToTail(optionName); // Mark as user-set
		}
		prefKV.FindOrCreateMember(optionName)->SetDouble(value);
		CALL_FORWARD(eventListeners, OnPlayerPreferenceChanged, this->player, optionName);
		// Флаш в БД сразу: сохранение только на дисконнекте теряло настройки при
		// рестарте/краше сервера (жалоба «paint/таймер слетают после реконнекта»).
		SaveLocalPrefs();
	}

	f64 GetPreferenceFloat(const char *optionName, f64 defaultValue = 0.0)
	{
		KeyValues3 *option = prefKV.FindMember(optionName);
		if (!option)
		{
			return defaultValue;
		}
		return option->GetDouble(defaultValue);
	}

	void SetPreferenceInt(const char *optionName, i64 value)
	{
		if (userSetPrefs.Find(optionName) == userSetPrefs.InvalidIndex())
		{
			userSetPrefs.AddToTail(optionName); // Mark as user-set
		}
		prefKV.FindOrCreateMember(optionName)->SetInt64(value);
		CALL_FORWARD(eventListeners, OnPlayerPreferenceChanged, this->player, optionName);
		// Флаш в БД сразу: сохранение только на дисконнекте теряло настройки при
		// рестарте/краше сервера (жалоба «paint/таймер слетают после реконнекта»).
		SaveLocalPrefs();
	}

	i64 GetPreferenceInt(const char *optionName, i64 defaultValue = 0)
	{
		KeyValues3 *option = prefKV.FindMember(optionName);
		if (!option)
		{
			return defaultValue;
		}
		return option->GetInt64(defaultValue);
	}

	void SetPreferenceStr(const char *optionName, const char *value)
	{
		if (userSetPrefs.Find(optionName) == userSetPrefs.InvalidIndex())
		{
			userSetPrefs.AddToTail(optionName); // Mark as user-set
		}
		prefKV.FindOrCreateMember(optionName)->SetString(value);
		CALL_FORWARD(eventListeners, OnPlayerPreferenceChanged, this->player, optionName);
		// Флаш в БД сразу: сохранение только на дисконнекте теряло настройки при
		// рестарте/краше сервера (жалоба «paint/таймер слетают после реконнекта»).
		SaveLocalPrefs();
	}

	const char *GetPreferenceStr(const char *optionName, const char *defaultValue = "")
	{
		KeyValues3 *option = prefKV.FindMember(optionName);
		if (!option)
		{
			return defaultValue;
		}
		// DebugPrintKV3(&prefKV);
		return option->GetString(defaultValue);
	}

	void SetPreferenceVector(const char *optionName, const Vector &value)
	{
		if (userSetPrefs.Find(optionName) == userSetPrefs.InvalidIndex())
		{
			userSetPrefs.AddToTail(optionName); // Mark as user-set
		}
		prefKV.FindOrCreateMember(optionName)->SetVector(value);
		CALL_FORWARD(eventListeners, OnPlayerPreferenceChanged, this->player, optionName);
		// Флаш в БД сразу: сохранение только на дисконнекте теряло настройки при
		// рестарте/краше сервера (жалоба «paint/таймер слетают после реконнекта»).
		SaveLocalPrefs();
	}

	Vector GetPreferenceVector(const char *optionName, const Vector &defaultValue = Vector(0.0f, 0.0f, 0.0f))
	{
		KeyValues3 *option = prefKV.FindMember(optionName);
		if (!option)
		{
			return defaultValue;
		}
		return option->GetVector(defaultValue);
	}

	void SetPreferenceTable(const char *optionName, const KeyValues3 &value)
	{
		if (userSetPrefs.Find(optionName) == userSetPrefs.InvalidIndex())
		{
			userSetPrefs.AddToTail(optionName); // Mark as user-set
		}
		KeyValues3 *option = prefKV.FindOrCreateMember(optionName);
		option->SetToEmptyTable();
		*option = value;
		CALL_FORWARD(eventListeners, OnPlayerPreferenceChanged, this->player, optionName);
		// Флаш в БД сразу: сохранение только на дисконнекте теряло настройки при
		// рестарте/краше сервера (жалоба «paint/таймер слетают после реконнекта»).
		SaveLocalPrefs();
	}

	void GetPreferenceTable(const char *optionName, KeyValues3 &output, const KeyValues3 &defaultValue = KeyValues3())
	{
		KeyValues3 *option = prefKV.FindMember(optionName);
		if (!option)
		{
			output = defaultValue;
			return;
		}
		output = *option;
	}
};

namespace KZ::option
{
	// Регистрация слушателя таймера: старт забега закрывает открытое cs2menus-меню
	// (иначе NavSelect=E дёргал пункты оставленного меню посреди рана).
	void InitOptionsMenu();
} // namespace KZ::option

// Регистрация остальных категорий реестра настроек (kz/option/menu/model.h) — каждая зовётся
// РОВНО ОДИН РАЗ из cs2kz.cpp::Load, рядом с KZHUDService::Init() (Task 15). Объявлены здесь
// (не в namespace, как и определены в *_prefs.cpp) — единственному вызывающему не нужно тянуть
// отдельные заголовки misc/jumpstats ради одной функции каждый.
void KZMiscMenu_Register();
void KZJumpstatsMenu_Register();
void KZLocalOptionsMenu_Register();
// Оформление самого меню настроек (menuFont/menuColor/menuSounds/menuPopupShift) —
// определена в hud/layout/menu.cpp, рядом со своим читателем (RenderMenu).
void KZMenuChromeMenu_Register();
