
#include "utils/utils.h"
#include "utils/hooks.h"
#include "utils/addresses.h"
#include "utils/simplecmds.h"
#include "utils/gamesystem.h"
#include "utils/gameconfig.h"
#include "utils/ctimer.h"

#include "entityclass.h"
#include "gamesystems/spawngroup_manager.h"
#include "steam/steam_gameserver.h"
#include "bufferstring.h"
#include "igameeventsystem.h"
#include "igamesystem.h"
#include "entityclass.h"
#include "gamesystems/spawngroup_manager.h"
#include "utils/simplecmds.h"
#include "utils/gamesystem.h"
#include "utils/async_file_io.h"
#include "steam/steam_gameserver.h"

#include "sdk/steamnetworkingsockets.h"

#include "kz/kz.h"
#include "kz/beam/kz_beam.h"
#include "kz/hud/kz_hud.h"
#include "kz/invisible/kz_invisible.h"
#include "kz/jumpstats/kz_jumpstats.h"
#include "kz/lead/kz_lead.h"
#include "kz/replays/cyb_awr_backfill.h"
#include "kz/option/kz_option.h"
#include "kz/paint/kz_paint.h"
#include "kz/quiet/kz_quiet.h"
#include "kz/timer/kz_timer.h"
#include "kz/timer/submission.h"
#include "kz/timer/queries/base_request.h"
#include "kz/telemetry/kz_telemetry.h"
#include "kz/trigger/kz_trigger.h"
#include "kz/db/kz_db.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/zones/kz_zones.h"
#include "kz/global/kz_global.h"
#include "kz/hud/kz_hud.h"
#include "kz/profile/kz_profile.h"
#include "kz/pistol/kz_pistol.h"
#include "kz/recording/kz_recording.h"
#include "kz/replays/kz_replaysystem.h"
#include "kz/racing/kz_racing.h"
#include "kz/misc/kz_customchangemap.h"
#include "utils/utils.h"
#include "utils/cvarquery.h"
// Полное определение CServerSideClientBase: хук ниже берёт индекс втаблицы из указателя на
// член-функцию, для этого мало forward-декларации. Цепочка kz/kz.h → movement/movement.h →
// player/player.h его уже даёт, но опираться на чужой инклюд для собственного хука нельзя.
#include "sdk/serversideclient.h"
#include "sdk/entity/cbasetrigger.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "sdk/usercmd.h"
// Свой protobuf (Task 11 — проводка клика panorama-меню), НЕ cstrike15_usermessages.pb.h SDK:
// см. комментарий в protobuf/kz_customhud.proto — тот файл симлинк в сабмодуль на чужом пине,
// который CS_UM_CustomHudClicked ещё не знает.
#include "kz_customhud.pb.h"
// protobuf/kz_customhud.proto объявляет `package cs2kz.customhud` (по образцу kz_replay.proto,
// см. using ReplayHeader в kz_replay.h) — protoc генерирует cs2kz::customhud::…, а не голое
// имя. Остальные (симлинк-)proto форка без package этой развязки не требуют, отсюда и была
// иллюзия глобального имени; здесь идём тем же путём, что kz_replay, а не снимаем package.
using CKZUsrMsg_CustomHudClicked = cs2kz::customhud::CKZUsrMsg_CustomHudClicked;

#include "vprof.h"
#ifdef DEBUG_TPM
#include "fmtstr.h"
#endif

#include "memdbgon.h"

extern CSteamGameServerAPIContext g_steamAPI;
extern CGameConfig *g_pGameConfig;

class GameSessionConfiguration_t
{
};

class EntListener : public IEntityListener
{
	virtual void OnEntityCreated(CEntityInstance *pEntity);
	virtual void OnEntitySpawned(CEntityInstance *pEntity);
	virtual void OnEntityDeleted(CEntityInstance *pEntity);
} entityListener;

static_global bool ignoreTouchEvent {};

static MovementPlayerManager *playerManager;

#ifdef DEBUG_TPM
static bool g_traceShapeEnabled = false;
CUtlVector<TraceHistory> traceHistory;
#endif

// ============================================================
// Entity hooks
// ============================================================

static KHook::Return<void> StartTouchPre(CBaseEntity *pThis, CBaseEntity *pOther)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (KZTriggerService::IsManagedByTriggerService(pThis, pOther) && !g_KZPlugin.simulatingPhysics)
	{
		return {KHook::Action::Supersede};
	}
	return {KHook::Action::Ignore};
}

static KHook::Return<void> StartTouchPost(CBaseEntity *pThis, CBaseEntity *pOther)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (KZTriggerService::IsManagedByTriggerService(pThis, pOther) && !g_KZPlugin.simulatingPhysics)
	{
		return {KHook::Action::Supersede};
	}
	return {KHook::Action::Ignore};
}

static KHook::Virtual<CBaseEntity, void, CBaseEntity *> startTouchHook(StartTouchPre, StartTouchPost);

static KHook::Return<void> TouchPre(CBaseEntity *pThis, CBaseEntity *pOther)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	// If it's a player touching trigger_push, we still need to disable jumpstats.
	KZPlayer *player = nullptr;
	if (KZ_STREQI(pThis->GetClassname(), "trigger_push") && KZ_STREQI(pOther->GetClassname(), "player"))
	{
		player = g_pKZPlayerManager->ToPlayer(static_cast<CCSPlayerPawn *>(pOther));
	}
	else if (KZ_STREQI(pThis->GetClassname(), "player") && KZ_STREQI(pOther->GetClassname(), "trigger_push"))
	{
		player = g_pKZPlayerManager->ToPlayer(static_cast<CCSPlayerPawn *>(pThis));
	}
	if (player)
	{
		player->jumpstatsService->InvalidateJumpstats("Base velocity detected");
	}
	if (KZTriggerService::IsManagedByTriggerService(pThis, pOther) && !g_KZPlugin.simulatingPhysics)
	{
		return {KHook::Action::Supersede};
	}
	return {KHook::Action::Ignore};
}

static KHook::Return<void> TouchPost(CBaseEntity *pThis, CBaseEntity *pOther)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (KZTriggerService::IsManagedByTriggerService(pThis, pOther) && !g_KZPlugin.simulatingPhysics)
	{
		return {KHook::Action::Supersede};
	}
	return {KHook::Action::Ignore};
}

static KHook::Virtual<CBaseEntity, void, CBaseEntity *> touchHook(TouchPre, TouchPost);

static KHook::Return<void> EndTouchPre(CBaseEntity *pThis, CBaseEntity *pOther)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (KZTriggerService::IsManagedByTriggerService(pThis, pOther) && !g_KZPlugin.simulatingPhysics)
	{
		return {KHook::Action::Supersede};
	}
	return {KHook::Action::Ignore};
}

static KHook::Return<void> EndTouchPost(CBaseEntity *pThis, CBaseEntity *pOther)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (KZTriggerService::IsManagedByTriggerService(pThis, pOther) && !g_KZPlugin.simulatingPhysics)
	{
		return {KHook::Action::Supersede};
	}
	return {KHook::Action::Ignore};
}

static KHook::Virtual<CBaseEntity, void, CBaseEntity *> endTouchHook(EndTouchPre, EndTouchPost);

void hooks::CallOriginalStartTouch(CBaseEntity *pThis, CBaseEntity *pOther)
{
	startTouchHook.CallOriginal(pThis, pOther);
}

void hooks::CallOriginalTouch(CBaseEntity *pThis, CBaseEntity *pOther)
{
	touchHook.CallOriginal(pThis, pOther);
}

void hooks::CallOriginalEndTouch(CBaseEntity *pThis, CBaseEntity *pOther)
{
	endTouchHook.CallOriginal(pThis, pOther);
}

static KHook::Return<void> TeleportPre(CBaseEntity *pThis, const Vector *newPosition, const QAngle *newAngles, const Vector *newVelocity)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (pThis->IsPawn())
	{
		MovementPlayer *player = g_pKZPlayerManager->ToPlayer(static_cast<CBasePlayerPawn *>(pThis));
		if (player)
		{
			player->OnTeleport(newPosition, newAngles, newVelocity);
		}
	}
	return {KHook::Action::Ignore};
}

static KHook::Virtual<CBaseEntity, void, const Vector *, const QAngle *, const Vector *> teleportHook(TeleportPre, nullptr);

static KHook::Return<void> ChangeTeamPost(CCSPlayerController *pThis, i32 team)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = g_pKZPlayerManager->ToPlayer(pThis);
	if (player)
	{
		player->OnChangeTeamPost(team);
	}
	return {KHook::Action::Ignore};
}

static KHook::Virtual<CCSPlayerController, void, i32> changeTeamHook(nullptr, ChangeTeamPost);

// ============================================================
// ISource2GameEntities hooks
// ============================================================

static KHook::Return<void> CheckTransmitPost(ISource2GameEntities *pThis, CCheckTransmitInfo **pInfos, int infoCount, CBitVec<16384> &unk1,
											 CBitVec<16384> &unk2, const Entity2Networkable_t **pNetworkables, const uint16 *pEntityIndicies,
											 int nEntities)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	KZ::quiet::OnCheckTransmit(pInfos, infoCount);
	KZProfileService::OnCheckTransmit();
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2GameEntities, void, CCheckTransmitInfo **, int, CBitVec<16384> &, CBitVec<16384> &, const Entity2Networkable_t **,
					  const uint16 *, int>
	checkTransmitHook(nullptr, CheckTransmitPost);

