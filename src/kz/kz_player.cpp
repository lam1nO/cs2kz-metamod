#include "kz.h"
#include "utils/utils.h"
#include "utils/ctimer.h"
#include "anticheat/kz_anticheat.h"
#include "beam/kz_beam.h"
#include "checkpoint/kz_checkpoint.h"
#include "db/kz_db.h"
#include "hud/kz_hud.h"
#include "invisible/kz_invisible.h"
#include "jumpstats/kz_jumpstats.h"
#include "language/kz_language.h"
#include "measure/kz_measure.h"
#include "zones/kz_zones.h"
#include "mode/kz_mode.h"
#include "noclip/kz_noclip.h"
#include "option/kz_option.h"
#include "paint/kz_paint.h"
#include "quiet/kz_quiet.h"
#include "spec/kz_spec.h"
#include "goto/kz_goto.h"
#include "style/kz_style.h"
#include "telemetry/kz_telemetry.h"
#include "timer/kz_timer.h"
#include "tip/kz_tip.h"
#include "trigger/kz_trigger.h"
#include "recording/kz_recording.h"
#include "savedrun/kz_savedrun.h"
#include "replays/kz_replaysystem.h"
#include "racing/kz_racing.h"
#include "global/kz_global.h"
#include "profile/kz_profile.h"
#include "pistol/kz_pistol.h"
#include "fov/kz_fov.h"
#include "ztopwatch/kz_ztopwatch.h"
#include "prac/kz_prac.h"

#include "cs2kz.h"
#include "convar.h"
#include "sdk/datatypes.h"
#include "sdk/entity/cbasetrigger.h"
#include "vprof.h"
#include "steam/isteamgameserver.h"
#include "tier0/memdbgon.h"
extern CSteamGameServerAPIContext g_steamAPI;
// Объявлен рядом с kz_anticheat (anticheat/detectors/cvars.cpp): turn-бинды не детектор,
// но по смыслу это тот же тумблер «помощник-бинды на наших non-global серверах».
extern CConVar<bool> kz_allow_turnbinds;

void KZPlayer::Init()
{
	MovementPlayer::Init();

	// TODO: initialize every service.
	delete this->anticheatService;
	delete this->beamService;
	delete this->checkpointService;
	delete this->jumpstatsService;
	delete this->languageService;
	delete this->databaseService;
	delete this->quietService;
	delete this->hudService;
	delete this->invisibleService;
	delete this->specService;
	delete this->timerService;
	delete this->optionService;
	delete this->paintService;
	delete this->pracService;
	delete this->noclipService;
	delete this->tipService;
	delete this->telemetryService;
	delete this->triggerService;
	delete this->recordingService;
	delete this->racingService;
	delete this->savedRunService;
	delete this->globalService;
	delete this->measureService;
	delete this->zonesService;
	delete this->profileService;
	delete this->pistolService;
	delete this->fovService;
	delete this->ztopwatchService;

	this->anticheatService = new KZAnticheatService(this);
	this->beamService = new KZBeamService(this);
	this->checkpointService = new KZCheckpointService(this);
	this->jumpstatsService = new KZJumpstatsService(this);
	this->databaseService = new KZDatabaseService(this);
	this->languageService = new KZLanguageService(this);
	this->noclipService = new KZNoclipService(this);
	this->quietService = new KZQuietService(this);
	this->hudService = new KZHUDService(this);
	this->invisibleService = new KZInvisibleService(this);
	this->specService = new KZSpecService(this);
	this->gotoService = new KZGotoService(this);
	this->timerService = new KZTimerService(this);
	this->optionService = new KZOptionService(this);
	this->paintService = new KZPaintService(this);
	this->pracService = new KZPracService(this);
	this->tipService = new KZTipService(this);
	this->telemetryService = new KZTelemetryService(this);
	this->triggerService = new KZTriggerService(this);
	this->recordingService = new KZRecordingService(this);
	this->racingService = new KZRacingService(this);
	this->savedRunService = new KZSavedRunService(this);
	this->globalService = new KZGlobalService(this);
	this->measureService = new KZMeasureService(this);
	this->zonesService = new KZZonesService(this);
	this->profileService = new KZProfileService(this);
	this->pistolService = new KZPistolService(this);
	this->fovService = new KZFOVService(this);
	this->ztopwatchService = new KZZtopwatchService(this);

	KZ::mode::InitModeService(this);
}

