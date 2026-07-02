#include "../kz.h"
#include "cs2kz.h"
#include "kz_hud.h"
#include "sdk/datatypes.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"

#include "kz/option/kz_option.h"
#include "kz/timer/kz_timer.h"
#include "kz/language/kz_language.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "kz/replays/kz_replaysystem.h"

#include <vendor/MultiAddonManager/public/imultiaddonmanager.h>
extern IMultiAddonManager *g_pMultiAddonManager;

#include <vendor/mm-cs2menus/src/public/ics2menus.h>
extern ICS2Menus *g_pMenus;

#include "tier0/memdbgon.h"

static CConVar<bool> kz_force_mhud("kz_force_mhud", FCVAR_NONE, "Force the particle-based MHUD even when MultiAddonManager is not available.", false);

static CConVarRef<bool> sv_suppress_viewpunch("sv_suppress_viewpunch");

static_global class KZTimerServiceEventListener_HUD : public KZTimerServiceEventListener
{
	virtual void OnTimerStopped(KZPlayer *player, u32 courseGUID) override;
	virtual void OnTimerEndPost(KZPlayer *player, u32 courseGUID, f32 time, u32 teleportsUsed) override;
} timerEventListener;

static_global class KZOptionServiceEventListener_HUD : public KZOptionServiceEventListener
{
	virtual void OnPlayerPreferencesLoaded(KZPlayer *player)
	{
		player->hudService->ResetShowPanel();
	}
} optionEventListener;

void KZHUDService::Init()
{
	KZTimerService::RegisterEventListener(&timerEventListener);
	KZOptionService::RegisterEventListener(&optionEventListener);
	// Remove FCVAR_REPLICATED so we can send per-player values.
	if (sv_suppress_viewpunch.IsValidRef() && sv_suppress_viewpunch.IsConVarDataAvailable())
	{
		sv_suppress_viewpunch.GetConVarData()->RemoveFlags(FCVAR_REPLICATED);
	}
}

bool KZHUDService::IsMHUDAvailable()
{
	return g_pMultiAddonManager != nullptr || kz_force_mhud.Get();
}

void KZHUDService::OnProcessMovement()
{
	if (sv_suppress_viewpunch.IsValidRef())
	{
		// Particle-MHUD активен только если hudType==1, ассеты доступны и хотя бы один
		// элемент включён. В режиме hudType==0 particle'ы не спавним → viewpunch не гасим.
		bool wantParticles = (this->GetHudType() == 1) && KZHUDService::IsMHUDAvailable()
							 && (this->IsMHUDSpeedEnabled() || this->IsMHUDPrespeedEnabled() || this->IsMHUDTimerEnabled()
								 || this->IsMHUDKeysEnabled());
		if (wantParticles != this->particlesActive)
		{
			this->particlesActive = wantParticles;
			utils::SendConVarValue(this->player->GetPlayerSlot(), sv_suppress_viewpunch, wantParticles ? "1" : "0");
			utils::SendConVarValue(this->player->GetPlayerSlot(), "view_punch_decay", wantParticles ? "99999" : "18");
		}
		auto dst = sv_suppress_viewpunch.GetConVarData()->Value(-1);
		auto traits = sv_suppress_viewpunch.TypeTraits();
		traits->Copy(dst, CVValue_t(this->particlesActive));
	}
}

void KZHUDService::OnProcessMovementPost()
{
	if (this->player->GetPlayerPawn()->m_fFlags() & FL_ONGROUND)
	{
		fromDuckbug = false;
	}
	if (this->player->GetMoveType() == MOVETYPE_LADDER)
	{
		fromDuckbug = false;
		crouchJumping = false;
	}
}

void KZHUDService::Reset()
{
	this->showPanel = this->player->optionService->GetPreferenceBool("showPanel", true);
	this->timerStoppedTime = {};
	this->currentTimeWhenTimerStopped = {};
	this->jumpedThisTick = false;
	this->fromDuckbug = false;
	this->crouchJumping = false;
	this->particlesActive = false;
	this->DestroyAllParticles();
}