// ============================================================
// ISource2Server hooks
// ============================================================

static KHook::Return<void> GameFramePre(ISource2Server *pThis, bool simulating, bool bFirstTick, bool bLastTick)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	g_KZPlugin.serverGlobals = *(g_pKZUtils->GetGlobals());
	g_pKZPlayerManager->PerformAuthChecks();
	RunSubmission::CheckAll();
	BaseRequest::CheckRequests();
	KZTelemetryService::ActiveCheck();
	KZBeamService::UpdateBeams();
	KZPaintService::OnGameFrame();
	KZInvisibleService::OnGameFrame();
	KZProfileService::OnGameFrame();
	KZ::replaysystem::OnGameFrame();
	KZRacingService::BroadcastRaceInfo();
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2Server, void, bool, bool, bool> gameFrameHook(GameFramePre, nullptr);

static KHook::Return<void> GameServerSteamAPIActivatedPre(ISource2Server *pThis)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	g_steamAPI.Init();
	g_pKZPlayerManager->OnSteamAPIActivated();
	KZ::misc::customchangemap::OnSteamAPIActivated();
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2Server, void> gameServerSteamAPIActivatedHook(GameServerSteamAPIActivatedPre, nullptr);

static KHook::Return<void> GameServerSteamAPIDeactivatedPre(ISource2Server *pThis)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2Server, void> gameServerSteamAPIDeactivatedHook(GameServerSteamAPIDeactivatedPre, nullptr);

// ============================================================
// ISource2GameClients hooks
// ============================================================

static KHook::Return<bool> ClientConnectPre(ISource2GameClients *pThis, CPlayerSlot slot, const char *pszName, uint64 xuid, const char *pszNetworkID,
											bool unk1, CBufferString *pRejectReason)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	g_pKZPlayerManager->OnClientConnect(slot, pszName, xuid, pszNetworkID, unk1, pRejectReason);
	return {KHook::Action::Ignore, true};
}

static KHook::Virtual<ISource2GameClients, bool, CPlayerSlot, const char *, uint64, const char *, bool, CBufferString *>
	clientConnectHook(ClientConnectPre, nullptr);

static KHook::Return<void> OnClientConnectedPre(ISource2GameClients *pThis, CPlayerSlot slot, const char *pszName, uint64 xuid,
												const char *pszNetworkID, const char *pszAddress, bool bFakePlayer)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	g_pKZPlayerManager->OnClientConnected(slot, pszName, xuid, pszNetworkID, pszAddress, bFakePlayer);
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2GameClients, void, CPlayerSlot, const char *, uint64, const char *, const char *, bool>
	onClientConnectedHook(OnClientConnectedPre, nullptr);

static KHook::Return<void> ClientFullyConnectPre(ISource2GameClients *pThis, CPlayerSlot slot)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	g_pKZPlayerManager->OnClientFullyConnect(slot);
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2GameClients, void, CPlayerSlot> clientFullyConnectHook(ClientFullyConnectPre, nullptr);

static KHook::Return<void> ClientPutInServerPre(ISource2GameClients *pThis, CPlayerSlot slot, char const *pszName, int type, uint64 xuid)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	g_pKZPlayerManager->OnClientPutInServer(slot, pszName, type, xuid);
	// Привязка жалобы к сессии: без join/leave непонятно, был ли игрок на сервере
	// в названное им время.
	KZ_LOG_INFO(LogChannel::Player, "[cyb] player_join steam_id=%llu name=%s slot=%d\n", xuid, pszName, slot.Get());
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2GameClients, void, CPlayerSlot, char const *, int, uint64> clientPutInServerHook(ClientPutInServerPre, nullptr);

static KHook::Return<void> ClientActivePost(ISource2GameClients *pThis, CPlayerSlot slot, bool bLoadGame, const char *pszName, uint64 xuid)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	g_pKZPlayerManager->OnClientActive(slot, bLoadGame, pszName, xuid);
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(slot);
	if (player->GetPlayerPawn())
	{
		hooks::AddEntityHooks(player->GetPlayerPawn());
	}
	else
	{
		Warning("[KZ] WARNING: Player pawn for slot %i not found!\n", slot.Get());
	}
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2GameClients, void, CPlayerSlot, bool, const char *, uint64> clientActiveHook(nullptr, ClientActivePost);

static KHook::Return<void> ClientDisconnectPost(ISource2GameClients *pThis, CPlayerSlot slot, ENetworkDisconnectionReason reason, const char *pszName,
												uint64 xuid, const char *pszNetworkID)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(slot);
	// Персист незавершённого рана (SavedRuns) — ПЕРВЫМ делом, до тирдауна ниже.
	// SwitchTeam(0) уничтожает пешку и может поднять player_death (движковый вопрос, чтением
	// не закрывается); этот путь идёт не через KZ::misc::JoinTeam, поэтому changingTeam == false
	// и OnPlayerDeath принял бы его за реальную смерть: TimerStop убил бы живой ран, а
	// DropFrozenRun — замороженный prac-ран, и SaveOnDisconnect не сохранил бы ничего.
	// Сериализация читает только сервисные поля, пешку не трогает (kz_savedrun.cpp) — здесь
	// состояние заведомо целее, чем после тирдауна.
	player->timerService->OnClientDisconnect();
	// Раздеваем ПЕРЕД тирдауном: SwitchTeam(0) уничтожает пешку, а с mp_death_drop_gun 1
	// движок роняет её оружие на пол. Уборщик (kz_weapon_ground_cleanup) его подберёт, но
	// только через период, и до тех пор ствол лежит у всех на виду — а реконнекты это
	// множат. Дешевле не мусорить вовсе. Порядок важен: после SwitchTeam(0) пешки уже нет.
	if (player->GetPlayerPawn() && player->GetPlayerPawn()->m_pItemServices())
	{
		player->GetPlayerPawn()->m_pItemServices()->RemoveAllItems(false);
	}
	// Immediately remove the player off the list. We don't need to keep them around.
	if (player->GetController())
	{
		player->GetController()->m_LastTimePlayerWasDisconnectedForPawnsRemove().SetTime(0.01f);
		player->GetController()->SwitchTeam(0);
	}
	if (player->GetPlayerPawn())
	{
		hooks::RemoveEntityHooks(player->GetPlayerPawn());
	}
	else
	{
		Warning("WARNING: Player pawn for slot %i not found!\n", slot.Get());
	}
	KZ_LOG_INFO(LogChannel::Player, "[cyb] player_leave steam_id=%llu name=%s reason=%d\n", xuid, pszName, (int)reason);
	// Незакрытые опросы конваров этого слота: колбэк держит слот, а слот вот-вот переиспользуют.
	cvarquery::OnClientDisconnect(slot);
	player->recordingService->OnClientDisconnect();
	player->optionService->OnClientDisconnect();
	player->racingService->OnClientDisconnect();
	player->globalService->OnClientDisconnect();
	// hudService->OnClientDisconnect() удалён вместе с particle-MHUD (задача 12): он только
	// звал DestroyAllParticles(), а PlayerManager::OnClientDisconnect ниже сразу вызывает
	// KZPlayer::Reset(), который уже гасит весь худ-стейт слота.
	g_pKZPlayerManager->OnClientDisconnect(slot, reason, pszName, xuid, pszNetworkID);
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2GameClients, void, CPlayerSlot, ENetworkDisconnectionReason, const char *, uint64, const char *>
	clientDisconnectHook(nullptr, ClientDisconnectPost);

static KHook::Return<void> ClientVoicePre(ISource2GameClients *pThis, CPlayerSlot slot)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	g_pKZPlayerManager->OnClientVoice(slot);
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2GameClients, void, CPlayerSlot> clientVoiceHook(ClientVoicePre, nullptr);

static KHook::Return<void> ClientCommandPre(ISource2GameClients *pThis, CPlayerSlot slot, const CCommand &args)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (KZ::misc::CheckBlockedRadioCommands(args[0]))
	{
		return {KHook::Action::Supersede};
	}
	if (scmd::OnClientCommand(slot, args))
	{
		return {KHook::Action::Supersede};
	}
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2GameClients, void, CPlayerSlot, const CCommand &> clientCommandHook(ClientCommandPre, nullptr);

