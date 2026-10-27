#pragma once
#include "kz/kz.h"

class PlayerCommand;

// Персональная боковая панель и инструменты; разметка — cyber/quickmenu.xml.
namespace KZ::quickmenu
{
	void Tick(KZPlayer *player);
	void FlushPaint(KZPlayer *player);
	void Open(KZPlayer *player);
	void Close(KZPlayer *player);
	void Reset(KZPlayer *player);
	void Cleanup();
	bool Owns(KZPlayer *player, CEntityHandle handle);
	bool OnClick(KZPlayer *player, uint32 packedHandle, const char *id);
	void OnUsercmds(KZPlayer *player, PlayerCommand *cmds, i32 count);
}
