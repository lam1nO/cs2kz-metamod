#pragma once
#include "common.h"
#include <playerslot.h>
#include <functional>

// Опрос клиентского конвара своими силами: CSVCMsg_GetCvarValue наружу, CCLCMsg_RespondCvarValue
// обратно. Порт с апстрима (origin/master:src/utils/cvarquery.{h,cpp}) — раньше это делал внешний
// metamod-плагин ClientCvarValue, которого на нашем флоте нет ни в одном профиле, из-за чего опрос
// не уходил вообще (разведка: .superpowers/sdd/2026-09-08-options-registry/recon-crosshair.md).
//
// Расхождение с апстримом: пассивный источник (cvarquery::OnClientConVar/SetChangeCallback — все
// конвары, что клиент репортит сам в userinfo и setinfo) НЕ портирован. Его кормит хук на
// CServerSideClientBase::ProcessSetConVar, а у нас этот слот втаблицы объявлен под старым именем
// ApplyConVars с другой сигнатурой (src/sdk/serversideclient.h:200) — включение пассивного пути
// требует переименования слота и в этот транш не входит. Активного Query() крестику достаточно.
namespace cvarquery
{
	enum class Status
	{
		ValueIntact = 0, // It got the value fine.
		CvarNotFound = 1,
		NotACvar = 2,     // There's a ConCommand, but it's not a ConVar.
		CvarProtected = 3 // The cvar was marked with FCVAR_SERVER_CAN_NOT_QUERY.
	};

	using Callback = std::function<void(CPlayerSlot slot, Status status, const char *name, const char *value)>;

	// The hooks that feed this live in hooks.cpp.
	void Shutdown();

	// The callback runs once, when the client answers; a client that never answers never calls back.
	bool Query(CPlayerSlot slot, const char *cvarName, Callback callback);

	void OnClientDisconnect(CPlayerSlot slot);

	void OnCvarValueResponse(CPlayerSlot slot, i32 cookie, Status status, const char *name, const char *value);
} // namespace cvarquery