// Клик по кнопке panorama-меню (Task 11): через ClientSvcUserMessage идут ВСЕ клиентские
// usermessage каждого игрока каждый тик, поэтому фильтр по типу — первая строка, до всякого
// разбора протобафа. slot берём из аргумента хука (не из сущности в сообщении) и резолвим
// через него hudService — так клик одного игрока физически не может попасть на чужую сущность
// меню: KZHUDService::OnLayoutMenuClick сверяет присланный handle с this->ownedMenuLayout
// ИМЕННО этого hudService (см. layout/menu.cpp).
//
// KZ_UM_CUSTOM_HUD_CLICKED / CKZUsrMsg_CustomHudClicked — СВОИ, не SDK-шные CS_UM_
// CustomHudClicked/CCSUsrMsg_CustomHudClicked: движок реально шлёт usermessage с типом 390
// (значение из апстримного пина hl2sdk bd17582, game/shared/cstrike15/cstrike15_usermessages
// .proto, строки 636-639 — CS_UM_CustomHudClicked = 390), но наш сабмодульный пин hl2sdk-cs2
// (5f891c9) этого типа ещё не знает, а `protobuf/cstrike15_usermessages.proto` — СИМЛИНК в
// рабочее дерево сабмодуля (правка там не переживёт переклонирование сабмодуля раннером и не
// входит в наш коммит, см. protobuf/kz_customhud.proto). Поэтому константу типа держим своей
// (значение то же самое — оно приходит по проводу от игры, а не выводится из SDK), а тело
// сообщения парсим СВОИМ protobuf-типом с теми же номерами полей (совместимость wire-формата
// определяют номера полей, не имя типа).
static constexpr int KZ_UM_CUSTOM_HUD_CLICKED = 390;

static KHook::Return<void> ClientSvcUserMessagePre(ISource2GameClients *pThis, CPlayerSlot slot, int type, uint32 size, const void *buf)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (type != KZ_UM_CUSTOM_HUD_CLICKED)
	{
		return {KHook::Action::Ignore};
	}

	CKZUsrMsg_CustomHudClicked msg;
	if (!msg.ParseFromArray(buf, size))
	{
		return {KHook::Action::Ignore};
	}

	KZPlayer *player = g_pKZPlayerManager->ToPlayer(slot);
	if (player && player->IsInGame() && player->hudService)
	{
		player->hudService->OnLayoutMenuClick(msg.custom_hud_layout(), msg.button_id().c_str());
	}

	return {KHook::Action::Ignore};
}

static KHook::Virtual<ISource2GameClients, void, CPlayerSlot, int, uint32, const void *> clientSvcUserMessageHook(ClientSvcUserMessagePre, nullptr);

// ============================================================
// INetworkServerService hooks
// ============================================================

static KHook::Return<void> StartupServerPost(INetworkServerService *pThis, const GameSessionConfiguration_t &config, ISource2WorldSession *,
											 const char *)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	g_KZPlugin.AddonInit();
	KZ::course::ClearCourses();
	KZ::mapapi::Init();
	KZ::replaysystem::Init();
	// Луч !lead забывает маршрут прошлой карты. Именно здесь, а не в KZPlayer::Reset:
	// на выделенном сервере Reset зовётся только с дисконнекта и late load, этот хук
	// игроков не сбрасывает (то же ограничение у weapon/hud/zones).
	KZLeadService::OnMapChanged();
	// Воркер бэклога AWR: снять защёлку «файл в работе», если HTTP-колбэк прошлой карты
	// не дошёл, и начать период автоподбора заново.
	CybAwrBackfill::OnMapChanged();
	// Смена карты доехала. Закрывает две вещи: «сервер завис на смене карты»
	// (строки нет — значит загрузка не завершилась) и «на какой версии это было».
	KZ_LOG_INFO(LogChannel::General, "[cyb] map_loaded map=%s cs2kz=%s\n", g_pKZUtils->GetCurrentMapName().Get(), PLUGIN_FULL_VERSION);
	// Маркер в КОНСОЛЬ (переживает падение, в отличие от лог-файла в overlay): после апдейта
	// CS2 25470087 сервер падал уже ПОСЛЕ загрузки карты, и по стадиям загрузки это было не
	// локализовать. Парный маркер первого игрового тика — в PhysicsSimulatePre.
	Msg("[CS2KZ] run: map loaded\n");
	utils::SetMapLoaded();
	KZ::zones::OnMapLoaded();
	return {KHook::Action::Ignore};
}

static KHook::Virtual<INetworkServerService, void, const GameSessionConfiguration_t &, ISource2WorldSession *, const char *>
	startupServerHook(nullptr, StartupServerPost);

// ============================================================
// IGameEventManager2 hooks
// ============================================================

static KHook::Return<bool> FireEventPre(IGameEventManager2 *pThis, IGameEvent *event, bool bDontBroadcast)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (event)
	{
		if (KZ_STREQI(event->GetName(), "player_death"))
		{
			CEntityInstance *instance = event->GetPlayerPawn("userid");
			if (instance)
			{
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(instance->GetEntityIndex());
				if (player)
				{
					player->timerService->OnPlayerDeath();
					player->quietService->SendFullUpdate();
				}
			}
		}
		else if (KZ_STREQI(event->GetName(), "round_prestart"))
		{
			hooks::HookEntities();
			KZ::mapapi::OnRoundPreStart();
			// Зоны платформы здесь НЕ спавнятся: движковая очистка мира идёт после этого события
			// и снесла бы их (баг cyb.118). Только помечаем мир «не готов».
			KZ::zones::OnRoundPreStart();
		}
		else if (KZ_STREQI(event->GetName(), "round_start"))
		{
			interfaces::pEngine->ServerCommand("sv_full_alltalk 1");
			KZTimerService::OnRoundStart();
			KZHUDService::OnRoundStart();
			KZProfileService::OnRoundStart();
			KZ::misc::OnRoundStart();
			KZ::mapapi::OnRoundStart();
			// Мир доделан движком — теперь ставим зоны платформы и считаем аудит выживаемости.
			KZ::zones::OnRoundStart();
			// Строго ПОСЛЕ обоих: родные курсы карты провалидированы, их cyber-номера дальше
			// не двигаются. Сам вызов срабатывает один раз за карту (см. шапку функции).
			KZ::course::ReportCoursesToPlatform();
			KZ::replaysystem::OnRoundStart();
		}
		else if (KZ_STREQI(event->GetName(), "player_team"))
		{
			event->SetBool("silent", true);
		}
		else if (KZ_STREQI(event->GetName(), "player_spawn"))
		{
			CEntityInstance *instance = event->GetPlayerPawn("userid");
			if (instance)
			{
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(instance->GetEntityIndex());
				if (player)
				{
					player->timerService->OnPlayerSpawn();
					// Дефолтный ствол — на КАЖДОМ спавне. До этого UpdatePistol звался только
					// из KZ::misc::JoinTeam (обёртка команды jointeam), а мимо неё пешка
					// поднимается штатно и часто: движковый форс-пик команды по
					// mp_force_pick_time, админ-форс, mp_restartgame/сторож пустой карты,
					// changelevel. В таких спавнах игрок оставался с тем, что дал движок по
					// mp_ct_default_secondary — то есть без ножа/пистолета, если карта или
					// плагин их не выдали. Отсюда репорт «зашёл на сервер — пистолета нет».
					// Здесь — только гейты «выдача этому спавну вообще положена»: есть
					// контроллер, не бот (реплей-бот одевается своим кодом, kz/replays) и
					// играющая команда (у спектатора выдавать нечему и некому). Они же
					// определяют, какие спавны попадают в счётчики гейта, поэтому боты и
					// наблюдатели картину не разбавляют.
					// Дальше развилка целиком в KZPistolService::OnPlayerSpawn: смена
					// команды (там выдаёт JoinTeam), уже правильные руки (не трогаем
					// сущности — защита от гонки с cyber-skins) и собственно страйп.
					// Гейт и его счётчик держим рядом с UpdatePistol, а не здесь: разъедутся.
					auto controller = player->GetController();
					if (controller && !player->IsFakeClient() && controller->m_iTeamNum() >= CS_TEAM_T)
					{
						player->pistolService->OnPlayerSpawn(player->timerService->IsChangingTeam());
					}
				}
			}
		}
		else if (KZ_STREQI(event->GetName(), "player_connect") || KZ_STREQI(event->GetName(), "player_disconnect"))
		{
			// Вход/выход невидимки не анонсируем клиентам (чат/консоль). Серверные
			// листенеры событие получают как обычно; лог [cyb] player_join/leave живёт
			// в клиент-хуках (ClientPutInServerPre/ClientDisconnectPost) и не тронут.
			// xuid берём из самого события — состояние игрока на этих эпохах ещё/уже
			// неполное. Переигрываем вызов с bDontBroadcast=true через KHook::Recall — аналог
			// RETURN_META_VALUE_NEWPARAMS при SourceHook: цепочка продолжается со следующего хука
			// с новым аргументом. Guard по bDontBroadcast оставлен как страховка от повторного
			// входа.
			if (!bDontBroadcast && KZInvisibleService::IsInvisibleSteamId(event->GetUint64("xuid")))
			{
				return KHook::Recall(&IGameEventManager2::FireEvent, KHook::Return<bool> {KHook::Action::Ignore, true}, pThis, event, true);
			}
		}
	}
	return {KHook::Action::Ignore, true};
}

static KHook::Virtual<IGameEventManager2, bool, IGameEvent *, bool> fireEventHook(FireEventPre, nullptr);

// ============================================================
// ICvar hooks
// ============================================================