std::string KZHUDService::GetSpeedText(const char *language)
{
	Vector velocity, baseVelocity;
	this->player->GetVelocity(&velocity);
	this->player->GetBaseVelocity(&baseVelocity);
	velocity += baseVelocity;
	// Keep the takeoff velocity on for a while after landing so the speed values flicker less.
	if ((this->player->GetPlayerPawn()->m_fFlags & FL_ONGROUND
		 && g_pKZUtils->GetServerGlobals()->curtime - this->player->landingTime > KZ_HUD_ON_GROUND_THRESHOLD)
		|| (this->player->GetPlayerPawn()->m_MoveType == MOVETYPE_LADDER && !player->IsButtonPressed(IN_JUMP)))
	{
		return KZLanguageService::PrepareMessageWithLang(language, "HUD - Speed Text", velocity.Length2D());
	}
	const Color baseCol = this->GetMHUDColorPref("mhudSpeedColor", Color(0xFF, 0xFF, 0xFF, 0xFF));
	const Color perfCol = this->GetMHUDColorPref("mhudPrespeedPerfColor", Color(0x40, 0xFF, 0x40, 0xFF));
	const Color jumpbugCol = this->GetMHUDColorPref("mhudPrespeedJumpbugColor", Color(0xFF, 0xFF, 0x20, 0xFF));
	const Color cjCol = this->GetMHUDColorPref("mhudSpeedCjColor", Color(0x71, 0xEE, 0xB8, 0xFF));
	Color tintCol = baseCol;
	if (this->player->IsPerfing() && !this->player->possibleLadderHop && !this->player->takeoffFromLadder)
	{
		tintCol = this->fromDuckbug ? jumpbugCol : perfCol;
	}
	char colorBuf[24];
	V_snprintf(colorBuf, sizeof(colorBuf), "<font color='#%02x%02x%02x'>", tintCol.r(), tintCol.g(), tintCol.b());
	char cjBuf[24];
	V_snprintf(cjBuf, sizeof(cjBuf), "<font color='#%02x%02x%02x'>", cjCol.r(), cjCol.g(), cjCol.b());
	std::string crouchJumpingText = this->crouchJumping ? std::string(" ") + cjBuf + "C</font>" : "";
	return KZLanguageService::PrepareMessageWithLang(language, "HUD - Speed Text (Takeoff)", velocity.Length2D(), colorBuf,
													 this->player->takeoffVelocity.Length2D(), crouchJumpingText.c_str());
}

std::string KZHUDService::GetKeyText(const char *language)
{
	// clang-format off
	return KZLanguageService::PrepareMessageWithLang(language, "HUD - Key Text",
		this->player->IsButtonPressed(IN_MOVELEFT) ? 'A' : '_',
		this->player->IsButtonPressed(IN_FORWARD) ? 'W' : '_',
		this->player->IsButtonPressed(IN_BACK) ? 'S' : '_',
		this->player->IsButtonPressed(IN_MOVERIGHT) ? 'D' : '_',
		this->player->IsButtonPressed(IN_DUCK) ? 'C' : '_',
		this->jumpedThisTick ? 'J' : '_'
	);

	// clang-format on
}

std::string KZHUDService::GetCheckpointText(const char *language)
{
	// clang-format off
	
	return KZLanguageService::PrepareMessageWithLang(language, "HUD - Checkpoint Text",
		KZ::replaysystem::IsReplayBot(this->player) ? KZ::replaysystem::GetCurrentCpIndex() : this->player->checkpointService->GetCurrentCpIndex(),
		KZ::replaysystem::IsReplayBot(this->player) ? KZ::replaysystem::GetCheckpointCount() : this->player->checkpointService->GetCheckpointCount(),
		KZ::replaysystem::IsReplayBot(this->player) ? KZ::replaysystem::GetTeleportCount() : this->player->checkpointService->GetTeleportCount()
	);

	// clang-format on
}