void KZPlayer::Reset()
{
	MovementPlayer::Reset();

	// Reset services that should not persist across player sessions.
	this->anticheatService->Reset();
	this->languageService->Reset();
	this->tipService->Reset();
	this->modeService->Reset();
	this->optionService->Reset();
	this->checkpointService->Reset();
	this->pracService->Reset();
	this->noclipService->Reset();
	this->quietService->Reset();
	this->invisibleService->Reset();
	this->jumpstatsService->Reset();
	this->hudService->Reset();
	this->timerService->Reset();
	this->specService->Reset();
	this->triggerService->Reset();
	this->measureService->Reset();
	this->zonesService->Reset();
	this->beamService->Reset();
	this->telemetryService->Reset();
	this->recordingService->Reset();
	this->paintService->Reset();
	this->profileService->Reset();
	this->ztopwatchService->Reset();
	this->savedRunService->Reset();

	g_pKZModeManager->SwitchToMode(this, KZOptionService::GetOptionStr("defaultMode", KZ_DEFAULT_MODE), true, true, false);
	g_pKZStyleManager->ClearStyles(this, true, false);
	CSplitString styles(KZOptionService::GetOptionStr("defaultStyles"), ",");
	FOR_EACH_VEC(styles, i)
	{
		g_pKZStyleManager->AddStyle(this, styles[i]);
	}
}

void KZPlayer::OnPlayerConnect(u64 steamID64)
{
	this->languageService->OnPlayerConnect(steamID64);
	// Невидимка помечается по xuid уже здесь: CheckTransmit не должен успеть отдать pawn.
	this->invisibleService->OnPlayerConnect(steamID64);
}

void KZPlayer::OnPlayerActive()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	// Mode/Styles stuff must be here for convars to be properly replicated.
	g_pKZModeManager->SwitchToMode(this, this->modeService->GetModeName(), true, true, false);
	g_pKZStyleManager->RefreshStyles(this, false);
	// Основной путь моста GG1 на коннекте: клиент стал active (signon FULL, контроллер
	// PlayerConnected) — раньше эмитить некуда (OnAuthorized обычно приходит во время
	// загрузки карты и скипается гейтом). Force-свитч выше тоже эмитит — дубль
	// безвреден (та же команда), но на его цепочку ранних выходов не полагаемся.
	this->profileService->EmitGG1Bridge();
	this->profileService->OnPlayerActive();

	this->optionService->OnPlayerActive();
	this->recordingService->EnsureCircularRecorderInitialized();
	this->invisibleService->OnPlayerActive();

	// Страховка доставки контент-ассета (particle-MHUD/звуки/jumpstats) на тяжёлых картах:
	// клиент уже active (карта скачана) — досылаем ассет, если MAM его ещё не догрузил.
	// Ботов пропускаем (фейк-клиенты аддоны не качают). Внутри EnsureClientAsset безвредно,
	// если ассет уже у клиента (no-op).
	if (!this->IsFakeClient())
	{
		g_KZPlugin.EnsureClientAsset(this->GetSteamId64(false));
	}

	// Серверный шум в чате на коннекте отключён (cyber): бета-предупреждение
	// ("Beta Join Warning") и ссылка на сайт ("Tip - website") не относятся
	// к нашему проекту и убраны как лишний спам.
}

void KZPlayer::OnPlayerFullyConnect()
{
	this->anticheatService->OnPlayerFullyConnect();
}

void KZPlayer::OnAuthorized()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer::OnAuthorized();
	this->databaseService->SetupClient();
	this->profileService->timeToNextRatingRefresh = 0.0f; // Force immediate refresh
	// Мост GG1 — путь ПОЗДНЕЙ аутентификации: Steam подтвердил тикет уже после входа
	// в игру (эмит с OnPlayerActive ушёл со steamId64=0 и был пропущен). Обычный ранний
	// auth приходит, пока клиент ещё качает карту (signon < FULL), — тогда этот вызов
	// скипается гейтом IsInGame внутри эмита, а мост уходит из OnPlayerActive.
	this->profileService->EmitGG1Bridge();
	this->globalService->OnPlayerAuthorized();
}