static KHook::Return<void> DispatchConCommandPre(ICvar *pThis, ConCommandRef cmd, const CCommandContext &ctx, const CCommand &args)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (KZ::misc::CheckBlockedRadioCommands(args[0]))
	{
		return {KHook::Action::Supersede};
	}
	scmd::NormalizeChatTrigger(cmd, ctx, args);
	if (KZOptionService::GetOptionInt("overridePlayerChat", true))
	{
		KZ::misc::ProcessConCommand(cmd, ctx, args);
	}
	if (scmd::OnDispatchConCommand(cmd, ctx, args))
	{
		return {KHook::Action::Supersede};
	}
	return {KHook::Action::Ignore};
}

static KHook::Virtual<ICvar, void, ConCommandRef, const CCommandContext &, const CCommand &> dispatchConCommandHook(DispatchConCommandPre, nullptr);

// ============================================================
// IGameEventSystem hooks
// ============================================================

static KHook::Return<void> PostEventPre(IGameEventSystem *pThis, CSplitScreenSlot nSlot, bool bLocalOnly, int nClientCount, const uint64 *clients,
										INetworkMessageInternal *pEvent, const CNetMessage *pData, unsigned long nSize, NetChannelBufType_t bufType)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	KZ::quiet::OnPostEvent(pEvent, pData, clients);
	return {KHook::Action::Ignore};
}

static KHook::Virtual<IGameEventSystem, void, CSplitScreenSlot, bool, int, const uint64 *, INetworkMessageInternal *, const CNetMessage *,
					  unsigned long, NetChannelBufType_t>
	postEventHook(PostEventPre, nullptr);

// ============================================================
// CEntitySystem hooks
// ============================================================

static KHook::Return<void> SpawnPre(CEntitySystem *pThis, int nCount, const EntitySpawnInfo_t *pInfo)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	KZ::mapapi::OnSpawn(nCount, pInfo);
	return {KHook::Action::Ignore};
}

static KHook::Virtual<CEntitySystem, void, int, const EntitySpawnInfo_t *> entitySystemSpawnHook(SpawnPre, nullptr);

// ============================================================
// CSpawnGroupMgrGameSystem hooks
// ============================================================

static KHook::Return<ILoadingSpawnGroup *> CreateLoadingSpawnGroupPre(CSpawnGroupMgrGameSystem *pThis, SpawnGroupHandle_t hSpawnGroup,
																	  bool bSynchronouslySpawnEntities, bool bConfirmResourcesLoaded,
																	  const CUtlVector<const CEntityKeyValues *> *pKeyValues)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	KZ::mapapi::OnCreateLoadingSpawnGroupHook(pKeyValues);
	return {KHook::Action::Ignore, nullptr};
}

static KHook::Virtual<CSpawnGroupMgrGameSystem, ILoadingSpawnGroup *, SpawnGroupHandle_t, bool, bool, const CUtlVector<const CEntityKeyValues *> *>
	createLoadingSpawnGroupHook(CreateLoadingSpawnGroupPre, nullptr);

// ============================================================
// CNetworkGameServerBase hooks
// ============================================================

static KHook::Return<bool> ActivateServerPost(CNetworkGameServerBase *pThis)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (!interfaces::pEngine->IsDedicatedServer())
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(0));
		player->Reset();
	}
	u64 id = g_pKZUtils->GetCurrentMapWorkshopID();
	u64 size = g_pKZUtils->GetCurrentMapSize();

	KZ_LOG_INFO(LogChannel::General, "Loading map %s, workshop ID %llu, size %llu\n", g_pKZUtils->GetCurrentMapVPK().Get(), id, size);

	RunSubmission::Clear();
	KZ::misc::OnActivateServer();
	KZPistolService::OnActivateServer();
	KZInvisibleService::OnActivateServer();
	KZDatabaseService::SetupMap();
	KZRecordingService::OnActivateServer();
	KZRacingService::OnActivateServer();
	KZGlobalService::OnActivateServer();

	char md5[33];
	g_pKZUtils->GetCurrentMapMD5(md5, sizeof(md5));
	KZ_LOG_INFO(LogChannel::General, "Map file md5: %s\n", md5);

	return {KHook::Action::Ignore, true};
}

static KHook::Virtual<CNetworkGameServerBase, bool> activateServerHook(nullptr, ActivateServerPost);

static KHook::Return<CServerSideClientBase *> ConnectClientPre(CNetworkGameServerBase *pThis, const char *pszName, ns_address *pAddr,
															   uint32 steam_handle, C2S_CONNECT_Message *pConnectMsg, const char *pszChallenge,
															   const byte *pAuthTicket, int nAuthTicketLength, bool bIsLowViolence)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	g_pKZPlayerManager->OnConnectClient(pszName, pAddr, steam_handle, pConnectMsg, pszChallenge, pAuthTicket, nAuthTicketLength, bIsLowViolence);
	return {KHook::Action::Ignore, nullptr};
}

static KHook::Return<CServerSideClientBase *> ConnectClientPost(CNetworkGameServerBase *pThis, const char *pszName, ns_address *pAddr,
																uint32 steam_handle, C2S_CONNECT_Message *pConnectMsg, const char *pszChallenge,
																const byte *pAuthTicket, int nAuthTicketLength, bool bIsLowViolence)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	g_pKZPlayerManager->OnConnectClientPost(pszName, pAddr, steam_handle, pConnectMsg, pszChallenge, pAuthTicket, nAuthTicketLength, bIsLowViolence);
	return {KHook::Action::Ignore, nullptr};
}

static KHook::Virtual<CNetworkGameServerBase, CServerSideClientBase *, const char *, ns_address *, uint32, C2S_CONNECT_Message *, const char *,
					  const byte *, int, bool>
	connectClientHook(ConnectClientPre, ConnectClientPost);

// ============================================================
// CServerSideClient hooks (cvar query)
// ============================================================

static KHook::Return<bool> ProcessRespondCvarValuePre(CServerSideClientBase *pThis, const CNetMessagePB<CCLCMsg_RespondCvarValue> &msg)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (pThis)
	{
		cvarquery::OnCvarValueResponse(pThis->GetPlayerSlot(), msg.cookie(), (cvarquery::Status)msg.status_code(), msg.name().c_str(),
									   msg.value().c_str());
	}
	return {KHook::Action::Ignore, true};
}

static KHook::Virtual<CServerSideClientBase, bool, const CNetMessagePB<CCLCMsg_RespondCvarValue> &> respondCvarValueHook(ProcessRespondCvarValuePre,
																														 nullptr);

// ============================================================
// IGameSystem hooks (CEntityDebugGameSystem vtable)
// ============================================================

static KHook::Return<void> ServerGamePostSimulatePost(IGameSystem *pThis, const EventServerGamePostSimulate_t *)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	ProcessTimers();
	KZRecordingService::ProcessFileWriteCompletion();
	if (g_asyncFileIO)
	{
		g_asyncFileIO->RunFrame();
	}
	KZRacingService::OnServerGamePostSimulate();
	KZGlobalService::OnServerGamePostSimulate();
	return {KHook::Action::Ignore};
}

static KHook::Virtual<IGameSystem, void, const EventServerGamePostSimulate_t *> serverGamePostSimulateHook(nullptr, ServerGamePostSimulatePost);

static KHook::Return<void> BuildGameSessionManifestPost(IGameSystem *pThis, const EventBuildGameSessionManifest_t *msg)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	Warning("[CS2KZ] IGameSystem::BuildGameSessionManifest\n");
	IEntityResourceManifest *pResourceManifest = msg->m_pResourceManifest;
	if (g_KZPlugin.IsAddonMounted())
	{
		Warning("[CS2KZ] Precache kz soundevents \n");
		pResourceManifest->AddResource(KZ_WORKSHOP_ADDON_SNDEVENT_FILE);
		// KZHUDService::PrecacheParticles(pResourceManifest) удалён вместе с particle-MHUD
		// (задача 12): апстрим вычистил particles/* из воркшоп-аддона, прекешировать больше нечего.
	}
	pResourceManifest->AddResource("particles/ui/hud/ui_map_def_utility_trail.vpcf");
	// Наша частица луча !lead из аддона GYMSTRIKE-KZ и стоковая рядом: cyb_lead_particle
	// переключает их живьём, а незарегистрированный в манифесте ассет у клиента не
	// прекешируется и не нарисуется вовсе.
	pResourceManifest->AddResource(KZ_LEAD_PARTICLE);
	pResourceManifest->AddResource(KZ_LEAD_PARTICLE_STOCK);
	return {KHook::Action::Ignore};
}

static KHook::Virtual<IGameSystem, void, const EventBuildGameSessionManifest_t *> buildGameSessionManifestHook(nullptr, BuildGameSessionManifestPost);

// ============================================================
// CCSPlayer_MovementServices virtual hooks
// ============================================================

static KHook::Return<void> PlayerRunCommandPre(CCSPlayer_MovementServices *pThis, PlayerCommand *pCmd)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(pThis);
	KZ::replaysystem::OnPlayerRunCommandPre(player, pCmd);
	return {KHook::Action::Ignore};
}

static KHook::Virtual<CCSPlayer_MovementServices, void, PlayerCommand *> playerRunCommandHook(PlayerRunCommandPre, nullptr);

static KHook::Return<void> FinishMovePre(CCSPlayer_MovementServices *pThis, PlayerCommand *pCmd, CMoveData *pMoveData)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(pThis);
	KZ::replaysystem::OnFinishMovePre(player, pMoveData);
	return {KHook::Action::Ignore};
}

