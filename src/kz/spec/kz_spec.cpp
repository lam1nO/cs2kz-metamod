#include "kz_spec.h"
#include "kz_spec_menu.h"
#include "../timer/kz_timer.h"
#include "kz/invisible/kz_invisible.h"
#include "kz/language/kz_language.h"
#include "utils/simplecmds.h"
#include "utils/ctimer.h"

static_global class KZTimerServiceEventListener_Spec : public KZTimerServiceEventListener
{
	virtual void OnTimerStartPost(KZPlayer *player, u32 courseGUID) override;
} timerEventListener;

void KZSpecService::Reset()
{
	this->ResetSavedPosition();
}

void KZSpecService::Init()
{
	KZTimerService::RegisterEventListener(&timerEventListener);
}

bool KZSpecService::HasSavedPosition()
{
	return this->savedPosition;
}

void KZSpecService::SavePosition()
{
	this->player->GetOrigin(&this->savedOrigin);
	this->player->GetAngles(&this->savedAngles);
	this->savedOnLadder = this->player->GetMoveType() == MOVETYPE_LADDER;
	this->savedPosition = true;
}

void KZSpecService::LoadPosition()
{
	if (!this->HasSavedPosition())
	{
		return;
	}
	this->player->Teleport(&this->savedOrigin, &this->savedAngles, nullptr);
	if (this->savedOnLadder)
	{
		this->player->SetMoveType(MOVETYPE_LADDER);
	}
}

void KZSpecService::ResetSavedPosition()
{
	this->savedOrigin = vec3_origin;
	this->savedAngles = vec3_angle;
	this->savedOnLadder = false;
	this->savedPosition = false;
}

bool KZSpecService::IsSpectating(KZPlayer *target)
{
	return this->GetSpectatedPlayer() == target;
}

i32 KZSpecService::CollectSpectateCandidates(const char *query, KZPlayer **candidates, i32 maxCandidates)
{
	KZPlayer *exact[MAXPLAYERS + 1] = {};
	KZPlayer *sub[MAXPLAYERS + 1] = {};
	i32 exactCount = 0;
	i32 subCount = 0;
	for (i32 i = 0; i <= MAXPLAYERS; i++)
	{
		KZPlayer *other = g_pKZPlayerManager->ToPlayer(i);
		if (!other || other == this->player || !other->GetController())
		{
			continue;
		}
		if (other->GetController()->IsObserverTeam())
		{
			continue;
		}
		if (KZ_STREQI(other->GetName(), query))
		{
			exact[exactCount++] = other;
		}
		else if (V_stristr(other->GetName(), query))
		{
			sub[subCount++] = other;
		}
	}
	KZPlayer **src = exactCount > 0 ? exact : sub;
	i32 total = exactCount > 0 ? exactCount : subCount;
	for (i32 i = 0; i < total && i < maxCandidates; i++)
	{
		candidates[i] = src[i];
	}
	return total;
}

bool KZSpecService::SpectatePlayer(const char *playerName)
{
	if (KZ_STREQI(playerName, "@me"))
	{
		if (!this->player->IsAlive())
		{
			this->player->languageService->PrintChat(true, false, "Spectate Failure (Dead)");
			return false;
		}
		return this->SpectatePlayer(this->player);
	}

	KZPlayer *candidates[KZ_SPEC_MENU_MAX_ITEMS] = {};
	i32 total = this->CollectSpectateCandidates(playerName, candidates, KZ_SPEC_MENU_MAX_ITEMS);
	if (total == 0)
	{
		this->player->languageService->PrintChat(true, false, "Spectate Failure (Player Not Found)", playerName);
		return false;
	}
	if (total == 1)
	{
		return this->SpectatePlayer(candidates[0]);
	}
	// Неоднозначная подстрока — меню выбора (внутри фолбэк, если cs2menus не загружен).
	KZ::spec::OpenSpectateMenu(this->player, candidates, MIN(total, KZ_SPEC_MENU_MAX_ITEMS), total);
	return true;
}