void KZPlayer::OnPhysicsSimulate()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer::OnPhysicsSimulate();
	this->recordingService->OnPhysicsSimulate();
	this->triggerService->OnPhysicsSimulate();
	this->modeService->OnPhysicsSimulate();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnPhysicsSimulate();
	}
	this->hudService->OnPhysicsSimulate();
	this->noclipService->HandleMoveCollision();
	this->EnableGodMode();
	this->UpdatePlayerModelAlpha();
	this->fovService->OnPhysicsSimulate();
	KZ::replaysystem::OnPhysicsSimulate(this);
}

void KZPlayer::OnPhysicsSimulatePost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer::OnPhysicsSimulatePost();
	this->anticheatService->OnPhysicsSimulatePost();
	this->recordingService->OnPhysicsSimulatePost();
	this->triggerService->OnPhysicsSimulatePost();
	this->telemetryService->OnPhysicsSimulatePost();
	this->modeService->OnPhysicsSimulatePost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnPhysicsSimulatePost();
	}
	this->timerService->OnPhysicsSimulatePost();
	// prac-часы тикают сразу за таймерным хуком: тот же тик, тот же шаг, и оба уже посчитаны
	// к DrawPanels ниже (иначе худ показывал бы время на тик позади).
	this->pracService->OnPhysicsSimulatePost();
	KZ::replaysystem::OnPhysicsSimulatePost(this);
	if (this->specService->GetSpectatedPlayer())
	{
		KZHUDService::DrawPanels(this->specService->GetSpectatedPlayer(), this);
	}
	else if (this->IsAlive())
	{
		KZHUDService::DrawPanels(this, this);
	}
	else
	{
		// Dead and not spectating anyone (death cam on own corpse / freeroam):
		// destroy particles here, otherwise a frozen MHUD stays up until the next DrawPanels.
		// Нижнюю панель и минимал-худ тоже гасим одноразово: DrawPanels для этого состояния
		// не вызывается, и остаток CP/TP висел бы до самозатухания канала.
		this->hudService->DestroyAllParticles();
		this->hudService->ClearBottomPanel();
		this->hudService->ClearMinimalHud();
	}
	this->measureService->OnPhysicsSimulatePost();
	this->quietService->OnPhysicsSimulatePost();
	this->profileService->OnPhysicsSimulatePost();
}

void KZPlayer::OnProcessUsercmds(PlayerCommand *cmds, int numcmds)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->recordingService->OnProcessUsercmds(cmds, numcmds);
	// this->anticheatService->OnProcessUsercmds(cmds, numcmds);
	this->modeService->OnProcessUsercmds(cmds, numcmds);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnProcessUsercmds(cmds, numcmds);
	}
}

void KZPlayer::OnProcessUsercmdsPost(PlayerCommand *cmds, int numcmds)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnProcessUsercmdsPost(cmds, numcmds);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnProcessUsercmdsPost(cmds, numcmds);
	}
}

void KZPlayer::OnSetupMove(PlayerCommand *pc)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->anticheatService->OnSetupMove(pc);
	this->recordingService->OnSetupMove(pc);
	this->modeService->OnSetupMove(pc);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnSetupMove(pc);
	}
}

void KZPlayer::OnSetupMovePost(PlayerCommand *pc)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnSetupMovePost(pc);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnSetupMovePost(pc);
	}
}

void KZPlayer::OnProcessMovement()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	MovementPlayer::OnProcessMovement();

	KZ::mode::ApplyModeSettings(this);
	KZ::replaysystem::OnProcessMovement(this);

	this->DisableTurnbinds();
	this->anticheatService->OnProcessMovement();
	this->hudService->OnProcessMovement();
	this->triggerService->OnProcessMovement();
	this->modeService->OnProcessMovement();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnProcessMovement();
	}

	this->jumpstatsService->OnProcessMovement();
	this->checkpointService->TpHoldPlayerStill();
}

