#include "kz_prac.h"
#include "utils/simplecmds.h"

SCMD(kz_prac, SCFL_TIMER | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->TogglePrac();
	return true;
}

SCMD_LINK(kz_practice, kz_prac);

SCMD(kz_praccp, SCFL_CHECKPOINT | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->SetPoint();
	return true;
}

SCMD(kz_practp, SCFL_CHECKPOINT | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->TpToPoint();
	return true;
}

SCMD(kz_pracprev, SCFL_CHECKPOINT | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->TpToPrevPoint();
	return true;
}

SCMD(kz_pracnext, SCFL_CHECKPOINT | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->TpToNextPoint();
	return true;
}

SCMD(kz_pracreset, SCFL_CHECKPOINT | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->ResetPoints();
	return true;
}
