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
#include "kz/style/kz_style.h"            // GetStyleName для лейбла стиля (деф. Normal) в строке 1
#include "kz/mode/kz_mode.h"              // KZModeService::GetModeShortName для метки режима в строке 1
#include "kz/replays/cyb_replay_common.h" // MapMode — тот же маппинг режима, что у PB/WR-фетча
#include "kz/jumpstats/kz_jumpstats.h"    // JumpType_Jumpbug/jumps.Tail() для приписки JB у скорости

#include <algorithm>

#include <vendor/MultiAddonManager/public/imultiaddonmanager.h>
extern IMultiAddonManager *g_pMultiAddonManager;

#include <vendor/mm-cs2menus/src/public/ics2menus.h>
extern ICS2Menus *g_pMenus;
// Умеет ли текущая сборка cs2menus SetSlotStatus (интерфейс 005) — см. cs2kz.cpp: со старой
// сборкой указатель получен по 004, и нового метода в его vtable нет.
extern bool g_menusHasSlotStatus;

#include "tier0/memdbgon.h"

// Формат времени для худа: mm:ss.cc (сотые), как у кибершока. Отдельно от utils::FormatTime
// (тысячные) — тот нужен другим местам (чат/сабмишен/реплеи), его формат не трогаем.
// Часы добавляются при необходимости. time всегда >= 0 (таймер/PB/WR).
static_function void FormatTimeHudCs(i32 rounded, char *output, u32 length)
{
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

static_function void FormatTimeHud(f64 time, char *output, u32 length)
{
	FormatTimeHudCs(RoundFloatToInt(time * 100), output, length);
}

// Heartbeat нижней панели: centre-канал гасит текст сам через несколько секунд, поэтому
// неизменившийся текст изредка переотправляется как есть (без пересборки). Одна секунда —
// с запасом до угасания и в 128 раз реже, чем слать каждый тик.
#define KZ_HUD_BOTTOM_HEARTBEAT 1.0f

// Heartbeat строки показаний в панели меню — на порядок чаще. Причина не в угасании (её
// держит сам cs2menus), а в том, что он ЧИСТИТ статус в EndDisplay: переход в подменю
// закрывает один показ и открывает другой, и на статичной цели (пауза на реплее) слепок не
// меняется — показания пропадали бы до целой секунды. Повтор того же текста в cs2menus
// бесплатен (сравнение строки), поэтому цена частоты — только взятие его мьютекса.
#define KZ_HUD_BOTTOM_MENU_HEARTBEAT 0.1f

// Heartbeat HTML-канала минимал-худа: show_survival_respawn_status живёт ровно duration=1s
// (см. utils::PrintHTMLCentre), heartbeat в 1.0s мигал бы на границе — переотправляем вдвое
// чаще. Centre-канал минимала живёт общим KZ_HUD_BOTTOM_HEARTBEAT (он гаснет медленнее).
#define KZ_HUD_MINIMAL_HTML_HEARTBEAT 0.5f

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
	// Слот реально освобождается (дисконнект) — только здесь гасим *Active-флаги:
	// ClearBottomPanel()/ClearMinimalHud() для НОВОГО игрока в этот слот не должны считать
	// себя обязанными клиру каналов, которыми никогда не владели.
	this->bottomPanelActive = false;
	this->minimalCentreActive = false;
	// Кэши отправки нижней панели и минимал-худа (слепки/тексты/heartbeat).
	this->ResetBottomPanelCache();
	this->DestroyAllParticles();
}

// Сброс кэшей отправки нижней панели и минимал-худа БЕЗ *Active-флагов: раунд-старт
// (OnRoundStart, в т.ч. посреди карты — например кик реплей-бота) не освобождает каналы
// получателя, поэтому не должен подавлять следующий клир-кадр (ClearBottomPanel/
// ClearMinimalHud полагаются на флаги, чтобы не слать лишний #SFUI_EmptyString).
// Флаги гасятся только в Reset(), где слот реально освобождён (дисконнект).
void KZHUDService::ResetBottomPanelCache()
{
	this->bottomStateValid = false;
	this->lastBottomText[0] = '\0';
	this->lastBottomSendTime = 0.0;
	this->lastMinimalCentreText[0] = '\0';
	this->lastMinimalCentreSendTime = 0.0;
	this->lastMinimalHtmlText[0] = '\0';
	this->lastMinimalHtmlSendTime = 0.0;
}