std::string KZHUDService::GetTimerText(const char *language)
{
	if (KZ::replaysystem::IsReplayBot(this->player))
	{
		char timeText[128];

		f64 time = KZ::replaysystem::GetTime();
		bool paused = KZ::replaysystem::GetPaused();
		bool timerRunning = KZ::replaysystem::GetEndTime() == 0.0f;
		// Show timer if time is not 0 or end time is not 0.
		if (time == 0.0f && KZ::replaysystem::GetEndTime() == 0.0f)
		{
			return std::string("");
		}
		if (!timerRunning)
		{
			time = KZ::replaysystem::GetEndTime();
		}
		utils::FormatTime(time, timeText, sizeof(timeText));
		// clang-format off
		return KZLanguageService::PrepareMessageWithLang(language, "HUD - Timer Text",
			timeText,
			timerRunning ? "" : KZLanguageService::PrepareMessageWithLang(language, "HUD - Stopped Text").c_str(),
			paused ? KZLanguageService::PrepareMessageWithLang(language, "HUD - Paused Text").c_str() : ""
		);
		// clang-format on
	}
	if (this->player->timerService->GetTimerRunning() || this->ShouldShowTimerAfterStop())
	{
		char timeText[128];

		// clang-format off
		f64 time = this->player->timerService->GetTimerRunning()
				? player->timerService->GetTime()
				: this->currentTimeWhenTimerStopped;
		bool timerRunning = this->player->timerService->GetTimerRunning();
		bool paused = this->player->timerService->GetPaused();
		
		utils::FormatTime(time, timeText, sizeof(timeText));
		return KZLanguageService::PrepareMessageWithLang(language, "HUD - Timer Text",
			timeText,
			timerRunning ? "" : KZLanguageService::PrepareMessageWithLang(language, "HUD - Stopped Text").c_str(),
			paused ? KZLanguageService::PrepareMessageWithLang(language, "HUD - Paused Text").c_str() : ""
		);
		// clang-format on
	}
	return std::string("");
}

// --- Версия C: палитра HUD (см. artifacts_from_outside/MovementHud.cs) ---
#define KZ_HUD_C_ACCENT "#3AA0F5" // активная клавиша / время
#define KZ_HUD_C_WHITE  "#FFFFFF" // скорость / основные числа
#define KZ_HUD_C_DIM    "#5B616D" // неактивная клавиша / разделители / стейдж
#define KZ_HUD_C_MUTED  "#9AA3AF" // подписи (U/S, CP, TP)