static_function f64 TeleportObserver(CPlayerUserId userID, Vector origin, QAngle angles)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
	if (player && player->GetObserverPawn())
	{
		player->GetObserverPawn()->Teleport(&origin, &angles, nullptr);
	}
	return 0.0f;
};

bool KZSpecService::SpectatePlayer(KZPlayer *target)
{
	if (!target || !this->CanSpectate())
	{
		return false;
	}
	// Join spectator team if not already in it.
	// Здесь вопрос не «он наблюдатель?», а «надо ли уводить его в наблюдатели», поэтому
	// IsObserverTeam() не годится: обычный игрок, ещё не выбравший команду, тоже сидит в
	// CS_TEAM_NONE, и пропуск входа оставил бы его неопределившимся — без паузы, без
	// гашения худа и под движковым mp_force_pick_time. Предикат тот же, что в
	// KZ::misc::JoinTeam: в NONE «уже наблюдает» только спрятанный нами невидимка.
	CCSPlayerController *controller = this->player->GetController();
	bool alreadyObserving =
		controller->GetTeam() == CS_TEAM_SPECTATOR || (controller->GetTeam() == CS_TEAM_NONE && this->player->invisibleService->IsObserving());
	if (!alreadyObserving)
	{
		KZ::misc::JoinTeam(this->player, CS_TEAM_SPECTATOR, true);
	}

	// Обсервер-pawn проверяем на null: на переходных кадрах смены команды его может не
	// быть, а разыменование сырого хендла роняло бы сервер (было до этой правки).
	CCSPlayerPawnBase *observerPawn = player->GetController()->GetObserverPawn();
	CPlayer_ObserverServices *obsService = observerPawn ? observerPawn->m_pObserverServices : nullptr;
	if (!obsService)
	{
		player->languageService->PrintChat(true, false, "Spectate Failure (Generic)");
		return false;
	}

	obsService->m_iObserverMode(OBS_MODE_IN_EYE);
	obsService->m_iObserverLastMode(OBS_MODE_NONE);
	obsService->m_hObserverTarget(target->GetPlayerPawn());

	if (target == this->player)
	{
		controller->m_DesiredObserverMode(OBS_MODE_ROAMING);
		obsService->m_iObserverMode(OBS_MODE_ROAMING);
		controller->m_hDesiredObserverTarget(target->GetPlayerPawn());
		obsService->m_hObserverTarget(target->GetPlayerPawn());
		Vector origin;
		QAngle angles;
		this->player->GetEyeOrigin(&origin);
		this->player->GetAngles(&angles);
		StartTimer<CPlayerUserId, Vector, QAngle>(TeleportObserver, player->GetClient()->GetUserID(), std::move(origin), std::move(angles), 0.0f,
												  false, false);
	}
	return true;
}

bool KZSpecService::CanSpectate()
{
	return !this->player->IsAlive() || this->player->timerService->GetPaused() || this->player->timerService->CanPause();
}

void KZSpecService::GetSpectatorList(CUtlVector<CUtlString> &spectatorList, KZPlayer *viewer)
{
	KZPlayer *spectator = this->player->specService->GetNextSpectator(nullptr);
	while (spectator)
	{
		// Невидимого зрителя в списке видят только он сам и другие невидимки.
		// Сам GetNextSpectator не фильтруем: это ещё и итератор ДОСТАВКИ сообщений
		// зрителям (kz_timer, language, replays) — невидимка должен их получать.
		if (!KZInvisibleService::ShouldHideFrom(spectator, viewer))
		{
			spectatorList.AddToTail(spectator->GetName());
		}
		spectator = this->player->specService->GetNextSpectator(spectator);
	}
}

