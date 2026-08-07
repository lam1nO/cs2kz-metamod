#include "src/common.h"
#include "utils/utils.h"
#include "utils/ctimer.h"
#include "kz/kz.h"
#include "utils/simplecmds.h"

#include "kz/anticheat/kz_anticheat.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "kz/invisible/kz_invisible.h"
#include "kz/jumpstats/kz_jumpstats.h"
#include "kz/quiet/kz_quiet.h"
#include "kz/mode/kz_mode.h"
#include "kz/language/kz_language.h"
#include "kz/style/kz_style.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/noclip/kz_noclip.h"
#include "kz/hud/kz_hud.h"
#include "kz/option/kz_option.h"
#include "kz/prac/kz_prac.h"
#include "kz/spec/kz_spec.h"
#include "kz/goto/kz_goto.h"
#include "kz/telemetry/kz_telemetry.h"
#include "kz/timer/kz_timer.h"
#include "kz/tip/kz_tip.h"
#include "kz/profile/kz_profile.h"
#include "kz/pistol/kz_pistol.h"
#include "kz/racing/kz_racing.h"

#include "sdk/gamerules.h"
#include "sdk/physicsgamesystem.h"
#include "sdk/entity/cbasetrigger.h"
#include "sdk/cskeletoninstance.h"

#define RESTART_CHECK_INTERVAL 1800.0f
static_global CTimer<> *mapRestartTimer;

SCMD(kz_hidelegs, SCFL_PLAYER | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->ToggleHideLegs();
	if (player->optionService->GetPreferenceBool("hideLegs"))
	{
		player->languageService->PrintChat(true, false, "Quiet Option - Hide Player Legs - Enable");
	}
	else
	{
		player->languageService->PrintChat(true, false, "Quiet Option - Hide Player Legs - Disable");
	}
	return MRES_SUPERCEDE;
}

SCMD(kz_hide, SCFL_PLAYER | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->quietService->ToggleHide();
	if (player->quietService->hideOtherPlayers)
	{
		player->languageService->PrintChat(true, false, "Quiet Option - Show Players - Disable");
	}
	else
	{
		player->languageService->PrintChat(true, false, "Quiet Option - Show Players - Enable");
	}
	return MRES_SUPERCEDE;
}

SCMD(kz_end, SCFL_MAP | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);

	// В prac по карте двигаемся только по prac-точкам: !end — такой же телепорт мимо них,
	// как и !r, просто в другой конец курса.
	if (player->pracService->RejectMapTeleport("teleport_to_end"))
	{
		return MRES_SUPERCEDE;
	}

	// If the player specify a course name, we first check if it's valid or not.
	if (V_strlen(args->ArgS()) > 0)
	{
		const KZCourseDescriptor *course = KZ::course::GetCourse(args->ArgS(), false, true);

		if (!course || !course || !course->hasEndPosition)
		{
			player->languageService->PrintChat(true, false, "No End Position For Course", args->ArgS());
			return MRES_SUPERCEDE;
		}
	}

	bool shouldTeleport = false;
	Vector tpOrigin;
	QAngle tpAngles;
	if (player->timerService->GetCourse())
	{
		if (player->timerService->GetCourse()->hasEndPosition)
		{
			tpOrigin = player->timerService->GetCourse()->endPosition;
			tpAngles = player->timerService->GetCourse()->endAngles;
			shouldTeleport = true;
		}
		else
		{
			CUtlString courseName = player->timerService->GetCourse()->GetName();
			player->languageService->PrintChat(true, false, "No End Position For Course", courseName.Get());
			return MRES_SUPERCEDE;
		}
	}

	// If we have no active course the map only has one course, !r should send the player to the end of that course.
	else if (KZ::course::GetCourseCount() == 1)
	{
		const KZCourseDescriptor *descriptor = KZ::course::GetFirstCourse();
		if (descriptor->hasEndPosition)
		{
			tpOrigin = descriptor->endPosition;
			tpAngles = descriptor->endAngles;
			shouldTeleport = true;
		}
		else
		{
			CUtlString courseName = KZ::course::GetFirstCourse()->GetName();
			player->languageService->PrintChat(true, false, "No End Position For Course", courseName.Get());
		}
	}
	else
	{
		player->languageService->PrintChat(true, false, "No Active Course");
		return MRES_SUPERCEDE;
	}

	if (shouldTeleport)
	{
		if (!player->timerService->CheckSafeguard())
		{
			return MRES_SUPERCEDE;
		}
		player->timerService->TimerStop(true, "teleport_to_end");
		// Как и !r: телепорт к концу снимает паузу у живого игрока, иначе он
		// приезжает замороженным (MOVETYPE_NONE, gravity 0).
		if (player->timerService->GetPaused() && player->IsAlive())
		{
			player->timerService->Resume(true);
		}
		if (player->GetPlayerPawn()->IsAlive())
		{
			if (player->noclipService->IsNoclipping())
			{
				player->noclipService->DisableNoclip();
				player->noclipService->HandleNoclip();
			}
		}
		else
		{
			KZ::misc::JoinTeam(player, CS_TEAM_CT, false);
		}
		player->Teleport(&tpOrigin, &tpAngles, &vec3_origin);
	}
	return MRES_SUPERCEDE;
}

