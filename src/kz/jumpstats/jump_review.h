#pragma once
#include "kz/kz.h"
class Jump;
class PlayerCommand;

namespace KZ::jumpreview
{
	void OnJump(KZPlayer *player, Jump *jump);
	void Tick(KZPlayer *player);
	void Reset(KZPlayer *player);
	void Cleanup();
	bool Owns(KZPlayer *player, CEntityHandle handle);
	bool IsOpen(KZPlayer *player);
	bool OnClick(KZPlayer *player, uint32 packedHandle, const char *id);
	void OnUsercmds(KZPlayer *player, PlayerCommand *cmds, i32 count);
	void Open(KZPlayer *player);
	void Close(KZPlayer *player);
} // namespace KZ::jumpreview