std::string KZHUDService::BuildVersionCHud(KZPlayer *dataSource, bool suppressSpeed, bool suppressTimer, bool suppressKeys, bool masterMode,
										   const char *language)
{
	// Данные (скорость/клавиши/таймер/CP-TP) — из dataSource (наблюдаемый при спектировании).
	// Настройки (тумблеры/цвета) — из this (сам игрок/спектатор): своя раскладка, чужие данные.
	// В мастер-режиме элемент рисуется, только если его per-element тумблер ВКЛ (opt-in).
	// В обычном — как раньше: рисуем всё, кроме suppress* (дублируемого particle-MHUD).
	const bool isReplay = KZ::replaysystem::IsReplayBot(dataSource);
	char buf[512];

	// Накапливаем строки в html, разделяя <br> только между непустыми (без висячих тегов).
	std::string html;
	auto addLine = [&](const std::string &line)
	{
		if (!html.empty())
		{
			html += "<br>";
		}
		html += line;
	};

	// В мастер-режиме показываем элемент только при включённом тумблере И если его не
	// рисует particle-MHUD (suppress*), иначе двойной рендер. Вне мастера — как раньше.
	// CP/TP particle-путём не рисуется (только HTML), поэтому suppress к нему не применяется.
	bool showSpeed = masterMode ? (this->IsMHUDSpeedEnabled() && !suppressSpeed) : !suppressSpeed;
	bool showKeys = masterMode ? (this->IsMHUDKeysEnabled() && !suppressKeys) : !suppressKeys;
	bool showTimer = masterMode ? (this->IsMHUDTimerEnabled() && !suppressTimer) : !suppressTimer;
	bool showCpTp = masterMode ? this->IsMHUDCpTpEnabled() : true;

	// --- 1. Скорость: крупное число + подпись U/S, при отрыве — престрейф-скорость
	//        в перф/CJ-цвете (логика как в GetSpeedText). ---
	if (showSpeed)
	{
		Vector velocity, baseVelocity;
		dataSource->GetVelocity(&velocity);
		dataSource->GetBaseVelocity(&baseVelocity);
		velocity += baseVelocity;
		i32 speed = RoundFloatToInt(velocity.Length2D());

		bool onGroundSettled = (dataSource->GetPlayerPawn()->m_fFlags() & FL_ONGROUND
								&& g_pKZUtils->GetServerGlobals()->curtime - dataSource->landingTime > KZ_HUD_ON_GROUND_THRESHOLD)
							   || (dataSource->GetPlayerPawn()->m_MoveType() == MOVETYPE_LADDER && !dataSource->IsButtonPressed(IN_JUMP));

		std::string takeoff;
		if (!onGroundSettled)
		{
			const Color baseCol = this->GetMHUDColorPref("mhudSpeedColor", Color(0xFF, 0xFF, 0xFF, 0xFF));
			const Color perfCol = this->GetMHUDColorPref("mhudPrespeedPerfColor", Color(0x40, 0xFF, 0x40, 0xFF));
			const Color jumpbugCol = this->GetMHUDColorPref("mhudPrespeedJumpbugColor", Color(0xFF, 0xFF, 0x20, 0xFF));
			Color tintCol = baseCol;
			if (dataSource->IsPerfing() && !dataSource->possibleLadderHop && !dataSource->takeoffFromLadder)
			{
				tintCol = dataSource->hudService->fromDuckbug ? jumpbugCol : perfCol;
			}
			char tk[96];
			V_snprintf(tk, sizeof(tk), " <font color='#%02x%02x%02x'>(%d)</font>", tintCol.r(), tintCol.g(), tintCol.b(),
					   RoundFloatToInt(dataSource->takeoffVelocity.Length2D()));
			takeoff = tk;
			if (dataSource->hudService->crouchJumping)
			{
				takeoff += " <font color='" KZ_HUD_C_ACCENT "'>C</font>";
			}
		}
		V_snprintf(buf, sizeof(buf), "<font color='" KZ_HUD_C_WHITE "'>%d</font> <font color='" KZ_HUD_C_MUTED "'>U/S</font>%s", speed,
				   takeoff.c_str());
		addLine(buf);
	}

	// --- 2. Ряд клавиш W A S D  J C (активная — accent, неактивная — dim) ---
	if (showKeys)
	{
		auto key = [&](const char *label, bool down)
		{
			char k[64];
			V_snprintf(k, sizeof(k), "<font color='%s'>%s</font>", down ? KZ_HUD_C_ACCENT : KZ_HUD_C_DIM, label);
			return std::string(k);
		};
		bool jump = dataSource->hudService->jumpedThisTick || dataSource->IsButtonPressed(IN_JUMP);
		std::string row = key("W", dataSource->IsButtonPressed(IN_FORWARD)) + " " + key("A", dataSource->IsButtonPressed(IN_MOVELEFT)) + " "
						  + key("S", dataSource->IsButtonPressed(IN_BACK)) + " " + key("D", dataSource->IsButtonPressed(IN_MOVERIGHT))
						  + "&#160;&#160;" + key("J", jump) + " " + key("C", dataSource->IsButtonPressed(IN_DUCK));
		addLine(row);
	}

	// --- 3. CP/TP в стиле версии C (per-element тумблер hudCpTp) ---
	if (showCpTp)
	{
		i32 cpIndex = isReplay ? KZ::replaysystem::GetCurrentCpIndex() : dataSource->checkpointService->GetCurrentCpIndex();
		i32 cpCount = isReplay ? KZ::replaysystem::GetCheckpointCount() : dataSource->checkpointService->GetCheckpointCount();
		i32 tpCount = isReplay ? KZ::replaysystem::GetTeleportCount() : (i32)dataSource->checkpointService->GetTeleportCount();
		V_snprintf(buf, sizeof(buf),
				   "<font color='" KZ_HUD_C_MUTED "'>CP</font> <font color='" KZ_HUD_C_WHITE "'>%d/%d</font> "
				   "<font color='" KZ_HUD_C_DIM "'>|</font> <font color='" KZ_HUD_C_MUTED "'>TP</font> <font color='" KZ_HUD_C_WHITE "'>%d</font>",
				   cpIndex, cpCount, tpCount);
		addLine(buf);
	}

	// --- 4. Время | STAGE n/total (время — accent, стейдж — dim) ---
	if (showTimer)
	{
		std::string timer = dataSource->hudService->GetTimerText(language);
		if (!timer.empty())
		{
			std::string stage;
			if (!isReplay)
			{
				const KZCourseDescriptor *course = dataSource->timerService->GetCourse();
				if (course && course->stageCount > 0)
				{
					char st[64];
					V_snprintf(st, sizeof(st), " <font color='" KZ_HUD_C_DIM "'>| STAGE %d/%d</font>", dataSource->timerService->GetCurrentStage(),
							   course->stageCount);
					stage = st;
				}
			}
			V_snprintf(buf, sizeof(buf), "<font color='" KZ_HUD_C_ACCENT "'>%s</font>%s", timer.c_str(), stage.c_str());
			addLine(buf);
		}
	}

	return html;
}

