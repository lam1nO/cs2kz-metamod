#pragma once
#include "../kz.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "KeyValues.h"
#include "interfaces/interfaces.h"
#include "filesystem.h"
#include "utils/ctimer.h"
#include "kz/option/kz_option.h"

class KZTipService : public KZBaseService
{
	using KZBaseService::KZBaseService;

private:
	bool showTips {};
	bool teamJoinedAtLeastOnce {};

public:
	virtual void Reset() override;
	void ToggleTips();
	static void Init();
	static f64 PrintTips();
	void OnPlayerJoinTeam(i32 team);
	void OnTimerStartPost();

	// Для реестра настроек (Task 7, AddActionToggle): читает и пишет через колбэки, а не
	// сырой prefKey, потому что запись должна попасть и в кэш-член (this->showTips), и в
	// optionService (см. Reset()/ToggleTips() — showTips персистентен с ревью задачи 7).
	bool GetShowTips() const
	{
		return this->showTips;
	}

private:
	bool ShouldPrintTip();
	void PrintTip();
	static void LoadTips();
	static void ShuffleTips();
};