void KZPlayer::OnProcessMovementPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");

	this->anticheatService->OnProcessMovementPost();
	this->jumpstatsService->UpdateJump();
	this->modeService->OnProcessMovementPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnProcessMovementPost();
	}
	this->jumpstatsService->OnProcessMovementPost();
	this->triggerService->OnProcessMovementPost();
	KZ::replaysystem::OnProcessMovementPost(this);
	this->ztopwatchService->OnProcessMovementPost();
	this->hudService->OnProcessMovementPost();
	MovementPlayer::OnProcessMovementPost();
}

void KZPlayer::OnPlayerMove()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnPlayerMove();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnPlayerMove();
	}
}

void KZPlayer::OnPlayerMovePost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnPlayerMovePost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnPlayerMovePost();
	}
}

void KZPlayer::OnCheckParameters()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckParameters();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckParameters();
	}
}

void KZPlayer::OnCheckParametersPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckParametersPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckParametersPost();
	}
}

void KZPlayer::OnCanMove()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCanMove();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCanMove();
	}
}

void KZPlayer::OnCanMovePost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCanMovePost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCanMovePost();
	}
}

void KZPlayer::OnFullWalkMove(bool &ground)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnFullWalkMove(ground);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnFullWalkMove(ground);
	}
}

void KZPlayer::OnFullWalkMovePost(bool ground)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnFullWalkMovePost(ground);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnFullWalkMovePost(ground);
	}
}

void KZPlayer::OnMoveInit()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnMoveInit();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnMoveInit();
	}
}

void KZPlayer::OnMoveInitPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnMoveInitPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnMoveInitPost();
	}
}

void KZPlayer::OnCheckWater()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckWater();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckWater();
	}
}

void KZPlayer::OnWaterMove()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnWaterMove();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnWaterMove();
	}
}

void KZPlayer::OnWaterMovePost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnWaterMovePost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnWaterMovePost();
	}
}

void KZPlayer::OnCheckWaterPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckWaterPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckWaterPost();
	}
}

void KZPlayer::OnCheckVelocity(const char *a3)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckVelocity(a3);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckVelocity(a3);
	}
}

void KZPlayer::OnCheckVelocityPost(const char *a3)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckVelocityPost(a3);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckVelocityPost(a3);
	}
}

void KZPlayer::OnDuck()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnDuck();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnDuck();
	}
}

void KZPlayer::OnDuckPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnDuckPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnDuckPost();
	}
}

void KZPlayer::OnCanUnduck()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCanUnduck();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCanUnduck();
	}
}

void KZPlayer::OnCanUnduckPost(bool &ret)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCanUnduckPost(ret);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCanUnduckPost(ret);
	}
}

void KZPlayer::OnLadderMove()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnLadderMove();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnLadderMove();
	}
}

void KZPlayer::OnLadderMovePost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnLadderMovePost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnLadderMovePost();
	}
}

void KZPlayer::OnCheckJumpButtonLegacy()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckJumpButtonLegacy();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckJumpButtonLegacy();
	}
	this->triggerService->OnCheckJumpButton();
}

void KZPlayer::OnCheckJumpButtonLegacyPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckJumpButtonLegacyPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckJumpButtonLegacyPost();
	}
}

void KZPlayer::OnCheckJumpButtonModern()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckJumpButtonModern();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckJumpButtonModern();
	}
	this->triggerService->OnCheckJumpButton();
	this->hudService->OnJump(true);
}

void KZPlayer::OnCheckJumpButtonModernPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckJumpButtonModernPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckJumpButtonModernPost();
	}
}

void KZPlayer::OnJumpLegacy()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->telemetryService->OnJumpLegacy();
	this->modeService->OnJumpLegacy();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnJumpLegacy();
	}
	this->hudService->OnJump();
	this->ztopwatchService->OnJump();
}

void KZPlayer::OnJumpLegacyPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->telemetryService->OnJumpLegacyPost();
	this->modeService->OnJumpLegacyPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnJumpLegacyPost();
	}
}