void KZHUDService::DrawPanels(KZPlayer *player, KZPlayer *target)
{
	KZHUDService *cfg = target->hudService;
	int hudType = cfg->GetHudType();

	// Particle-путь активен только для hudType==1 (MHUD), при живом игроке и доступных ассетах.
	bool useParticles = (hudType == 1) && target->IsAlive() && KZHUDService::IsMHUDAvailable();

	if (useParticles)
	{
		// player = источник данных (наблюдаемый при спектировании).
		target->hudService->UpdateParticles(player);
	}
	else
	{
		// Гасим particle'ы: либо hudType==0, либо MHUD недоступен, либо игрок мёртв.
		target->hudService->DestroyAllParticles();
	}

	// Yield the center channel while a cs2menus HTML menu is open.
	if (g_pMenus && g_pMenus->GetActiveMenuType(target->GetPlayerSlot().Get()) == MenuType::Html)
	{
		return;
	}
	if (!target->hudService->IsShowingPanel())
	{
		return;
	}
	const char *language = target->languageService->GetLanguage();

	std::string htmlText;

	if (hudType == 1)
	{
		// MHUD-режим: particle рисует speed/timer/keys; HTML рисует только CP/TP
		// (particle-путь CP/TP не реализован). Compact-панель не применяется.
		if (cfg->IsMHUDCpTpEnabled())
		{
			// Передаём suppress*=true, чтобы BuildVersionCHud рисовал только CP/TP.
			// masterMode=true включает per-element gate; speed/timer/keys выключены suppress'ом,
			// CP/TP suppress не имеет → рисуется если IsMHUDCpTpEnabled().
			htmlText = cfg->BuildVersionCHud(player, /*suppressSpeed=*/true, /*suppressTimer=*/true, /*suppressKeys=*/true,
											 /*masterMode=*/true, language);
		}
	}
	else
	{
		// Стандартный HTML-режим (hudType==0): весь HUD через BuildVersionCHud.
		// suppress=false (particle не активен), masterMode=false (рисуем всё по per-element pref).
		if (cfg->IsCompactPanel())
		{
			std::string timerText = player->hudService->GetTimerText(language);
			std::string speedText = player->hudService->GetSpeedText(language);
			if (!timerText.empty() && !speedText.empty())
			{
				htmlText = timerText + "<br>" + speedText;
			}
			else
			{
				htmlText = timerText + speedText;
			}
		}
		else
		{
			// masterMode=true: показывать элемент только если его per-element тумблер ВКЛ.
			htmlText = cfg->BuildVersionCHud(player, /*suppressSpeed=*/false, /*suppressTimer=*/false, /*suppressKeys=*/false,
											 /*masterMode=*/true, language);
		}
	}

	htmlText = htmlText.substr(0, htmlText.find_last_not_of('\n') + 1);

	if (!htmlText.empty())
	{
		target->PrintHTMLCentre(false, false, htmlText.c_str());
	}
}

