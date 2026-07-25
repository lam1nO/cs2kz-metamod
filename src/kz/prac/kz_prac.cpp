#include "kz_prac.h"

#include "kz/language/kz_language.h"
#include "kz/noclip/kz_noclip.h"
#include "kz/savedrun/kz_savedrun.h"
#include "utils/utils.h"

CConVar<bool> kz_prac_enable("kz_prac_enable", FCVAR_NONE, "Whether the !prac practice mode is available to players.", true);
CConVar<i32> kz_prac_run_policy("kz_prac_run_policy", FCVAR_NONE,
								"What happens to a run that went through !prac: 0 = becomes NUB (counted), 1 = not counted at all.", 0);

void KZPracService::Reset()
{
	this->inPrac = false;
	this->frozen = {};
	this->ClearPoints();
}

void KZPracService::ClearPoints()
{
	this->points.RemoveAll();
	this->currentIndex = 0;
}

void KZPracService::TogglePrac()
{
	if (this->inPrac)
	{
		this->ExitPrac();
	}
	else
	{
		this->EnterPrac();
	}
}

void KZPracService::EnterPrac()
{
	if (!kz_prac_enable.Get())
	{
		this->player->languageService->PrintChat(true, false, "Prac - Disabled");
		this->player->PlayErrorSound();
		return;
	}
	if (!this->player->IsAlive())
	{
		this->player->languageService->PrintChat(true, false, "Prac - Must Be Alive");
		this->player->PlayErrorSound();
		return;
	}

	const bool timerRunning = this->player->timerService->GetTimerRunning();
	if (timerRunning)
	{
		// !pro существует ровно для «не дай мне сломать PRO», а prac ломает его как телепорт.
		if (!this->player->timerService->CheckSafeguardPro())
		{
			return;
		}
		// Гарды входа = гарды паузы (не в воздухе, не сразу после приземления,
		// не в antipause-зоне, не под кулдауном). Сообщения печатает сам CanPause.
		if (!this->player->timerService->CanPause(true))
		{
			return;
		}

		// Публичного геттера GUID у таймера нет, но есть GetCourse() -> дескриптор (kz_timer.h:409).
		const KZCourseDescriptor *courseDesc = this->player->timerService->GetCourse();
		if (!courseDesc)
		{
			this->player->languageService->PrintChat(true, false, "No Active Course");
			this->player->PlayErrorSound();
			return;
		}

		this->frozen = {};
		this->frozen.active = true;
		this->frozen.courseGUID = courseDesc->guid;
		this->frozen.timer = this->player->timerService->SnapshotForSave();
		const CUtlVector<KZCheckpointService::Checkpoint> &cps = this->player->checkpointService->GetCheckpointsForSave();
		FOR_EACH_VEC(cps, i)
		{
			this->frozen.checkpoints.AddToTail(cps[i]);
		}
		this->frozen.cpIndex = this->player->checkpointService->GetRawCpIndex();
		// Штраф печём в снапшот СРАЗУ, а не при возврате: тогда и возврат через !prac, и
		// восстановление после дисконнекта (Task 7) дают одинаковый результат без дублей логики.
		// +1 телепорт = ран становится NUB.
		this->frozen.tpCount = this->player->checkpointService->GetTeleportCount() + 1;
		if (kz_prac_run_policy.Get() == 1)
		{
			// Политика «не засчитывать»: ран восстановится уже невалидным.
			this->frozen.timer.valid = false;
		}
		this->player->GetOrigin(&this->frozen.origin);
		this->player->GetAngles(&this->frozen.angles);

		this->player->timerService->TimerStop(false);
	}
	else
	{
		this->frozen = {};
	}

	// Порядок важен: inPrac до включения ноуклипа, иначе HandleNoclip успеет
	// сработать по «карательной» ветке.
	this->inPrac = true;
	this->ClearPoints();
	this->player->noclipService->EnableNoclip();
	this->player->noclipService->HandleNoclip();

	if (this->frozen.active)
	{
		this->player->languageService->PrintChat(true, false, "Prac - Enter With Run");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Prac - Enter Free");
	}
}

void KZPracService::ExitPrac()
{
	this->player->noclipService->DisableNoclip();
	this->player->noclipService->HandleNoclip();

	if (!this->frozen.active)
	{
		this->inPrac = false;
		this->ClearPoints();
		this->player->languageService->PrintChat(true, false, "Prac - Exit Free");
		return;
	}

	// inPrac снимаем ДО восстановления: RestoreFromSnapshot поднимает timerRunning,
	// и дальше листенер OnTimerStart уже не должен ничего вето́ровать.
	this->inPrac = false;

	this->player->timerService->RestoreFromSnapshot(this->frozen.courseGUID, this->frozen.timer);
	// tpCount в снапшоте уже со штрафом (+1, см. EnterPrac) — здесь только применяем.
	// SetTeleportCountForRestore не нужен: счётчик бампает KZCheckpointService::DoTeleport,
	// а мы телепортируем напрямую через KZPlayer::Teleport, который его не трогает.
	this->player->checkpointService->RestoreFromSnapshot(this->frozen.checkpoints, this->frozen.cpIndex, this->frozen.tpCount);
	this->player->Teleport(&this->frozen.origin, &this->frozen.angles, &vec3_origin);
	this->player->timerService->ForcePause();

	this->frozen = {};
	this->ClearPoints();
	this->player->languageService->PrintChat(true, false, "Prac - Exit To Run");
}