static KHook::Virtual<CCSPlayer_MovementServices, void, PlayerCommand *, CMoveData *> finishMoveHook(FinishMovePre, nullptr);

// ============================================================
// Signature-based hooks
// ============================================================

static KHook::Return<int> RecvServerBrowserPacketPost(RecvPktInfo_t &info, void *pSock)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	return {KHook::Action::Ignore};
}

static KHook::Function<int, RecvPktInfo_t &, void *> RecvServerBrowserPacket(nullptr, RecvServerBrowserPacketPost);

#ifdef DEBUG_TPM
static KHook::Return<bool> TraceShapePost(const void *physicsQuery, const Ray_t &ray, const Vector &start, const Vector &end,
										  const CTraceFilter *pTraceFilter, trace_t *pm)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (!g_traceShapeEnabled)
	{
		return {KHook::Action::Ignore};
	}
	bool ret = *(bool *)KHook::GetOriginalValuePtr();
	f32 error;
	Vector velocity;
	for (u32 i = 0; i < 2; i++)
	{
		if (g_pKZPlayerManager->ToPlayer(i) && g_pKZPlayerManager->ToPlayer(i)->GetMoveServices())
		{
			error = g_pKZPlayerManager->ToPlayer(i)->GetMoveServices()->m_flAccumulatedJumpError();
			velocity = g_pKZPlayerManager->ToPlayer(i)->currentMoveData->m_vecVelocity;
			break;
		}
	}
	traceHistory.AddToTail({start, end, ray, pm->DidHit(), pm->m_vStartPos, pm->m_vEndPos, pm->m_vHitNormal, pm->m_vHitPoint, pm->m_flHitOffset,
							pm->m_flFraction, error, velocity});
	return {KHook::Action::Ignore};
}
#endif

static KHook::Function<bool, const void *, const Ray_t &, const Vector &, const Vector &, const CTraceFilter *, trace_t *> TraceShape(
#ifdef DEBUG_TPM
	nullptr, TraceShapePost
#else
	nullptr, nullptr
#endif
);

static KHook::Return<void> CPhysicsGameSystemFrameBoundaryPost(void *pThis)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	KZ::misc::OnPhysicsGameSystemFrameBoundary(pThis);
	return {KHook::Action::Ignore};
}

static KHook::Function<void, void *> CPhysicsGameSystemFrameBoundary(nullptr, CPhysicsGameSystemFrameBoundaryPost);

static KHook::Return<void> PhysicsSimulatePre(CCSPlayerController *controller)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	// Первый игровой тик — в консоль, один раз. Отделяет «упали на загрузке карты» от
	// «упали в игровом такте» без ядра (23.09.2026, билд 25470087).
	static bool firstTickLogged = false;
	if (!firstTickLogged)
	{
		firstTickLogged = true;
		Msg("[CS2KZ] run: first physics tick\n");
	}
	if (controller->m_bIsHLTV)
	{
		return {KHook::Action::Supersede};
	}
	g_KZPlugin.simulatingPhysics = true;
	playerManager->ToPlayer(controller)->OnPhysicsSimulate();
	return {KHook::Action::Ignore};
}

static KHook::Return<void> PhysicsSimulatePost(CCSPlayerController *controller)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (controller->m_bIsHLTV)
	{
		return {KHook::Action::Ignore};
	}
	MovementPlayer *player = playerManager->ToPlayer(controller);
	player->OnPhysicsSimulatePost();
	g_KZPlugin.simulatingPhysics = false;
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayerController, void> PhysicsSimulate(PhysicsSimulatePre, PhysicsSimulatePost);

static KHook::Return<i32> ProcessUsercmdsPre(CCSPlayerController *controller, PlayerCommand *cmds, int numcmds, bool paused, float margin);

static KHook::Member<CCSPlayerController, i32, PlayerCommand *, int, bool, float> ProcessUsercmds(ProcessUsercmdsPre, nullptr);

static KHook::Return<i32> ProcessUsercmdsPre(CCSPlayerController *controller, PlayerCommand *cmds, int numcmds, bool paused, float margin)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(controller);
	player->OnProcessUsercmds(cmds, numcmds);
	auto retValue = ProcessUsercmds.CallOriginal(controller, cmds, numcmds, paused, margin);
	player->OnProcessUsercmdsPost(cmds, numcmds);
	return {KHook::Action::Supersede, retValue};
}

static KHook::Return<void> SetupMovePre(CCSPlayer_MovementServices *ms, PlayerCommand *pc, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->currentMoveData = mv;
	player->moveDataPre = CMoveData(*mv);
	player->OnSetupMove(pc);
	return {KHook::Action::Ignore};
}

static KHook::Return<void> SetupMovePost(CCSPlayer_MovementServices *ms, PlayerCommand *pc, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnSetupMovePost(pc);
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, PlayerCommand *, CMoveData *> SetupMove(SetupMovePre, SetupMovePost);

static KHook::Return<void> ProcessMovementPre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->currentMoveData = mv;
	player->moveDataPre = CMoveData(*mv);
	player->OnProcessMovement();
	return {KHook::Action::Ignore};
}

static KHook::Return<void> ProcessMovementPost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->moveDataPost = CMoveData(*mv);
	player->OnProcessMovementPost();
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *> ProcessMovement(ProcessMovementPre, ProcessMovementPost);

static KHook::Return<bool> PlayerMovePre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnPlayerMove();
	return {KHook::Action::Ignore, false};
}

static KHook::Return<bool> PlayerMovePost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnPlayerMovePost();
	return {KHook::Action::Ignore, false};
}

static KHook::Member<CCSPlayer_MovementServices, bool, CMoveData *> PlayerMove(PlayerMovePre, PlayerMovePost);

static KHook::Return<void> CheckParametersPre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnCheckParameters();
	return {KHook::Action::Ignore};
}

static KHook::Return<void> CheckParametersPost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnCheckParametersPost();
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *> CheckParameters(CheckParametersPre, CheckParametersPost);

static KHook::Return<bool> CanMovePre(CCSPlayerPawnBase *pawn)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(pawn)->OnCanMove();
	return {KHook::Action::Ignore, false};
}

static KHook::Return<bool> CanMovePost(CCSPlayerPawnBase *pawn)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(pawn)->OnCanMovePost();
	return {KHook::Action::Ignore, false};
}

static KHook::Member<CCSPlayerPawnBase, bool> CanMove(CanMovePre, CanMovePost);

static KHook::Return<void> FullWalkMovePre(CCSPlayer_MovementServices *ms, CMoveData *mv, bool ground)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnFullWalkMove(ground);
	return {KHook::Action::Ignore};
}

static KHook::Return<void> FullWalkMovePost(CCSPlayer_MovementServices *ms, CMoveData *mv, bool ground)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnFullWalkMovePost(ground);
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *, bool> FullWalkMove(FullWalkMovePre, FullWalkMovePost);

static KHook::Return<bool> MoveInitPre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnMoveInit();
	return {KHook::Action::Ignore, false};
}

static KHook::Return<bool> MoveInitPost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnMoveInitPost();
	return {KHook::Action::Ignore, false};
}

static KHook::Member<CCSPlayer_MovementServices, bool, CMoveData *> MoveInit(MoveInitPre, MoveInitPost);

static KHook::Return<bool> CheckWaterPre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnCheckWater();
	return {KHook::Action::Ignore, false};
}

static KHook::Return<bool> CheckWaterPost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnCheckWaterPost();
	return {KHook::Action::Ignore, false};
}

static KHook::Member<CCSPlayer_MovementServices, bool, CMoveData *> CheckWater(CheckWaterPre, CheckWaterPost);

static KHook::Return<void> WaterMovePre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnWaterMove();
#ifdef WATER_FIX
	if (player->enableWaterFix)
	{
		player->ignoreNextCategorizePosition = true;
	}
#endif
	return {KHook::Action::Ignore};
}

static KHook::Return<void> WaterMovePost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnWaterMovePost();
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *> WaterMove(WaterMovePre, WaterMovePost);

static KHook::Return<void> CheckVelocityPre(CCSPlayer_MovementServices *ms, CMoveData *mv, const char *a3)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnCheckVelocity(a3);
	return {KHook::Action::Ignore};
}

static KHook::Return<void> CheckVelocityPost(CCSPlayer_MovementServices *ms, CMoveData *mv, const char *a3)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnCheckVelocityPost(a3);
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *, const char *> CheckVelocity(CheckVelocityPre, CheckVelocityPost);

static KHook::Return<void> DuckPre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnDuck();
	player->processingDuck = true;
	return {KHook::Action::Ignore};
}

static KHook::Return<void> DuckPost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->processingDuck = false;
	player->OnDuckPost();
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *> Duck(DuckPre, DuckPost);

static KHook::Return<bool> CanUnduckPre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnCanUnduck();
	return {KHook::Action::Ignore, false};
}

static KHook::Return<bool> CanUnduckPost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	bool canUnduck = *(bool *)KHook::GetOriginalValuePtr();
	player->OnCanUnduckPost(canUnduck);
	return {KHook::Action::Ignore, canUnduck};
}

