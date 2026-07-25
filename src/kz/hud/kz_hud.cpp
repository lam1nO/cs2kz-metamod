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
#include "kz/prac/kz_prac.h"
#include "kz/replays/kz_replaysystem.h"
#include "kz/style/kz_style.h" // GetStyleName для лейбла стиля (деф. Normal) в строке 1
#include "kz/mode/kz_mode.h" // KZModeService::GetModeShortName для метки режима в строке 1
#include "kz/replays/cyb_replay_common.h" // MapMode — тот же маппинг режима, что у PB/WR-фетча

#include <vendor/MultiAddonManager/public/imultiaddonmanager.h>
extern IMultiAddonManager *g_pMultiAddonManager;

#include <vendor/mm-cs2menus/src/public/ics2menus.h>
extern ICS2Menus *g_pMenus;

#include "tier0/memdbgon.h"

// Формат времени для худа: mm:ss.cc (сотые), как у кибершока. Отдельно от utils::FormatTime
// (тысячные) — тот нужен другим местам (чат/сабмишен/реплеи), его формат не трогаем.
// Часы добавляются при необходимости. time всегда >= 0 (таймер/PB/WR).
static_function void FormatTimeHud(f64 time, char *output, u32 length)
{
	i32 rounded = RoundFloatToInt(time * 100); // время в сотых долях секунды
	i32 centis = rounded % 100;
	rounded = (rounded - centis) / 100;
	i32 seconds = rounded % 60;
	rounded = (rounded - seconds) / 60;
	i32 minutes = rounded % 60;
	rounded = (rounded - minutes) / 60;
	i32 hours = rounded;
	if (hours == 0)
	{
		snprintf(output, length, "%02i:%02i.%02i", minutes, seconds, centis);
	}
	else
	{
		snprintf(output, length, "%i:%02i:%02i.%02i", hours, minutes, seconds, centis);
	}
}

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
		// Particle-путь активен при выбранном MHUD, доступных ассетах и хотя бы одном включённом элементе.
		bool wantParticles = KZHUDService::IsMHUDAvailable() && this->GetHudType() == HUD_TYPE_MHUD
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

void KZHUDService::OnJoinSpectator()
{
	if (this->particlesActive)
	{
		// Вернуть клиенту дефолты viewpunch, которые particle-путь подменял.
		utils::SendConVarValue(this->player->GetPlayerSlot(), sv_suppress_viewpunch, "0");
		utils::SendConVarValue(this->player->GetPlayerSlot(), "view_punch_decay", "18");
	}
	this->particlesActive = false;
	this->DestroyAllParticles();
}