void KZ::misc::TeleportToCourse(KZPlayer *player, const KZCourseDescriptor *course)
{
	// Общая воронка всех рестартов: !r/!restart, !course <имя|номер>, !main, !b/!bonus/!b1..!b9
	// и выбор пункта в меню !courses. В prac рестарта нет ни одним из этих путей (решение
	// пользователя 07.08) — гард стоит здесь, а не в каждой команде, чтобы новый вызывающий
	// не появился мимо запрета. Отказ ДО CheckSafeguardRestart и DropFrozenRun ниже: иначе
	// запрещённая команда всё равно убила бы замороженный ран.
	if (player->pracService->RejectMapTeleport("teleport_to_start"))
	{
		return;
	}
	if (!player->timerService->CheckSafeguardRestart())
	{
		return;
	}
	// Та же причина, что уйдёт в run_stop из OnTeleportToStart ниже — одно действие игрока,
	// одно значение, две строки коррелируют.
	player->pracService->DropFrozenRun("teleport_to_start");

	// Рестарт снимает паузу — но только у живого игрока: у спектатора paused
	// выставлен всегда (OnPlayerJoinTeam), а Resume лезет в pawn/move services
	// без null-чеков. «Паузу» спектатора снимает OnPlayerSpawn после JoinTeam.
	if (player->timerService->GetPaused() && player->IsAlive())
	{
		player->timerService->Resume(true);
	}

	player->timerService->OnTeleportToStart();
	if (player->GetPlayerPawn()->IsAlive())
	{
		// Fix players spawning 500u under spawn positions.
		if (player->noclipService->IsNoclipping())
		{
			player->noclipService->DisableNoclip();
			player->noclipService->HandleNoclip();
		}
	}
	else
	{
		KZ::misc::JoinTeam(player, CS_TEAM_CT, false);
	}

	if (course)
	{
		player->Teleport(&course->startPosition, &course->startAngles, &vec3_origin);
		return;
	}

	// Then prioritize custom start position.
	if (player->checkpointService->HasCustomStartPosition())
	{
		player->checkpointService->TpToStartPosition();
		return;
	}

	// If we have no custom start position, we try to get the start position of the current course.
	if (player->timerService->GetCourse())
	{
		if (player->timerService->GetCourse()->hasStartPosition)
		{
			player->Teleport(&player->timerService->GetCourse()->startPosition, &player->timerService->GetCourse()->startAngles, &vec3_origin);
			return;
		}
	}

	// If we have no active course the map only has one course, !r should send the player to that course.
	if (KZ::course::GetCourseCount() == 1)
	{
		const KZCourseDescriptor *descriptor = KZ::course::GetFirstCourse();
		if (descriptor && descriptor->hasStartPosition)
		{
			player->Teleport(&descriptor->startPosition, &descriptor->startAngles, &vec3_origin);
			return;
		}
	}

	// If this fails, we just try to find any valid spawn.
	Vector spawnOrigin;
	QAngle spawnAngles;
	if (utils::FindValidSpawn(spawnOrigin, spawnAngles))
	{
		player->Teleport(&spawnOrigin, &spawnAngles, &vec3_origin);
		return;
	}

	// Attempt to just find any spawn at all, ignoring stuck checks.
	if (utils::FindValidSpawn(spawnOrigin, spawnAngles, true))
	{
		player->Teleport(&spawnOrigin, &spawnAngles, &vec3_origin);
		return;
	}

	// Last resort, just respawn the player.
	player->GetPlayerPawn()->Respawn();
	player->pistolService->UpdatePistol();
}

void KZ::misc::HandleTeleportToCourse(KZPlayer *player, const CCommand *args)
{
	const KZCourseDescriptor *startPosCourse = nullptr;
	// If the player specify a course name, we first check if it's valid or not.
	if (V_strlen(args->ArgS()) > 0)
	{
		CUtlString courseArg = args->ArgS();
		// Trim whitespace
		courseArg.Trim();
		if (utils::IsNumeric(courseArg.Get()))
		{
			i32 courseID = atoi(courseArg.Get());
			startPosCourse = KZ::course::GetCourseByCourseID(courseID);
		}
		else
		{
			startPosCourse = KZ::course::GetCourse(courseArg.Get(), false, true);
		}

		if (!startPosCourse || !startPosCourse->hasStartPosition)
		{
			player->languageService->PrintChat(true, false, "No Start Position For Course", courseArg.Get());
			return;
		}
	}
	KZ::misc::TeleportToCourse(player, startPosCourse);
}

