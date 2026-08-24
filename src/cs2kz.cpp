#include "cs2kz.h"

#include "entity2/entitysystem.h"
#include "steam/steam_gameserver.h"

#include "sdk/cgameresourceserviceserver.h"
#include "utils/utils.h"
#include "utils/hooks.h"
#include "utils/gameconfig.h"
#include "utils/async_file_io.h"

#include "movement/movement.h"
#include "kz/kz.h"
#include "kz/anticheat/kz_anticheat.h"
#include "kz/db/kz_db.h"
#include "kz/hud/kz_hud.h"
#include "kz/mode/kz_mode.h"
#include "kz/spec/kz_spec.h"
#include "kz/goto/kz_goto.h"
#include "kz/style/kz_style.h"
#include "kz/quiet/kz_quiet.h"
#include "kz/invisible/kz_invisible.h"
#include "kz/ztopwatch/kz_ztopwatch.h"
#include "kz/tip/kz_tip.h"
#include "kz/option/kz_option.h"
#include "kz/outbox/kz_outbox.h"
#include "kz/language/kz_language.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/global/kz_global.h"
#include "kz/beam/kz_beam.h"
#include "kz/pistol/kz_pistol.h"
#include "kz/prac/kz_prac.h"
#include "kz/recording/kz_recording.h"
#include "kz/replays/kz_replaysystem.h"
#include "kz/racing/kz_racing.h"
#include "kz/misc/kz_customchangemap.h"
#include "kz/zones/kz_zones.h"

#include <vendor/MultiAddonManager/public/imultiaddonmanager.h>
#include <vendor/ClientCvarValue/public/iclientcvarvalue.h>
#include <vendor/ixwebsocket/ixwebsocket/IXNetSystem.h>
#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include "tier0/memdbgon.h"
KZPlugin g_KZPlugin;

IMultiAddonManager *g_pMultiAddonManager;
IClientCvarValue *g_pClientCvarValue;
ICS2Menus *g_pMenus;
// Умеет ли рядом стоящий cs2menus строку показаний под меню (SetSlotStatus, интерфейс 005).
// Отдельный флаг, а не «g_pMenus != nullptr»: со СТАРОЙ сборкой cs2menus интерфейс 005 не
// находится, и запрашивать только его нельзя — g_pMenus стал бы null и форк остался бы вообще
// без меню (!options/!maps/!rpmenu) из-за одной строки худа. Поэтому откатываемся на 004 и
// просто не зовём новый метод: у полученного по 004 указателя его нет в vtable.
bool g_menusHasSlotStatus;
CSteamGameServerAPIContext g_steamAPI;

// Получить интерфейс меню: сначала актуальная ревизия, при неудаче — предыдущая.
static void AcquireMenusInterface()
{
	g_pMenus = (ICS2Menus *)g_SMAPI->MetaFactory(CS2MENUS_INTERFACE, nullptr, nullptr);
	g_menusHasSlotStatus = g_pMenus != nullptr;
	if (!g_pMenus)
	{
		g_pMenus = (ICS2Menus *)g_SMAPI->MetaFactory(CS2MENUS_INTERFACE_004, nullptr, nullptr);
	}
}

PLUGIN_EXPOSE(KZPlugin, g_KZPlugin);

