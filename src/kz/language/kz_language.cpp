#include "kz_language.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "KeyValues.h"
#include "interfaces/interfaces.h"
#include "filesystem.h"
#include "utils/ctimer.h"
#include "kz/option/kz_option.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "kz/timer/kz_timer.h"

#include <map>

#include <vendor/ClientCvarValue/public/iclientcvarvalue.h>
#include <vendor/MultiAddonManager/public/imultiaddonmanager.h>

extern IMultiAddonManager *g_pMultiAddonManager;

static_global class KZOptionServiceEventListener_Language : public KZOptionServiceEventListener
{
	virtual void OnPlayerPreferencesLoaded(KZPlayer *player)
	{
		player->languageService->OnPlayerPreferencesLoaded();
	}
} optionEventListener;

extern IClientCvarValue *g_pClientCvarValue;

static_global KeyValues *translationKV;
static_global KeyValues *languagesKV;
static_global KeyValues *addonsKV;

// Плоский индекс фраз: имя секции -> её KeyValues. Нужен потому, что KeyValues::FindKey
// трактует '/' в имени как разделитель ПУТИ до вложенной секции, а парсер (LoadFromFile)
// кладёт ключ буквально, со слэшем внутри. Асимметрия делает НЕДОСТИЖИМОЙ любую фразу со
// слэшем в имени: FindKey("HUD - Bottom CP/TP Text") ищет секцию "HUD - Bottom CP" и в ней
// "TP Text", не находит, и GetTranslatedFormat возвращает само имя ключа — игрок видит в
// худе строку «HUD - Bottom CP/TP Text» вместо «CP 4/4 | TP 12» (баг найден 05.08). Тем же
// были сломаны и апстримные "MHUD - CP/TP Enabled/Disabled", "MHUD - PB/WR Enabled/Disabled",
// "WR/SR Command Usage[ - Console]" — просто их видно реже. Индекс обходит подсекции
// напрямую, поэтому имя сравнивается целиком и слэш в нём ничего не значит.
//
// Сравнение имён РЕГИСТРОНЕЗАВИСИМОЕ — ровно как у FindKey (тот интернирует имя через
// символ-таблицу KeyValues, а она регистр игнорирует). Байтовое сравнение сразу ломает
// склейки ключей в рантайме: kz_mode_manager собирает "Command Description - kz_%s" из
// MODE_NAME_SHORT (заглавные VNL/CKZ/KZT), а в файле лежит "Command Description - kz_vnl" —
// в !help игрок видел сырой ключ. Регистр — свойство ИНДЕКСА, а не отдельной склейки.
struct PhraseNameLess
{
	using is_transparent = void;

	static const char *Cstr(const char *name)
	{
		return name;
	}

	static const char *Cstr(const std::string &name)
	{
		return name.c_str();
	}

	template<typename L, typename R>
	bool operator()(const L &lhs, const R &rhs) const
	{
		return V_stricmp(Cstr(lhs), Cstr(rhs)) < 0;
	}
};

// std::map, а не unordered_map: гетерогенный поиск (is_transparent) по const char* в C++17
// есть только у упорядоченных контейнеров, иначе каждый lookup строил бы временную
// std::string на ключ. Сам поиск РЕДКИЙ — PrepareMessageWithLang заходит сюда только на
// промахе formattedTemplateCache, т.е. один раз на пару (язык, фраза), — но лишняя
// аллокация на пути построения текста всё равно не нужна.
static_global std::map<std::string, KeyValues *, PhraseNameLess> phraseIndex;

// Перестроить индекс по текущему translationKV. Зовётся после загрузки всех *.phrases.txt.
// При дубликате имени (одна фраза в двух файлах) побеждает ПЕРВОЕ вхождение — как у
// FindKey, который возвращает первую подходящую подсекцию.
static_function void RebuildPhraseIndex()
{
	phraseIndex.clear();
	if (!translationKV)
	{
		return;
	}
	for (KeyValues *kv = translationKV->GetFirstSubKey(); kv; kv = kv->GetNextKey())
	{
		const char *name = kv->GetName();
		if (!name || !name[0])
		{
			continue;
		}
		phraseIndex.emplace(name, kv);
	}
}

static_function KeyValues *FindPhrase(const char *phrase)
{
	auto it = phraseIndex.find(phrase);
	return it == phraseIndex.end() ? nullptr : it->second;
}

void KZLanguageService::Init()
{
	KZLanguageService::LoadConfigFiles();
	KZOptionService::RegisterEventListener(&optionEventListener);
}