SCMD(kz_restart, SCFL_TIMER | SCFL_MAP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	KZ::misc::HandleTeleportToCourse(player, args);
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_r, kz_restart, SCFL_HELP);

SCMD(kz_lj, SCFL_JUMPSTATS | SCFL_MAP | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);

	// Тот же запрет: телепорт в jumpstat-зону уводит игрока с отрабатываемого элемента.
	if (player->pracService->RejectMapTeleport("jumpstat_area"))
	{
		return MRES_SUPERCEDE;
	}

	Vector destPos;
	QAngle destAngles;
	if (g_pMappingApi->GetJumpstatArea(destPos, destAngles))
	{
		if (!player->timerService->CheckSafeguard())
		{
			return MRES_SUPERCEDE;
		}
		player->timerService->TimerStop(true, "jumpstat_area");
		player->Teleport(&destPos, &destAngles, &vec3_origin);
	}
	else
	{
		player->languageService->PrintChat(true, false, "No Jumpstat Area Found", args->ArgS());
	}

	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_ljarea, kz_lj);
SCMD_LINK(kz_jsarea, kz_lj);

SCMD(kz_playercheck, SCFL_PLAYER)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	KZPlayer *targetPlayer = nullptr;
	if (args->ArgC() < 2)
	{
		targetPlayer = player;
	}
	else
	{
		for (i32 i = 0; i <= MAXPLAYERS; i++)
		{
			CBasePlayerController *controller = g_pKZPlayerManager->players[i]->GetController();

			if (!controller)
			{
				continue;
			}

			if (V_strstr(V_strlower((char *)g_pKZPlayerManager->players[i]->GetName()), V_strlower((char *)args->ArgS())))
			{
				targetPlayer = g_pKZPlayerManager->ToPlayer(i);
				break;
			}
		}
	}
	if (!targetPlayer)
	{
		player->languageService->PrintChat(true, false, "Error Message (Player Not Found)", args->ArgS());
		return MRES_SUPERCEDE;
	}
	player->languageService->PrintChat(
		true, false, targetPlayer->IsAuthenticated() ? "Player Authenticated (Steam)" : "Player Not Authenticated (Steam)", targetPlayer->GetName());
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_pc, kz_playercheck);

static_function void CloseTeamMenu(KZPlayer *player)
{
	IGameEvent *event = interfaces::pGameEventManager->CreateEvent("entity_killed");
	if (event)
	{
		event->SetInt("entindex_killed", player->GetPlayerPawn()->entindex());
		IGameEventListener2 *listener = g_pKZUtils->GetLegacyGameEventListener(player->GetPlayerSlot());
		if (listener)
		{
			listener->FireGameEvent(event);
		}
		interfaces::pGameEventManager->FreeEvent(event);
	}
}

SCMD(jointeam, SCFL_HIDDEN)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	int newTeam = atoi(args->Arg(1));
	// Смена играющей команды живым игроком (меню выбора команды) — это CommitSuicide + Respawn +
	// телепорт на первый валидный спаун (см. JoinTeam ниже), то есть обход запрета рестарта в
	// prac. Уход в наблюдатели не трогаем: prac его переживает by design, а возврат приходит
	// через LoadPosition на то же место. Сверка с текущей командой — чтобы повторный выбор своей
	// же команды (JoinTeam для неё no-op) не давал ложного отказа.
	if (newTeam == CS_TEAM_CT || newTeam == CS_TEAM_T)
	{
		const int currentTeam = player->GetController() ? player->GetController()->GetTeam() : CS_TEAM_NONE;
		if (player->IsAlive() && newTeam != currentTeam && player->pracService->RejectMapTeleport("team_change"))
		{
			CloseTeamMenu(player);
			return MRES_SUPERCEDE;
		}
	}
	if (newTeam == CS_TEAM_SPECTATOR || newTeam == CS_TEAM_NONE)
	{
		if (!player->timerService->GetPaused() && !player->timerService->CanPause())
		{
			CloseTeamMenu(player);
			return MRES_SUPERCEDE;
		}
	}
	else if (player->IsAlive() && !player->timerService->CheckSafeguard())
	{
		CloseTeamMenu(player);
		return MRES_SUPERCEDE;
	}
	KZ::misc::JoinTeam(player, newTeam, true);
	return MRES_SUPERCEDE;
}