// Раунд-старт (в т.ч. первый на новой карте — mp_restartgame из OnActivateServer). Главное —
// смена карты: curtime отсчитывается от её загрузки, а Reset() игрока на выделенном сервере
// при смене карты не зовётся — переживший смену lastBottomSendTime оказался бы «в будущем»
// и заглушил heartbeat, а совпавший со старым слепок (типовой idle: CP 0/0 | TP 0) — и
// отправку по изменению: низ пропал бы на всю карту. Свежий кэш шлёт всё первым же тиком.
void KZHUDService::OnRoundStart()
{
	for (int i = 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (!player || !player->hudService)
		{
			continue;
		}
		player->hudService->ResetBottomPanelCache();
	}
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

// Наша HTML-панель и нижняя панель — два НЕЗАВИСИМО позиционируемых движком канала: ни один
// из них не знает о высоте другого, поэтому достаточно высокая панель (все элементы + клавиши
// в 2 ряда) просто накрывает низ, и нижнюю строку не видно (репорт 05.08). Развести их можно
// только отступом — пустыми строками над содержимым нижней панели. Порог — высота верха,
// начиная с которой он дотягивается до низа; дальше по строке отступа на строку верха, с
// потолком, чтобы низ не уехал за край экрана.
//
// ЧЕГО ЭТОТ РЫЧАГ НЕ УМЕЕТ (репорты 05.08 по cyb.104, из-за них дефолт CP/TP переехал в саму
// панель — см. kz_hud_cptp_in_panel): строка отступа НЕ равна строке верхнего оверлея. Движок
// вписывает ВЕСЬ текст канала в рамку фиксированной высоты, поэтому каждая добавленная строка
// уменьшает кегль (тестер: «шрифт cp/tp мельчает по мере включения элементов худа») и двигает
// содержимое вниз только до нижнего края рамки — под достаточно высоким верхом низ остаётся
// накрытым при ЛЮБОМ отступе. За пределом вместимости хвостовые строки просто не рисуются
// (пропавший второй ряд клавиш под !rpmenu). Отсюда правило: чем меньше строк уходит в этот
// канал, тем лучше; отступ — не первый инструмент, а последний.
//
// ПОД ОТКРЫТЫМ Html-МЕНЮ ОТСТУП НЕ РАБОТАЕТ ВОВСЕ и потому там отключён (репорт 07.08 по
// cyb.107): рамка обоих plain-text-каналов лежит в области меню, и сдвинуть содержимое за её
// пределы отступ не может ни в centre (остаётся под меню), ни в alert (остаётся в его
// середине, только поверх) — а кегль он мельчит исправно: 6 строк отступа + строка худа дали
// «шрифт мельчайший». Эти cvar'ы остаются рычагом только для развода низа с НАШЕЙ
// HTML-панелью (kz_hud_cptp_in_panel 0).
//
// Пороги — cvar'ы, а НЕ константы: реальная высота зависит от кегля, языка и разрешения
// клиента, серверу она недоступна, а перебирать значения пересборкой форка (CI + публикация
// + раскатка) — час на итерацию. Крутятся по rcon на живом сервере во время смоука.
static CConVar<int> kz_hud_bottom_pad_free("kz_hud_bottom_pad_free", FCVAR_NONE,
										   "Lines of the top overlay the bottom panel survives without any padding.", 3);
static CConVar<int> kz_hud_bottom_pad_max("kz_hud_bottom_pad_max", FCVAR_NONE, "Maximum padding of the bottom panel, in lines (0 = disabled).", 6);

// Бюджет ВСЕГО текста centre-канала в строках (отступ + содержимое). Канал показывает
// ограниченное число строк: лишние с хвоста не рисуются, а сам текст движок ужимает под
// фиксированную рамку — чем больше строк, тем мельче кегль. Репорт 05.08: под открытым
// !rpmenu (отступ 5 + четыре строки худа = 9) пропадал ВТОРОЙ ряд клавиш — поэтому фикс
// cyb.104 «2 ряда клавиш в спеках» и не был виден: ряд отправлялся, но не рисовался.
// Отступ уступает содержимому: сначала строки худа, отступ — только в остаток бюджета.
// Значение эмпирическое (реальная вместимость зависит от клиента) — крутится по rcon.
// С 07.08 под меню отступа нет, так что бюджет ограничивает только развод с нашей HTML-панелью.
static CConVar<int> kz_hud_bottom_max_lines("kz_hud_bottom_max_lines", FCVAR_NONE,
											"Bottom centre panel budget: lines left for padding on top of the content.", 7);

// CP/TP обновлённого стиля: 1 (деф.) — последняя строка HTML-панели, 0 — прежняя отдельная
// нижняя панель centre-канала с отступом. Дефолт сменён 05.08 по трём репортам сразу: в
// centre-канале кегль зависит от числа строк (каждая строка отступа мельчила CP/TP —
// «ооочень маленький шрифт»), а вниз отступ двигает лишь до нижнего края рамки канала,
// поэтому высокая HTML-панель всё равно накрывала низ («наслаивается»). В HTML-панели кегль
// задаётся классом и не плывёт, а панель — один блок, сама себя накрыть не может.
// Рубильник оставлен, чтобы вернуть прежнюю раскладку по rcon, не пересобирая плагин.
static CConVar<bool> kz_hud_cptp_in_panel("kz_hud_cptp_in_panel", FCVAR_NONE,
										  "Draw the CP/TP line as the last row of the HTML panel instead of a separate bottom panel.", true);

// Канал нижней панели: 0 (деф.) — centre-print, 1 — alert. Запаска из спеки фикс-пака на
// случай, если под открытым Html-меню centre-канал остаётся ПОД меню при любом отступе
// (отступ упирается в нижний край рамки канала). Alert — третий канал апстрима (его пустая
// фраза «HUD - Alert Text» описана как «actually more center»), рисуется движком в своём
// месте: проверяется одной rcon-командой на канарейке, без пересборки плагина.
// Проверено на канарейке 07.08 (cyb.107, kz_hud_bottom_channel 1, скриншот тестера): alert
// действительно рисуется ПОВЕРХ меню — но в его же середине. То есть канал меняет только
// порядок перекрытия, а не место, и «под меню» отдельным каналом недостижимо: позицию всех
// трёх каналов задаёт клиент, а под меню текст попадёт только из САМОЙ панели меню (cs2menus).
// ПОЭТОМУ под открытым Html-меню этот cvar НИ НА ЧТО не влияет, пока рядом cs2menus 005:
// там низ уходит строкой в панель меню (SetSlotStatus), а канал остаётся только фолбэком
// для старой сборки cs2menus и обычным путём CP/TP при kz_hud_cptp_in_panel 0.
static CConVar<int> kz_hud_bottom_channel("kz_hud_bottom_channel", FCVAR_NONE, "Bottom panel channel: 0 = centre print, 1 = alert.", 0);

// Жёсткий потолок отступа: значения cvar'ов недоверенные (их крутят по rcon на живом
// сервере), а padLines возвращается в u8 — kz_hud_bottom_pad_max 300 давал (u8)297 = 41
// пустую строку, т.е. низ уезжал за экран. Больше десятка строк отступа не осмысленно ни
// на одном разрешении.
#define KZ_HUD_BOTTOM_PAD_HARD_MAX 16

// contentLines — сколько строк займёт САМО содержимое низа: отступ не имеет права вытеснить
// его за бюджет канала (иначе хвост просто не рисуется, см. kz_hud_bottom_max_lines).
// Бюджет ограничивает ТОЛЬКО отступ: содержимое здесь не режется намеренно — потерять
// строку худа хуже, чем вылезти за эмпирическую вместимость канала.
static_function u8 BottomPadLines(int linesAbove, int contentLines)
{
	// Клэмпы обязательны: freeLines -1 дал бы отступ даже пустому верху, maxPad 300 —
	// переполнение u8 на возврате.
	const int freeLines = (std::max)(0, kz_hud_bottom_pad_free.Get());
	int maxPad = (std::min)(KZ_HUD_BOTTOM_PAD_HARD_MAX, kz_hud_bottom_pad_max.Get());
	// Остаток бюджета после содержимого; отрицательное значение (содержимое само не влезло)
	// трактуем как «отступа нет вовсе».
	// Клэмп бюджета — как у maxPad выше: значение приходит по rcon, INT_MIN дал бы
	// знаковое переполнение на вычитании.
	const int budget = (std::max)(0, (std::min)(KZ_HUD_BOTTOM_PAD_HARD_MAX, kz_hud_bottom_max_lines.Get()));
	const int budgetLeft = budget - (std::max)(0, contentLines);
	maxPad = (std::min)(maxPad, budgetLeft);
	if (maxPad <= 0)
	{
		return 0; // 0 — способ выключить развод целиком, не пересобирая плагин
	}
	const int pad = linesAbove - freeLines;
	if (pad <= 0)
	{
		return 0;
	}
	return (u8)(pad > maxPad ? maxPad : pad);
}

static_function std::string PadAbove(const std::string &text, u8 padLines)
{
	if (!padLines || text.empty())
	{
		return text;
	}
	return std::string(padLines, '\n') + text;
}

// Число строк в готовом тексте HTML-панели: разделитель рядов там <br>.
static_function int CountHtmlLines(const std::string &html)
{
	if (html.empty())
	{
		return 0;
	}
	int lines = 1;
	for (size_t pos = html.find("<br>"); pos != std::string::npos; pos = html.find("<br>", pos + 4))
	{
		lines++;
	}
	return lines;
}

// Выбросить пустые сегменты из строки, склеенной фиксированным разделителем. Нужен минимал-
// стилю: его композиция задана фразой («{checkpoint_text}\n{timer_text}»), и выключенный
// тумблером элемент подставляется пустой строкой — без чистки на его месте остаётся пустой
// ряд или висячий <br>. Порядок сегментов и сам разделитель при этом остаются из фразы.
static_function std::string DropEmptySegments(const std::string &text, const char *sep)
{
	const size_t sepLen = V_strlen(sep);
	std::string out;
	out.reserve(text.size());
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t next = text.find(sep, pos);
		const size_t end = next == std::string::npos ? text.size() : next;
		if (end > pos)
		{
			if (!out.empty())
			{
				out += sep;
			}
			out.append(text, pos, end - pos);
		}
		if (next == std::string::npos)
		{
			break;
		}
		pos = next + sepLen;
	}
	return out;
}

// Престрейф в скобках и приписка C кибершоковской панели — ФИКСИРОВАННАЯ палитра, не
// MHUD-префы игрока: префы mhud*Color принадлежат particle-MHUD («Внешний вид MHUD»), и
// сохранённое там экзотическое значение красило престрейф в невидимый цвет на тёмной панели
// (баг 25.07: у игрока mhudSpeedColor = чёрный ⇒ (престрейф) не виден вне перфа).
// Объявлены до GetSpeedText: жёлтый KZ_HUD_C_JUMPBUG красит и бейдж JB минимал-стиля.
#define KZ_HUD_C_PERF    "#40FF40" // престрейф после перфа
#define KZ_HUD_C_JUMPBUG "#FFFF20" // престрейф после jumpbug/duckbug
#define KZ_HUD_C_CJ      "#71EEB8" // приписка C (crouch-jump)

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
	// JB против C: строгая классификация jumpbug'а — тип последнего прыжка (jumps.Tail() —
	// текущий, если ещё в воздухе; Jump::End() на приземлении может дотюнить тип, поэтому
	// перечитываем, пока индикатор виден) — та же ветка, что в BuildVersionCHud. JB
	// приоритетнее приписки C; красится тем же фиксированным жёлтым бейджа JB, что и в
	// BuildVersionCHud (KZ_HUD_C_JUMPBUG), — канал html, цвет легален (сосед C цветной).
	bool isJumpbug = src->jumpstatsService->jumps.Count() > 0 && src->jumpstatsService->jumps.Tail().GetJumpType() == JumpType_Jumpbug;
	std::string suffixText;
	if (isJumpbug)
	{
		suffixText = " <font color='" KZ_HUD_C_JUMPBUG "'>JB</font>";
	}
	else if (src->hudService->crouchJumping)
	{
		suffixText = std::string(" ") + cjBuf + "C</font>";
	}
	return KZLanguageService::PrepareMessageWithLang(language, "HUD - Speed Text (Takeoff)", velocity.Length2D(), colorBuf,
													 src->takeoffVelocity.Length2D(), suffixText.c_str());
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