bool KZPlugin::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
	setlocale(LC_ALL, "en_US.utf8");
	PLUGIN_SAVEVARS();
	modules::Initialize();
	if (!interfaces::Initialize(ismm, error, maxlen))
	{
		return false;
	}

	KZOptionService::InitOptions();
	InitKZLogging();
	kz_log_to_file.Set((bool)KZOptionService::GetOptionInt("logToFile", true));

	if (!utils::Initialize(ismm, error, maxlen))
	{
		return false;
	}

	ConVar_Register();
	hooks::Initialize();
	ix::initNetSystem();
	movement::InitDetours();
	KZCheckpointService::Init();
	KZPracService::Init();
	KZTimerService::Init();
	KZSpecService::Init();
	KZGotoService::Init();
	KZHUDService::Init();
	KZ::option::InitOptionsMenu();
	KZLanguageService::Init();
	KZBeamService::Init();
	KZPistolService::Init();
	KZ::misc::Init();
	KZ::misc::customchangemap::Init();
	KZQuietService::Init();
	KZInvisibleService::Init();
	KZZtopwatchService::Init();
	AsyncFileIO::Init();
	// Дисковый outbox завершённых ранов: каталоги + таймер ретраера — при загрузке,
	// чтобы после краша/рестарта дослать write-ahead файлы, оставшиеся с прошлой жизни.
	KZOutboxService::Init();
	KZRecordingService::Init();
	if (!KZ::mode::CheckModeCvars())
	{
		return false;
	}

	ismm->AddListener(this, this);
	KZ::mapapi::Init();
	KZ::mode::InitModeManager();
	KZ::style::InitStyleManager();

	KZ::mode::DisableReplicatedModeCvars();

	KZTipService::Init();
	KZAnticheatService::Init();
	if (late)
	{
		g_steamAPI.Init();
		g_pKZPlayerManager->OnLateLoad();
		// We need to reset the map for mapping api to properly load in.
		utils::ResetMap();
		KZ::replaysystem::Init();
	}

	// We don't need command filtering for KZ maps.
	CommandLine()->AppendParm("-disable_workshop_command_filtering", "");
	KZ_LOG_DEBUG(LogChannel::General, "Plugin loaded successfully. (late load: %s)", late ? "true" : "false");
	KZ::replaysystem::InitWatcher();
	return true;
}

bool KZPlugin::Unload(char *error, size_t maxlen)
{
	this->unloading = true;
	// Меню редактора зон — до всего остального: их колбэки держат указатели в наш DLL,
	// и живой хэндл в cs2menus после выгрузки — вызов в выгруженный код.
	KZ::zones::DestroyEditorMenus();
	KZ::misc::UnrestrictTimeLimit();
	KZRecordingService::Shutdown();
	AsyncFileIO::Cleanup();
	KZ::misc::customchangemap::Cleanup();
	KZRacingService::Cleanup();
	ix::uninitNetSystem();
	hooks::Cleanup();
	KZ::mode::EnableReplicatedModeCvars();
	utils::Cleanup();
	g_pKZModeManager->Cleanup();
	g_pKZStyleManager->Cleanup();
	g_pPlayerManager->Cleanup();
	KZDatabaseService::Cleanup();
	KZGlobalService::Cleanup();
	KZLanguageService::Cleanup();
	KZOptionService::Cleanup();
	KZ::replaysystem::Cleanup();
	KZAnticheatService::CleanupSvCheatsWatcher();
	ConVar_Unregister();
	LoggingSystem_UnregisterLoggingListener(&g_KZLoggingListener);
	kz_log_to_file.Set(false);
	g_KZLoggingListener.CheckFile();
	return true;
}

void KZPlugin::AllPluginsLoaded()
{
	KZDatabaseService::Init();
	KZ::mode::LoadModePlugins();
	KZ::style::LoadStylePlugins();
	g_pKZPlayerManager->ResetPlayers();
	// ResetPlayers обнулил флаги невидимок, выставленные нашим Init() → повторное
	// применение списка (late load на живом сервере не должен молча снимать невидимость).
	KZInvisibleService::OnAllPluginsLoaded();
	this->UpdateSelfMD5();
	g_pMultiAddonManager = (IMultiAddonManager *)g_SMAPI->MetaFactory(MULTIADDONMANAGER_INTERFACE, nullptr, nullptr);
	g_pClientCvarValue = (IClientCvarValue *)g_SMAPI->MetaFactory(CLIENTCVARVALUE_INTERFACE, nullptr, nullptr);
	AcquireMenusInterface();
}

void KZPlugin::OnPluginLoad(PluginId id)
{
	g_pMultiAddonManager = (IMultiAddonManager *)g_SMAPI->MetaFactory(MULTIADDONMANAGER_INTERFACE, nullptr, nullptr);
	g_pClientCvarValue = (IClientCvarValue *)g_SMAPI->MetaFactory(CLIENTCVARVALUE_INTERFACE, nullptr, nullptr);
	AcquireMenusInterface();
}

void KZPlugin::OnPluginUnload(PluginId id)
{
	g_pMultiAddonManager = (IMultiAddonManager *)g_SMAPI->MetaFactory(MULTIADDONMANAGER_INTERFACE, nullptr, nullptr);
	g_pClientCvarValue = (IClientCvarValue *)g_SMAPI->MetaFactory(CLIENTCVARVALUE_INTERFACE, nullptr, nullptr);
	AcquireMenusInterface();
}