SCMD(switchhands, SCFL_HIDDEN)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	return MRES_IGNORED;
}

SCMD_LINK(switchhandsleft, switchhands);
SCMD_LINK(switchhandsright, switchhands);

static_function f64 CheckRestart()
{
	utils::ResetMapIfEmpty();
	return RESTART_CHECK_INTERVAL;
}

void KZ::misc::Init()
{
	KZ::misc::EnforceTimeLimit();
	mapRestartTimer = StartTimer(CheckRestart, RESTART_CHECK_INTERVAL, true, true);
	CConVarRef<int32> sv_infinite_ammo("sv_infinite_ammo");
	if (sv_infinite_ammo.IsValidRef() && sv_infinite_ammo.IsConVarDataAvailable())
	{
		sv_infinite_ammo.RemoveFlags(FCVAR_CHEAT);
	}
	CConVarRef<CUtlString> bot_stop("bot_stop");
	if (bot_stop.IsValidRef() && bot_stop.IsConVarDataAvailable())
	{
		bot_stop.RemoveFlags(FCVAR_CHEAT);
	}
}

// TODO: Fullupdate spectators on spec_mode/spec_next/spec_player/spec_prev

void KZ::misc::JoinTeam(KZPlayer *player, int newTeam, bool restorePos, bool savePos)
{
	int currentTeam = player->GetController()->GetTeam();

	// Ниже команда меняется движковыми вызовами (ChangeTeam/CommitSuicide/SwitchTeam), которые
	// могут поднять player_death как побочный эффект смены команды - OnPlayerDeath не должен
	// путать это с реальной смертью. Снимаем в конце функции - в ней нет ранних return; если
	// такой появится, снять флаг нужно и в нём.
	player->timerService->SetChangingTeam(true);

	// Don't use CS_TEAM_NONE
	if (newTeam == CS_TEAM_NONE)
	{
		newTeam = CS_TEAM_SPECTATOR;
	}

	// В CS_TEAM_NONE игрока уводит ТОЛЬКО KZInvisibleService (скрытие из TAB), и для него
	// это уже «в наблюдателях» — повторно прогонять вход не нужно. Спрашиваем именно
	// IsObserving, а не команду: только что зашедший игрок (в т.ч. невидимка) тоже сидит
	// в NONE, но вход в наблюдатели — пауза таймера, гашение худа — ему ещё предстоит.
	bool alreadyObserving = currentTeam == CS_TEAM_SPECTATOR || (currentTeam == CS_TEAM_NONE && player->invisibleService->IsObserving());

	if (newTeam == CS_TEAM_SPECTATOR && !alreadyObserving)
	{
		if (savePos && currentTeam != CS_TEAM_NONE)
		{
			player->specService->SavePosition();
		}

		if (!player->timerService->GetPaused() && !player->timerService->CanPause())
		{
			player->timerService->TimerStop(true, "pause_denied");
		}
		player->GetController()->ChangeTeam(CS_TEAM_SPECTATOR);
		// Невидимку в CS_TEAM_NONE уводит сторож следующим кадром, а НЕ мы здесь: наш
		// вызывающий (SpectatePlayer) сразу после этой функции читает observer pawn и
		// ставит цель наблюдения, а вторая смена команды в том же кадре может его
		// пересоздать — админ получал бы «Spectate Failure» вместо слежки за читером.
		// Цена — строка невидимки живёт в TAB один снапшот-тик.
		// Партикли мхуда гасим сразу: у обсервера движение не тикает и штатное
		// выключение в OnProcessMovement не сработает (жалоба: висят в спеках).
		player->hudService->OnJoinSpectator();
		// pracService->OnJoinSpectator() здесь НЕ зовём: он висит на движковом хуке
		// KZPlayer::OnChangeTeamPost, который ловит и смены команды мимо этой обёртки.
		player->quietService->SendFullUpdate();
		// TODO: put spectators of this player to freecam, and send them full updates
	}
	else if (newTeam == CS_TEAM_CT && currentTeam != CS_TEAM_CT || newTeam == CS_TEAM_T && currentTeam != CS_TEAM_T)
	{
		// Игрок сам попросился в игру: снимаем наблюдение до смены команды, иначе сторож
		// невидимки увидит живую команду при поднятом observing и вернёт его обратно.
		player->invisibleService->OnObserveEnd();
		if (player->GetPlayerPawn())
		{
			player->GetPlayerPawn()->CommitSuicide(false, true);
		}
		player->GetController()->SwitchTeam(newTeam);
		player->GetController()->Respawn();
		if (restorePos && player->specService->HasSavedPosition())
		{
			player->specService->LoadPosition();
		}
		else
		{
			player->timerService->TimerStop(true, "team_change");
			// Just joining a team alone can put you into weird invalid spawns.
			// Need to teleport the player to a valid one.
			Vector spawnOrigin {};
			QAngle spawnAngles {};
			if (utils::FindValidSpawn(spawnOrigin, spawnAngles))
			{
				auto pawn = player->GetPlayerPawn();
				player->Teleport(&spawnOrigin, &spawnAngles, &vec3_origin);
				// CS2 bug: m_flWaterJumpTime is not properly initialized upon player spawn.
				// If the player is teleported on the very first tick of movement and lose the ground flag,
				// the player might get teleported to a random place.
				pawn->m_pWaterServices()->m_flWaterJumpTime(0.0f);
			}
		}
		player->specService->ResetSavedPosition();
	}

	// The pistol service doesn't need to do anything if the player doesn't join a playing team.
	if (newTeam >= CS_TEAM_T)
	{
		player->pistolService->OnPlayerJoinTeam();
	}
	player->tipService->OnPlayerJoinTeam(newTeam);
	player->timerService->SetChangingTeam(false);
}

