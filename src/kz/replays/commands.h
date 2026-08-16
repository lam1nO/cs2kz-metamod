#ifndef KZ_REPLAYCOMMANDS_H
#define KZ_REPLAYCOMMANDS_H

#include "sdk/datatypes.h"

class KZPlayer;

namespace KZ::replaysystem::commands
{
	enum class RecordType
	{
		WR,
		WRPro,
		SR,
		SRPro,
		PB,
		PBPro,
		GPB,
		GPBPro,
		SPB,
		SPBPro
	};

	// Navigation functions
	void NavigateReplay(KZPlayer *player, u32 targetTick);

	// Command handlers
	void LoadReplay(KZPlayer *player, const char *uuid);
	void CheckReplayLoadProgress(KZPlayer *player);
	void CancelReplayLoad(KZPlayer *player);
	void JumpToReplayTime(KZPlayer *player, const char *input);
	void JumpToReplayTick(KZPlayer *player, const char *input);
	void GetReplayInfo(KZPlayer *player);
	void ToggleReplayPause(KZPlayer *player);
	// Скорость воспроизведения: сколько кадров записи проигрывается за серверный тик.
	// Кламп — [KZ_REPLAY_SPEED_MIN, KZ_REPLAY_SPEED_MAX]; announce=false для меню,
	// которое и так показывает значение в своей строке.
	void SetReplaySpeed(KZPlayer *player, f32 speed, bool announce = true);
	f32 GetReplaySpeed();
	// Скорость в текст без хвостовых нулей («0.25», «1», «1.75»), без суффикса «x».
	void FormatReplaySpeed(f32 speed, char *out, size_t size);
	// Шаг по тикам записи на стопкадре (frames < 0 — назад). Если реплей не на паузе,
	// ставит его на паузу: покадровый просмотр без стопкадра смысла не имеет.
	// announce=false для меню: cs2menus повторяет adjust на удержании клавиши, и строка
	// в чат на каждый тик залила бы чат.
	void StepReplay(KZPlayer *player, i32 frames, bool announce = true);
	// Штатно завершает воспроизведение: убирает бота и снимает флаг плейбека
	// (та же последовательность, что при естественном конце реплея).
	void StopReplay(KZPlayer *player);
	void ListReplays(KZPlayer *player, const char *input);
	void ToggleLegsVisibility(KZPlayer *player);
	void LoadReplayForRecord(KZPlayer *player, RecordType type, const char *courseArg, const char *modeArg);
} // namespace KZ::replaysystem::commands

#endif // KZ_REPLAYCOMMANDS_H