// Числовое ядро состояния таймера: время/флаги БЕЗ строк и аллокаций — источник для
// строки таймера кибершоковского худа (GetTimerParts). Логика как в GetTimerText
// (реплей-бот / обычный забег / grace после стопа); данные — this->player.
// outIdleZero — обычный игрок в простое: нулевой таймер, суффиксы стопа/паузы не показываются.
// false — только реплей-бот, которому показывать нечего.
bool KZHUDService::GetTimerNumbers(f64 &outTime, bool &outRunning, bool &outPaused, bool &outIdleZero)
{
	outTime = 0.0;
	outRunning = false;
	outPaused = false;
	outIdleZero = false;

	if (KZ::replaysystem::IsReplayBot(this->player))
	{
		f64 time = KZ::replaysystem::GetTime();
		outPaused = KZ::replaysystem::GetPaused();
		outRunning = KZ::replaysystem::GetEndTime() == 0.0f;
		// Таймер не показываем, если и текущее, и конечное время нулевые.
		if (time == 0.0f && KZ::replaysystem::GetEndTime() == 0.0f)
		{
			return false;
		}
		outTime = outRunning ? time : KZ::replaysystem::GetEndTime();
		return true;
	}
	if (this->player->timerService->GetTimerRunning() || this->ShouldShowTimerAfterStop())
	{
		outRunning = this->player->timerService->GetTimerRunning();
		outTime = outRunning ? this->player->timerService->GetTime() : this->currentTimeWhenTimerStopped;
		outPaused = this->player->timerService->GetPaused();
		return true;
	}
	// Обычный игрок в простое: нулевой таймер (цвет белый / текст 00:00.00 задаёт
	// BuildVersionCHud по outRunning=false), без суффиксов.
	outIdleZero = true;
	return true;
}