void KZPlugin::AddonInit()
{
	static_persist bool addonLoaded;
	if (g_pMultiAddonManager != nullptr && !addonLoaded)
	{
		addonLoaded = g_pMultiAddonManager->AddAddon(KZLanguageService::GetBaseAddon(), true);
		CConVarRef<bool> mm_cache_clients_with_addons("mm_cache_clients_with_addons");
		CConVarRef<float> mm_cache_clients_duration("mm_cache_clients_duration");
		mm_cache_clients_with_addons.Set(true);
		mm_cache_clients_duration.Set(30.0f);
	}
}

bool KZPlugin::IsAddonMounted()
{
	if (g_pMultiAddonManager != nullptr)
	{
		return g_pMultiAddonManager->IsAddonMounted(KZLanguageService::GetBaseAddon(), true);
	}
	return false;
}

void KZPlugin::EnsureClientAsset(u64 steamID64)
{
	// Гарантируем, что контент-ассет (particles/sounds/HUD) доедет клиенту даже на тяжёлых
	// картах. MAM v1.5 формирует клиенту очередь докачки в жёстком порядке [воркшоп-карта,
	// серверные mm_extra_addons (сюда входит ассет), ...] — ассет ВСЕГДА после карты, и
	// переставить его перед картой через API нельзя. На combobreaker (~65 МБ, холодный кэш)
	// клиент может не дойти до ассета: движок дропает его на воркшоп-попапе карты.
	// Метод вызывается из OnPlayerActive — то есть карта у клиента УЖЕ скачана (он active).
	// AddClientAddon(..., bRefresh=true) сам решает, нужен ли реконнект: он вычитает уже
	// скачанные клиентом аддоны (downloadedAddons, сохраняются т.к. mm_cache_clients_with_addons=true);
	// если ассет уже скачан — очередь пуста и реконнекта не будет (no-op). Если не скачан —
	// один изолированный реконнект догрузит ТОЛЬКО ассет (карта уже в downloadedAddons).
	// Remove перед Add сбрасывает per-client запись, чтобы фикс срабатывал и после смены карты
	// (иначе повторный AddClientAddon упёрся бы в дедуп и не переслал ассет).
	if (g_pMultiAddonManager == nullptr || !steamID64)
	{
		return;
	}
	const char *asset = KZLanguageService::GetBaseAddon();
	g_pMultiAddonManager->RemoveClientAddon(asset, steamID64);
	g_pMultiAddonManager->AddClientAddon(asset, steamID64, true);
}

bool KZPlugin::Pause(char *error, size_t maxlen)
{
	return true;
}

bool KZPlugin::Unpause(char *error, size_t maxlen)
{
	return true;
}

void *KZPlugin::OnMetamodQuery(const char *iface, int *ret)
{
	if (strcmp(iface, KZ_MODE_MANAGER_INTERFACE) == 0)
	{
		*ret = META_IFACE_OK;
		return g_pKZModeManager;
	}
	else if (strcmp(iface, KZ_STYLE_MANAGER_INTERFACE) == 0)
	{
		*ret = META_IFACE_OK;
		return g_pKZStyleManager;
	}
	else if (strcmp(iface, KZ_UTILS_INTERFACE) == 0)
	{
		*ret = META_IFACE_OK;
		return g_pKZUtils;
	}
	else if (strcmp(iface, KZ_MAPPING_INTERFACE) == 0)
	{
		*ret = META_IFACE_OK;
		return g_pMappingApi;
	}
	*ret = META_IFACE_FAILED;

	return NULL;
}

void KZPlugin::UpdateSelfMD5()
{
	ISmmPluginManager *pluginManager = (ISmmPluginManager *)g_SMAPI->MetaFactory(MMIFACE_PLMANAGER, nullptr, nullptr);
	const char *path;
	pluginManager->Query(g_PLID, &path, nullptr, nullptr);
	g_pKZUtils->GetFileMD5(path, this->md5, sizeof(this->md5));
}

CGameEntitySystem *GameEntitySystem()
{
	return interfaces::pGameResourceServiceServer->GetGameEntitySystem();
}