void KZHUDService::ResetShowPanel()
{
	this->showPanel = this->player->optionService->GetPreferenceBool("showPanel", true);
}

void KZHUDService::TogglePanel()
{
	this->showPanel = !this->showPanel;
	this->player->optionService->SetPreferenceBool("showPanel", this->showPanel);
	if (!this->showPanel)
	{
		utils::PrintAlert(this->player->GetController(), "#SFUI_EmptyString");
		utils::PrintCentre(this->player->GetController(), "#SFUI_EmptyString");
		this->player->languageService->PrintHTMLCentre(false, false, "HUD - HTML Panel Disabled");
	}
}

void KZHUDService::OnTimerStopped(f64 currentTimeWhenTimerStopped)
{
	// g_pKZUtils->GetServerGlobals() becomes invalid when the plugin is unloading.
	if (g_KZPlugin.unloading)
	{
		return;
	}
	this->timerStoppedTime = g_pKZUtils->GetServerGlobals()->curtime;
	this->currentTimeWhenTimerStopped = currentTimeWhenTimerStopped;
}

void KZTimerServiceEventListener_HUD::OnTimerStopped(KZPlayer *player, u32 courseGUID)
{
	player->hudService->OnTimerStopped(player->timerService->GetTime());
}

void KZTimerServiceEventListener_HUD::OnTimerEndPost(KZPlayer *player, u32 courseGUID, f32 time, u32 teleportsUsed)
{
	player->hudService->OnTimerStopped(time);
}

bool KZHUDService::IsCompactPanel()
{
	return this->player->optionService->GetPreferenceBool("compactPanel");
}

void KZHUDService::ToggleCompactPanel()
{
	this->player->optionService->SetPreferenceBool("compactPanel", !this->IsCompactPanel());
}

SCMD(kz_panel, SCFL_HUD)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (args->ArgC() >= 2)
	{
		if (KZ_STREQI(args->Arg(1), "compact"))
		{
			player->hudService->ToggleCompactPanel();
			if (player->hudService->IsCompactPanel())
			{
				player->languageService->PrintChat(true, false, "HUD Option - Compact Panel - Enable");
			}
			else
			{
				player->languageService->PrintChat(true, false, "HUD Option - Compact Panel - Disable");
			}
			return MRES_SUPERCEDE;
		}
		else
		{
			player->languageService->PrintChat(true, false, "Panel Command Usage");
			return MRES_SUPERCEDE;
		}
	}
	player->hudService->TogglePanel();
	if (player->hudService->IsShowingPanel())
	{
		player->languageService->PrintChat(true, false, "HUD Option - Info Panel - Enable");
	}
	else
	{
		player->languageService->PrintChat(true, false, "HUD Option - Info Panel - Disable");
	}
	return MRES_SUPERCEDE;
}