void KZPlayer::OnJumpModern()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->telemetryService->OnJumpModern();
	this->modeService->OnJumpModern();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnJumpModern();
	}
	this->ztopwatchService->OnJump();
}

void KZPlayer::OnJumpModernPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->telemetryService->OnJumpModernPost();
	this->modeService->OnJumpModernPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnJumpModernPost();
	}
}

void KZPlayer::OnAirMove()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->anticheatService->OnAirMove();
	this->modeService->OnAirMove();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnAirMove();
	}
}

void KZPlayer::OnAirMovePost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnAirMovePost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnAirMovePost();
	}
}

void KZPlayer::OnAirAccelerate(Vector &wishdir, f32 &wishspeed, f32 &accel)
{
	this->modeService->OnAirAccelerate(wishdir, wishspeed, accel);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnAirAccelerate(wishdir, wishspeed, accel);
	}
	this->jumpstatsService->OnAirAccelerate();
}

void KZPlayer::OnAirAcceleratePost(Vector wishdir, f32 wishspeed, f32 accel)
{
	this->modeService->OnAirAcceleratePost(wishdir, wishspeed, accel);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnAirAcceleratePost(wishdir, wishspeed, accel);
	}
	this->jumpstatsService->OnAirAcceleratePost(wishdir, wishspeed, accel);
}

void KZPlayer::OnFriction()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnFriction();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnFriction();
	}
}

void KZPlayer::OnFrictionPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnFrictionPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnFrictionPost();
	}
}

void KZPlayer::OnWalkMove()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnWalkMove();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnWalkMove();
	}
}

void KZPlayer::OnWalkMovePost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnWalkMovePost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnWalkMovePost();
	}
}

void KZPlayer::OnTryPlayerMove(Vector *pFirstDest, trace_t *pFirstTrace, bool *bIsSurfing)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnTryPlayerMove(pFirstDest, pFirstTrace, bIsSurfing);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnTryPlayerMove(pFirstDest, pFirstTrace, bIsSurfing);
	}
	this->jumpstatsService->OnTryPlayerMove();
}

void KZPlayer::OnTryPlayerMovePost(Vector *pFirstDest, trace_t *pFirstTrace, bool *bIsSurfing)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnTryPlayerMovePost(pFirstDest, pFirstTrace, bIsSurfing);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnTryPlayerMovePost(pFirstDest, pFirstTrace, bIsSurfing);
	}
	this->jumpstatsService->OnTryPlayerMovePost();
}

void KZPlayer::OnCategorizePosition(bool bStayOnGround)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCategorizePosition(bStayOnGround);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCategorizePosition(bStayOnGround);
	}
}

void KZPlayer::OnCategorizePositionPost(bool bStayOnGround)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCategorizePositionPost(bStayOnGround);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCategorizePositionPost(bStayOnGround);
	}
}

void KZPlayer::OnFinishGravity()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnFinishGravity();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnFinishGravity();
	}
}

void KZPlayer::OnFinishGravityPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnFinishGravityPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnFinishGravityPost();
	}
}

void KZPlayer::OnCheckFalling()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckFalling();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckFalling();
	}
}

void KZPlayer::OnCheckFallingPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnCheckFallingPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnCheckFallingPost();
	}
}

void KZPlayer::OnPostPlayerMove()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnPostPlayerMove();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnPostPlayerMove();
	}
}

void KZPlayer::OnPostPlayerMovePost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnPostPlayerMovePost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnPostPlayerMovePost();
	}
}

void KZPlayer::OnPostThink()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnPostThink();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnPostThink();
	}
	MovementPlayer::OnPostThink();
}

void KZPlayer::OnPostThinkPost()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->modeService->OnPostThinkPost();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnPostThinkPost();
	}
}

void KZPlayer::OnStartTouchGround()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->anticheatService->CreateLandEvent();
	this->jumpstatsService->EndJump();
	this->timerService->OnStartTouchGround();
	this->modeService->OnStartTouchGround();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnStartTouchGround();
	}
}