void KZLanguageService::LoadConfigFiles()
{
	// Первой строкой, ДО delete: индекс держит указатели внутрь translationKV, а перезагрузка
	// (kz_reload_translations — публичная rcon-команда) удаляет его и потом читает ~27 файлов.
	// Всё это время висячие указатели были бы достижимы из GetTranslatedFormat.
	phraseIndex.clear();
	if (translationKV)
	{
		delete translationKV;
	}
	if (languagesKV)
	{
		delete languagesKV;
	}
	if (addonsKV)
	{
		delete addonsKV;
	}
	translationKV = new KeyValues("Phrases");
	translationKV->UsesEscapeSequences(true);
	languagesKV = new KeyValues("Languages");
	languagesKV->UsesEscapeSequences(true);
	addonsKV = new KeyValues("Addons");
	addonsKV->UsesEscapeSequences(true);
	KZLanguageService::LoadTranslations();
	// Строго ПОСЛЕ загрузки всех *.phrases.txt: индекс держит указатели на подсекции
	// translationKV, и любая перезагрузка (пересоздание KV выше) их инвалидирует.
	RebuildPhraseIndex();
	KZLanguageService::LoadLanguages();
	KZLanguageService::ClearTemplateCache();
}

void KZLanguageService::Cleanup()
{
	// Индекс держит указатели внутрь translationKV — снимаем ДО его удаления.
	phraseIndex.clear();
	if (translationKV)
	{
		delete translationKV;
	}
	if (languagesKV)
	{
		delete languagesKV;
		if (addonsKV)
		{
			delete addonsKV;
		}
	}
}

const char *KZLanguageService::GetBaseAddon()
{
	return addonsKV->GetString(KZ_BASE_ADDON_KEY, KZ_WORKSHOP_ADDON_ID);
}

void KZLanguageService::LoadLanguages()
{
	char fullPath[1024];
	g_SMAPI->PathFormat(fullPath, sizeof(fullPath), "%s/addons/cs2kz/translations/config.txt", g_SMAPI->GetBaseDir());
	if (!languagesKV->LoadFromFile(g_pFullFileSystem, fullPath, nullptr))
	{
		KZ_LOG_WARN(LogChannel::Language, "Failed to load translation config file.\n");
	}
	g_SMAPI->PathFormat(fullPath, sizeof(fullPath), "%s/addons/cs2kz/translations/menu-addons.txt", g_SMAPI->GetBaseDir());
	if (!addonsKV->LoadFromFile(g_pFullFileSystem, fullPath, nullptr))
	{
		KZ_LOG_WARN(LogChannel::Language, "Failed to load addon config file.\n");
	}
}

void KZLanguageService::LoadTranslations()
{
	char buffer[1024];
	g_SMAPI->PathFormat(buffer, sizeof(buffer), "addons/cs2kz/translations/*.phrases.txt");
	FileFindHandle_t findHandle = {};
	const char *fileName = g_pFullFileSystem->FindFirst(buffer, &findHandle);
	if (fileName)
	{
		do
		{
			char fullPath[1024];
			g_SMAPI->PathFormat(fullPath, sizeof(fullPath), "%s/addons/cs2kz/translations/%s", g_SMAPI->GetBaseDir(), fileName);
			if (!translationKV->LoadFromFile(g_pFullFileSystem, fullPath, nullptr))
			{
				KZ_LOG_WARN(LogChannel::Language, "Failed to load %s\n", fileName);
			}
			fileName = g_pFullFileSystem->FindNext(findHandle);
		} while (fileName);
		g_pFullFileSystem->FindClose(findHandle);
	}
}

void KZLanguageService::OnPlayerPreferencesLoaded()
{
	const char *language = this->player->optionService->GetPreferenceStr("preferredLanguage");
	bool shouldReconnect = !(this->player->checkpointService->GetCheckpointCount() || this->player->timerService->GetTimerRunning());
	if (language[0])
	{
		KZLanguageService::UpdateLanguage(this->player->GetSteamId64(false), language, LanguageInfo::CacheLevel::CACHE_PREF, shouldReconnect);
		if (!shouldReconnect)
		{
			this->player->languageService->PrintChat(false, false, "Language Change - Manual Menu Change Required");
		}
	}
}

const char *KZLanguageService::GetLanguage()
{
	return KZLanguageService::clientLanguageInfos[this->player->GetSteamId64(false)].language;
}