std::string KZHUDService::GetSpeedText(const char *language, KZPlayer *dataSource)
{
	// dataSource — источник ДАННЫХ (скорость/перф/крауч-джамп), settings (цвета, через
	// GetMHUDColorPref/MHUDSettingsSource) — всегда this (получатель). При вызове со
	// спектатора: cfg->GetSpeedText(language, player) — тот же контракт, что в BuildVersionCHud.
	KZPlayer *src = dataSource ? dataSource : this->player;
	Vector velocity, baseVelocity;
	src->GetVelocity(&velocity);
	src->GetBaseVelocity(&baseVelocity);
	velocity += baseVelocity;
	// Keep the takeoff velocity on for a while after landing so the speed values flicker less.
	if ((src->GetPlayerPawn()->m_fFlags & FL_ONGROUND
		 && g_pKZUtils->GetServerGlobals()->curtime - src->landingTime > KZ_HUD_ON_GROUND_THRESHOLD)
		|| (src->GetPlayerPawn()->m_MoveType == MOVETYPE_LADDER && !src->IsButtonPressed(IN_JUMP)))
	{
		return KZLanguageService::PrepareMessageWithLang(language, "HUD - Speed Text", velocity.Length2D());
	}
	const Color baseCol = this->GetMHUDColorPref("mhudSpeedColor", Color(0xFF, 0xFF, 0xFF, 0xFF));
	const Color perfCol = this->GetMHUDColorPref("mhudPrespeedPerfColor", Color(0x40, 0xFF, 0x40, 0xFF));
	const Color jumpbugCol = this->GetMHUDColorPref("mhudPrespeedJumpbugColor", Color(0xFF, 0xFF, 0x20, 0xFF));
	const Color cjCol = this->GetMHUDColorPref("mhudSpeedCjColor", Color(0x71, 0xEE, 0xB8, 0xFF));
	Color tintCol = baseCol;
	if (src->IsPerfing() && !src->possibleLadderHop && !src->takeoffFromLadder)
	{
		tintCol = src->hudService->fromDuckbug ? jumpbugCol : perfCol;
	}
	char colorBuf[24];
	V_snprintf(colorBuf, sizeof(colorBuf), "<font color='#%02x%02x%02x'>", tintCol.r(), tintCol.g(), tintCol.b());
	char cjBuf[24];
	V_snprintf(cjBuf, sizeof(cjBuf), "<font color='#%02x%02x%02x'>", cjCol.r(), cjCol.g(), cjCol.b());
	std::string crouchJumpingText = src->hudService->crouchJumping ? std::string(" ") + cjBuf + "C</font>" : "";
	return KZLanguageService::PrepareMessageWithLang(language, "HUD - Speed Text (Takeoff)", velocity.Length2D(), colorBuf,
													 src->takeoffVelocity.Length2D(), crouchJumpingText.c_str());
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

// Разбор состояния таймера для кибершоковского худа: время (сотые) и суффикс паузы/стопа
// раздельно, чтобы красить их разными цветами. outRunning — идёт ли активный забег (пауза =
// идёт → true); цвет/обнуление времени по нему решает BuildVersionCHud. Логика как в
// GetTimerText (реплей-бот / обычный забег / grace после стопа); данные — this->player,
// суффикс-фразы — в языке получателя. Обычному игроку в простое (idle) отдаём нулевой таймер
// (outRunning=false); false — только реплей-боту, которому показывать нечего.
bool KZHUDService::GetTimerParts(const char *language, std::string &outTime, std::string &outSuffix, bool &outRunning)
{
	f64 time = 0.0;
	bool timerRunning = false;
	bool paused = false;
	outRunning = false;

	if (KZ::replaysystem::IsReplayBot(this->player))
	{
		time = KZ::replaysystem::GetTime();
		paused = KZ::replaysystem::GetPaused();
		timerRunning = KZ::replaysystem::GetEndTime() == 0.0f;
		// Таймер не показываем, если и текущее, и конечное время нулевые.
		if (time == 0.0f && KZ::replaysystem::GetEndTime() == 0.0f)
		{
			return false;
		}
		if (!timerRunning)
		{
			time = KZ::replaysystem::GetEndTime();
		}
	}
	else if (this->player->timerService->GetTimerRunning() || this->ShouldShowTimerAfterStop())
	{
		timerRunning = this->player->timerService->GetTimerRunning();
		time = timerRunning ? this->player->timerService->GetTime() : this->currentTimeWhenTimerStopped;
		paused = this->player->timerService->GetPaused();
	}
	else
	{
		// Обычный игрок в простое: показываем нулевой таймер (цвет белый / текст 00:00.00
		// задаёт BuildVersionCHud по outRunning=false), без суффикса.
		char zeroText[64];
		FormatTimeHud(0.0, zeroText, sizeof(zeroText));
		outTime = zeroText;
		outSuffix.clear();
		return true;
	}

	outRunning = timerRunning;
	char timeText[64];
	FormatTimeHud(time, timeText, sizeof(timeText));
	outTime = timeText;
	outSuffix.clear();
	if (!timerRunning)
	{
		outSuffix += KZLanguageService::PrepareMessageWithLang(language, "HUD - Stopped Text");
	}
	if (paused)
	{
		outSuffix += KZLanguageService::PrepareMessageWithLang(language, "HUD - Paused Text");
	}
	return true;
}

// --- Кибершоковский стандартный худ: палитра и скобки таймера --------------------------
// Активные клавиши теперь на KZ_HUD_C_CYAN (см. ряд W A S D J C ниже), как число скорости.
// Зелёный KZ_HUD_C_TIMER — только для таймера и WR-времени, клавиши не перекрашивает.
#define KZ_HUD_C_ACCENT "#3AA0F5" // синий акцент (сейчас не используется, оставлен под подстройку)
#define KZ_HUD_C_WHITE  "#FFFFFF" // скорость / время PB / числа
#define KZ_HUD_C_DIM    "#5B616D" // разделители / рамки || | / суффикс паузы-стопа / "--"
#define KZ_HUD_C_MUTED  "#9AA3AF" // подписи (режим, стиль, Stage, координаты)
#define KZ_HUD_C_TIMER  "#4CD964" // таймер и WR-время (кибершоковский зелёный)
#define KZ_HUD_C_CYAN   "#22D3EE" // число скорости (циан); подстроить: #00E5FF/#00FFFF
#define KZ_HUD_C_RED    "#FF3B3B" // overlap клавиш (W+S / A+D при hudKeysOverlap): все нажатые красным
// Престрейф в скобках и приписка C — тоже ФИКСИРОВАННАЯ палитра, не MHUD-префы игрока:
// префы mhud*Color принадлежат particle-MHUD («Внешний вид MHUD»), и сохранённое там
// экзотическое значение красило престрейф в невидимый цвет на тёмной панели (баг 25.07:
// у игрока mhudSpeedColor = чёрный ⇒ (престрейф) не виден вне перфа).
#define KZ_HUD_C_PERF    "#40FF40" // престрейф после перфа
#define KZ_HUD_C_JUMPBUG "#FFFF20" // престрейф после jumpbug/duckbug
#define KZ_HUD_C_CJ      "#71EEB8" // приписка C (crouch-jump)

// Скобки вокруг таймера. Уголковые ⌈ ⌋ (U+2308/230B) «кибершоковее», но лежат в
// Mathematical-блоке Unicode — вне гарантированного набора игрового шрифта, риск tofu на
// живом сервере (см. дизайн-спеку). Поэтому по умолчанию ASCII; чтобы проверить уголковые —
// закомментировать ASCII-пару и раскомментировать HTML-entity-пару ниже, прогнать
// glyph-тест на dev-боксе.
#define KZ_HUD_BRACKET_OPEN  "["
#define KZ_HUD_BRACKET_CLOSE "]"
// #define KZ_HUD_BRACKET_OPEN  "&#8968;" // ⌈ U+2308
// #define KZ_HUD_BRACKET_CLOSE "&#8971;" // ⌋ U+230B

// Размеры шрифта строк стандартного HTML-худа (класс движка внутри color-тега, см. ниже).
// Явная иерархия по важности: скорость > таймер > вторичная инфа > клавиши/стиль. ВНИМАНИЕ:
// имена классов — гипотезы (движок кибершока вероятно знает fontSize-l/m/sm/s); если класс
// неизвестен, размер молча откатится на дефолт — проверить вживую. Все четыре в одном месте:
// иерархия правится одной строкой после живого теста.
#define KZ_HUD_FS_SPEED     "fontSize-l"  // скорость — главный акцент
#define KZ_HUD_FS_TIMER     "fontSize-l"  // таймер — крупный, одного кегля со скоростью (по фото кибершока)
#define KZ_HUD_FS_SECONDARY "fontSize-sm" // PB/WR, CP/TP, престрейф, Stage, координаты — вторичная инфа
#define KZ_HUD_FS_KEYS      "fontSize-m"  // 2 ряда клавиш (C W J / A S D). Был l — по просьбе тестера уменьшен на ступень (m); заодно меньше риск обрезки низа панели
#define KZ_HUD_FS_MINOR     "fontSize-s"  // метка стиля — наименее заметное

std::string KZHUDService::BuildVersionCHud(KZPlayer *dataSource, bool suppressSpeed, bool suppressTimer, bool suppressKeys, bool masterMode,
										   const char *language)
{
	// Данные (скорость/таймер/стейдж/PB-WR/CP-TP) — из dataSource (наблюдаемый при спектировании).
	// Настройки (тумблеры/цвета/язык/раскладка) — из this (сам игрок/спектатор): своя раскладка,
	// чужие данные. В мастер-режиме элемент рисуется, только если его per-element тумблер ВКЛ.
	// В обычном — как раньше: рисуем всё, кроме suppress* (дублируемого particle-MHUD).
	const bool isReplay = KZ::replaysystem::IsReplayBot(dataSource);
	const bool compact = this->IsCompactPanel();
	char buf[1024]; // с запасом: строка 1 = левый nbsp-паддинг (× множитель) + таймер + режим + стиль

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

	// Компакт-режим: только строки 1-2 (таймер+скорость), всегда — как в прежнем компакте
	// (per-element тумблеры в компакте не смотрим). Полный режим: прежняя логика тумблеров.
	// showExtra — стейдж/PB-WR (только полный режим). CP-TP/клавиши/showpos — по своим тумблерам.
	bool showTimer = compact ? true : (masterMode ? (this->IsMHUDTimerEnabled() && !suppressTimer) : !suppressTimer);
	bool showSpeed = compact ? true : (masterMode ? (this->IsMHUDSpeedEnabled() && !suppressSpeed) : !suppressSpeed);
	bool showKeys = compact ? false : (masterMode ? (this->IsMHUDKeysEnabled() && !suppressKeys) : !suppressKeys);
	bool showCpTp = compact ? false : (masterMode ? this->IsMHUDCpTpEnabled() : true);
	bool showExtra = !compact;
	// showpos — тумблер получателя (this->player), данные наблюдаемого. Считаем заранее: нужен
	// и для координат, и для решения о зазоре нижней группы (клавиши/CP-TP/showpos).
	bool showPos = !compact && this->player->optionService->GetPreferenceBool("showPos", false);

	// Типографика (иерархия): скорость — KZ_HUD_FS_SPEED, таймер — KZ_HUD_FS_TIMER, вторичная инфа
	// (PB/WR, CP/TP, престрейф, Stage, координаты) — KZ_HUD_FS_SECONDARY, клавиши и метка стиля —
	// KZ_HUD_FS_MINOR (значения классов — в #define рядом с палитрой выше, правятся одной строкой).
	// Классы вкладываем В color-теги: <font class='...'><font color='...'>X</font></font>.
	// Это прогрессивное улучшение — если движок не подхватит класс, размер молча дефолтный,
	// но цвета и раскладка остаются корректными (класс лишь меняет кегль, не текст/цвет).

	// --- Строка 1: [ 00:07.96 ] Стиль — таймер в скобках (m): зелёный когда идёт/пауза, белый
	//        00:00.00 когда стоп/idle; суффикс паузы/стопа DIM, рядом метка стиля MUTED (s, только
	//        активный не-деф. стиль). Символ скобки — через KZ_HUD_BRACKET_*. ---
	if (showTimer)
	{
		std::string tTime, tSuffix;
		bool tRunning = false;
		if (dataSource->hudService->GetTimerParts(language, tTime, tSuffix, tRunning))
		{
			// Идёт забег или пауза (tRunning) → зелёный + фактическое время. Стоп/idle обычного
			// игрока → белый + 00:00.00. Реплей-бот не трогаем (свой финальный тайм остаётся).
			bool notRunning = !tRunning && !isReplay;
			const char *timerColor = notRunning ? KZ_HUD_C_WHITE : KZ_HUD_C_TIMER;
			if (notRunning)
			{
				char zeroText[64];
				FormatTimeHud(0.0, zeroText, sizeof(zeroText));
				tTime = zeroText;
			}
			// Метка режима — рядом с временем, ЗАГЛАВНЫМИ (CKZ/KZT/VNL), как на кибершоке «[..] CKZ».
			// Короткое имя режима наблюдаемого прогоняем через CybReplayCommon::MapMode (тот же
			// маппинг/вайтлист, что у PB/WR-фетча в kz_timer.cpp) → ckz/vnl/kzt, апаем в верхний
			// регистр. Пустая строка = кастомный режим сверх этих трёх → метку не показываем.
			// Реплей-бот пропускаем (своего режима как у игрока нет).
			// Видимая ширина правой части («&#160;&#160;режим» + «&#160;&#160;стиль») в «символах» —
			// нужна для левого паддинга, чтобы ВРЕМЯ встало по центру экрана (см. leftPad ниже).
			int rightVisChars = 0;
			std::string modeTag;
			if (!isReplay && dataSource->modeService)
			{
				const char *modeApi = CybReplayCommon::MapMode(dataSource->modeService->GetModeShortName());
				if (modeApi[0])
				{
					char up[8] = {0};
					for (int i = 0; modeApi[i] && i < 7; i++)
					{
						up[i] = (modeApi[i] >= 'a' && modeApi[i] <= 'z') ? (char)(modeApi[i] - 32) : modeApi[i];
					}
					char mt[128];
					V_snprintf(mt, sizeof(mt), "&#160;&#160;<font class='" KZ_HUD_FS_SECONDARY "'><font color='" KZ_HUD_C_MUTED "'>%s</font></font>", up);
					modeTag = mt;
					rightVisChars += 2 + (int)V_strlen(up); // 2 разделителя + буквы режима
				}
			}
			// Метка стиля — только для активного не-дефолтного стиля. "Normal" как шум убран:
			// без активных стилей метки нет вовсе (пустой styleTag → без висячего разделителя).
			std::string styleTag;
			if (!isReplay && dataSource->styleServices.Count() > 0)
			{
				const char *styleName = dataSource->styleServices[0]->GetStyleName();
				char st[128];
				V_snprintf(st, sizeof(st), "&#160;&#160;<font class='" KZ_HUD_FS_MINOR "'><font color='" KZ_HUD_C_MUTED "'>%s</font></font>", styleName);
				styleTag = st;
				rightVisChars += 2 + (int)V_strlen(styleName); // 2 разделителя + имя стиля
			}
			// PRO/NUB/PRAC слева от времени. PRAC имеет приоритет: игрок в режиме отработки,
			// таймера у него нет вообще. Флаг берём из dataSource, а не из this->player —
			// блок рисует состояние НАБЛЮДАЕМОГО игрока, поэтому зритель видит PRAC сам.
			std::string proNubTag;
			int leftVisChars = 0;
			const bool inPrac = !isReplay && dataSource->pracService && dataSource->pracService->IsInPrac();
			if ((tRunning || inPrac) && !isReplay && dataSource->checkpointService)
			{
				const char *pnColor;
				const char *pnText;
				if (inPrac)
				{
					pnColor = KZ_HUD_C_MUTED;
					pnText = "PRAC";
				}
				else
				{
					bool pro = dataSource->checkpointService->GetTeleportCount() == 0;
					pnColor = pro ? KZ_HUD_C_TIMER : KZ_HUD_C_MUTED;
					pnText = pro ? "PRO" : "NUB";
				}
				char pn[128];
				V_snprintf(pn, sizeof(pn), "<font class='" KZ_HUD_FS_SECONDARY "'><font color='%s'>%s</font></font>&#160;&#160;", pnColor, pnText);
				proNubTag = pn;
				leftVisChars = 2 + (int)V_strlen(pnText);
			}
			// Левый паддинг nbsp'ами (&#160;, рендерится с шириной) ≈ ширины правой части, чтобы центр
			// ВРЕМЕНИ (область «:» в «00:00.00») встал под центр экрана (движок центрирует строку целиком):
			// тогда одиночная скорость снизу окажется ровно под серединой времени. nbsp У́ЖЕ буквы
			// (≈0.5–0.6), поэтому его нужно БОЛЬШЕ, чем «символов» правой части — множитель
			// kLeftPadNbspPerChar под живую подстройку (цель «визуально по центру», не пиксель).
			// Паддинг в том же кегле, что правая часть (SECONDARY), чтобы ширины сопоставлялись.
			// PRO/NUB — реальный контент слева, поэтому из бюджета паддинга вычитаем его ширину
			// (leftVisChars): [pad][PRO][ время ][режим][стиль] — PRO зеркалит режим вокруг центра времени.
			const int kLeftPadNbspPerChar = 2; // nbsp на 1 «символ» правой части; крутить после теста
			std::string leftPad;
			int padBudget = rightVisChars - leftVisChars;
			if (padBudget > 0)
			{
				std::string fs;
				int padCount = padBudget * kLeftPadNbspPerChar;
				for (int i = 0; i < padCount; i++)
				{
					fs += "&#160;"; // nbsp — добивка с шириной (≈0.5–0.6 буквы) под центровку времени
				}
				// std::string-сборка (не char-буфер): padCount растёт с множителем, буфер бы переполнился.
				leftPad = "<font class='" KZ_HUD_FS_SECONDARY "'>" + fs + "</font>";
			}
			// Суффикс состояния «(СТОП)»/«(НА ПАУЗЕ)» УБРАН из худа целиком (ломал перенос строки).
			// Состояние читается по цвету времени: идёт → зелёное фактическое время,
			// не идёт → белое 00:00.00 (см. timerColor/notRunning выше). tSuffix из GetTimerParts
			// больше не используется (сам метод его по-прежнему может отдавать — здесь игнорируем).
			// Порядок как на кибершоке: (паддинг) → PRO/NUB → время → режим → стиль-если-есть.
			V_snprintf(buf, sizeof(buf),
					   "%s%s<font class='" KZ_HUD_FS_TIMER "'><font color='%s'>" KZ_HUD_BRACKET_OPEN "&#160;%s&#160;" KZ_HUD_BRACKET_CLOSE
					   "</font></font>%s%s",
					   leftPad.c_str(), proNubTag.c_str(), timerColor, tTime.c_str(), modeTag.c_str(), styleTag.c_str());
			addLine(buf);
		}
	}

	// --- Строка 2: 2064 (784) [C] — крупная белая скорость (m), мелкий престрейф (s) в
	//        перф/jumpbug/базовом цвете, приписка C при crouch-jump в cj-цвете. Всё — по
	//        текущему условию onGroundSettled (престрейф/C скрыты, когда игрок осел на земле). ---
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
			// Цвет — из палитры стандартного худа (см. KZ_HUD_C_PERF рядом с ней), а НЕ из
			// mhud*Color игрока: те префы про particle-MHUD, и чужое значение делало престрейф
			// невидимым. Зазор — nbsp, как во всех остальных строках (обычный пробел схлопывается).
			const char *tint = KZ_HUD_C_WHITE;
			if (dataSource->IsPerfing() && !dataSource->possibleLadderHop && !dataSource->takeoffFromLadder)
			{
				tint = dataSource->hudService->fromDuckbug ? KZ_HUD_C_JUMPBUG : KZ_HUD_C_PERF;
			}
			char tk[128];
			V_snprintf(tk, sizeof(tk), "&#160;<font class='" KZ_HUD_FS_SECONDARY "'><font color='%s'>(%d)</font></font>", tint,
					   RoundFloatToInt(dataSource->takeoffVelocity.Length2D()));
			takeoff = tk;
			if (dataSource->hudService->crouchJumping)
			{
				takeoff += "&#160;<font class='" KZ_HUD_FS_SECONDARY "'><font color='" KZ_HUD_C_CJ "'>C</font></font>";
			}
		}
		V_snprintf(buf, sizeof(buf), "<font class='" KZ_HUD_FS_SPEED "'><font color='" KZ_HUD_C_CYAN "'>%d</font></font>%s", speed, takeoff.c_str());
		addLine(buf);
	}

	// Активный курс наблюдаемого (в забеге / grace после стопа). Строки 3-4 привязаны к нему:
	// без курса (свежий спавн, не начинал бег) стейдж/PB-WR не показываем — чтобы "PB -- | WR --"
	// не висел вечно. "Строка не прыгает" (через "--") относится к async-догрузке ВО ВРЕМЯ бега:
	// внутри забега курс есть → строка стабильна, пока PB/WR подтягиваются.
	const KZCourseDescriptor *course = (showExtra && !isReplay) ? dataSource->timerService->GetCourse() : nullptr;

	// Курс для строки PB/WR: активный курс, а если игрок ещё НЕ в старт-зоне (только зашёл / стоит
	// вне зоны) — главный курс карты (cyber 0). Так PB/WR (из платформенного api-кэша) показываются
	// сразу при заходе, не дожидаясь входа в старт-зону. Только для живого игрока (не реплей).
	const KZCourseDescriptor *pbwrCourse = course;
	if (!pbwrCourse && showExtra && !isReplay)
	{
		pbwrCourse = KZ::course::GetCourseByCyberNumber(0);
	}

	// --- Строка 3: Stage n/N — ТОЛЬКО многостейджевые карты (stageCount > 1). Лейбл MUTED,
	//        число WHITE. На линейных картах строки нет (без слова Linear). ---
	if (course && course->stageCount > 1)
	{
		V_snprintf(buf, sizeof(buf),
				   "<font class='" KZ_HUD_FS_SECONDARY "'><font color='" KZ_HUD_C_MUTED "'>Stage</font> <font color='" KZ_HUD_C_WHITE "'>%d/%d</font></font>",
				   dataSource->timerService->GetCurrentStage(), course->stageCount);
		addLine(buf);
	}

	// --- Строка 4: || PB 00:55.25 || WR 00:53.50 || — рамки/разделители везде ДВОЙНЫЕ ||  DIM,
	//        лейблы PB/WR MUTED, время PB белое, время WR зелёное. PB/WR — из платформенного
	//        api-кэша (совпадает с сайтом), при его промахе/недоступности — фолбэк на локальные
	//        кэши плагина (globalPBCache/localPBCache для PB; wrCache→srCache для WR). Курс —
	//        pbwrCourse (активный или главный вне старт-зоны).
	//        Центрирование: оба поля ВСЕГДА одной знаковой ширины — реальное «MM:SS.CC» (8 симв.,
	//        FormatTimeHud при hours=0) ИЛИ равноширинный плейсхолдер «--:--.--» (8, DIM) при
	//        отсутствии значения. Равная ширина ⇒ центральный || стоит по построению, без паддинга. ---
	if (pbwrCourse)
	{
		// Плейсхолдер отсутствующего значения — той же знаковой ширины, что формат времени
		// «MM:SS.CC» (8 символов). Держит поля PB/WR равными ⇒ || по центру без добивки.
		const char *timePlaceholder = "--:--.--";

		f64 pbTime = 0.0, wrTime = 0.0;
		bool hasPB = dataSource->timerService->GetHudPBTime(pbTime, pbwrCourse);
		bool hasWR = dataSource->timerService->GetHudWorldRecordTime(wrTime, pbwrCourse);

		// Ячейка значения: <font color>text</font> без паддинга (ширину держит формат/плейсхолдер).
		auto cell = [&](const char *color, const char *text) -> std::string
		{
			char c[64];
			V_snprintf(c, sizeof(c), "<font color='%s'>%s</font>", color, text);
			return std::string(c);
		};

		std::string pbVal, wrVal;
		if (hasPB)
		{
			char pbBuf[32];
			FormatTimeHud(pbTime, pbBuf, sizeof(pbBuf));
			pbVal = cell(KZ_HUD_C_WHITE, pbBuf);
		}
		else
		{
			pbVal = cell(KZ_HUD_C_DIM, timePlaceholder);
		}
		if (hasWR)
		{
			char wrBuf[32];
			FormatTimeHud(wrTime, wrBuf, sizeof(wrBuf));
			wrVal = cell(KZ_HUD_C_TIMER, wrBuf);
		}
		else
		{
			wrVal = cell(KZ_HUD_C_DIM, timePlaceholder);
		}
		V_snprintf(buf, sizeof(buf),
				   "<font class='" KZ_HUD_FS_SECONDARY "'><font color='" KZ_HUD_C_DIM "'>||&#160;</font><font color='" KZ_HUD_C_WHITE "'>PB</font>&#160;%s"
				   "<font color='" KZ_HUD_C_DIM "'>&#160;||&#160;</font><font color='" KZ_HUD_C_WHITE "'>WR</font>&#160;%s"
				   "<font color='" KZ_HUD_C_DIM "'>&#160;||</font></font>",
				   pbVal.c_str(), wrVal.c_str());
		addLine(buf);
	}

	// --- Зазор между верхней группой (таймер/скорость/Stage/PB-WR) и нижней (клавиши/CP-TP/
	//        showpos): маленькая пустая строка в мелком кегле (FS_MINOR), а не полная строка —
	//        органичный отступ, не зияние; экономит высоту под крупные клавиши (l). Только если
	//        обе группы непусты (иначе висячий <br>). Высота center-HTML ограничена (обрезка низа). ---
	bool lowerGroup = showKeys || showCpTp || showPos;
	if (lowerGroup && !html.empty())
	{
		// <br> закрывает строку PB/WR; nbsp мелким кеглем = невысокая строка-зазор; addLine ниже
		// добавит свой <br> перед клавишами. Класс не подхватится → откат на полную строку (не хуже).
		html += "<br><font class='" KZ_HUD_FS_MINOR "'>&#160;</font>";
	}

	// --- Клавиши в стиле MHUD: 2 строки раскладкой клавиатуры, каждая — отдельный addLine
	//        (движок центрирует ряды сам, W встаёт над S). Буквы ВСЕГДА видны: нажата → циан,
	//        отпущена → dim, чтобы читалась раскладка. Подпись «Keys:» убрана — раскладка
	//        самодостаточна. Ряд 1: C W J (C=duck слева, W=forward центр, J=jump справа),
	//        ряд 2: A S D. Зазор между буквами — пара nbsp. ---
	if (showKeys)
	{
		// overlap = одновременно нажаты ПРОТИВОПОЛОЖНЫЕ клавиши (W+S или A+D). Когда настройка
		// hudKeysOverlap ВКЛ и есть overlap — ВСЕ нажатые клавиши красим в KZ_HUD_C_RED вместо
		// обычного циана нажатия; ненажатые остаются dim. Настройка — тот же преф hudKeysOverlap,
		// что у MHUD (читаем через IsMHUDKeysOverlapEnabled с this = настройки получателя), теперь
		// он применяется и к стандартному HTML-худу.
		bool wDown = dataSource->IsButtonPressed(IN_FORWARD);
		bool sDown = dataSource->IsButtonPressed(IN_BACK);
		bool aDown = dataSource->IsButtonPressed(IN_MOVELEFT);
		bool dDown = dataSource->IsButtonPressed(IN_MOVERIGHT);
		bool overlap = (wDown && sDown) || (aDown && dDown);
		const char *pressedColor = (overlap && this->IsMHUDKeysOverlapEnabled()) ? KZ_HUD_C_RED : KZ_HUD_C_CYAN;
		auto key = [&](const char *label, bool down)
		{
			char k[64];
			V_snprintf(k, sizeof(k), "<font color='%s'>%s</font>", down ? pressedColor : KZ_HUD_C_DIM, label);
			return std::string(k);
		};
		bool jump = dataSource->hudService->jumpedThisTick || dataSource->IsButtonPressed(IN_JUMP);
		const char *sep = "&#160;&#160;"; // пара nbsp — читаемый зазор между буквами
		// Ряд 1 (верх): C W J.
		std::string row1 = key("C", dataSource->IsButtonPressed(IN_DUCK)) + sep + key("W", wDown) + sep + key("J", jump);
		// Ряд 2 (низ): A S D — W окажется над S при центровке движком.
		std::string row2 = key("A", aDown) + sep + key("S", sDown) + sep + key("D", dDown);
		addLine(std::string("<font class='" KZ_HUD_FS_KEYS "'>") + row1 + "</font>");
		addLine(std::string("<font class='" KZ_HUD_FS_KEYS "'>") + row2 + "</font>");
	}

	// --- CP/TP (per-element тумблер hudCpTp, деф. вкл) — ПОД клавишами. ВЗАИМОИСКЛЮЧАЕТСЯ с showpos:
	//        когда включён showpos, строка координат ЗАМЕНЯЕТ CP/TP (а не добавляется сверху), иначе
	//        при всех включённых тумблерах низ панели center-HTML обрезал координаты. showpos игроки
	//        используют редко, так что временно скрыть CP/TP при нём не страшно (решение пользователя).
	//        Стиль под палитру (лейблы WHITE, числа WHITE, разделитель DIM). ---
	if (showCpTp && !showPos)
	{
		i32 cpIndex = isReplay ? KZ::replaysystem::GetCurrentCpIndex() : dataSource->checkpointService->GetCurrentCpIndex();
		i32 cpCount = isReplay ? KZ::replaysystem::GetCheckpointCount() : dataSource->checkpointService->GetCheckpointCount();
		i32 tpCount = isReplay ? KZ::replaysystem::GetTeleportCount() : (i32)dataSource->checkpointService->GetTeleportCount();
		V_snprintf(buf, sizeof(buf),
				   "<font class='" KZ_HUD_FS_SECONDARY "'><font color='" KZ_HUD_C_WHITE "'>CP</font> <font color='" KZ_HUD_C_WHITE "'>%d/%d</font> "
				   "<font color='" KZ_HUD_C_DIM "'>|</font> <font color='" KZ_HUD_C_WHITE "'>TP</font> <font color='" KZ_HUD_C_WHITE "'>%d</font></font>",
				   cpIndex, cpCount, tpCount);
		addLine(buf);
	}

	// --- Координаты и углы (!showpos). Тумблер — настройка получателя (this), данные —
	//        наблюдаемого (dataSource). В компакте скрыто (как раньше). Стиль как раньше. ---
	if (showPos)
	{
		Vector origin;
		QAngle angles;
		dataSource->GetOrigin(&origin);
		dataSource->GetAngles(&angles);
		std::string posText =
			KZLanguageService::PrepareMessageWithLang(language, "HUD - Position Text", origin.x, origin.y, origin.z, angles.x, angles.y);
		V_snprintf(buf, sizeof(buf), "<font class='" KZ_HUD_FS_SECONDARY "'><font color='" KZ_HUD_C_MUTED "'>%s</font></font>", posText.c_str());
		addLine(buf);
	}

	return html;
}