void KZPlayer::OnStopTouchGround()
{
	VPROF_BUDGET(__func__, "CS2KZ");
	if (!inPerf && !this->GetCvarValueFromModeStyles(MODECVAR_SV_LEGACY_JUMP)->m_bValue)
	{
		f32 landingTick = this->GetMoveServices()->m_ModernJump().m_nLastLandedTick() + this->GetMoveServices()->m_ModernJump().m_flLastLandedFrac();
		f32 windowMin = landingTick - this->GetCvarValueFromModeStyles(MODECVAR_SV_BHOP_TIME_WINDOW)->m_fl32Value * 0.5f * ENGINE_FIXED_TICK_RATE;
		f32 windowMax = landingTick + this->GetCvarValueFromModeStyles(MODECVAR_SV_BHOP_TIME_WINDOW)->m_fl32Value * 0.5f * ENGINE_FIXED_TICK_RATE;
		f32 startTime = this->currentMoveData->m_flSubtickStartFraction + this->currentMoveData->m_nTickCount;

		this->inPerf = (startTime >= windowMin && startTime <= windowMax) && this->jumped;
	}
	this->timerService->OnStopTouchGround();
	this->modeService->OnStopTouchGround();
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnStopTouchGround();
	}
	this->jumpstatsService->AddJump();
	this->triggerService->OnStopTouchGround();
	this->hudService->OnStopTouchGround();
}

void KZPlayer::OnChangeMoveType(MoveType_t oldMoveType)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->anticheatService->OnChangeMoveType(oldMoveType);
	this->jumpstatsService->OnChangeMoveType(oldMoveType);
	this->timerService->OnChangeMoveType(oldMoveType);
	this->modeService->OnChangeMoveType(oldMoveType);
	FOR_EACH_VEC(this->styleServices, i)
	{
		this->styleServices[i]->OnChangeMoveType(oldMoveType);
	}
}

void KZPlayer::OnTeleport(const Vector *origin, const QAngle *angles, const Vector *velocity)
{
	VPROF_BUDGET(__func__, "CS2KZ");
	this->lastTeleportTime = g_pKZUtils->GetServerGlobals()->curtime;
	this->jumpstatsService->HandleTeleport();
	this->modeService->OnTeleport(origin, angles, velocity);
	this->timerService->OnTeleport(origin, angles, velocity);
	this->recordingService->OnTeleport(origin, angles, velocity);
	if (origin)
	{
		this->beamService->OnTeleport();
	}
	this->triggerService->OnTeleport();
}

void KZPlayer::DisableTurnbinds()
{
	CCSPlayerPawn *pawn = this->GetPlayerPawn();
	if (!pawn)
	{
		return;
	}

	// holding both doesn't do anything. xor(1, 1) == 0
	bool usingTurnbinds = (this->IsButtonPressed(IN_TURNLEFT) ^ this->IsButtonPressed(IN_TURNRIGHT));
	QAngle angles;
	this->GetAngles(&angles);
	// Turn-бинды разрешены (cyber, решение пользователя 15.08): гасим только при
	// kz_allow_turnbinds 0. Угол запоминаем в ОБОИХ случаях — иначе после переключения
	// cvar на живом сервере первый же кадр с зажатым биндом швырнёт игрока на yaw,
	// протухший с момента последнего «разрешённого» кадра.
	// Интерлок: в глобальном режиме бинды подавляем всегда, чем бы ни был выставлен cvar.
	// Раны оттуда уходят в глобальную БД cs2kz (timer/submission.cpp SubmitGlobal), где
	// turn-бинды запрещены; сейчас мы non-global и это спит, но появись ключ — мы бы молча
	// погнали туда раны на биндах.
	bool turnbindsAllowed = kz_allow_turnbinds.Get() && !KZGlobalService::IsAvailable();
	if (usingTurnbinds && !turnbindsAllowed)
	{
		angles.y = this->lastValidYaw;
		// NOTE(GameChaos): Using SetAngles, which uses Teleport makes player movement really weird
		g_pKZUtils->SnapViewAngles(pawn, angles);
		// Уведомление "Turnbinds Disabled" в чат убрано (cyber): сама блокировка
		// turnbind'ов сохранена, но текстовый спам — лишний серверный шум.
	}
	else
	{
		this->lastValidYaw = angles.y;
	}
	this->oldUsingTurnbinds = usingTurnbinds;
}