void KZPracService::DropFrozenRun(const char *reason)
{
	if (!this->inPrac && !this->frozen.active)
	{
		return;
	}
	const bool hadRun = this->frozen.active;
	this->inPrac = false;
	this->frozen = {};
	this->ClearPoints();
	if (hadRun && reason)
	{
		this->player->languageService->PrintChat(true, false, "Prac - Run Lost");
		KZ_LOG_DEBUG(LogChannel::Timer, "[prac] frozen run dropped for %s: %s\n", this->player->GetName(), reason);
	}
}

bool KZPracService::RequirePrac()
{
	if (!this->inPrac)
	{
		this->player->languageService->PrintChat(true, false, "Prac - Only In Prac");
		this->player->PlayErrorSound();
		return false;
	}
	return true;
}

bool KZPracService::RequirePracPoint()
{
	if (!this->RequirePrac())
	{
		return false;
	}
	if (this->points.Count() == 0)
	{
		this->player->languageService->PrintChat(true, false, "Prac - No Points");
		this->player->PlayErrorSound();
		return false;
	}
	return true;
}

void KZPracService::SetPoint()
{
	if (!this->RequirePrac())
	{
		return;
	}
	CCSPlayerPawn *pawn = this->player->GetPlayerPawn();
	if (!pawn)
	{
		return;
	}

	PracPoint pt = {};
	this->player->GetOrigin(&pt.origin);
	this->player->GetVelocity(&pt.velocity);
	this->player->GetAngles(&pt.angles);
	pt.onGround = (pawn->m_fFlags() & FL_ONGROUND) != 0;
	CCSPlayer_MovementServices *ms = this->player->GetMoveServices();
	if (ms)
	{
		pt.duckAmount = ms->m_flDuckAmount;
		pt.stamina = ms->m_flStamina;
		pt.ladderNormal = ms->m_vecLadderNormal();
		pt.onLadder = pawn->m_MoveType() == MOVETYPE_LADDER;
	}

	this->points.AddToTail(pt);
	this->currentIndex = this->points.Count() - 1;
	this->player->languageService->PrintChat(true, false, "Prac - Point Set", this->points.Count());
	this->player->checkpointService->PlayCheckpointSound();
}

void KZPracService::DoTpToPoint(const PracPoint &pt)
{
	CCSPlayerPawn *pawn = this->player->GetPlayerPawn();
	if (!pawn || !pawn->IsAlive())
	{
		return;
	}

	// Ноуклип снимаем всегда: смысл practp — продолжить движение по-настоящему.
	this->player->noclipService->DisableNoclip();
	this->player->noclipService->HandleNoclip();

	// В отличие от обычного чекпоинта передаём НЕнулевую скорость — это вся суть prac-точки.
	this->player->Teleport(&pt.origin, &pt.angles, &pt.velocity);

	CCSPlayer_MovementServices *ms = this->player->GetMoveServices();
	if (ms)
	{
		ms->m_flDuckAmount(pt.duckAmount);
		ms->m_flStamina(pt.stamina);
		if (pt.onLadder)
		{
			ms->m_vecLadderNormal(pt.ladderNormal);
			this->player->SetMoveType(MOVETYPE_LADDER);
		}
		else
		{
			ms->m_vecLadderNormal(vec3_origin);
			this->player->SetMoveType(MOVETYPE_WALK);
		}
	}
	if (pt.onGround)
	{
		pawn->m_fFlags(pawn->m_fFlags() | FL_ONGROUND);
	}
	this->player->checkpointService->PlayTeleportSound();
}

void KZPracService::TpToPoint()
{
	if (!this->RequirePracPoint())
	{
		return;
	}
	this->DoTpToPoint(this->points[this->currentIndex]);
}

void KZPracService::TpToPrevPoint()
{
	if (!this->RequirePracPoint())
	{
		return;
	}
	this->currentIndex = MAX(0, this->currentIndex - 1);
	this->DoTpToPoint(this->points[this->currentIndex]);
}

void KZPracService::TpToNextPoint()
{
	if (!this->RequirePracPoint())
	{
		return;
	}
	this->currentIndex = MIN(this->points.Count() - 1, this->currentIndex + 1);
	this->DoTpToPoint(this->points[this->currentIndex]);
}

void KZPracService::ResetPoints()
{
	if (!this->RequirePrac())
	{
		return;
	}
	this->ClearPoints();
	this->player->languageService->PrintChat(true, false, "Prac - Points Cleared");
	this->player->checkpointService->PlayCheckpointResetSound();
}