static KHook::Member<CCSPlayer_MovementServices, bool, CMoveData *> CanUnduck(CanUnduckPre, CanUnduckPost);

static KHook::Return<bool> LadderMovePre(CCSPlayer_MovementServices *ms, CMoveData *mv);

static KHook::Member<CCSPlayer_MovementServices, bool, CMoveData *> LadderMove(LadderMovePre, nullptr);

static KHook::Return<bool> LadderMovePre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnLadderMove();
	Vector oldVelocity = mv->m_vecVelocity;
	MoveType_t oldMoveType = player->GetPlayerPawn()->m_MoveType();
	bool result = LadderMove.CallOriginal(ms, mv);
	if (player->GetPlayerPawn()->m_lifeState() != LIFE_DEAD && !result && oldMoveType == MOVETYPE_LADDER)
	{
		player->SetMoveType(MOVETYPE_WALK, false);
	}
	if (!result && oldMoveType == MOVETYPE_LADDER)
	{
		player->RegisterTakeoff(false, true, &player->lastValidLadderOrigin);
		player->OnChangeMoveType(MOVETYPE_LADDER);
	}
	else if (result && oldMoveType != MOVETYPE_LADDER && player->GetPlayerPawn()->m_MoveType() == MOVETYPE_LADDER
			 && !(player->GetPlayerPawn()->m_fFlags & FL_ONGROUND))
	{
		player->RegisterLanding(oldVelocity, false);
		player->OnChangeMoveType(MOVETYPE_WALK);
	}
	else if (result && oldMoveType == MOVETYPE_LADDER && player->GetPlayerPawn()->m_MoveType() == MOVETYPE_WALK)
	{
		player->RegisterTakeoff(player->IsButtonPressed(IN_JUMP), true);
		player->possibleLadderHop = true;
		player->OnChangeMoveType(MOVETYPE_LADDER);
	}
	if (result && player->GetPlayerPawn()->m_MoveType() == MOVETYPE_LADDER)
	{
		player->GetOrigin(&player->lastValidLadderOrigin);
	}
	player->OnLadderMovePost();
	return {KHook::Action::Supersede, result};
}

static KHook::Return<void> CheckJumpButtonLegacyPre(CCSPlayerLegacyJump *legacy, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	CCSPlayer_MovementServices *ms = legacy->m_pMovementServices;
	MovementPlayer *player = playerManager->ToPlayer(ms);
#ifdef WATER_FIX
	if (player->enableWaterFix && ms->pawn->m_MoveType() == MOVETYPE_WALK && ms->pawn->m_flWaterLevel() > 0.5f && ms->pawn->m_fFlags & FL_ONGROUND)
	{
		if (ms->m_nButtons().m_pButtonStates[0] & IN_JUMP)
		{
			ms->m_nButtons().m_pButtonStates[1] |= IN_JUMP;
		}
	}
#endif
	player->OnCheckJumpButtonLegacy();
	return {KHook::Action::Ignore};
}

static KHook::Return<void> CheckJumpButtonLegacyPost(CCSPlayerLegacyJump *legacy, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(legacy->m_pMovementServices)->OnCheckJumpButtonLegacyPost();
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayerLegacyJump, void, CMoveData *> CheckJumpButtonLegacy(CheckJumpButtonLegacyPre, CheckJumpButtonLegacyPost);

static KHook::Return<void> CheckJumpButtonModernPre(CCSPlayerModernJump *modern, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(modern->m_pMovementServices)->OnCheckJumpButtonModern();
	return {KHook::Action::Ignore};
}

static KHook::Return<void> CheckJumpButtonModernPost(CCSPlayerModernJump *modern, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(modern->m_pMovementServices)->OnCheckJumpButtonModernPost();
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayerModernJump, void, CMoveData *> CheckJumpButtonModern(CheckJumpButtonModernPre, CheckJumpButtonModernPost);

static KHook::Return<void> OnJumpLegacyPre(CCSPlayerLegacyJump *legacy, CMoveData *mv);

static KHook::Member<CCSPlayerLegacyJump, void, CMoveData *> OnJumpLegacy(OnJumpLegacyPre, nullptr);

static KHook::Return<void> OnJumpLegacyPre(CCSPlayerLegacyJump *legacy, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	CCSPlayer_MovementServices *ms = legacy->m_pMovementServices;
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnJumpLegacy();
	Vector oldOutWishVel = mv->m_outWishVel;
	OnJumpLegacy.CallOriginal(legacy, mv);
	if (mv->m_outWishVel != oldOutWishVel)
	{
		player->inPerf = (!player->takeoffFromLadder && !player->oldWalkMoved);
		player->RegisterTakeoff(true, player->possibleLadderHop);
		player->OnStopTouchGround();
	}
	player->OnJumpLegacyPost();
	return {KHook::Action::Supersede};
}

static KHook::Return<void> OnJumpModernPre(CCSPlayerModernJump *modern, CMoveData *mv);

static KHook::Member<CCSPlayerModernJump, void, CMoveData *> OnJumpModern(OnJumpModernPre, nullptr);

static KHook::Return<void> OnJumpModernPre(CCSPlayerModernJump *modern, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	CCSPlayer_MovementServices *ms = modern->m_pMovementServices;
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->OnJumpModern();
	Vector oldOutWishVel = mv->m_outWishVel;
	OnJumpModern.CallOriginal(modern, mv);
	if (mv->m_outWishVel != oldOutWishVel)
	{
		player->inPerf = (!player->takeoffFromLadder && !player->oldWalkMoved);
		player->RegisterTakeoff(true, player->possibleLadderHop);
		player->OnStopTouchGround();
	}
	player->OnJumpModernPost();
	return {KHook::Action::Supersede};
}

static KHook::Return<void> AirMovePre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnAirMove();
	return {KHook::Action::Ignore};
}

static KHook::Return<void> AirMovePost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnAirMovePost();
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *> AirMove(AirMovePre, AirMovePost);

static KHook::Return<void> AirAcceleratePre(CCSPlayer_MovementServices *ms, CMoveData *mv, Vector &wishdir, f32 wishspeed, f32 accel)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnAirAccelerate(wishdir, wishspeed, accel);
	return {KHook::Action::Ignore};
}

static KHook::Return<void> AirAcceleratePost(CCSPlayer_MovementServices *ms, CMoveData *mv, Vector &wishdir, f32 wishspeed, f32 accel)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnAirAcceleratePost(wishdir, wishspeed, accel);
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *, Vector &, f32, f32> AirAccelerate(AirAcceleratePre, AirAcceleratePost);

static KHook::Return<void> FrictionPre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnFriction();
	return {KHook::Action::Ignore};
}

static KHook::Return<void> FrictionPost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnFrictionPost();
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *> Friction(FrictionPre, FrictionPost);

static KHook::Return<void> WalkMovePre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnWalkMove();
	return {KHook::Action::Ignore};
}

static KHook::Return<void> WalkMovePost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
	player->walkMoved = true;
	player->OnWalkMovePost();
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *> WalkMove(WalkMovePre, WalkMovePost);

static KHook::Return<void> TryPlayerMovePre(CCSPlayer_MovementServices *ms, CMoveData *mv, Vector *pFirstDest, trace_t *pFirstTrace,
											bool *bIsSurfing);

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *, Vector *, trace_t *, bool *> TryPlayerMove(TryPlayerMovePre, nullptr);

static KHook::Return<void> TryPlayerMovePre(CCSPlayer_MovementServices *ms, CMoveData *mv, Vector *pFirstDest, trace_t *pFirstTrace, bool *bIsSurfing)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
#ifdef DEBUG_TPM
	traceHistory.RemoveAll();
	f32 initialError = ms->m_flAccumulatedJumpError();
	Vector initialVelocity = mv->m_vecVelocity;
	g_traceShapeEnabled = true;
	player->OnTryPlayerMove(pFirstDest, pFirstTrace, bIsSurfing);
	Vector oldVelocity = mv->m_vecVelocity;
	i32 count = traceHistory.Count();
	TryPlayerMove.CallOriginal(ms, mv, pFirstDest, pFirstTrace, bIsSurfing);
	if (traceHistory.Count() != count)
	{
		for (i32 i = 0; i + count < traceHistory.Count(); i++)
		{
			if (traceHistory[i].end != traceHistory[i + count].end)
			{
				META_CONPRINTF("Trace not matching! Previous traces (initial error %f, initial velocity %s):\n", initialError,
							   VecToString(initialVelocity));
				for (i32 j = 0; j <= i; j++)
				{
					META_CONPRINTF("Pred %f %f %f -> %f %f %f, error %f, velocity %s ", traceHistory[j].start.x, traceHistory[j].start.y,
								   traceHistory[j].start.z, traceHistory[j].end.x, traceHistory[j].end.y, traceHistory[j].end.z,
								   traceHistory[j].error, VecToString(traceHistory[j].velocity));
					if (traceHistory[j].didHit)
					{
						META_CONPRINTF("hit %s (normal %s, hitpoint %s)\n", VecToString(traceHistory[j].m_vEndPos),
									   VecToString(traceHistory[j].m_vHitNormal), VecToString(traceHistory[j].m_vHitPoint));
					}
					else
					{
						META_CONPRINTF("missed\n");
					}
					META_CONPRINTF("Real %f %f %f -> %f %f %f, error %f, velocity %s ", traceHistory[j + count].start.x,
								   traceHistory[j + count].start.y, traceHistory[j + count].start.z, traceHistory[j + count].end.x,
								   traceHistory[j + count].end.y, traceHistory[j + count].end.z, traceHistory[j + count].error,
								   VecToString(traceHistory[j + count].velocity));
					if (traceHistory[j + count].didHit)
					{
						META_CONPRINTF("hit %s (normal %s, hitpoint %s)\n", VecToString(traceHistory[j + count].m_vEndPos),
									   VecToString(traceHistory[j + count].m_vHitNormal), VecToString(traceHistory[j + count].m_vHitPoint));
					}
					else
					{
						META_CONPRINTF("missed\n");
					}
				}
				break;
			}
		}
	}
	g_traceShapeEnabled = false;
