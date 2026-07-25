#include "kz_checkpoint.h"
#include "utils/simplecmds.h"
#include "kz/language/kz_language.h"
#include "kz/option/kz_option.h"
#include "kz/prac/kz_prac.h"

// В prac обычные чекпоинты не существуют: их состояние заморожено в снапшоте и будет
// восстановлено на выходе, поэтому трогать его нельзя. Команды !cp/!tp/!prev/!next
// перенаправляются на prac-стек — мышечная память игрока работает без новых биндов
// (решение пользователя 25.07). Перенаправление стоит ТОЛЬКО на уровне команд:
// внутренние вызывающие (восстановление SavedRuns через DoTeleport) не затронуты.

SCMD(kz_checkpoint, SCFL_CHECKPOINT)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (player->pracService->IsInPrac())
	{
		player->pracService->SetPoint();
		return MRES_SUPERCEDE;
	}
	player->checkpointService->SetCheckpoint();
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_cp, kz_checkpoint, SCFL_HELP);

SCMD(kz_teleport, SCFL_CHECKPOINT)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (player->pracService->IsInPrac())
	{
		player->pracService->TpToPoint();
		return MRES_SUPERCEDE;
	}
	player->checkpointService->TpToCheckpoint();
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_tp, kz_teleport, SCFL_HELP);

SCMD(kz_undo, SCFL_CHECKPOINT | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	// Отмены prac-телепорта нет, а буфер отмены принадлежит замороженному рану: применив его
	// в prac, игрок уехал бы в позицию ДО входа. Явный отказ вместо тихой порчи состояния.
	if (player->pracService->IsInPrac())
	{
		player->languageService->PrintChat(true, false, "Prac - No Undo");
		player->PlayErrorSound();
		return MRES_SUPERCEDE;
	}
	player->checkpointService->UndoTeleport();
	return MRES_SUPERCEDE;
}

SCMD(kz_prevcp, SCFL_CHECKPOINT)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (player->pracService->IsInPrac())
	{
		player->pracService->TpToPrevPoint();
		return MRES_SUPERCEDE;
	}
	player->checkpointService->TpToPrevCp();
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_pcp, kz_prevcp, SCFL_HELP);

SCMD(kz_nextcp, SCFL_CHECKPOINT)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (player->pracService->IsInPrac())
	{
		player->pracService->TpToNextPoint();
		return MRES_SUPERCEDE;
	}
	player->checkpointService->TpToNextCp();
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_ncp, kz_nextcp, SCFL_HELP);

SCMD(kz_setstartpos, SCFL_CHECKPOINT | SCFL_MAP | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->checkpointService->SetStartPosition();
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_ssp, kz_setstartpos, SCFL_HELP);

SCMD(kz_clearstartpos, SCFL_CHECKPOINT)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->checkpointService->ClearStartPosition();
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_csp, kz_clearstartpos, SCFL_HELP);

SCMD(kz_cpsound, SCFL_CHECKPOINT | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->optionService->SetPreferenceBool("checkpointSound", !player->optionService->GetPreferenceBool("checkpointSound", true));
	if (player->optionService->GetPreferenceBool("checkpointSound", true))
	{
		player->languageService->PrintChat(true, false, "Checkpoint Options - Checkpoint Sound - Enable");
	}
	else
	{
		player->languageService->PrintChat(true, false, "Checkpoint Options - Checkpoint Sound - Disable");
	}
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_checkpointsound, kz_cpsound);

SCMD(kz_tpsound, SCFL_CHECKPOINT | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->optionService->SetPreferenceBool("teleportSound", !player->optionService->GetPreferenceBool("teleportSound", true));
	if (player->optionService->GetPreferenceBool("teleportSound", true))
	{
		player->languageService->PrintChat(true, false, "Checkpoint Options - Teleport Sound - Enable");
	}
	else
	{
		player->languageService->PrintChat(true, false, "Checkpoint Options - Teleport Sound - Disable");
	}
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_teleportsound, kz_tpsound);

SCMD(kz_cpmessage, SCFL_CHECKPOINT | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	bool current = player->optionService->GetPreferenceBool("checkpointMessage", true);
	player->optionService->SetPreferenceBool("checkpointMessage", !current);
	if (!current)
	{
		player->languageService->PrintChat(true, false, "Checkpoint Options - Checkpoint Message - Enable");
	}
	else
	{
		player->languageService->PrintChat(true, false, "Checkpoint Options - Checkpoint Message - Disable");
	}
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_cpmsg, kz_cpmessage);