static_function void SanitizeMsg(const char *input, char *output, u32 size)
{
	int x = 0;
	for (int i = 0; input[i] != '\0'; i++)
	{
		if (x + 1 == size)
		{
			break;
		}

		int character = input[i];

		if (character > 0x10)
		{
			if (character == '\\')
			{
				output[x++] = character;
			}
			output[x++] = character;
		}
	}

	output[x] = '\0';
}

void KZ::misc::ProcessConCommand(ConCommandRef cmd, const CCommandContext &ctx, const CCommand &args)
{
	if (!GameEntitySystem())
	{
		return;
	}
	CPlayerSlot slot = ctx.GetPlayerSlot();

	CCSPlayerController *controller = (CCSPlayerController *)utils::GetController(slot);

	KZPlayer *player = NULL;

	if (!cmd.IsValidRef() || !controller || !(player = g_pKZPlayerManager->ToPlayer(controller)))
	{
		return;
	}
	const char *p {};
	const char *commandName = cmd.GetName();

	// Is it a chat message?
	if (!V_stricmp(commandName, "say") || !V_stricmp(commandName, "say_team"))
	{
		if (args.ArgC() < 2)
		{
			// no argument, happens when the player just types "say" or "say_team" in console
			return;
		}
		// 3 cases:
		// say ""
		// say_team ""
		// say /<insert text>
		i32 argLen = strlen(args.ArgS());
		p = args.ArgS();
		bool wrappedInQuotes = false;
		if (args.ArgS()[0] == '"' && args.ArgS()[argLen - 1] == '"')
		{
			argLen -= 2;
			wrappedInQuotes = true;
			p += 1;
		}
		if (argLen < 1 || args[1][0] == SCMD_CHAT_SILENT_TRIGGER || args[1][0] == SCMD_CHAT_TRIGGER)
		{
			// arg is too short, silent (/) command, or chat-trigger (!) command —
			// не ре-броадкастим: silent/! -команды скрыты у всех. Само `say` доезжает
			// до CSSharp (ProcessConCommand не суперсидит), так что !maps/!mcustom работают.
			return;
		}

		CUtlString message;
		message.SetDirect(p, strlen(p) - wrappedInQuotes);
		auto name = player->GetName();
		std::string text = message.Get();

		// We have to replace these CS2 quotes with normal quotes.
		size_t pos = 0;
		while ((pos = text.find("\xE2\x80\x8B", pos)) != std::string::npos)
		{
			text.replace(pos, 3, "\"");
		}

		std::string coloredPrefix = player->profileService->GetPrefix(true);
		std::string prefix = player->profileService->GetPrefix(false);
		std::string playerColor = player->anticheatService->isBanned ? "{grey}" : "{lime}";
		if (player->IsAlive())
		{
			utils::SayChat(player->GetController(), "%s %s%s{default}: %s", coloredPrefix.c_str(), playerColor.c_str(), name, text.c_str());
			utils::PrintConsoleAll("%s %s: %s", prefix.c_str(), name, text.c_str());
			KZ_LOG_INFO(LogChannel::General, "%s %s: %s\n", prefix.c_str(), name, text.c_str());
			player->racingService->SendChatMessage(text.c_str());
		}
		else
		{
			utils::SayChat(player->GetController(), "{grey}* %s %s%s{default}: %s", coloredPrefix.c_str(), playerColor.c_str(), name, text.c_str());
			utils::PrintConsoleAll("* %s %s: %s", prefix.c_str(), name, text.c_str());
			KZ_LOG_INFO(LogChannel::General, "* %s %s: %s\n", prefix.c_str(), name, text.c_str());
			player->racingService->SendChatMessage(text.c_str());
		}
	}

	return;
}