#else
	player->OnTryPlayerMove(pFirstDest, pFirstTrace, bIsSurfing);
	Vector oldVelocity = mv->m_vecVelocity;
	TryPlayerMove.CallOriginal(ms, mv, pFirstDest, pFirstTrace, bIsSurfing);
#endif
	if (mv->m_vecVelocity != oldVelocity)
	{
		player->SetCollidingWithWorld();
	}
	player->OnTryPlayerMovePost(pFirstDest, pFirstTrace, bIsSurfing);
	return {KHook::Action::Supersede};
}

static KHook::Return<void> CategorizePositionPre(CCSPlayer_MovementServices *ms, CMoveData *mv, bool bStayOnGround);

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *, bool> CategorizePosition(CategorizePositionPre, nullptr);

static KHook::Return<void> CategorizePositionPre(CCSPlayer_MovementServices *ms, CMoveData *mv, bool bStayOnGround)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer *player = playerManager->ToPlayer(ms);
#ifdef WATER_FIX
	if (player->enableWaterFix && player->ignoreNextCategorizePosition)
	{
		player->ignoreNextCategorizePosition = false;
		return {KHook::Action::Supersede};
	}
#endif
	player->OnCategorizePosition(bStayOnGround);
	Vector oldVelocity = mv->m_vecVelocity;
	bool oldOnGround = !!(player->GetPlayerPawn()->m_fFlags() & FL_ONGROUND);

	CategorizePosition.CallOriginal(ms, mv, bStayOnGround);

	bool ground = !!(player->GetPlayerPawn()->m_fFlags() & FL_ONGROUND);
	if (oldOnGround != ground)
	{
		if (ground)
		{
			player->RegisterLanding(oldVelocity);
			player->OnStartTouchGround();
		}
		else
		{
			player->RegisterTakeoff(false);
			player->OnStopTouchGround();
		}
	}
	player->OnCategorizePositionPost(bStayOnGround);
	return {KHook::Action::Supersede};
}

static KHook::Return<void> CheckFallingPre(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnCheckFalling();
	return {KHook::Action::Ignore};
}

static KHook::Return<void> CheckFallingPost(CCSPlayer_MovementServices *ms, CMoveData *mv)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(ms)->OnCheckFallingPost();
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayer_MovementServices, void, CMoveData *> CheckFalling(CheckFallingPre, CheckFallingPost);

static KHook::Return<void> PostThinkPre(CCSPlayerPawnBase *pawn)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(pawn)->OnPostThink();
	return {KHook::Action::Ignore};
}

static KHook::Return<void> PostThinkPost(CCSPlayerPawnBase *pawn)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	playerManager->ToPlayer(pawn)->OnPostThinkPost();
	return {KHook::Action::Ignore};
}

static KHook::Member<CCSPlayerPawnBase, void> PostThink(PostThinkPre, PostThinkPost);

struct SignatureHook
{
	const char *name;
	void (*configure)(void *address);
};

#define SIGNATURE_HOOK(hook) {#hook, [](void *address) { hook.Configure(address); }}

static_global const SignatureHook SIGNATURE_HOOKS[] = {
	SIGNATURE_HOOK(RecvServerBrowserPacket),
	SIGNATURE_HOOK(CPhysicsGameSystemFrameBoundary),
#ifdef DEBUG_TPM
	SIGNATURE_HOOK(TraceShape),
#endif
	SIGNATURE_HOOK(PhysicsSimulate),
	SIGNATURE_HOOK(ProcessUsercmds),
	SIGNATURE_HOOK(SetupMove),
	SIGNATURE_HOOK(ProcessMovement),
	SIGNATURE_HOOK(PlayerMove),
	SIGNATURE_HOOK(CheckParameters),
	SIGNATURE_HOOK(CanMove),
	SIGNATURE_HOOK(FullWalkMove),
	SIGNATURE_HOOK(MoveInit),
	SIGNATURE_HOOK(CheckWater),
	SIGNATURE_HOOK(WaterMove),
	SIGNATURE_HOOK(CheckVelocity),
	SIGNATURE_HOOK(Duck),
	SIGNATURE_HOOK(CanUnduck),
	SIGNATURE_HOOK(LadderMove),
	SIGNATURE_HOOK(CheckJumpButtonLegacy),
	SIGNATURE_HOOK(CheckJumpButtonModern),
	SIGNATURE_HOOK(OnJumpLegacy),
	SIGNATURE_HOOK(OnJumpModern),
	SIGNATURE_HOOK(AirMove),
	SIGNATURE_HOOK(AirAccelerate),
	SIGNATURE_HOOK(Friction),
	SIGNATURE_HOOK(WalkMove),
	SIGNATURE_HOOK(TryPlayerMove),
	SIGNATURE_HOOK(CategorizePosition),
	SIGNATURE_HOOK(CheckFalling),
	SIGNATURE_HOOK(PostThink),
};

#undef SIGNATURE_HOOK

