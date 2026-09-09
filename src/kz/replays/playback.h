#ifndef KZ_REPLAYPLAYBACK_H
#define KZ_REPLAYPLAYBACK_H
#include "common.h"
#include "sdk/datatypes.h"

class KZPlayer;
class PlayerCommand;
class CMoveData;
struct TickData;
struct SubtickData;
class CBasePlayerWeapon;
struct EconInfo;

namespace KZ::replaysystem::playback
{
	// Core playback functions
	void OnPhysicsSimulate(KZPlayer *player);
	void OnProcessMovement(KZPlayer *player);
	void OnProcessMovementPost(KZPlayer *player);
	void OnFinishMovePre(KZPlayer *player, CMoveData *mv);
	void OnPhysicsSimulatePost(KZPlayer *player);
	void OnPlayerRunCommandPre(KZPlayer *player, PlayerCommand *command);

	// Weapon management during playback
	void CheckWeapon(KZPlayer &player, PlayerCommand &cmd);
	void InitializeWeapons();

	// Playback state management
	void StartReplay();

	// Пропуск записанных пауз при воспроизведении: сегменты строятся из событий
	// TIMER_PAUSE→TIMER_RESUME. BuildPauseSegments — на старте, ClearPauseSegments — на
	// остановке/конце. SnapSeekTargetOutOfPause/ResetPauseCursor — на навигации (сик).
	void BuildPauseSegments();
	void ClearPauseSegments();
	u32 SnapSeekTargetOutOfPause(u32 tick);
	void ResetPauseCursor(u32 tick);

	// ЭФФЕКТИВНАЯ шкала тиков — запись без паузных сегментов (их плейбек пропускает). Всё, что
	// видит зритель (время в меню/!rpinfo, перемотка ±N сек, шаг по тикам), считается в ней и
	// переводится в сырой индекс tickData только на границе: иначе длительность включала бы
	// вырезанные паузы, а сик «−10 с» из кадра сразу после паузы приземлялся бы внутрь неё и
	// снапом возвращался на тот же кадр — назад не отмотать (баг с канарейки 09.09).
	u32 EffectiveTickCount();
	u32 RawTickToEffective(u32 rawTick);
	u32 EffectiveTickToRaw(u32 effectiveTick);

	// Скорость кадра, на котором стоит бот прямо сейчас (с учётом дробной позиции
	// плейхеда при замедлении). Нужна худу: на паузе velocity пешки принудительно
	// обнулена, чтобы бота не унесло физикой, и спрашивать её бесполезно — там всегда 0.
	// false, если игрок не реплей-бот или реплей не воспроизводится.
	bool GetDisplayedFrameVelocity(KZPlayer *player, Vector &out);

	// Navigation support
	void NavigateToTick(u32 targetTick);
	void ApplyTickState(KZPlayer *player, const TickData *tickData);
} // namespace KZ::replaysystem::playback

#endif // KZ_REPLAYPLAYBACK_H
