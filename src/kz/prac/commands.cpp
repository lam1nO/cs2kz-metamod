#include "kz_prac.h"
#include "utils/simplecmds.h"

SCMD(kz_prac, SCFL_TIMER | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->TogglePrac();
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_practice, kz_prac);

SCMD(kz_praccp, SCFL_CHECKPOINT | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->SetPoint();
	return MRES_SUPERCEDE;
}

SCMD(kz_practp, SCFL_CHECKPOINT | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->TpToPoint();
	return MRES_SUPERCEDE;
}

SCMD(kz_pracprev, SCFL_CHECKPOINT | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->TpToPrevPoint();
	return MRES_SUPERCEDE;
}

SCMD(kz_pracnext, SCFL_CHECKPOINT | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->TpToNextPoint();
	return MRES_SUPERCEDE;
}

SCMD(kz_pracreset, SCFL_CHECKPOINT | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->ResetPoints();
	return MRES_SUPERCEDE;
}