void KZPlayer::EnableGodMode()
{
	CCSPlayerPawn *pawn = this->GetPlayerPawn();
	if (!pawn)
	{
		return;
	}
	if (pawn->m_bTakesDamage())
	{
		pawn->m_bTakesDamage(false);
	}
}

void KZPlayer::UpdatePlayerModelAlpha()
{
	CCSPlayerPawn *pawn = this->GetPlayerPawn();
	if (!pawn)
	{
		return;
	}
	Color ogColor = pawn->m_clrRender();
	// Дефолт скрытия ног — ВКЛ: свежий игрок без сохранённого выбора заходит со скрытыми ногами
	// (мешают обзору вниз в KZ). Кто хочет — вернёт через !hidelegs. Заданный выбор не трогается.
	bool hideLegs = this->optionService->GetPreferenceBool("hideLegs", true);
	if (hideLegs && pawn->m_clrRender().a() == 255)
	{
		pawn->m_clrRender(Color(255, 255, 255, 254));
	}
	else if (!hideLegs && pawn->m_clrRender().a() != 255)
	{
		pawn->m_clrRender(Color(255, 255, 255, 255));
	}
}

bool KZPlayer::JustTeleported(f32 threshold)
{
	return g_pKZUtils->GetServerGlobals()->curtime - this->lastTeleportTime < threshold;
}

void KZPlayer::ToggleHideLegs()
{
	// Дефолт true (как в apply-сайте): первый !hidelegs у свежего игрока ПОКАЗЫВАЕТ ноги.
	this->optionService->SetPreferenceBool("hideLegs", !this->optionService->GetPreferenceBool("hideLegs", true));
}

void KZPlayer::PlayErrorSound()
{
	utils::PlaySoundToClient(this->GetPlayerSlot(), MV_SND_ERROR);
}

void KZPlayer::TouchTriggersAlongPath(const Vector &start, const Vector &end, const bbox_t &bounds)
{
	this->triggerService->TouchTriggersAlongPath(start, end, bounds);
}

void KZPlayer::UpdateTriggerTouchList()
{
	this->triggerService->UpdateTriggerTouchList();
}

void KZPlayer::OnChangeTeamPost(i32 team)
{
	// Наш собственный перевод СПЕК→NONE (скрытие невидимки) — не второй уход в наблюдатели:
	// повторный OnPausePost сдвинул бы кулдаун паузы внутри бегущего рана.
	if (this->invisibleService->OnChangeTeamPost(team))
	{
		return;
	}
	// Подменять здесь CS_TEAM_NONE на CS_TEAM_SPECTATOR НЕЛЬЗЯ, хотя невидимка наблюдает
	// именно из NONE: вход в наблюдатели уже отработал шагом раньше — KZ::misc::JoinTeam
	// сперва делает ChangeTeam(SPECTATOR), и пауза встаёт на нём, а наш перевод SPEC→NONE
	// проглатывается строкой выше. Осталась бы только ложь: Hook_ClientDisconnect зовёт
	// SwitchTeam(0) на КАЖДОМ выходе, и подмена ставила бы паузу и дёргала prac на уже
	// сохранённом ране уходящего игрока.
	this->timerService->OnPlayerJoinTeam(team);
	if (team == CS_TEAM_SPECTATOR)
	{
		// Здесь, а не в обёртке KZ::misc::JoinTeam: это движковый хук ChangeTeam, он ловит и
		// админ-форсы/движковые свитчи, у которых стек prac-точек иначе переживал бы уход в спек.
		this->pracService->OnJoinSpectator();
	}
}

const CVValue_t *KZPlayer::GetCvarValueFromModeStyles(KzModeCvars cvar)
{
	const char *name = KZ::mode::modeCvarNames[cvar];
	FOR_EACH_VEC_BACK(this->styleServices, i)
	{
		if (this->styleServices[i]->GetTweakedConvarValue(name))
		{
			return this->styleServices[i]->GetTweakedConvarValue(name);
		}
	}
	return &this->modeService->GetModeConVarValues()[cvar];
}
