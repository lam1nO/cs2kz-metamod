#ifndef KZ_REPLAYPLAYBACK_H
#define KZ_REPLAYPLAYBACK_H
#include "common.h"
#include "sdk/datatypes.h"
#include "awr_cut.h"

class KZPlayer;
class PlayerCommand;
class CMoveData;
struct TickData;
struct SubtickData;
struct RpEvent;
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

	// ПРОПУСКАЕМЫЕ сегменты воспроизведения. Записанные паузы (пары событий
	// TIMER_PAUSE→TIMER_RESUME) — частный случай; в AWR-режиме к ним добавляются мёртвые
	// интервалы разреза (replay->awrDead). BuildSkipSegments — на старте, ClearPauseSegments —
	// на остановке/конце. SnapSeekTargetOutOfPause/ResetPauseCursor — на навигации (сик).
	void BuildSkipSegments();
	void ClearPauseSegments();
	u32 SnapSeekTargetOutOfPause(u32 tick);
	void ResetPauseCursor(u32 tick);
	// Индекс кадра попадает в пропускаемый сегмент (события/прыжки оттуда не проигрываются).
	bool IsTickIndexSkipped(u32 idx);

	// Первый индекс кадра с serverTick >= заданного; tickCount, если такого нет.
	u32 TickIndexForServerTick(const TickData *ticks, u32 tickCount, u32 serverTick);

	// Записанные паузы в ИНДЕКСАХ кадров, включительно ([startIdx, endIdx-1]) — общий код
	// для BuildSkipSegments и для разреза AWR. Публичен: тем же кодом пользуются воркер
	// бэкфилла и !lead, у которых нет g_currentReplay.
	std::vector<awr::Interval> PauseIntervalsFromEvents(const TickData *ticks, u32 tickCount, const RpEvent *events, u32 numEvents);

	// Окно САМОГО рана в индексах кадров (включительно) по событиям таймера плюс id курса
	// этой пары START/END. false — окна нет (нет пары START/END, курсы пары разные, окно
	// вырождено). Публична: тот же вывод окна нужен воркеру бэкфилла, !lead и сверке курса с
	// шапкой в commands.cpp (реестр курсов доступен только на главном потоке).
	bool RunWindowFromEvents(const TickData *ticks, u32 tickCount, const RpEvent *events, u32 numEvents, u32 &outStart, u32 &outEnd,
							 i32 &outCourseId);

	// Разрез AWR по уже разобранным данным реплея (адаптер TickData→awr::Frame внутри).
	awr::CutResult ComputeCutFor(const TickData *ticks, u32 tickCount, const RpEvent *events, u32 numEvents, u64 timeMs);

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
