#include "kz_noclip.h"

#include "kz/timer/kz_timer.h"
#include "kz/language/kz_language.h"
#include "kz/option/kz_option.h"
#include "kz/prac/kz_prac.h"
#include "kz/savedrun/kz_savedrun.h"

#include "utils/utils.h"
#include "utils/simplecmds.h"

#define FL_NOCLIP (1 << 3)

void KZNoclipService::Reset()
{
	this->lastNoclipTime = {};
	this->inNoclip = {};
}

void KZNoclipService::HandleNoclip()
{
	CCSPlayerPawn *pawn = this->player->GetPlayerPawn();
	// В prac таймер уже остановлен, а состояние рана держит KZPracService. Ноуклип
	// не должен ни стопить таймер, ни сносить запись SavedRuns — иначе замороженный
	// ран потеряется при дисконнекте.
	const bool suppressTimerKill = this->player->pracService->IsInPrac();
	if (this->inNoclip)
	{
		if ((pawn->m_fFlags() & FL_NOCLIP) == 0)
		{
			pawn->m_fFlags(pawn->m_fFlags() | FL_NOCLIP);
		}
		if (pawn->m_MoveType() != MOVETYPE_NOCLIP)
		{
			this->player->SetMoveType(MOVETYPE_NOCLIP);
			// Момент фактического включения ноуклипа. В prac он не убивает ран, но делает
			// недействительной попытку: prac-часы в 0 и стоп (сам метод no-op вне prac).
			// Висим здесь, а не на команде !nc, чтобы ноуклип из меню/бинда правило не обошёл.
			this->player->pracService->OnNoclipEnabled();
			// Инвалидация ДО TimerStop, только если таймер реально бежал - иначе InvalidateCurrent
			// не может резолвить курс (см. guard внутри) и это просто лишний no-op вызов.
			if (!suppressTimerKill)
			{
				if (this->player->timerService->GetTimerRunning())
				{
					this->player->savedRunService->InvalidateCurrent("noclip");
				}
				this->player->timerService->TimerStop(true, "noclip");
			}
		}
		// if (pawn->m_Collision().m_CollisionGroup() != KZ_COLLISION_GROUP_NOTRIGGER)
		// {
		// 	pawn->m_Collision().m_CollisionGroup() = KZ_COLLISION_GROUP_NOTRIGGER;
		// 	pawn->CollisionRulesChanged();
		// }
		this->lastNoclipTime = g_pKZUtils->GetServerGlobals()->curtime;
		if (!suppressTimerKill)
		{
			if (this->player->timerService->GetTimerRunning())
			{
				this->player->savedRunService->InvalidateCurrent("noclip");
			}
			this->player->timerService->TimerStop(true, "noclip");
		}
	}
	else
	{
		if ((pawn->m_fFlags() & FL_NOCLIP) != 0)
		{
			pawn->m_fFlags(pawn->m_fFlags() & ~FL_NOCLIP);
		}
		if (pawn->m_nActualMoveType() == MOVETYPE_NOCLIP)
		{
			this->player->SetMoveType(MOVETYPE_WALK);
			if (!suppressTimerKill)
			{
				if (this->player->timerService->GetTimerRunning())
				{
					this->player->savedRunService->InvalidateCurrent("noclip");
				}
				this->player->timerService->TimerStop(true, "noclip");
			}
		}
		if (pawn->m_Collision().m_CollisionGroup() != KZ_COLLISION_GROUP_STANDARD)
		{
			pawn->m_Collision().m_CollisionGroup() = KZ_COLLISION_GROUP_STANDARD;
			pawn->CollisionRulesChanged();
		}
	}
	if (pawn->m_nActualMoveType() == MOVETYPE_NOCLIP || pawn->m_MoveType() == MOVETYPE_NOCLIP)
	{
		if (!suppressTimerKill)
		{
			if (this->player->IsAlive() && this->player->timerService->GetTimerRunning())
			{
				this->player->savedRunService->InvalidateCurrent("noclip");
				this->player->timerService->TimerStop(true, "noclip");
			}
		}
	}
}

// Commands

SCMD(kz_noclip, SCFL_PLAYER)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player->noclipService->IsNoclipping() && !player->timerService->CheckSafeguard())
	{
		return MRES_SUPERCEDE;
	}
	player->noclipService->ToggleNoclip();
	if (player->noclipService->IsNoclipping())
	{
		player->languageService->PrintChat(true, false, "Noclip - Enable");
	}
	else
	{
		player->languageService->PrintChat(true, false, "Noclip - Disable");
	}
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_nc, kz_noclip);
SCMD_LINK(noclip, kz_noclip);

void KZNoclipService::HandleMoveCollision()
{
	CCSPlayerPawn *pawn = this->player->GetPlayerPawn();
	if (!pawn)
	{
		return;
	}
	if (pawn->m_lifeState() != LIFE_ALIVE)
	{
		this->DisableNoclip();
		return;
	}
	this->HandleNoclip();
}