KZPlayer *KZSpecService::GetSpectatedPlayer()
{
	if (!player || player->IsAlive())
	{
		return NULL;
	}
	if (!player->GetController() || !player->GetController()->m_hObserverPawn())
	{
		return NULL;
	}
	CPlayer_ObserverServices *obsService = player->GetController()->m_hObserverPawn()->m_pObserverServices;
	if (!obsService)
	{
		return NULL;
	}
	if (!obsService->m_hObserverTarget().IsValid())
	{
		return NULL;
	}
	CCSPlayerPawn *pawn = this->player->GetPlayerPawn();
	CBasePlayerPawn *target = (CBasePlayerPawn *)obsService->m_hObserverTarget().Get();
	// If the player is spectating their own corpse, consider that as not spectating anyone.
	return target == pawn ? nullptr : g_pKZPlayerManager->ToPlayer(target);
}

KZPlayer *KZSpecService::GetNextSpectator(KZPlayer *current)
{
	for (int i = current ? current->index + 1 : 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (player->specService->IsSpectating(this->player))
		{
			return player;
		}
	}
	return nullptr;
}

void KZTimerServiceEventListener_Spec::OnTimerStartPost(KZPlayer *player, u32 courseGUID)
{
	player->specService->Reset();
}

SCMD(kz_spec, SCFL_SPEC | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}

	// !spec <подстрока ника> — спек по совпадению (точное имя приоритетно).
	if (args->ArgC() >= 2)
	{
		if (!player->specService->CanSpectate())
		{
			player->languageService->PrintChat(true, false, "Spectate Failure (Generic)");
			return MRES_SUPERCEDE;
		}
		player->specService->SpectatePlayer(args->Arg(1));
		return MRES_SUPERCEDE;
	}

	// !spec без аргументов — тоггл: спектатор возвращается в игру на сохранённое место.
	if (player->GetController() && player->GetController()->IsObserverTeam())
	{
		if (player->specService->HasSavedPosition())
		{
			KZ::misc::JoinTeam(player, CS_TEAM_CT, true);
		}
		else
		{
			player->languageService->PrintChat(true, false, "Spec - No Saved Position");
		}
		return MRES_SUPERCEDE;
	}

	// Живой (или мёртвый вне спека) — свободная камера из своей точки.
	if (!player->specService->CanSpectate())
	{
		player->languageService->PrintChat(true, false, "Spectate Failure (Generic)");
		return MRES_SUPERCEDE;
	}
	player->specService->SpectatePlayer("@me");
	return MRES_SUPERCEDE;
}

SCMD(kz_specs, SCFL_SPEC)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	KZPlayer *targetPlayer = player->IsAlive() ? player : player->specService->GetSpectatedPlayer();

	if (!targetPlayer)
	{
		player->languageService->PrintChat(true, false, "Spectator List (None)");
		return MRES_SUPERCEDE;
	}
	CUtlVector<CUtlString> spectatorList;
	targetPlayer->specService->GetSpectatorList(spectatorList, player);
	if (spectatorList.Count() == 0)
	{
		if (targetPlayer == player)
		{
			player->languageService->PrintChat(true, false, "Spectator List (None)");
		}
		else
		{
			player->languageService->PrintChat(true, false, "Target Spectator List (None)", targetPlayer->GetName());
		}
	}
	else
	{
		CUtlString spectatorListString;
		for (i32 i = 0; i < spectatorList.Count(); i++)
		{
			spectatorListString += spectatorList[i];
			if (i != spectatorList.Count() - 1)
			{
				spectatorListString += ", ";
			}
		}
		if (targetPlayer == player)
		{
			player->languageService->PrintChat(true, false, "Spectator List", spectatorList.Count(), spectatorListString.Get());
		}
		else
		{
			player->languageService->PrintChat(true, false, "Target Spectator List", targetPlayer->GetName(), spectatorList.Count(),
											   spectatorListString.Get());
		}
	}
	return MRES_SUPERCEDE;
}

SCMD_LINK(kz_speclist, kz_specs);
