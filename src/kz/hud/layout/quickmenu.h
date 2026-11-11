#pragma once
#include "kz/kz.h"
#include "kz/hud/layout/quickmenu_position.h"

class PlayerCommand;

// Персональная боковая панель и инструменты; разметка — cyber/quickmenu.xml.
namespace KZ::quickmenu
{
	bool CanEditTab();
	TabPosition GetTabPosition(KZPlayer *player);
	void SetTabPosition(KZPlayer *player, int x, int y);
	void NudgeTabPosition(KZPlayer *player, int dx, int dy);
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