// ============================================================
// hooks::Initialize
// ============================================================
bool hooks::Initialize(char *error, size_t maxlen)
{
	// Resolve every signature before anything is hooked, so outdated gamedata aborts the load instead of running with missing detours.
	void *signatureAddresses[KZ_ARRAYSIZE(SIGNATURE_HOOKS)];
	std::string missingSignatures;
	for (u32 i = 0; i < KZ_ARRAYSIZE(SIGNATURE_HOOKS); i++)
	{
		signatureAddresses[i] = g_pGameConfig->ResolveSignature(SIGNATURE_HOOKS[i].name);
		if (!signatureAddresses[i])
		{
			missingSignatures += missingSignatures.empty() ? "" : ", ";
			missingSignatures += SIGNATURE_HOOKS[i].name;
		}
	}
	if (!missingSignatures.empty())
	{
		snprintf(error, maxlen, "Failed to resolve signatures: %s", missingSignatures.c_str());
		KZ_LOG_WARN(LogChannel::General, "%s\n", error);
		return false;
	}

	// Втаблицы — тоже до первого хука: не нашлась (сменилось имя класса после апдейта) — честный
	// отказ загрузки, а не разыменование nullptr внутри AddGlobal.
	void *networkGameServerVtbl = modules::engine->FindVirtualTable("CNetworkGameServer");
	void *serverSideClientVtbl = modules::engine->FindVirtualTable("CServerSideClient");
	void *entityDebugGameSystemVtbl = modules::server->FindVirtualTable("CEntityDebugGameSystem");
	void *gameEntitySystemVtbl = modules::server->FindVirtualTable("CGameEntitySystem");
	void *spawnGroupMgrVtbl = modules::server->FindVirtualTable("CSpawnGroupMgrGameSystem");
	void *moveServicesVtbl = modules::server->FindVirtualTable("CCSPlayer_MovementServices");
	void *playerControllerVtbl = modules::server->FindVirtualTable("CCSPlayerController");
	if (!networkGameServerVtbl || !serverSideClientVtbl || !entityDebugGameSystemVtbl || !gameEntitySystemVtbl || !spawnGroupMgrVtbl
		|| !moveServicesVtbl || !playerControllerVtbl)
	{
		snprintf(error, maxlen, "Failed to resolve one or more virtual tables required for hooking.");
		KZ_LOG_WARN(LogChannel::General, "%s\n", error);
		return false;
	}

	playerManager = static_cast<MovementPlayerManager *>(g_pPlayerManager);

	// Entity hooks
	startTouchHook.Configure(g_pGameConfig->GetOffset("StartTouch"));
	touchHook.Configure(g_pGameConfig->GetOffset("Touch"));
	endTouchHook.Configure(g_pGameConfig->GetOffset("EndTouch"));
	teleportHook.Configure(g_pGameConfig->GetOffset("Teleport"));
	changeTeamHook.Configure(g_pGameConfig->GetOffset("ControllerChangeTeam"));
	playerRunCommandHook.Configure(g_pGameConfig->GetOffset("PlayerRunCommand"));
	finishMoveHook.Configure(g_pGameConfig->GetOffset("FinishMove"));

	// Interface hooks
	checkTransmitHook.Configure(&ISource2GameEntities::CheckTransmit);
	checkTransmitHook.Add(g_pSource2GameEntities);

	gameFrameHook.Configure(&ISource2Server::GameFrame);
	gameFrameHook.Add(interfaces::pServer);

	gameServerSteamAPIActivatedHook.Configure(&ISource2Server::GameServerSteamAPIActivated);
	gameServerSteamAPIActivatedHook.Add(interfaces::pServer);

	gameServerSteamAPIDeactivatedHook.Configure(&ISource2Server::GameServerSteamAPIDeactivated);
	gameServerSteamAPIDeactivatedHook.Add(interfaces::pServer);

	clientConnectHook.Configure(&ISource2GameClients::ClientConnect);
	clientConnectHook.Add(g_pSource2GameClients);

	onClientConnectedHook.Configure(&ISource2GameClients::OnClientConnected);
	onClientConnectedHook.Add(g_pSource2GameClients);

	clientFullyConnectHook.Configure(&ISource2GameClients::ClientFullyConnect);
	clientFullyConnectHook.Add(g_pSource2GameClients);

	clientPutInServerHook.Configure(&ISource2GameClients::ClientPutInServer);
	clientPutInServerHook.Add(g_pSource2GameClients);

	clientActiveHook.Configure(&ISource2GameClients::ClientActive);
	clientActiveHook.Add(g_pSource2GameClients);

	clientDisconnectHook.Configure(&ISource2GameClients::ClientDisconnect);
	clientDisconnectHook.Add(g_pSource2GameClients);

	clientVoiceHook.Configure(&ISource2GameClients::ClientVoice);
	clientVoiceHook.Add(g_pSource2GameClients);

	clientCommandHook.Configure(&ISource2GameClients::ClientCommand);
	clientCommandHook.Add(g_pSource2GameClients);

	clientSvcUserMessageHook.Configure(&ISource2GameClients::ClientSvcUserMessage);
	clientSvcUserMessageHook.Add(g_pSource2GameClients);

	startupServerHook.Configure(&INetworkServerService::StartupServer);
	startupServerHook.Add(g_pNetworkServerService);

	fireEventHook.Configure(&IGameEventManager2::FireEvent);
	fireEventHook.Add(interfaces::pGameEventManager);

	dispatchConCommandHook.Configure(&ICvar::DispatchConCommand);
	dispatchConCommandHook.Add(g_pCVar);

	postEventHook.Configure(&IGameEventSystem::PostEventAbstract);
	postEventHook.Add(interfaces::pGameEventSystem);

	// Hooks by searching virtual tables
	{
		void *vtable = networkGameServerVtbl;
		activateServerHook.Configure(&CNetworkGameServerBase::ActivateServer);
		activateServerHook.AddGlobal((CNetworkGameServerBase *)&vtable);

		connectClientHook.Configure(&CNetworkGameServerBase::ConnectClient);
		connectClientHook.AddGlobal((CNetworkGameServerBase *)&vtable);
	}

	{
		void *vtable = serverSideClientVtbl;
		respondCvarValueHook.Configure(&CServerSideClientBase::ProcessRespondCvarValue);
		respondCvarValueHook.AddGlobal((CServerSideClientBase *)&vtable);
	}

	{
		void *vtable = entityDebugGameSystemVtbl;
		serverGamePostSimulateHook.Configure(&IGameSystem::OnServerGamePostSimulate);
		serverGamePostSimulateHook.AddGlobal((IGameSystem *)&vtable);

		buildGameSessionManifestHook.Configure(&IGameSystem::OnBuildGameSessionManifest);
		buildGameSessionManifestHook.AddGlobal((IGameSystem *)&vtable);
	}

	{
		void *vtable = gameEntitySystemVtbl;
		entitySystemSpawnHook.Configure(&CEntitySystem::Spawn);
		entitySystemSpawnHook.AddGlobal((CEntitySystem *)&vtable);
	}

	{
		void *vtable = spawnGroupMgrVtbl;
		createLoadingSpawnGroupHook.Configure(&CSpawnGroupMgrGameSystem::CreateLoadingSpawnGroup);
		createLoadingSpawnGroupHook.AddGlobal((CSpawnGroupMgrGameSystem *)&vtable);
	}

	{
		void *vtable = moveServicesVtbl;
		playerRunCommandHook.AddGlobal((CCSPlayer_MovementServices *)&vtable);
		finishMoveHook.AddGlobal((CCSPlayer_MovementServices *)&vtable);
	}

	{
		// На всю втаблицу контроллера, как прежний SH_ADD_MANUALVPHOOK: поштучный Add из
		// HookEntities (round_prestart) пропускал всех, кто зашёл позже начала раунда, — у них
		// не срабатывали пауза при уходе в спек, prac и невидимки (OnChangeTeamPost).
		void *vtable = playerControllerVtbl;
		changeTeamHook.AddGlobal((CCSPlayerController *)&vtable);
	}

	// Signature-based hooks
	for (u32 i = 0; i < KZ_ARRAYSIZE(SIGNATURE_HOOKS); i++)
	{
		SIGNATURE_HOOKS[i].configure(signatureAddresses[i]);
	}
	return true;
}

void hooks::Cleanup()
{
	startTouchHook.ClearHooks();
	touchHook.ClearHooks();
	endTouchHook.ClearHooks();
	teleportHook.ClearHooks();
	changeTeamHook.ClearHooks();

	checkTransmitHook.ClearHooks();
	gameFrameHook.ClearHooks();
	gameServerSteamAPIActivatedHook.ClearHooks();
	gameServerSteamAPIDeactivatedHook.ClearHooks();
	clientConnectHook.ClearHooks();
	onClientConnectedHook.ClearHooks();
	clientFullyConnectHook.ClearHooks();
	clientPutInServerHook.ClearHooks();
	clientActiveHook.ClearHooks();
	clientDisconnectHook.ClearHooks();
	clientVoiceHook.ClearHooks();
	clientCommandHook.ClearHooks();
	clientSvcUserMessageHook.ClearHooks();
	startupServerHook.ClearHooks();
	fireEventHook.ClearHooks();
	dispatchConCommandHook.ClearHooks();
	postEventHook.ClearHooks();
	activateServerHook.ClearHooks();
	connectClientHook.ClearHooks();
	respondCvarValueHook.ClearHooks();
	serverGamePostSimulateHook.ClearHooks();
	buildGameSessionManifestHook.ClearHooks();
	entitySystemSpawnHook.ClearHooks();
	createLoadingSpawnGroupHook.ClearHooks();
	playerRunCommandHook.ClearHooks();
	finishMoveHook.ClearHooks();

	cvarquery::Shutdown();

	if (GameEntitySystem())
	{
		GameEntitySystem()->RemoveListenerEntity(&entityListener);
	}
}

// ============================================================
// Entity hook management
// ============================================================
void hooks::AddEntityHooks(CBaseEntity *entity)
{
	if (KZTriggerService::IsValidTrigger(entity) || !V_stricmp(entity->GetClassname(), "player"))
	{
		startTouchHook.Add(entity);
		touchHook.Add(entity);
		endTouchHook.Add(entity);
		CCSPlayerPawn *pawn = static_cast<CCSPlayerPawn *>(entity);
		if (!V_stricmp(entity->GetClassname(), "player") && g_pKZPlayerManager->ToPlayer(pawn))
		{
			teleportHook.Add(entity);
		}
	}
}

void hooks::RemoveEntityHooks(CBaseEntity *entity)
{
	if (KZTriggerService::IsValidTrigger(entity) || !V_stricmp(entity->GetClassname(), "player"))
	{
		startTouchHook.Remove(entity);
		touchHook.Remove(entity);
		endTouchHook.Remove(entity);
		if (!KZTriggerService::IsValidTrigger(entity))
		{
			teleportHook.Remove(entity);
		}
	}
}

void EntListener::OnEntityCreated(CEntityInstance *pEntity) {}

void EntListener::OnEntitySpawned(CEntityInstance *pEntity)
{
	if (KZTriggerService::IsValidTrigger(static_cast<CBaseEntity *>(pEntity)))
	{
		CBaseTrigger *trigger = static_cast<CBaseTrigger *>(pEntity);
		trigger->m_fEffects() &= ~EF_NODRAW;
		hooks::AddEntityHooks(static_cast<CBaseEntity *>(pEntity));
		KZ::mapapi::CheckEndTimerTrigger((CBaseTrigger *)pEntity);
	}
}

void EntListener::OnEntityDeleted(CEntityInstance *pEntity)
{
	if (KZTriggerService::IsValidTrigger(static_cast<CBaseEntity *>(pEntity)))
	{
		hooks::RemoveEntityHooks(static_cast<CBaseEntity *>(pEntity));
	}
}

void hooks::HookEntities()
{
	startTouchHook.ClearHooks();
	touchHook.ClearHooks();
	endTouchHook.ClearHooks();
	teleportHook.ClearHooks();

	GameEntitySystem()->RemoveListenerEntity(&entityListener);
	for (CEntityIdentity *entID = GameEntitySystem()->m_EntityList.m_pFirstActiveEntity; entID != NULL; entID = entID->m_pNext)
	{
		hooks::AddEntityHooks(static_cast<CBaseEntity *>(entID->m_pInstance));
	}
	GameEntitySystem()->AddListenerEntity(&entityListener);
}