const char *KZLanguageService::GetTranslatedFormat(const char *language, const char *phrase)
{
	// Через индекс, а не FindKey: тот режет имя по '/' (см. phraseIndex выше).
	KeyValues *kv = FindPhrase(phrase);
	if (!kv)
	{
		// KZ_LOG_DEBUG(LogChannel::Language, "Warning: Phrase '%s' not found, returning orignal message!\n", phrase);
		return phrase;
	}
	const char *outFormat = kv->GetString(language);
	if (outFormat[0] == '\0')
	{
		if (!V_stricmp(language, "#format"))
		{
			// It is fine to have no format.
			return NULL;
		}
		// KZ_LOG_DEBUG(LogChannel::Language, "Warning: Phrase '%s' not found for language %s!\n", phrase, language);
		return kv->GetString(KZ_DEFAULT_LANGUAGE);
	}
	return outFormat;
}

void KZLanguageService::UpdateLanguage(u64 xuid, const char *langKey, LanguageInfo::CacheLevel cacheLevel, bool shouldReconnect)
{
	// Manual override > Loaded preference > Queried ConVar
	auto &langInfo = KZLanguageService::clientLanguageInfos[xuid];
	const char *addon = addonsKV->GetString(langKey, addonsKV->GetString(KZOptionService::GetOptionStr("defaultLanguage", KZ_DEFAULT_LANGUAGE)));
	if (langInfo.cacheLevel > cacheLevel)
	{
		return;
	}
	langInfo.cacheLevel = cacheLevel;
	if (!KZ_STREQILEN(langInfo.lastAddon, addon, sizeof(langInfo.lastAddon)))
	{
		if (KZ_STREQI(langInfo.lastAddon, KZLanguageService::GetBaseAddon()))
		{
			KZ_LOG_INFO(LogChannel::Language, "Adding %s for client %llu\n", addon, xuid);
		}
		else
		{
			KZ_LOG_INFO(LogChannel::Language, "Adding %s and removing %s for client %llu\n", addon, langInfo.lastAddon, xuid);
		}
		if (g_pMultiAddonManager)
		{
			g_pMultiAddonManager->RemoveClientAddon(langInfo.lastAddon, xuid);
			g_pMultiAddonManager->AddClientAddon(addon, xuid, true);
		}
		V_strncpy(langInfo.lastAddon, addon, sizeof(langInfo.lastAddon));
	}
	V_strncpy(langInfo.language, langKey, sizeof(langInfo.language));
}

void KZLanguageService::OnPlayerConnect(u64 steamID64)
{
	if (!steamID64 || KZLanguageService::clientLanguageInfos[steamID64].cacheLevel > LanguageInfo::CacheLevel::CACHE_NONE)
	{
		return;
	}
	this->UpdateLanguage(steamID64, KZOptionService::GetOptionStr("defaultLanguage", KZ_DEFAULT_LANGUAGE), LanguageInfo::CacheLevel::CACHE_NONE,
						 false);
	if (g_pClientCvarValue)
	{
		// clang-format off
		g_pClientCvarValue->QueryCvarValue(this->player->GetPlayerSlot(), "cl_language",
			[steamID64](CPlayerSlot nSlot, ECvarValueStatus eStatus, const char *pszCvarName, const char *pszCvarValue)
			{
				if (eStatus == ECvarValueStatus::ValueIntact)
				{
					const char* langKey = languagesKV->GetString(pszCvarValue, pszCvarValue);
					KZ_LOG_INFO(LogChannel::Language, "Received client convar value: %s\n", langKey);
					KZLanguageService::UpdateLanguage(steamID64, langKey, LanguageInfo::CacheLevel::CACHE_CVAR, true);
				}
		});
		// clang-format on
	}
}

KZLanguageService::LanguageInfo::LanguageInfo()
{
	V_strncpy(this->language, KZOptionService::GetOptionStr("defaultLanguage", KZ_DEFAULT_LANGUAGE), sizeof(this->language));
}

SCMD(kz_language, SCFL_PREFERENCE | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	char language[32] {};
	V_snprintf(language, sizeof(language), "%s", args->Arg(1));
	V_strlower(language);
	bool shouldReconnect = !(player->checkpointService->GetCheckpointCount() || player->timerService->GetTimerRunning());
	KZLanguageService::UpdateLanguage(player->GetSteamId64(false), language, KZLanguageService::LanguageInfo::CacheLevel::CACHE_OVERRIDE, true);
	player->optionService->SetPreferenceStr("preferredLanguage", language);
	if (!shouldReconnect)
	{
		player->languageService->PrintChat(true, false, "Switch Language", language);
		player->languageService->PrintChat(false, false, "Language Change - Manual Menu Change Required");
	}
	return MRES_SUPERCEDE;
}

CON_COMMAND_F(kz_reload_translations, "Reload translation configuration files", FCVAR_NONE)
{
	KZLanguageService::LoadConfigFiles();
}
