#include "kz_prac.h"
#include "utils/simplecmds.h"

SCMD(kz_prac, SCFL_TIMER | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->pracService->TogglePrac();
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_practice, kz_prac);
