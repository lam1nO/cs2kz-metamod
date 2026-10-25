#include "cs2kz.h"
#include "kz/kz.h"
#include "kz_replaysystem.h"
#include "bot.h"
#include "data.h"
#include "playback.h"
#include "events.h"
#include "commands.h"
#include "session.h"
#include "cyb_replay_download.h"
#include "kz/spec/kz_spec.h"
#include "kz/timer/kz_timer.h"

namespace KZ::replaysystem::session
{
	static thread_local int current = 0;
	static CHandle<CCSPlayerController> owners[Count];
	static uint64_t generations[Count] {};
	static ProgressAnchor anchors[Count];
	static bool captured[Count] {};

	int Current()
	{
		return current;
	}

	uint64_t Generation()
	{
		return generations[current];
	}

	int ForPlayer(KZPlayer *player)
	{
		if (!player || !player->GetController())
		{
			return 0;
		}
		const int botIndex = bot::SessionForBot(player->GetController());
		if (botIndex)
		{
			return botIndex;
		}
		if (player->IsFakeClient())
		{
			return 0;
		}
		const int slot = player->GetPlayerSlot().Get();
		return slot >= 0 && slot <= MAXPLAYERS ? slot + 1 : 0;
	}

	Scope::Scope(int index) : previous(current)
	{
		current = index >= 0 && index < Count ? index : 0;
	}

	Scope::Scope(KZPlayer *player) : Scope(ForPlayer(player)) {}

	Scope::~Scope()
	{
		current = previous;
	}

	void CaptureProgress(KZPlayer *player)
	{
		Scope scope(player);
		Bind(player);
		if (!Owns(player))
		{
			return;
		}
		auto &anchor = anchors[current];
		anchor = {};
		anchor.active = player->timerService->GetTimerRunning();
		player->GetOrigin(&anchor.origin);
		const auto *course = player->timerService->GetCourse();
		if (course)
		{
			anchor.course = course->GetName().Get();
		}
		captured[current] = true;
	}

	ProgressAnchor TakeProgress(KZPlayer *player)
	{
		if (!captured[current])
		{
			CaptureProgress(player);
		}
		captured[current] = false;
		return anchors[current];
	}

	void Release()
	{
		++generations[current];
		anchors[current] = {};
		captured[current] = false;
		data::CancelAsyncLoad();
		bot::KickBot();
		playback::ClearPauseSegments();
		data::FreeReplayData(data::GetCurrentReplay());
		data::FreeReplayData(&data::GetLoadStatus()->completedReplay);
		auto status = data::GetLoadStatus();
		status->successCallback = {};
		status->failureCallback = {};
		status->state = data::LoadingState::Idle;
		CybReplayDownload::ClearPending();
		owners[current].Term();
	}

	void Bind(KZPlayer *player)
	{
		if (!current || !player)
		{
			return;
		}
		if (owners[current] != player->GetController())
		{
			Release();
			owners[current] = player->GetController();
		}
	}

	bool Owns(KZPlayer *player)
	{
		return player && current && owners[current] == player->GetController();
	}

	bool CanControlObserved(KZPlayer *player)
	{
		Scope scope(player);
		if (!Owns(player) || !data::IsReplayPlaying())
		{
			return false;
		}
		auto target = player->specService->GetSpectatedPlayer();
		return target && target->GetController() == bot::GetBot();
	}

	void ProcessFrames()
	{
		for (int i = 1; i < Count; ++i)
		{
			Scope scope(i);
			auto controller = owners[i].Get();
			auto owner = controller ? g_pKZPlayerManager->ToPlayer(controller) : nullptr;
			if (!owner || !owner->IsConnected())
			{
				if (data::IsReplayValid() || data::IsLoading() || owners[i].IsValid() || bot::GetBot())
				{
					Release();
				}
				continue;
			}
			data::ProcessAsyncLoadCompletion();
		}
	}
} // namespace KZ::replaysystem::session

namespace KZ::replaysystem
{

	void Init()
	{
		KZ::replaysystem::item::InitItemAttributes();
	}

	void Cleanup()
	{
		for (int i = 0; i < session::Count; ++i)
		{
			session::Scope scope(i);
			session::Release();
		}
		CleanupWatcher();
	}

	void OnRoundStart()
	{
		for (int i = 0; i < session::Count; ++i)
		{
			session::Scope scope(i);
			session::Release();
		}
	}

	void OnGameFrame()
	{
		session::ProcessFrames();
	}

	void OnPhysicsSimulate(KZPlayer *player)
	{
		playback::OnPhysicsSimulate(player);
	}

	void OnProcessMovement(KZPlayer *player)
	{
		playback::OnProcessMovement(player);
	}

	void OnProcessMovementPost(KZPlayer *player)
	{
		playback::OnProcessMovementPost(player);
	}

	void OnFinishMovePre(KZPlayer *player, CMoveData *pMoveData)
	{
		playback::OnFinishMovePre(player, pMoveData);
	}

	void OnPhysicsSimulatePost(KZPlayer *player)
	{
		playback::OnPhysicsSimulatePost(player);
	}

	void OnPlayerRunCommandPre(KZPlayer *player, PlayerCommand *command)
	{
		playback::OnPlayerRunCommandPre(player, command);
	}

	bool IsReplayBot(KZPlayer *player)
	{
		return bot::IsValidBot(player ? player->GetController() : nullptr);
	}

	bool CanTouchTrigger(KZPlayer *player, CBaseTrigger *trigger)
	{
		// Don't care about non-bot players.
		if (!bot::IsValidBot(player->GetController()))
		{
			return true;
		}

		// Don't care about non timer triggers.
		const KzTrigger *kzTrigger = KZ::mapapi::GetKzTrigger(trigger);
		if (!kzTrigger)
		{
			return true;
		}

		if (KZ::mapapi::IsTimerTrigger(kzTrigger->type) || KZ::mapapi::IsTeleportTrigger(kzTrigger->type))
		{
			return false;
		}

		return true;
	}

	i32 GetCurrentCpIndex()
	{
		return data::GetCurrentCpIndex();
	}

	i32 GetCheckpointCount()
	{
		return data::GetCheckpointCount();
	}

	i32 GetTeleportCount()
	{
		return data::GetTeleportCount();
	}

	bool IsAwrMode()
	{
		return data::IsAwrMode();
	}

	f32 GetTime()
	{
		return data::GetReplayTime();
	}

	f32 GetEndTime()
	{
		return data::GetEndTime();
	}

	bool GetPaused()
	{
		return data::GetPaused();
	}

} // namespace KZ::replaysystem