void KZ::misc::OnRoundStart()
{
	CCSGameRules *gameRules = g_pKZUtils->GetGameRules();
	if (gameRules)
	{
		gameRules->m_bGameRestart(true);
		gameRules->m_iRoundWinStatus(1);
		// Make sure that the round time is synchronized with the global time.
		gameRules->m_fRoundStartTime().SetTime(0.0f);
		gameRules->m_flGameStartTime().SetTime(0.0f);
	}
}

static_global bool clipsDrawn = false;
static_global bool triggersDrawn = false;

static_function void ResetOverlays()
{
	g_pKZUtils->ClearOverlays();
	clipsDrawn = false;
	triggersDrawn = false;
}

void OnDebugColorCvarChanged(CConVar<Color> *ref, CSplitScreenSlot nSlot, const Color *pNewValue, const Color *pOldValue)
{
	ResetOverlays();
}

// clang-format off
CConVar<bool> kz_showplayerclips("kz_showplayerclips", FCVAR_NONE, "Draw player clips (listen server only)", false,
	[](CConVar<bool> *ref, CSplitScreenSlot nSlot, const bool *bNewValue, const bool *bOldValue)
	{
		ResetOverlays();
	}
);

CConVar<bool> kz_showtriggers("kz_showtriggers", FCVAR_NONE, "Draw triggers (listen server only)", false,
	[](CConVar<bool> *ref, CSplitScreenSlot nSlot, const bool *bNewValue, const bool *bOldValue)
	{
		ResetOverlays();
	}
);