// Разбор состояния таймера для кибершоковского худа: время в формате до сотых.
// outRunning — идёт ли активный забег (пауза = идёт → true); цвет/обнуление времени по
// нему решает BuildVersionCHud (состояние стоп/пауза читается по цвету, текстового
// суффикса у худа нет).
bool KZHUDService::GetTimerParts(std::string &outTime, bool &outRunning)
{
	f64 time;
	bool paused, idleZero;
	if (!this->GetTimerNumbers(time, outRunning, paused, idleZero))
	{
		return false;
	}
	char timeText[64];
	FormatTimeHud(time, timeText, sizeof(timeText));
	outTime = timeText;
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
// Скобок вокруг времени больше нет (убраны 25.07 по решению пользователя) — прежние
// #define KZ_HUD_BRACKET_* и вариант с уголковыми ⌈ ⌋ удалены вместе с ними.

// Размеры шрифта строк стандартного HTML-худа (класс движка внутри color-тега, см. ниже).
// Явная иерархия по важности: скорость > таймер > вторичная инфа > клавиши/стиль. ВНИМАНИЕ:
// имена классов — гипотезы (движок кибершока вероятно знает fontSize-l/m/sm/s); если класс
// неизвестен, размер молча откатится на дефолт — проверить вживую. Все четыре в одном месте:
// иерархия правится одной строкой после живого теста.
#define KZ_HUD_FS_SPEED     "fontSize-l"  // скорость — главный акцент
#define KZ_HUD_FS_TIMER     "fontSize-l"  // таймер — крупный, одного кегля со скоростью (по фото кибершока)
#define KZ_HUD_FS_SECONDARY "fontSize-sm" // PB/WR, CP/TP, престрейф, Stage, координаты — вторичная инфа
#define KZ_HUD_FS_KEYS      "fontSize-m"  // клавиши (оба варианта раскладки: 2 ряда / одна строка). Был l — уменьшен на ступень по просьбе тестера; заодно меньше риск обрезки низа панели
#define KZ_HUD_FS_MINOR     "fontSize-s"  // метка стиля — наименее заметное

// Метка режима наблюдаемого ЗАГЛАВНЫМИ (CKZ/KZT/VNL): короткое имя через
// CybReplayCommon::MapMode (тот же маппинг/вайтлист, что у PB/WR-фетча в kz_timer.cpp).
// Пустая строка = кастомный режим сверх этих трёх (метку не показываем) или нет modeService.
// Используется строкой 1 кибершоковского худа; реплей-бот отсеивается вызывающим.
static_function void GetModeTagUpper(KZPlayer *dataSource, char *out, i32 size)
{
	out[0] = '\0';
	if (!dataSource->modeService)
	{
		return;
	}
	const char *modeApi = CybReplayCommon::MapMode(dataSource->modeService->GetModeShortName());
	i32 i = 0;
	for (; i < size - 1 && modeApi[i]; i++)
	{
		out[i] = (modeApi[i] >= 'a' && modeApi[i] <= 'z') ? (char)(modeApi[i] - 32) : modeApi[i];
	}
	out[i] = '\0';
}

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

	// Компакт-режим — это ВЁРСТКА, а не отдельный набор элементов: он оставляет только строки
	// 1-2 (таймер+скорость), убирая клавиши/стейдж/PB-WR/showpos. Сами элементы в нём подчиняются
	// тем же per-element тумблерам, что и в полной раскладке (единый контракт: тумблер действует
	// на всех путях худа; «выключить целиком» — это hudType=Off, а не компакт).
	// showExtra — стейдж/PB-WR (только полный режим). Клавиши/showpos/CP-TP — по своим тумблерам.
	bool showTimer = masterMode ? (this->IsMHUDTimerEnabled() && !suppressTimer) : !suppressTimer;
	bool showSpeed = masterMode ? (this->IsMHUDSpeedEnabled() && !suppressSpeed) : !suppressSpeed;
	bool showKeys = compact ? false : (masterMode ? (this->IsMHUDKeysEnabled() && !suppressKeys) : !suppressKeys);
	bool showExtra = !compact;
	// showpos — тумблер получателя (this->player), данные наблюдаемого. Считаем заранее: нужен
	// и для координат, и для решения о зазоре нижней группы (клавиши/CP-TP/showpos).
	bool showPos = !compact && this->player->optionService->GetPreferenceBool("showPos", false);
	// CP/TP — нижняя строка панели (гейт hudCpTp получателя). Компакт её НЕ убирает: у элемента
	// свой тумблер, и в прежней нижней панели он тоже переживал компакт. Без ветки masterMode
	// (в отличие от соседей выше): suppress-флага у CP/TP нет и быть не может — particle-MHUD
	// такого элемента не рисует, дублировать нечего, поэтому тумблер действует всегда.
	bool showCpTp = kz_hud_cptp_in_panel.Get() && this->IsMHUDCpTpEnabled();

	// Типографика (иерархия): скорость — KZ_HUD_FS_SPEED, таймер — KZ_HUD_FS_TIMER, вторичная инфа
	// (PB/WR, CP/TP, престрейф, Stage, координаты) — KZ_HUD_FS_SECONDARY, клавиши и метка стиля —
	// KZ_HUD_FS_MINOR (значения классов — в #define рядом с палитрой выше, правятся одной строкой).
	// Классы вкладываем В color-теги: <font class='...'><font color='...'>X</font></font>.
	// Это прогрессивное улучшение — если движок не подхватит класс, размер молча дефолтный,
	// но цвета и раскладка остаются корректными (класс лишь меняет кегль, не текст/цвет).

	// --- Строка 1: 00:07.96 CKZ Стиль — время: зелёное когда идёт/пауза, белое 00:00.00
	//        когда стоп/idle; рядом метка режима и метка стиля MUTED (s, только активный
	//        не-деф. стиль). Скобок вокруг времени нет (убраны 25.07). ---
	if (showTimer)
	{
		std::string tTime;
		bool tRunning = false;
		if (dataSource->hudService->GetTimerParts(tTime, tRunning))
		{
			// Идёт забег или пауза (tRunning) → зелёный + фактическое время. Стоп/idle обычного
			// игрока → белый + 00:00.00. Реплей-бот не трогаем (свой финальный тайм остаётся).
			const bool inPrac = dataSource->pracService && dataSource->pracService->IsInPrac();
			const bool pracTimeRunning = inPrac && dataSource->pracService->IsPracTimeRunning();
			bool notRunning = !tRunning && !isReplay;
			const char *timerColor = notRunning ? KZ_HUD_C_WHITE : KZ_HUD_C_TIMER;
			if (notRunning)
			{
				char stoppedText[64];
				if (pracTimeRunning)
				{
					// В prac настоящий таймер стоит by design, а время идёт по prac-часам —
					// показываем ИХ и зелёным, как любое живое время. Чьё это время, объясняет
					// метка PRAC слева.
					FormatTimeHud(dataSource->pracService->GetPracTime(), stoppedText, sizeof(stoppedText));
					tTime = stoppedText;
					timerColor = KZ_HUD_C_TIMER;
				}
				else if (inPrac)
				{
					// Часы стоят: действующей попытки нет (ноуклип обнулил её / свободный prac до
					// первого касания старта). Плейсхолдер той же ширины, что «MM:SS.CC», DIM —
					// тот же идиом, что у пустых PB/WR ниже. Белое 00:00.00 читалось бы как
					// «время ноль, часы вот-вот пойдут», а они не пойдут без старта или !practp.
					tTime = "--:--.--";
					timerColor = KZ_HUD_C_DIM;
				}
				else
				{
					FormatTimeHud(0.0, stoppedText, sizeof(stoppedText));
					tTime = stoppedText;
				}
			}
			// Метка режима — рядом с временем, ЗАГЛАВНЫМИ (CKZ/KZT/VNL), как на кибершоке «[..] CKZ»
			// (GetModeTagUpper; пусто = кастомный режим → метки нет). Реплей-бот пропускаем
			// (своего режима как у игрока нет).
			// Видимая ширина правой части («&#160;&#160;режим» + «&#160;&#160;стиль») в «символах» —
			// нужна для левого паддинга, чтобы ВРЕМЯ встало по центру экрана (см. leftPad ниже).
			int rightVisChars = 0;
			std::string modeTag;
			if (!isReplay)
			{
				char up[8];
				GetModeTagUpper(dataSource, up, sizeof(up));
				if (up[0])
				{
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
			// PRO/NUB — тот же критерий, что у Pro/Standard-типа времени таймера
			// (см. KZTimerService::GetCurrentTimeType: GetTeleportCount() == 0 -> PRO).
			std::string proNubTag;
			int leftVisChars = 0;
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
			// PRO/NUB/PRAC — реальный контент слева, поэтому из бюджета паддинга вычитаем его ширину
			// (leftVisChars): [pad][PRO/NUB/PRAC][ время ][режим][стиль] — метка зеркалит режим вокруг центра времени.
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
			// не идёт → белое 00:00.00 (см. timerColor/notRunning выше).
			// Порядок как на кибершоке: (паддинг) → PRO/NUB/PRAC → время → режим → стиль-если-есть.
			// Квадратные скобки вокруг времени убраны (решение пользователя 25.07) — время
			// само по себе читается, скобки только шумели. Симметричные nbsp внутри них ушли
			// вместе с ними: центровку они не держали (её держит leftPad, посчитанный выше).
			V_snprintf(buf, sizeof(buf), "%s%s<font class='" KZ_HUD_FS_TIMER "'><font color='%s'>%s</font></font>%s%s", leftPad.c_str(),
					   proNubTag.c_str(), timerColor, tTime.c_str(), modeTag.c_str(), styleTag.c_str());
			addLine(buf);
		}
	}

	// --- Строка 2: 2064 (784) [JB] — крупная белая скорость (m), мелкий престрейф (s) в
	//        перф/jumpbug/базовом цвете, приписка JB при строго классифицированном jumpbug
	//        (KZ_HUD_C_JUMPBUG) либо C при crouch-jump (KZ_HUD_C_CJ) — взаимоисключающе, JB
	//        приоритетнее. Всё — по текущему условию onGroundSettled (престрейф/C/JB скрыты,
	//        когда игрок осел на земле). ---
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
			// JB против C: перечитываем классификацию, пока индикатор виден (grace) — тип
			// ставится на отрыве, но Jump::End() на приземлении может дотюнить классификацию;
			// jumps.Tail() это последний (текущий, если ещё в воздухе) прыжок (см.
			// Jump::GetJumpType, kz_jumpstats.h:316-319). JB приоритетнее.
			bool isJumpbug =
				dataSource->jumpstatsService->jumps.Count() > 0 && dataSource->jumpstatsService->jumps.Tail().GetJumpType() == JumpType_Jumpbug;
			if (isJumpbug)
			{
				takeoff += "&#160;<font class='" KZ_HUD_FS_SECONDARY "'><font color='" KZ_HUD_C_JUMPBUG "'>JB</font></font>";
			}
			else if (dataSource->hudService->crouchJumping)
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
	// Тумблер проверяем ДО фолбэка: GetCourseByCyberNumber — линейный поиск по всем курсам,
	// а строится худ каждый тик — при выключенной строке PB/WR платить за него незачем.
	const bool showPbWr = this->IsMHUDPbWrEnabled();
	const KZCourseDescriptor *pbwrCourse = course;
	if (!pbwrCourse && showPbWr && showExtra && !isReplay)
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
	//        Секция целиком отключается тумблером hudPbWr (Элементы → «Строка PB/WR»).
	if (pbwrCourse && showPbWr)
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

	// --- Зазор между верхней группой (таймер/скорость/Stage/PB-WR) и нижней (клавиши/
	//        showpos): маленькая пустая строка в мелком кегле (FS_MINOR), а не полная строка —
	//        органичный отступ, не зияние; экономит высоту под крупные клавиши (l). Только если
	//        обе группы непусты (иначе висячий <br>). Высота center-HTML ограничена (обрезка низа). ---
	bool lowerGroup = showKeys || showPos || showCpTp;
	if (lowerGroup && !html.empty())
	{
		// <br> закрывает строку PB/WR; nbsp мелким кеглем = невысокая строка-зазор; addLine ниже
		// добавит свой <br> перед клавишами. Класс не подхватится → откат на полную строку (не хуже).
		html += "<br><font class='" KZ_HUD_FS_MINOR "'>&#160;</font>";
	}

	// --- Клавиши. Раскладка — по тумблеру hudKeysTwoRows (Элементы → «Клавиши в 2 строки»):
	//        ВКЛ (деф.) — 2 ряда клавиатурой, каждый ряд отдельным addLine (движок центрирует
	//        сам, W встаёт над S): ряд 1 = C W J (C=duck слева, W=forward центр, J=jump справа),
	//        ряд 2 = A S D; буквы всегда видны (нажата → циан, отпущена → dim), зазор — пара nbsp.
	//        ВЫКЛ — одна строка в формате апстрима cs2kz: буква если нажата, «_» если нет,
	//        порядок A W S D C J, БЕЗ font-тегов (решение E1) — дефолтный кегль/цвет панели.
	//        Подпись «Keys:» убрана — раскладка самодостаточна. ---
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
		if (this->IsMHUDKeysTwoRowsEnabled())
		{
			// Ряд 1 (верх): C W J.
			std::string row1 = key("C", dataSource->IsButtonPressed(IN_DUCK)) + sep + key("W", wDown) + sep + key("J", jump);
			// Ряд 2 (низ): A S D — W окажется над S при центровке движком.
			std::string row2 = key("A", aDown) + sep + key("S", sDown) + sep + key("D", dDown);
			addLine(std::string("<font class='" KZ_HUD_FS_KEYS "'>") + row1 + "</font>");
			addLine(std::string("<font class='" KZ_HUD_FS_KEYS "'>") + row2 + "</font>");
		}
		else
		{
			// Одна строка — вариант для тех, кому раскладка в 2 ряда занимает слишком много
			// высоты панели. Формат апстрима: одиночные пробелы-разделители (в HTML-панели
			// одиночный пробел рендерится, схлопываются только повторные), без font-тегов —
			// цвет нажатия/overlap здесь не применяется by design (см. комментарий выше).
			char row[32];
			V_snprintf(row, sizeof(row), "%c %c %c %c %c %c", aDown ? 'A' : '_', wDown ? 'W' : '_', sDown ? 'S' : '_', dDown ? 'D' : '_',
					   dataSource->IsButtonPressed(IN_DUCK) ? 'C' : '_', jump ? 'J' : '_');
			addLine(row);
		}
	}

	// --- Строка CP/TP — нижняя строка панели (решение E1 «низом» осталось, сменился канал:
	//        см. kz_hud_cptp_in_panel). Данные — наблюдаемого, у реплей-бота из реплей-системы,
	//        как в ComputeBottomState. Кегль — вторичной инфы, один в один с PB/WR: в centre-канале
	//        он зависел от числа строк отступа и с полным набором элементов становился нечитаемым.
	//        Идёт ДО showpos намеренно: высота center-HTML ограничена (обрезка низа), и если
	//        обрезать, то отладочные координаты (по умолчанию выключены), а не счётчик CP/TP. ---
	if (showCpTp && (isReplay || dataSource->checkpointService))
	{
		const i32 cp = isReplay ? KZ::replaysystem::GetCurrentCpIndex() : dataSource->checkpointService->GetCurrentCpIndex();
		const i32 cpCount = isReplay ? KZ::replaysystem::GetCheckpointCount() : dataSource->checkpointService->GetCheckpointCount();
		const i32 tp = isReplay ? KZ::replaysystem::GetTeleportCount() : (i32)dataSource->checkpointService->GetTeleportCount();
		std::string cpTpText = KZLanguageService::PrepareMessageWithLang(language, "HUD - Bottom CP/TP Text", cp, cpCount, tp);
		V_snprintf(buf, sizeof(buf), "<font class='" KZ_HUD_FS_SECONDARY "'><font color='" KZ_HUD_C_WHITE "'>%s</font></font>", cpTpText.c_str());
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

// Можно ли ПРЯМО СЕЙЧАС звать метод cs2menus 005 (SetSlotStatus): указатель есть И получен он
// по 005-строке. Гейт обязан стоять на КАЖДОМ пути вызова, а не только на выборе приёмника:
// bottomOnMenuStatus — СОХРАНЁННОЕ состояние, оно переживает горячую замену cs2menus на сборку
// без 005 (флаг уже false, а состояние ещё true), и путь гашения прежнего приёмника уехал бы
// вызовом по индексу 005-метода на короткой vtable — это падение сервера, а не отказ.
// Единая функция, а не два одинаковых условия: разъехавшись, они дают ровно этот сценарий.
static_function bool MenuStatusAvailable()
{
	return g_pMenus != nullptr && g_menusHasSlotStatus;
}

// Нижняя панель — обычный centre-канал (HUD_PRINTCENTER, ClientPrintFilter), НЕ HTML-канал
// show_survival_respawn_status: движок рисует его ниже центра экрана, что и даёт «низ» худа.
// Содержимое: строка CP/TP (гейт hudCpTp получателя, данные наблюдаемого, у реплей-бота —
// из реплей-системы, как апстримный GetCheckpointText). Только обновлённый стиль: в
// минимале нижней панели нет (CP/TP там рисует апстрим-композиция, см. UpdateMinimalHud).
// Второй режим (menuOpen) — плайн-худ спектатора под открытым Html-меню: скорость/время/
// клавиши наблюдаемого вместо CP/TP (гейт — в DrawPanels, любой стиль), каждый элемент — по
// своему тумблеру ПОЛУЧАТЕЛЯ (hudSpeed/hudTimer/hudKeys), как и на остальных путях худа.
//
// Слепок состояния (без строк/аллокаций — тактовый путь). player — данные (наблюдаемый),
// target — настройки (получатель). menuOpen — плайн-худ под открытым Html-меню: скорость/
// время/клавиши вместо CP/TP (см. FormatBottomText).
void KZHUDService::ComputeBottomState(KZPlayer *player, KZPlayer *target, BottomPanelState &out, bool menuOpen, int linesAbove)
{
	out = BottomPanelState {};
	KZHUDService *cfg = target->hudService;
	const bool isReplay = KZ::replaysystem::IsReplayBot(player);
	// Язык получателя — часть слепка (kz_language меняет его на месте, без реконнекта).
	V_strncpy(out.lang, target->languageService->GetLanguage(), sizeof(out.lang));

	if (menuOpen)
	{
		// Значения — в гранулярности отображения (целые юниты, сотые секунды): слепок не
		// должен меняться чаще текста.
		out.menuOpen = true;
		// Приёмник выбираем ЗДЕСЬ, а не на отправке: от него зависит вёрстка (см. ниже про
		// клавиши), а вёрстка обязана быть функцией слепка — иначе текст и слепок разъедутся.
		out.menuStatus = MenuStatusAvailable();
		// Тумблеры элементов — с ПОЛУЧАТЕЛЯ (cfg), данные — с наблюдаемого: тот же контракт
		// data/settings, что в BuildVersionCHud и UpdateMinimalHud. До 05.08 плайн-худ под меню
		// их не смотрел вовсе и показывал выключенные элементы.
		out.showSpeed = cfg->IsMHUDSpeedEnabled();
		out.showKeys = cfg->IsMHUDKeysEnabled();
		if (out.showSpeed)
		{
			Vector velocity, baseVelocity;
			player->GetVelocity(&velocity);
			player->GetBaseVelocity(&baseVelocity);
			velocity += baseVelocity;
			out.speed = RoundFloatToInt(velocity.Length2D());
			// Точка отрыва в скобках — то же условие, что в GetSpeedText: в воздухе либо сразу
			// после приземления (сглаживание мерцания); на лестнице — только с зажатым прыжком.
			const bool onGround = player->GetPlayerPawn()->m_fFlags & FL_ONGROUND
								  && g_pKZUtils->GetServerGlobals()->curtime - player->landingTime > KZ_HUD_ON_GROUND_THRESHOLD;
			const bool ladderIdle = player->GetPlayerPawn()->m_MoveType == MOVETYPE_LADDER && !player->IsButtonPressed(IN_JUMP);
			if (!onGround && !ladderIdle)
			{
				out.showPrespeed = true;
				out.prespeed = RoundFloatToInt(player->takeoffVelocity.Length2D());
			}
		}
		// Таймер: то же числовое ядро, что у худа (реплей-бот внутри); idle-игроку строку
		// не показываем — как GetTimerText.
		f64 time;
		bool running, paused, idleZero;
		if (cfg->IsMHUDTimerEnabled() && player->hudService->GetTimerNumbers(time, running, paused, idleZero) && !idleZero)
		{
			out.hasTimer = true;
			out.timeCs = RoundFloatToInt(time * 100);
			out.timerRunning = running;
			out.timerPaused = paused;
		}
		// Маска клавиш + раскладка получателя: тот же преф hudKeysTwoRows, что у HTML-худа —
		// иначе у игрока с двухстрочными клавишами второй ряд в спеках просто пропадал.
		// (J = отпрыг этим тиком либо зажатый IN_JUMP.)
		if (out.showKeys)
		{
			// Два ряда клавиш — только в plain-text-канале. В строке показаний меню их нет:
			// строка там ровно одна (вторая отняла бы пункт у самого меню), поэтому клавиши
			// идут компактной группой. В САМОМ худе (меню закрыто) два ряда остаются — их
			// чинили отдельно по репорту 05.08, и этот путь их не касается.
			out.keysTwoRows = !out.menuStatus && cfg->IsMHUDKeysTwoRowsEnabled();
			const bool jump = player->hudService->jumpedThisTick || player->IsButtonPressed(IN_JUMP);
			out.keyMask = (u8)((player->IsButtonPressed(IN_MOVELEFT) ? KPF_Left : 0) | (player->IsButtonPressed(IN_FORWARD) ? KPF_Forward : 0)
							   | (player->IsButtonPressed(IN_BACK) ? KPF_Back : 0) | (player->IsButtonPressed(IN_MOVERIGHT) ? KPF_Right : 0)
							   | (player->IsButtonPressed(IN_DUCK) ? KPF_Duck : 0) | (jump ? KPF_Jump : 0));
		}
		// Под открытым Html-меню отступа НЕТ ВОВСЕ (репорт 07.08 по cyb.107): рамка канала
		// лежит в области меню, за её пределы отступ содержимое не выводит — а кегль делит
		// исправно (движок вписывает ВЕСЬ текст канала в рамку фиксированной высоты, и 6 строк
		// отступа + строка худа дали «шрифт мельчайший»). Плата есть, выигрыша нет — шлём
		// голое содержимое; linesAbove в этой ветке не используется намеренно.
		out.padLines = 0;
		return;
	}

	// Гейт на checkpointService — как в панельном пути (BuildVersionCHud) и в блоке PRO/NUB:
	// у реплей-бота данные идут из реплей-системы, у живого игрока сервис обязан быть.
	if (cfg->IsMHUDCpTpEnabled() && (isReplay || player->checkpointService))
	{
		out.showCpTp = true;
		out.cp = isReplay ? KZ::replaysystem::GetCurrentCpIndex() : player->checkpointService->GetCurrentCpIndex();
		out.cpCount = isReplay ? KZ::replaysystem::GetCheckpointCount() : player->checkpointService->GetCheckpointCount();
		out.tp = isReplay ? KZ::replaysystem::GetTeleportCount() : (i32)player->checkpointService->GetTeleportCount();
	}
	out.padLines = BottomPadLines(linesAbove, out.showCpTp ? 1 : 0);
}

// Текст нижней панели — функция слепка (включая язык): рендерит ровно то, что сравнивает
// ComputeBottomState — текст и слепок не могут разъехаться. Зовётся только на изменении
// слепка, т.е. на смене CP/TP/языка — аллокация PrepareMessageWithLang редкая и не
// тактовая. Разметки нет — канал plain-text.
void KZHUDService::FormatBottomText(const BottomPanelState &state, char *buf, i32 size)
{
	buf[0] = '\0';
	if (state.menuOpen && state.menuStatus)
	{
		// Строка показаний ВНУТРИ панели меню (cs2menus 005) — ровно ОДНА строка: каждая лишняя
		// ужимает окно пунктов самого меню (бюджет панели 9 строк на всё). Отсюда предельно
		// короткая вёрстка: время без подписи, скорость голыми цифрами, клавиши одной группой.
		// Ориентир длины — kHtmlWrapChars (35 знаков) в cs2menus: типовое
		// «00:06.92 | 274 (250) | _W__CJ» = 29 знаков, запас есть.
		std::string line;
		auto addPart = [&line](const std::string &part)
		{
			if (part.empty())
			{
				return;
			}
			if (!line.empty())
			{
				line += " | ";
			}
			line += part;
		};
		if (state.hasTimer)
		{
			char timeText[32];
			FormatTimeHudCs(state.timeCs, timeText, sizeof(timeText));
			std::string part = timeText;
			// Метка состояния — КОРОТКАЯ, отдельными фразами: апстримные « (СТОП)»/« (НА ПАУЗЕ)»
			// вместе со скоростью и клавишами не влезают в строку, а перенос стоит пункта меню.
			// Выкинуть состояние нельзя: на реплее пауза игроку важнее подписи «Скорость».
			if (state.timerPaused)
			{
				part += " " + KZLanguageService::PrepareMessageWithLang(state.lang, "HUD - Menu Status Paused");
			}
			else if (!state.timerRunning)
			{
				part += " " + KZLanguageService::PrepareMessageWithLang(state.lang, "HUD - Menu Status Stopped");
			}
			addPart(part);
		}
		if (state.showSpeed)
		{
			char speedText[32];
			if (state.showPrespeed)
			{
				V_snprintf(speedText, sizeof(speedText), "%d (%d)", state.speed, state.prespeed);
			}
			else
			{
				V_snprintf(speedText, sizeof(speedText), "%d", state.speed);
			}
			addPart(speedText);
		}
		if (state.showKeys)
		{
			// Тот же порядок A W S D C J, что в однорядной раскладке худа, но без пробелов:
			// нажатие — буква, отпущено — «_» (канал без цветов, конвенция апстрима).
			char keys[8];
			V_snprintf(keys, sizeof(keys), "%c%c%c%c%c%c", (state.keyMask & KPF_Left) ? 'A' : '_', (state.keyMask & KPF_Forward) ? 'W' : '_',
					   (state.keyMask & KPF_Back) ? 'S' : '_', (state.keyMask & KPF_Right) ? 'D' : '_', (state.keyMask & KPF_Duck) ? 'C' : '_',
					   (state.keyMask & KPF_Jump) ? 'J' : '_');
			addPart(keys);
		}
		V_strncpy(buf, line.c_str(), size);
		return;
	}
	if (state.menuOpen)
	{
		// Плайн-худ под меню: скорость (преспид) / время / клавиши, каждый — по своему тумблеру
		// получателя (слепок уже посчитан в ComputeBottomState). Канал plain-text — фразы
		// без разметки: у взлётной скорости своя нижняя фраза (апстримная содержит font-теги).
		// Разделитель между строками добавляем ТОЛЬКО к непустому тексту, иначе выключенный
		// элемент оставлял бы пустой ряд.
		std::string text;
		auto addRow = [&text](const std::string &row)
		{
			if (!text.empty())
			{
				text += "\n";
			}
			text += row;
		};
		// Время и скорость — ОДНОЙ строкой (порядок как в HTML-панели: сначала время).
		// Раздельными строками содержимое худа под меню занимало четыре строки и вместе с
		// отступом вылезало за бюджет канала — не рисовался второй ряд клавиш. Разделитель
		// « | » — тот же, что в строке CP/TP (пробелы в этом канале ненадёжны).
		std::string head;
		if (state.hasTimer)
		{
			char timeText[32];
			FormatTimeHudCs(state.timeCs, timeText, sizeof(timeText));
			// clang-format off
			head = KZLanguageService::PrepareMessageWithLang(state.lang, "HUD - Timer Text",
				timeText,
				state.timerRunning ? "" : KZLanguageService::PrepareMessageWithLang(state.lang, "HUD - Stopped Text").c_str(),
				state.timerPaused ? KZLanguageService::PrepareMessageWithLang(state.lang, "HUD - Paused Text").c_str() : "");
			// clang-format on
		}
		if (state.showSpeed)
		{
			std::string speedText;
			if (state.showPrespeed)
			{
				// clang-format off
				speedText = KZLanguageService::PrepareMessageWithLang(state.lang, "HUD - Bottom Speed Text (Takeoff)",
					(f64)state.speed, (f64)state.prespeed);
				// clang-format on
			}
			else
			{
				speedText = KZLanguageService::PrepareMessageWithLang(state.lang, "HUD - Speed Text", (f64)state.speed);
			}
			head = head.empty() ? speedText : head + " | " + speedText;
		}
		if (!head.empty())
		{
			addRow(head);
		}
		if (state.showKeys && state.keysTwoRows)
		{
			// Те же два ряда, что в HTML-худе: «C W J» сверху, «A S D» снизу (W встаёт над S
			// при центровке движком). Канал plain-text — цвета нажатия недоступны, поэтому
			// нажатие показываем апстримной конвенцией: буква против «_».
			char row1[16], row2[16];
			V_snprintf(row1, sizeof(row1), "%c %c %c", (state.keyMask & KPF_Duck) ? 'C' : '_', (state.keyMask & KPF_Forward) ? 'W' : '_',
					   (state.keyMask & KPF_Jump) ? 'J' : '_');
			V_snprintf(row2, sizeof(row2), "%c %c %c", (state.keyMask & KPF_Left) ? 'A' : '_', (state.keyMask & KPF_Back) ? 'S' : '_',
					   (state.keyMask & KPF_Right) ? 'D' : '_');
			addRow(row1);
			addRow(row2);
		}
		else if (state.showKeys)
		{
			// clang-format off
			addRow(KZLanguageService::PrepareMessageWithLang(state.lang, "HUD - Key Text",
				(state.keyMask & KPF_Left) ? 'A' : '_',
				(state.keyMask & KPF_Forward) ? 'W' : '_',
				(state.keyMask & KPF_Back) ? 'S' : '_',
				(state.keyMask & KPF_Right) ? 'D' : '_',
				(state.keyMask & KPF_Duck) ? 'C' : '_',
				(state.keyMask & KPF_Jump) ? 'J' : '_'));
			// clang-format on
		}
		V_strncpy(buf, PadAbove(text, state.padLines).c_str(), size);
		return;
	}
	if (state.showCpTp)
	{
		std::string line = KZLanguageService::PrepareMessageWithLang(state.lang, "HUD - Bottom CP/TP Text", state.cp, state.cpCount, state.tp);
		V_strncpy(buf, PadAbove(line, state.padLines).c_str(), size);
	}
}

// Текст нижней панели в выбранный приёмник: menuStatus — строка показаний внутри панели
// открытого меню (cs2menus 005), иначе канал движка (alert=false — centre-print, true — alert).
static_function void SendBottomText(KZPlayer *player, bool menuStatus, bool alert, const char *text)
{
	if (menuStatus)
	{
		// Гейт по MenuStatusAvailable, а не по одному g_pMenus: сюда приходит и СОХРАНЁННЫЙ
		// bottomOnMenuStatus (гашение прежнего приёмника), который мог пережить подмену
		// cs2menus на сборку без 005 — см. MenuStatusAvailable.
		if (MenuStatusAvailable())
		{
			g_pMenus->SetSlotStatus(player->GetPlayerSlot().Get(), text);
		}
		return;
	}
	if (alert)
	{
		player->PrintAlert(false, false, "%s", text);
	}
	else
	{
		player->PrintCentre(false, false, "%s", text);
	}
}

// Погасить приёмник, в который писали. У строки меню это пустой текст (движок снимет её и
// вернёт пункту меню строку бюджета), у канала — одноразовый пустой токен: сам канал гаснет
// лишь через несколько секунд, а остаток нижней панели читается как зависший худ (тот же
// приём, что в TogglePanel).
static_function void ClearBottomSink(KZPlayer *player, bool menuStatus, bool alert)
{
	SendBottomText(player, menuStatus, alert, menuStatus ? "" : "#SFUI_EmptyString");
}

// Тик нижней панели получателя (this): пересчитать слепок; отправлять только на его
// изменении (пересборка текста — тоже только тут) либо heartbeat'ом раз в
// KZ_HUD_BOTTOM_HEARTBEAT — centre-канал надёжный (BUF_RELIABLE), слать 128/с каждому
// получателю расточительно, а неизменившийся текст переотправляется как есть без пересборки.
void KZHUDService::UpdateBottomPanel(KZPlayer *dataSource, bool menuOpen, int linesAbove)
{
	BottomPanelState state;
	ComputeBottomState(dataSource, this->player, state, menuOpen, linesAbove);
	if (!state.HasContent())
	{
		// Слать нечего (hudCpTp выкл; под меню — все три тумблера выкл): одноразовый клир
		// стирает остаток, дальше — no-op.
		this->ClearBottomPanel();
		return;
	}
	// Приёмник берём один раз на тик: канал могли переключить по rcon прямо сейчас, а строка
	// меню появляется/исчезает вместе с самим меню — тогда прежний гасим одноразово, иначе в
	// нём остался бы наш текст до самозатухания (в канале) или до чужого клира (в меню).
	const bool menuStatus = state.menuStatus;
	const bool alert = kz_hud_bottom_channel.Get() == 1;
	if (this->bottomPanelActive && (this->bottomOnMenuStatus != menuStatus || (!menuStatus && this->bottomOnAlert != alert)))
	{
		ClearBottomSink(this->player, this->bottomOnMenuStatus, this->bottomOnAlert);
		// Слепок мог не измениться, и без сброса текст ушёл бы в новый приёмник только по
		// heartbeat'у (до секунды) — у стоящего игрока это выглядит как «худ пропал».
		this->bottomStateValid = false;
	}
	this->bottomOnMenuStatus = menuStatus;
	this->bottomOnAlert = alert;
	f64 now = g_pKZUtils->GetServerGlobals()->curtime;
	if (this->bottomStateValid && state == this->lastBottomState)
	{
		// now < lastBottomSendTime = curtime пошёл заново (кэш пережил смену карты —
		// подстраховка к сбросу в OnRoundStart): считаем heartbeat истёкшим.
		const f64 heartbeat = menuStatus ? KZ_HUD_BOTTOM_MENU_HEARTBEAT : KZ_HUD_BOTTOM_HEARTBEAT;
		const bool heartbeatDue = now < this->lastBottomSendTime || now - this->lastBottomSendTime >= heartbeat;
		if (!heartbeatDue)
		{
			return;
		}
		SendBottomText(this->player, menuStatus, alert, this->lastBottomText);
		this->lastBottomSendTime = now;
		return;
	}
	char buf[256];
	this->FormatBottomText(state, buf, sizeof(buf));
	this->lastBottomState = state;
	this->bottomStateValid = true;
	V_strncpy(this->lastBottomText, buf, sizeof(this->lastBottomText));
	SendBottomText(this->player, menuStatus, alert, buf);
	this->lastBottomSendTime = now;
	this->bottomPanelActive = true;
}

void KZHUDService::ClearBottomPanel()
{
	// Инвалидация кэша всегда: после клира следующий непустой слепок обязан отправиться.
	this->bottomStateValid = false;
	this->lastBottomText[0] = '\0';
	if (!this->bottomPanelActive)
	{
		return;
	}
	this->bottomPanelActive = false;
	// Гасим ИМЕННО тот приёмник, в который писали: alert общий — там же печатают гонки и
	// античит, и слепой клир стёр бы этому игроку чужое сообщение; строка меню общая с самим
	// меню — её чистит и cs2menus на закрытии, но остановка спектейта закрытием не является.
	ClearBottomSink(this->player, this->bottomOnMenuStatus, this->bottomOnAlert);
}

// Тик минимал-худа получателя (this): апстрим-композиция cs2kz на живых строителях —
// centre-канал (HUD_PRINTCENTER) = CP/TP + таймер («HUD - Center Text»), HTML-канал
// (show_survival_respawn_status) = скорость + клавиши («HUD - HTML Center Text»);
// компакт-режим апстрима — только html (таймер<br>скорость), centre пуст. dataSource —
// наблюдаемый (данные), настройки/язык/цвета — this (получатель): Get*Text зовутся на
// hudService наблюдаемого (их данные — this->player), GetSpeedText — на получателе с
// dataSource (его цветовые префы — часть настроек), тот же контракт, что у BuildVersionCHud.
// Порядок и разделители композиции — апстримные (берутся из фраз), но per-element тумблеры
// (hudKeys/hudCpTp/hudTimer/hudSpeed) применяются и здесь: выключенный элемент приходит
// пустой строкой, а осиротевший разделитель снимает DropEmptySegments. Particle-подавления
// нет — на particle-пути минимал не рисуется вовсе.
// Отправка per-канал дедуплицируется слепком последнего отправленного текста + heartbeat
// (в движении текст меняется почти каждый тик — шлётся каждый тик, это цена стиля; в
// простое стабилен). Слепок мог обрезаться своим буфером — тогда сравнение никогда не
// совпадает и текст шлётся каждый тик: корректно, просто без экономии.
void KZHUDService::UpdateMinimalHud(KZPlayer *dataSource)
{
	const char *language = this->player->languageService->GetLanguage();
	// Тумблеры элементов читаем с ПОЛУЧАТЕЛЯ (this = настройки), данные — с dataSource:
	// тот же контракт разведения data/settings, что в BuildVersionCHud и GetSpeedText.
	// Апстримные геттеры про наши per-element префы не знают вовсе — до 05.08 из-за этого
	// в минималистичном стиле выключенные элементы (cp/tp и прочие) продолжали рисоваться.
	const bool wantKeys = this->IsMHUDKeysEnabled();
	const bool wantCpTp = this->IsMHUDCpTpEnabled();
	const bool wantTimer = this->IsMHUDTimerEnabled();
	const bool wantSpeed = this->IsMHUDSpeedEnabled();
	std::string centreText;
	std::string htmlText;
	if (this->IsCompactPanel())
	{
		std::string timerText = wantTimer ? dataSource->hudService->GetTimerText(language) : std::string();
		std::string speedText = wantSpeed ? this->GetSpeedText(language, dataSource) : std::string();
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
		std::string keyText = wantKeys ? dataSource->hudService->GetKeyText(language) : std::string();
		std::string checkpointText = wantCpTp ? dataSource->hudService->GetCheckpointText(language) : std::string();
		std::string timerText = wantTimer ? dataSource->hudService->GetTimerText(language) : std::string();
		std::string speedText = wantSpeed ? this->GetSpeedText(language, dataSource) : std::string();
		// clang-format off
		centreText = KZLanguageService::PrepareMessageWithLang(language, "HUD - Center Text",
			keyText.c_str(), checkpointText.c_str(), timerText.c_str(), speedText.c_str());
		htmlText = KZLanguageService::PrepareMessageWithLang(language, "HUD - HTML Center Text",
			keyText.c_str(), checkpointText.c_str(), timerText.c_str(), speedText.c_str());
		// clang-format on
		// Композиционные фразы склеивают элементы ФИКСИРОВАННЫМ разделителем, поэтому
		// выключенный элемент оставляет по себе пустой ряд (\n) или лишний <br>. Порядок и
		// сам разделитель остаются апстримными (фраза может отличаться по языкам) — мы лишь
		// выбрасываем пустые сегменты.
		centreText = DropEmptySegments(centreText, "\n");
		htmlText = DropEmptySegments(htmlText, "<br>");
	}
	// Хвостовые \n срезаются, как в апстриме (пустой таймер в idle оставляет висячий перенос).
	centreText = centreText.substr(0, centreText.find_last_not_of('\n') + 1);
	htmlText = htmlText.substr(0, htmlText.find_last_not_of('\n') + 1);

	f64 now = g_pKZUtils->GetServerGlobals()->curtime;

	if (centreText.empty())
	{
		// Компакт: centre минимал не занимает — одноразовый клир остатка полной раскладки.
		if (this->minimalCentreActive)
		{
			this->minimalCentreActive = false;
			this->lastMinimalCentreText[0] = '\0';
			this->player->PrintCentre(false, false, "#SFUI_EmptyString");
		}
	}
	else
	{
		const bool changed = V_strcmp(this->lastMinimalCentreText, centreText.c_str()) != 0;
		// now < lastSendTime = curtime пошёл заново (кэш пережил смену карты — подстраховка
		// к сбросу в OnRoundStart): считаем heartbeat истёкшим. Тот же приём в html ниже.
		const bool heartbeatDue = now < this->lastMinimalCentreSendTime || now - this->lastMinimalCentreSendTime >= KZ_HUD_BOTTOM_HEARTBEAT;
		if (changed || heartbeatDue)
		{
			V_strncpy(this->lastMinimalCentreText, centreText.c_str(), sizeof(this->lastMinimalCentreText));
			this->player->PrintCentre(false, false, "%s", centreText.c_str());
			this->lastMinimalCentreSendTime = now;
			this->minimalCentreActive = true;
		}
	}

	if (htmlText.empty())
	{
		// Нечего слать (реплей-бот без времени в компакте): канал сам гаснет за duration=1s.
		this->lastMinimalHtmlText[0] = '\0';
	}
	else
	{
		const bool changed = V_strcmp(this->lastMinimalHtmlText, htmlText.c_str()) != 0;
		const bool heartbeatDue = now < this->lastMinimalHtmlSendTime || now - this->lastMinimalHtmlSendTime >= KZ_HUD_MINIMAL_HTML_HEARTBEAT;
		if (changed || heartbeatDue)
		{
			V_strncpy(this->lastMinimalHtmlText, htmlText.c_str(), sizeof(this->lastMinimalHtmlText));
			this->player->PrintHTMLCentre(false, false, "%s", htmlText.c_str());
			this->lastMinimalHtmlSendTime = now;
		}
	}
}

// Одноразовый клир минимал-худа при уходе из него (стиль Updated, particle-путь, тип Off,
// открытое меню, смерть без спектейта): centre гасится пустым токеном — сам бы висел ещё
// несколько секунд; html не трогаем — его либо тут же перерисовывает новый владелец
// (обновлённый худ/меню), либо он сам гаснет за duration=1s (см. utils::PrintHTMLCentre).
// Зовётся каждый тик на не-минимальных путях (no-op без minimalCentreActive).
void KZHUDService::ClearMinimalHud()
{
	// Инвалидация кэша всегда: после клира следующий текст обязан отправиться.
	this->lastMinimalCentreText[0] = '\0';
	this->lastMinimalHtmlText[0] = '\0';
	if (!this->minimalCentreActive)
	{
		return;
	}
	this->minimalCentreActive = false;
	this->player->PrintCentre(false, false, "#SFUI_EmptyString");
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
	// погашены выше), ни HTML-панель, ни нижнюю панель, ни минимал (клир остатков —
	// одноразовый).
	if (cfg->GetHudType() == HUD_TYPE_OFF)
	{
		cfg->ClearBottomPanel();
		cfg->ClearMinimalHud();
		return;
	}

	// Yield the center channel while a cs2menus HTML menu is open.
	// Живому игроку (player == target) худ молчит целиком, как раньше. Спектатору вместо
	// глушения шлём plain-text худ наблюдаемого (скорость/время/клавиши) машинерией нижней
	// панели — дедуп/heartbeat штатные, закрытие меню меняет слепок (menuOpen) и низ
	// перерисовывается/стирается сам. Высоту меню больше не измеряем: отступ под меню
	// отключён (см. ComputeBottomState), а GetItemCount/GetActiveMenu брали мьютекс cs2menus
	// каждый тик на каждого спектатора.
	if (g_pMenus && g_pMenus->GetActiveMenuType(target->GetPlayerSlot().Get()) == MenuType::Html)
	{
		cfg->ClearMinimalHud();
		if (player != target)
		{
			cfg->UpdateBottomPanel(player, /*menuOpen=*/true);
		}
		else
		{
			cfg->ClearBottomPanel();
		}
		return;
	}
	const char *language = target->languageService->GetLanguage();

	// HTML fallback: Standard type is selected, OR no addons are available at all
	// (no MultiAddonManager/assets), OR the particle path isn't active for some other
	// reason — target is dead, or this is a spectator (player != target, see the gate
	// above). needHtml and useParticles must stay mutually exclusive, otherwise the
	// recipient ends up with no HUD at all. (Off уже отсеян ранним return выше.)
	bool needHtml = !available || cfg->GetHudType() == HUD_TYPE_STANDARD || !useParticles;

	// --- Минималистичный стиль: весь худ = апстрим-композиция cs2kz (см. UpdateMinimalHud).
	//        Кибершоковская HTML-панель и нижняя панель не рисуются вовсе; их остаток при
	//        переключении стиля стирает одноразовый ClearBottomPanel (centre-канал переходит
	//        к минималу), html просто перерисовывается новым текстом. ---
	if (needHtml && cfg->GetTimerStyle() == HUD_TIMER_STYLE_MINIMAL)
	{
		cfg->ClearBottomPanel();
		cfg->UpdateMinimalHud(player);
		return;
	}
	// Уход из минимала (стиль Updated / оживший particle-путь): одноразовый клир его
	// centre-канала; no-op, пока минимал не был активен.
	cfg->ClearMinimalHud();

	std::string htmlText;

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

	// --- Нижняя панель (обычный centre-канал): строка CP/TP — ТОЛЬКО при kz_hud_cptp_in_panel 0.
	//        По умолчанию CP/TP уже нарисован последней строкой HTML-панели выше (там кегль
	//        фиксированный и накрыть себя панель не может), а этот канал остаётся пустым.
	//        Ниже — про прежнюю раскладку (cvar 0): СОПРОВОЖДАЕТ
	//        HTML-панель (needHtml): CP/TP в particle-MHUD не существует (UpdateParticles —
	//        только Speed/Timer/Keys), преф hudCpTp всегда был про HTML-путь — поэтому и
	//        спектатор (всегда HTML), и мёртвый, и получатель с типом MHUD на HTML-фолбэке
	//        видят низ. Не шлём только на живом particle-пути владельца (needHtml=false):
	//        там centre-канал остаётся свободным. Отправка/пересборка — по изменению слепка
	//        + heartbeat (см. UpdateBottomPanel); переход «был текст → стало нечего» стирает
	//        остаток одноразовым клиром. Каждый получатель (владелец/спектатор) получает
	//        свой вызов DrawPanels → includeSpectators=false. ---
	if (needHtml && !kz_hud_cptp_in_panel.Get())
	{
		// Высота нашей же HTML-панели — из её готового текста: при всех включённых элементах
		// и клавишах в 2 ряда она дорастала до нижней панели и накрывала CP/TP.
		cfg->UpdateBottomPanel(player, /*menuOpen=*/false, /*linesAbove=*/CountHtmlLines(htmlText));
	}
	else
	{
		// CP/TP уехал последней строкой В САМУ панель (деф.) — centre-канал свободен, остаток
		// прежней нижней панели стирает одноразовый клир (в т.ч. при смене cvar'а на живом
		// сервере). На particle-пути владельца (needHtml=false) — тот же клир, что и раньше.
		cfg->ClearBottomPanel();
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