void KZHUDService::DrawPanels(KZPlayer *player, KZPlayer *target)
{
	KZHUDService *cfg = target->hudService;

	bool available = KZHUDService::IsMHUDAvailable();
	// hudType: 0 = Standard (HTML-панель), 1 = MHUD (particle-оверлей), 2 = Off (ничего).
	//
	// Particle-путь — ТОЛЬКО для живого владельца (player == target && target->IsAlive()).
	// Причина не в правах, а в движке: у CS2 нет честного screen-space API для HUD поверх
	// экрана произвольного клиента — particle-MHUD физически позиционируется относительно
	// взгляда ВЛАДЕЛЬЦА (это апстримный контракт, апстримный IsAlive-гейт был ровно про
	// «owner жив и смотрит своими глазами»). cyb.34 разрешил тот же путь спектатору
	// (player != target) на тех же particle-хендлах: там позиция считается по взгляду
	// НАБЛЮДАЕМОГО, а не самого спектатора, и при поворотах цели оверлей уезжает за
	// экран спектатора (живой баг cyb.36). Поэтому спектатор — всегда HTML: needHtml
	// ниже подхватывает это автоматически, т.к. useParticles=false как только player != target.
	// Мёртвый игрок без цели наблюдения (свой труп / фриролл) сюда вообще не попадает —
	// DrawPanels для него не вызывается, particle'ы гасятся отдельно в
	// KZPlayer::OnPhysicsSimulatePost.
	bool useParticles = available && cfg->GetHudType() == HUD_TYPE_MHUD && player == target && target->IsAlive();

	if (useParticles)
	{
		// player == target здесь всегда (см. гейт выше) — источник данных — сам владелец.
		target->hudService->UpdateParticles(player);
	}
	else
	{
		// Гасим particle'ы: MHUD недоступен, не выбран, игрок мёртв, либо это спектатор
		// (particle-путь спектатору принципиально не положен, см. комментарий выше).
		target->hudService->DestroyAllParticles();
	}

	// Тип Off — единственный рычаг «выключить худ целиком»: не рисуем ни particle (уже
	// погашены выше), ни HTML-панель. Заменил прежний тумблер showPanel (см. kz_panel).
	if (cfg->GetHudType() == HUD_TYPE_OFF)
	{
		return;
	}

	// Yield the center channel while a cs2menus HTML menu is open.
	if (g_pMenus && g_pMenus->GetActiveMenuType(target->GetPlayerSlot().Get()) == MenuType::Html)
	{
		return;
	}
	const char *language = target->languageService->GetLanguage();

	std::string htmlText;

	// HTML fallback: Standard type is selected, OR no addons are available at all
	// (no MultiAddonManager/assets), OR the particle path isn't active for some other
	// reason — target is dead, or this is a spectator (player != target, see the gate
	// above). needHtml and useParticles must stay mutually exclusive, otherwise the
	// recipient ends up with no HUD at all. (Off уже отсеян ранним return выше.)
	bool needHtml = !available || cfg->GetHudType() == HUD_TYPE_STANDARD || !useParticles;
	if (needHtml)
	{
		// Единый HTML-путь: BuildVersionCHud сам уважает компакт-режим (только строки 1-2,
		// внутри по cfg->IsCompactPanel()). masterMode=true — per-element тумблеры. Развязка
		// data/settings сохраняется: вызываем на cfg (настройки получателя) с dataSource=player
		// (данные наблюдаемого), внутри цвета/язык берутся с this=cfg, данные — с dataSource.
		htmlText = cfg->BuildVersionCHud(player, /*suppressSpeed=*/false, /*suppressTimer=*/false, /*suppressKeys=*/false,
										 /*masterMode=*/true, language);
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

SCMD(kz_showpos, SCFL_HUD | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	bool next = !player->optionService->GetPreferenceBool("showPos", false);
	player->optionService->SetPreferenceBool("showPos", next);
	player->languageService->PrintChat(true, false, next ? "HUD Option - Show Pos - Enable" : "HUD Option - Show Pos - Disable");
	return MRES_SUPERCEDE;
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
	// Без аргумента: включить/выключить худ целиком. Роль бывшего тумблера showPanel теперь
	// у типа Off (единый рычаг), поэтому !panel переключает тип Off ↔ Standard.
	if (player->hudService->GetHudType() == KZHUDService::HUD_TYPE_OFF)
	{
		player->hudService->SetHudType(KZHUDService::HUD_TYPE_STANDARD);
		player->languageService->PrintChat(true, false, "HUD Option - Info Panel - Enable");
	}
	else
	{
		player->hudService->SetHudType(KZHUDService::HUD_TYPE_OFF);
		player->languageService->PrintChat(true, false, "HUD Option - Info Panel - Disable");
	}
	return MRES_SUPERCEDE;
}