CConVar<Color> kz_playerclip_color("kz_playerclip_color", FCVAR_NONE, "Color of player clips (rgba) drawn by kz_toggleplayerclips.", Color(0x80, 0, 0x80, 0xFF), OnDebugColorCvarChanged);
CConVar<Color> kz_trigger_teleport_color("kz_trigger_teleport_color", FCVAR_NONE, "Color of trigger_teleport (rgba) drawn by kz_showtriggers.", Color(255, 0, 255, 0x80), OnDebugColorCvarChanged);
CConVar<Color> kz_trigger_multiple_colors[KZTRIGGER_COUNT] = 
{
	{"kz_trigger_multiple_color", FCVAR_NONE, "Color of trigger_multiple (rgba) drawn by kz_showtriggers.", Color(255, 199, 105, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_modifiers_color", FCVAR_NONE, "Color of Mapping API's Modifiers trigger (rgba) drawn by kz_showtriggers.", Color(255, 255, 128, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_reset_checkpoints_color", FCVAR_NONE, "Color of Mapping API's Reset Checkpoints trigger (rgba) drawn by kz_showtriggers.", Color(255, 140, 204, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_single_bhop_reset_color", FCVAR_NONE, "Color of Mapping API's Single bhop reset trigger (rgba) drawn by kz_showtriggers.", Color(140, 255, 212, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_anti_bhop_color", FCVAR_NONE, "Color of Mapping API's Anti Bhop trigger (rgba) drawn by kz_showtriggers.", Color(255, 64, 64, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_start_zone_color", FCVAR_NONE, "Color of Mapping API's Start Zone trigger (rgba) drawn by kz_showtriggers.", Color(0, 255, 0, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_end_zone_color", FCVAR_NONE, "Color of Mapping API's End Zone trigger (rgba) drawn by kz_showtriggers.", Color(255, 0, 0, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_split_zone_color", FCVAR_NONE, "Color of Mapping API's Split Zone trigger (rgba) drawn by kz_showtriggers.", Color(0, 255, 228, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_checkpoint_zone_color", FCVAR_NONE, "Color of Mapping API's Checkpoint Zone trigger (rgba) drawn by kz_showtriggers.", Color(219, 255, 0, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_stage_zone_color", FCVAR_NONE, "Color of Mapping API's Stage Zone trigger (rgba) drawn by kz_showtriggers.", Color(255, 157, 0, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_general_teleport_color", FCVAR_NONE, "Color of Mapping API's General Teleport trigger (rgba) drawn by kz_showtriggers.", Color(230, 117, 255, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_multi_bhop_color", FCVAR_NONE, "Color of Mapping API's Multi Bhop trigger (rgba) drawn by kz_showtriggers.", Color(79, 31, 255, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_single_bhop_color", FCVAR_NONE, "Color of Mapping API's Single Bhop trigger (rgba) drawn by kz_showtriggers.", Color(31, 107, 255, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_sequential_bhop_color", FCVAR_NONE, "Color of Mapping API's Sequential Bhop trigger (rgba) drawn by kz_showtriggers.", Color(196, 84, 214, 0x80), OnDebugColorCvarChanged},
	{"kz_trigger_mappingapi_push_color", FCVAR_NONE, "Color of Mapping API's Push trigger (rgba) drawn by kz_showtriggers.", Color(155, 255, 0, 0x80), OnDebugColorCvarChanged},
};

// clang-format on

static_function void DrawClipMeshes(CPhysicsGameSystem *gs)
{
	if (clipsDrawn)
	{
		return;
	}
	const Color playerClipColor = kz_playerclip_color.Get();

	FOR_EACH_MAP(gs->m_PhysicsSpawnGroups, groupIndex)
	{
		CPhysicsGameSystem::PhysicsSpawnGroups_t &group = gs->m_PhysicsSpawnGroups[groupIndex];
		CPhysAggregateInstance *instance = group.m_pLevelAggregateInstance;
		if (!instance)
		{
			continue;
		}

		CPhysAggregateData *aggregateData = instance->aggregateData;
		if (!aggregateData)
		{
			KZ_LOG_DEBUG(LogChannel::Misc, "PhysicsSpawnGroup %i: No aggregate data found for instance %p\n", groupIndex, instance);
			continue;
		}

		FOR_EACH_VEC(aggregateData->m_Parts, partIndex)
		{
			const VPhysXBodyPart_t *part = aggregateData->m_Parts[partIndex];
			if (!part)
			{
				continue;
			}

			clipsDrawn = true;
			FOR_EACH_VEC(part->m_rnShape.m_hulls, hullIndex)
			{
				const RnHullDesc_t &hull = part->m_rnShape.m_hulls[hullIndex];
				const RnCollisionAttr_t &collisionAttr = aggregateData->m_CollisionAttributes[hull.m_nCollisionAttributeIndex];
				if (collisionAttr.HasInteractsAsLayer(LAYER_INDEX_CONTENTS_PLAYER_CLIP))
				{
					CTransform transform;
					transform.SetToIdentity();
					Ray_t ray;
					ray.Init(hull.m_Hull.m_Bounds.m_vMinBounds, hull.m_Hull.m_Bounds.m_vMaxBounds, hull.m_Hull.m_VertexPositions.Base(),
							 hull.m_Hull.m_VertexPositions.Count());
					g_pKZUtils->DebugDrawMesh(transform, ray, playerClipColor.r(), playerClipColor.g(), playerClipColor.b(), playerClipColor.a(),
											  true, false, -1.0f);
				}
			}

			FOR_EACH_VEC(part->m_rnShape.m_meshes, meshIndex)
			{
				const RnMeshDesc_t &mesh = part->m_rnShape.m_meshes[meshIndex];
				const RnCollisionAttr_t &collisionAttr = aggregateData->m_CollisionAttributes[mesh.m_nCollisionAttributeIndex];
				if (collisionAttr.HasInteractsAsLayer(LAYER_INDEX_CONTENTS_PLAYER_CLIP))
				{
					FOR_EACH_VEC(mesh.m_Mesh.m_Triangles, triangleIndex)
					{
						const RnTriangle_t &triangle = mesh.m_Mesh.m_Triangles[triangleIndex];
						g_pKZUtils->AddTriangleOverlay(mesh.m_Mesh.m_Vertices[triangle.m_nIndex[0]], mesh.m_Mesh.m_Vertices[triangle.m_nIndex[1]],
													   mesh.m_Mesh.m_Vertices[triangle.m_nIndex[2]], playerClipColor.r(), playerClipColor.g(),
													   playerClipColor.b(), playerClipColor.a(), false, -1.0f);
					}
				}
			}
		}
	}
}

static_function void DrawTriggers()
{
	if (triggersDrawn || !GameEntitySystem())
	{
		return;
	}
	EntityInstanceIter_t iter;
	for (CEntityInstance *pEnt = iter.First(); pEnt; pEnt = iter.Next())
	{
		if (V_strstr(pEnt->GetClassname(), "trigger_"))
		{
			CBaseTrigger *pTrigger = static_cast<CBaseTrigger *>(pEnt);
			CSkeletonInstance *pSkeleton = static_cast<CSkeletonInstance *>(pTrigger->m_CBodyComponent()->m_pSceneNode());
			CPhysAggregateInstance *pPhysInstance = pSkeleton ? (CPhysAggregateInstance *)pSkeleton->m_modelState().m_pVPhysicsAggregate() : nullptr;
			const KzTrigger *kzTrigger = KZ::mapapi::GetKzTrigger(pTrigger);
			Color triggerColor;
			if (KZ_STREQI(pEnt->GetClassname(), "trigger_teleport"))
			{
				triggerColor = kz_trigger_teleport_color.Get();
			}
			else if (kzTrigger)
			{
				triggerColor = kz_trigger_multiple_colors[kzTrigger->type].Get();
			}
			else
			{
				triggerColor = kz_trigger_multiple_colors[0].Get();
			}
			auto *aggregateData = pPhysInstance ? pPhysInstance->aggregateData : nullptr;
			if (!aggregateData)
			{
				KZ_LOG_DEBUG(LogChannel::Misc, "Trigger %i: No aggregate data found for instance %p\n", pTrigger->entindex(), pPhysInstance);
				continue;
			}
			FOR_EACH_VEC(aggregateData->m_Parts, i)
			{
				const VPhysXBodyPart_t *part = aggregateData->m_Parts[i];
				if (!part)
				{
					continue;
				}
				triggersDrawn = true;
				FOR_EACH_VEC(part->m_rnShape.m_hulls, j)
				{
					const RnHullDesc_t &hull = part->m_rnShape.m_hulls[j];
					const RnCollisionAttr_t &collisionAttr = aggregateData->m_CollisionAttributes[hull.m_nCollisionAttributeIndex];

					CTransform transform;
					transform.SetToIdentity();
					transform.m_vPosition = pTrigger->m_CBodyComponent()->m_pSceneNode()->m_vecAbsOrigin();
					transform.m_orientation = Quaternion(pTrigger->m_CBodyComponent()->m_pSceneNode()->m_angAbsRotation());
					Ray_t ray;
					ray.Init(hull.m_Hull.m_Bounds.m_vMinBounds, hull.m_Hull.m_Bounds.m_vMaxBounds, hull.m_Hull.m_VertexPositions.Base(),
							 hull.m_Hull.m_Vertices.Count());
					g_pKZUtils->DebugDrawMesh(transform, ray, triggerColor.r(), triggerColor.g(), triggerColor.b(), triggerColor.a(), true, false,
											  -1.0f);
				}
				FOR_EACH_VEC(part->m_rnShape.m_meshes, j)
				{
					const RnMeshDesc_t &mesh = part->m_rnShape.m_meshes[j];
					const RnCollisionAttr_t &collisionAttr = aggregateData->m_CollisionAttributes[mesh.m_nCollisionAttributeIndex];
					CTransform transform;
					transform.SetToIdentity();
					transform.m_vPosition = pTrigger->m_CBodyComponent()->m_pSceneNode()->m_vecAbsOrigin();
					transform.m_orientation = Quaternion(pTrigger->m_CBodyComponent()->m_pSceneNode()->m_angAbsRotation());
					FOR_EACH_VEC(mesh.m_Mesh.m_Triangles, k)
					{
						const RnTriangle_t &triangle = mesh.m_Mesh.m_Triangles[k];
						Vector tri[3] = {utils::TransformPoint(transform, mesh.m_Mesh.m_Vertices[triangle.m_nIndex[0]]),
										 utils::TransformPoint(transform, mesh.m_Mesh.m_Vertices[triangle.m_nIndex[1]]),
										 utils::TransformPoint(transform, mesh.m_Mesh.m_Vertices[triangle.m_nIndex[2]])};
						g_pKZUtils->AddTriangleOverlay(tri[0], tri[1], tri[2], triggerColor.r(), triggerColor.g(), triggerColor.b(), triggerColor.a(),
													   false, -1.0f);
					}
				}
			}
		}
	}
}

void KZ::misc::OnPhysicsGameSystemFrameBoundary(void *pThis)
{
	KZ::misc::CheckTimeLimitOverride();
	static_persist CPhysicsGameSystem *physicsGameSystem = nullptr;
	// Map probably reloaded, mark clips as not drawn.
	if (pThis != physicsGameSystem)
	{
		physicsGameSystem = (CPhysicsGameSystem *)pThis;
		g_pKZUtils->ClearOverlays();
		clipsDrawn = false;
	}
	if (kz_showtriggers.Get())
	{
		DrawTriggers();
	}
	if (kz_showplayerclips.Get())
	{
		DrawClipMeshes(physicsGameSystem);
	}
}

void KZ::misc::OnActivateServer()
{
	KZ::misc::EnforceTimeLimit();
	g_pKZUtils->UpdateCurrentMapMD5();

	interfaces::pEngine->ServerCommand("exec cs2kz.cfg");
	KZ::misc::InitTimeLimit();

	// Restart round to ensure settings (e.g. mp_weapons_allow_map_placed) are applied
	interfaces::pEngine->ServerCommand("mp_restartgame 1");
	kz_showplayerclips.Set(false);
	kz_showtriggers.Set(false);
}
