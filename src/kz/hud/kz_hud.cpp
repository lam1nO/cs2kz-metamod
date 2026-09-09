#include "../kz.h"
#include "cs2kz.h"
#include "kz_hud.h"
#include "sdk/datatypes.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "utils/logging.h" // KZ_LOG_* — отказ конвара подавки viewpunch не должен быть немым

#include "kz/option/kz_option.h"
#include "kz/hud/share/hud_share.h" // ClearSlotState — слот отката/кулдаун обмена гасятся вместе со слотом
#include "kz/timer/kz_timer.h"
#include "kz/language/kz_language.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "kz/prac/kz_prac.h"
#include "kz/spec/kz_spec.h"
#include "kz/replays/kz_replaysystem.h"
#include "kz/replays/data.h"              // состояние плейбека: пауза реплея для источника скорости
#include "kz/replays/playback.h"          // GetDisplayedFrameVelocity — скорость кадра на паузе
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

// Дефолтные цвета худа (объявления — extern в kz_hud.h). Значения апстрима, не менялись ни
// на бит; жили в particles.cpp и переехали сюда вместе с удалением particle-пути.
const Color MHUD_DEF_BASE_COLOR(255, 255, 255, 255);
const Color MHUD_DEF_PERF_COLOR(0x40, 0xFF, 0x40, 0xFF);
const Color MHUD_DEF_JUMPBUG_COLOR(0xFF, 0xFF, 0x20, 0xFF);
const Color MHUD_DEF_CJ_COLOR(0x71, 0xEE, 0xB8, 0xFF);
const Color MHUD_DEF_TIMER_TP_COLOR(255, 255, 255, 255);
const Color MHUD_DEF_TIMER_PRO_COLOR(0x5F, 0x99, 0xD9, 0xFF);
const Color MHUD_DEF_TIMER_PAUSED_COLOR(0xFF, 0xFF, 0x00, 0xFF);
const Color MHUD_DEF_TIMER_STOPPED_COLOR(0xFF, 0xA0, 0xA0, 0xFF);
// Дефолт синхронизирован с текущими настройками игрока (задача hud-defaults): было (255,64,64).
const Color MHUD_DEF_KEYS_OVERLAP_COLOR(0xFF, 0x00, 0x00, 0xFF);
const Color MHUD_DEF_KEYS_PRESSED_COLOR(0x3B, 0xED, 0xA0, 0xFF);
const Color MHUD_DEF_KEYS_OVERLAP_GLOW_COLOR(0xFF, 0x40, 0x40, 0xFF);

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
// с запасом до угасания и в 64 раза реже, чем слать каждый тик (тик CS2 = 1/64,
// ENGINE_FIXED_TICK_INTERVAL).
#define KZ_HUD_BOTTOM_HEARTBEAT 1.0f

// Heartbeat строки показаний в панели меню — на порядок чаще. Причина не в угасании (её
// держит сам cs2menus), а в том, что он ЧИСТИТ статус в EndDisplay: переход в подменю
// закрывает один показ и открывает другой, и на статичной цели (пауза на реплее) слепок не
// меняется — показания пропадали бы до целой секунды. Повтор того же текста в cs2menus
// бесплатен (сравнение строки), поэтому цена частоты — только взятие его мьютекса.
// Плавность показаний этот heartbeat НЕ задаёт: меняющийся слепок уходит в SetSlotStatus
// каждый тик (ветка ниже по коду), а как часто из него перерисуется панель — дело cs2menus
// (HtmlStatusInterval, деф. 0 = на каждой смене).
#define KZ_HUD_BOTTOM_MENU_HEARTBEAT 0.1f

static CConVar<bool> kz_force_mhud("kz_force_mhud", FCVAR_NONE, "Force the panorama HUD even when MultiAddonManager is not available.", false);

// Аварийный рычаг-диагностика (дефект «дёргается камера в ноуклипе сквозь стены», окно
// cyb.150-152): позволяет отделить «сущность custom_hud_layout существует» от «отрисовка».
// false — НИ ОДНА из двух наших сущностей (худ EnsureOwnedLayout / меню настроек
// EnsureMenuLayout) не заводится, а уже существующие уничтожаются немедленно колбэком ниже.
// Это НЕ фикс: рычаг только убирает сущность из уравнения для теста, корень дефекта
// по-прежнему не найден. HTML-худ (hudType Standard) не зависит от этой сущности и продолжает
// работать при false — см. DrawPanels/IsMHUDAvailable.
static void OnHudLayoutEnabledChanged(CConVar<bool> *ref, CSplitScreenSlot nSlot, const bool *bNewValue, const bool *bOldValue)
{
	// Смена состояния конвара — одна строка на переключение, не на игрока и не в такте.
	KZ_LOG_INFO(LogChannel::General, "[cyb] hud_layout_killswitch_changed enabled=%s\n", *bNewValue ? "true" : "false");
	if (!*bNewValue)
	{
		// Рычаг бесполезен для диагностики, если оставить живые сущности до смены типа худа/
		// реконнекта — гасим ВСЕХ немедленно. LayoutCleanup — тот же путь, что при выгрузке
		// плагина (cs2kz.cpp): он же снимает захват ввода меню (DestroyOwnedMenuLayout) и
		// обнуляет кэши классов/dialog-переменных вместе с сущностями.
		KZHUDService::LayoutCleanup();
	}
	// Обратно в true — ничего доп. делать не нужно: EnsureOwnedLayout/EnsureMenuLayout сами
	// заведут сущность на следующем DrawPanels/!hudmenu, без реконнекта игрока.
}

CConVar<bool> kz_hud_layout_enabled("kz_hud_layout_enabled", FCVAR_NONE,
									 "Diagnostic kill-switch: разрешить создание сущности custom_hud_layout "
									 "(panorama-худ и его меню настроек). false отключает обе сущности сразу "
									 "и уничтожает уже созданные; HTML-худ продолжает работать.",
									 true, OnHudLayoutEnabledChanged);

// Подавка тряски камеры (viewpunch). Конвар движка cheat+replicated: из RCON/cfg он не
// берётся («cheat protected, change ignored»), поэтому серверное значение пишем прямо в его
// ConVarData, а клиенту досылаем персонально (см. OnProcessMovement).
static CConVarRef<bool> sv_suppress_viewpunch("sv_suppress_viewpunch");

// К типу худа подавка больше НЕ привязана. Раньше её включал particle-путь MHUD, и так как
// MHUD был дефолтным худом, гладкий полёт (ноуклип сквозь стены) получали практически все;
// удаление particle-пути (c4b5e5f) вернуло тряску всему флоту. Привязка к panorama-худу
// оставила бы с тряской тех, кто выбрал HTML-худ или выключил худ вовсе — для них это
// выглядело бы случайной поломкой. Поэтому один серверный конвар на всех игроков.
static CConVar<bool> kz_suppress_viewpunch("kz_suppress_viewpunch", FCVAR_NONE, "Suppress view punch (camera shake) for all players.", true);

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
		// Одноразовая перезапись настроек худа новыми дефолтами (layout/defaults.cpp) —
		// СТРОГО до RefreshLayoutPrefs: иначе кэш набрался бы старыми значениями и новый худ
		// игрок увидел бы только после перезахода. Повторные вызовы этого события (LOCAL,
		// затем GLOBAL, затем OnPlayerActive) миграцию не перезапускают — её гасит маркер
		// ревизии в префах.
		player->hudService->ApplyHudDefaults();
		// Кэш префов layout-худа (Task 5): без обновления здесь правка настройки не видна
		// до перезахода — ровно тот класс багов, что уже был в этом худе.
		player->hudService->RefreshLayoutPrefs();
	}
} optionEventListener;

void KZHUDService::Init()
{
	KZTimerService::RegisterEventListener(&timerEventListener);
	KZOptionService::RegisterEventListener(&optionEventListener);
	InitMenuPrefs();
	// Снять FCVAR_REPLICATED, иначе персональные значения конвара до клиентов не доходят
	// (движок реплицирует одно общее) — без этого подавка не работает вообще.
	if (sv_suppress_viewpunch.IsValidRef() && sv_suppress_viewpunch.IsConVarDataAvailable())
	{
		sv_suppress_viewpunch.GetConVarData()->RemoveFlags(FCVAR_REPLICATED);
	}
	else
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] viewpunch_suppress_unavailable reason=convar_data_unavailable cvar=sv_suppress_viewpunch\n");
	}
}

bool KZHUDService::IsMHUDAvailable()
{
	// kz_hud_layout_enabled=false перекрывает ВСЁ (в т.ч. kz_force_mhud) — рычаг обязан
	// гасить сущность безусловно. Единственные три вызывающих места (menu.cpp/entity.cpp —
	// решают, создавать ли сущность, и DrawPanels ниже по файлу — решает, идти ли panorama-
	// путём) уже реагируют на false здесь как на «недоступно», значит отдельного явного
	// разбора в каждом из них не нужно.
	return kz_hud_layout_enabled.Get() && (g_pMultiAddonManager != nullptr || kz_force_mhud.Get());
}

bool KZHUDService::IsHudLayoutKillSwitchOff()
{
	return !kz_hud_layout_enabled.Get();
}

// === Preferences, общие для HTML- и panorama-путей (задача 12 подняла их сюда из
// удалённого src/kz/hud/particles/particles.cpp вместе с particle-путём; логика не менялась). ===

static_function i64 PackColor(const Color &c)
{
	return ((i64)c.r() << 24) | ((i64)c.g() << 16) | ((i64)c.b() << 8) | (i64)c.a();
}

static_function Color UnpackColor(i64 packed)
{
	return Color((u8)((packed >> 24) & 0xFF), (u8)((packed >> 16) & 0xFF), (u8)((packed >> 8) & 0xFF), (u8)(packed & 0xFF));
}

Color KZHUDService::GetMHUDColorPref(const char *name, const Color &defaultColor)
{
	// Цвета — настройка: источник настроек (сам игрок / спектатор), не данные.
	i64 packed = this->MHUDSettingsSource()->optionService->GetPreferenceInt(name, PackColor(defaultColor));
	return UnpackColor(packed);
}

// === Тип худа (hudType) ============================================================
// 0 = Standard (классическая HTML-панель по центру), 2 = Off (не рисуется ничего),
// 3 = Panorama (custom_hud_layout). 1 (MHUD, particle-оверлей) удалён в задаче 12.

int KZHUDService::GetHudType()
{
	// Читаем из настроек источника (сам игрок / спектатор).
	// Миграция очень старого конфига: если hudType не задан вовсе и mhudMaster=true (легаси-
	// преф ещё до появления hudType) → считаем это как старое значение 1 (particle-MHUD),
	// нормализуемое дальше вместе с любым другим сохранённым 1 (см. ниже).
	auto *opts = this->MHUDSettingsSource()->optionService;
	int stored = opts->GetPreferenceInt("hudType", -1);
	if (stored == -1)
	{
		bool legacyMaster = opts->GetPreferenceBool("mhudMaster", false);
		// Дефолт для игрока без единой записи в БД — Panorama, а не Standard: синхронизирован
		// с текущими настройками игрока (задача hud-defaults, hudType=3). Легаси-миграция
		// mhudMaster=true (значение 1) ниже нормализуется в HUD_TYPE_PANORAMA тем же путём.
		int migrated = legacyMaster ? 1 : HUD_TYPE_PANORAMA;
		// Миграция пишет hudType только после загрузки префов из БД: этот геттер дёргается
		// на первом тике движения, задолго до InitializeLocalPrefs. Запись здесь до загрузки
		// зафиксировала бы дефолт в fail-closed prefKV и SaveLocalPrefs потом не смог бы её
		// перезаписать (см. IsLoaded). Читающий путь просто отдаёт вычисленный дефолт.
		if (opts->IsLoaded())
		{
			opts->SetPreferenceInt("hudType", migrated);
		}
		stored = migrated;
	}
	// Задача 12: значение 1 (particle-MHUD) больше не действительно — путь удалён, апстрим
	// вычистил particles/* из воркшоп-аддона. Молча оставить игрока с недействительным типом
	// нельзя (пустой экран): мигрируем сохранённую 1 в Panorama и перезаписываем преф.
	if (stored == 1)
	{
		stored = HUD_TYPE_PANORAMA;
		if (opts->IsLoaded())
		{
			opts->SetPreferenceInt("hudType", stored);
		}
	}
	return stored;
}

void KZHUDService::SetHudType(int type)
{
	this->MHUDSettingsSource()->optionService->SetPreferenceInt("hudType", type);
}

// Все тумблеры/раскладка — НАСТРОЙКИ: читаем из источника настроек (сам игрок/спектатор),
// а не из данных наблюдаемого. Иначе у спектатора «прыгал» бы HUD при смене цели.
// Per-element тумблеры.
bool KZHUDService::IsMHUDSpeedEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudSpeed", true);
}

bool KZHUDService::IsMHUDTimerEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudTimer", true);
}

bool KZHUDService::IsMHUDKeysEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudKeys", true);
}

bool KZHUDService::IsMHUDCpTpEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudCpTp", true);
}

bool KZHUDService::IsMHUDKeysOverlapEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudKeysOverlap", true);
}

// Ниже — настройки ТОЛЬКО стандартного HTML-худа, но живут рядом с остальными тумблерами:
// тот же источник настроек.
bool KZHUDService::IsMHUDKeysTwoRowsEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudKeysTwoRows", true);
}

bool KZHUDService::IsMHUDPbWrEnabled()
{
	return this->MHUDSettingsSource()->optionService->GetPreferenceBool("hudPbWr", true);
}

// Единственная точка расчёта SpeedInfo (Task 6/R3): ветвей показа скорости несколько
// (BuildVersionCHud, ComputeBottomState, panorama-layout в layout/mhud.cpp) —
// считать её обязана КАЖДАЯ одинаково, иначе баг вида «0 на паузе» лечится в одном стиле худа
// и остаётся в остальных. Данные — из MHUDDataSource() (this->player, если mhudSource не
// задан спектейтом) — та же развязка data/settings, что и у остального худа.
SpeedInfo KZHUDService::GetSpeedInfo()
{
	KZPlayer *src = this->MHUDDataSource();
	SpeedInfo info {};
	info.velocity = KZHUDService::GetDisplayVelocity(src);

	// На взлёте (не отлежался KZ_HUD_ON_GROUND_THRESHOLD после приземления и не стоит на
	// лестнице без прыжка) престрейф валиден; иначе показывать нечего — hasPrespeed=false.
	info.hasPrespeed = !((src->GetPlayerPawn()->m_fFlags() & FL_ONGROUND
						  && g_pKZUtils->GetServerGlobals()->curtime - src->landingTime > KZ_HUD_ON_GROUND_THRESHOLD)
						 || (src->GetPlayerPawn()->m_MoveType() == MOVETYPE_LADDER && !src->IsButtonPressed(IN_JUMP)));
	if (info.hasPrespeed)
	{
		info.prespeed = src->takeoffVelocity;
	}
	info.perfing = src->IsPerfing() && !src->possibleLadderHop && !src->takeoffFromLadder;
	info.jumpbug = src->hudService->fromDuckbug;
	// Crouch-jump имеет смысл только на взлёте (см. useTakeoff у апстрима) — вне hasPrespeed
	// красить скорость в CJ-цвет было бы враньём (взлёта уже/ещё нет).
	info.crouchJump = info.hasPrespeed && src->hudService->crouchJumping;
	// walkedOff — «ушёл с края»: отрыв без прыжка и не с лестницы (апстрим один-в-один,
	// origin/master:src/kz/hud/kz_hud.cpp:113). Как и crouchJump, осмыслен только на взлёте:
	// вне hasPrespeed престрейф всё равно скрыт, но false здесь честнее нулевого поля.
	info.walkedOff = info.hasPrespeed && !src->jumped && !src->takeoffFromLadder;
	return info;
}

// Подавка viewpunch. Зовётся каждый тик на каждого игрока (из KZPlayer::OnProcessMovement),
// поэтому логов на такт здесь нет: пишем только на смене состояния, один раз на смену.
void KZHUDService::OnProcessMovement()
{
	if (!sv_suppress_viewpunch.IsValidRef() || !sv_suppress_viewpunch.IsConVarDataAvailable())
	{
		// Один раз на процесс: иначе строка в такте на каждого игрока.
		static bool warnedUnavailable = false;
		if (!warnedUnavailable)
		{
			warnedUnavailable = true;
			KZ_LOG_WARN(LogChannel::General, "[cyb] viewpunch_suppress_unavailable reason=convar_data_unavailable cvar=sv_suppress_viewpunch\n");
		}
		return;
	}

	bool wantSuppress = kz_suppress_viewpunch.Get();
	// Смена самого конвара — состояние сервера, не выводимое из БД: пишем ОДНУ строку на
	// смену на весь сервер (а не на каждого игрока, иначе строка размножится по слотам).
	static i32 loggedConVarState = -1;
	if (loggedConVarState != (i32)wantSuppress)
	{
		loggedConVarState = (i32)wantSuppress;
		KZ_LOG_INFO(LogChannel::General, "[cyb] viewpunch_suppress_changed enabled=%i\n", (i32)wantSuppress);
	}
	// Смена состояния отслеживается полем-кэшем: выключение конвара на живом сервере обязано
	// вернуть ванильное поведение сразу (view_punch_decay = 18, серверное значение = 0), а не
	// залипнуть у клиента до перезахода — клиенту значения досылаются ТОЛЬКО на смене.
	if (wantSuppress != this->viewpunchSuppressed)
	{
		this->viewpunchSuppressed = wantSuppress;
		utils::SendConVarValue(this->player->GetPlayerSlot(), sv_suppress_viewpunch, wantSuppress ? "1" : "0");
		utils::SendConVarValue(this->player->GetPlayerSlot(), "view_punch_decay", wantSuppress ? "99999" : "18");
	}
	// Серверное значение конвара — на каждом тике: движок и чужой код могут его перетереть,
	// а решает подавку именно оно (клиентский decay лишь убирает остаточную тряску предикта).
	auto dst = sv_suppress_viewpunch.GetConVarData()->Value(-1);
	auto traits = sv_suppress_viewpunch.TypeTraits();
	traits->Copy(dst, CVValue_t(this->viewpunchSuppressed));
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
	// Reset зовётся и на дисконнекте: без сброса новый клиент в том же слоте не получит
	// sv_suppress_viewpunch/view_punch_decay (состояние «уже подавлено» осталось бы от прошлого).
	this->viewpunchSuppressed = false;
	this->showPanel = this->player->optionService->GetPreferenceBool("showPanel", true);
	this->timerStoppedTime = {};
	this->currentTimeWhenTimerStopped = {};
	this->jumpedThisTick = false;
	this->fromDuckbug = false;
	this->crouchJumping = false;
	// Слот реально освобождается (дисконнект) — только здесь гасим *Active-флаг:
	// ClearBottomPanel() для НОВОГО игрока в этот слот не должен считать себя обязанным
	// клиру канала, которым никогда не владел.
	this->bottomPanelActive = false;
	// Кэш отправки нижней панели (слепки/тексты/heartbeat).
	this->ResetBottomPanelCache();
	// Слепок отправленной панели — улика конкретного игрока: новому в этом слоте она чужая,
	// а `kz_hud panel` показал бы её как свою.
	this->lastPanelSent.clear();
	this->lastPanelSentTime = {};
	// Слот реально освобождается — сущность и кэши классов панелей иначе достались бы
	// следующему игроку в этом слоте (реконнект/новый игрок).
	this->DestroyOwnedLayout();
	// Значения cl_crosshair* и флаг confirmed (Task 10) — DestroyOwnedLayout больше их не
	// трогает (только кэш классов ЭТОЙ сущности): здесь слот реально освобождается, и без
	// сброса новый игрок унаследовал бы «подтверждённые» cl_crosshair* ПРЕДЫДУЩЕГО, а
	// ApplyCrosshair нарисовал бы ему чужой крестик как настоящий.
	this->crosshair = MHUDCrosshairSettings();
	// Кэш префов худа — ровно тот же класс улики, что crosshair выше: слот освобождается, и без
	// сброса НОВЫЙ игрок в этом слоте рисовался бы настройками предыдущего до срабатывания
	// OnPlayerPreferencesLoaded, а спектатор с mhudMimicSpec мимикрировал бы под чужой набор
	// (loaded остался бы true — см. MHUDLayoutPrefs::loaded в kz_hud.h).
	this->layoutPrefs = MHUDLayoutPrefs();
	// Слот последней применённой цели mhudMimicSpec (entity.cpp/EnsureOwnedLayout) — слот
	// освобождается, оставленное здесь значение не влияло бы на корректность (запись выше уже
	// снесла сущность), но живёт по тому же правилу, что и остальные поля этого блока: кэш
	// сбрасывается там же, где реально освобождается владение слотом.
	this->layoutMimicSource = CPlayerSlot(-1);
	// Меню (Task 11) — своя сущность с курсорным захватом (см. layout/menu.cpp): дисконнект
	// обязан снять его так же, как ownedLayout выше, иначе следующий игрок в этом слоте
	// унаследует чужой menuOpen/diff-кэш, а у отключившегося сам захват уйдёт вместе с
	// сущностью, но наш флаг остался бы висеть, если не сбросить явно.
	this->DestroyOwnedMenuLayout();
	// Состояние обмена настройками худа (KZ::hudshare): слот отката живёт в памяти сессии, не в
	// префах, и по тому же правилу, что и кэши выше — слот реально освобождается, чужой снимок
	// «как было» (и чужой кулдаун выдачи кода) новому игроку в этом слоте не принадлежат.
	KZ::hudshare::ClearSlotState(this->player->GetPlayerSlot());
	// Меню реплея (layout/rpmenu.cpp) — свои сущности, та же причина.
	this->CloseReplayMenu("disconnect");
}

// Сброс кэша отправки нижней панели БЕЗ *Active-флага: раунд-старт (OnRoundStart, в т.ч.
// посреди карты — например кик реплей-бота) не освобождает канал получателя, поэтому не
// должен подавлять следующий клир-кадр (ClearBottomPanel полагается на флаг, чтобы не слать
// лишний #SFUI_EmptyString). Флаг гасится только в Reset(), где слот реально освобождён
// (дисконнект).
void KZHUDService::ResetBottomPanelCache()
{
	this->bottomStateValid = false;
	this->lastBottomText[0] = '\0';
	this->lastBottomSendTime = 0.0;
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
		// Меню (Task 11): смена карты сносит саму сущность движком (переживает её только наш
		// menuOpen-флаг на KZPlayer) — CloseLayoutMenu() безопасен и без валидной сущности
		// (см. её тело), но обязан быть вызван явно, иначе следующий Toggle() решит, что меню
		// всё ещё открыто, и попытается его "закрыть" вместо открытия.
		if (player->hudService->IsLayoutMenuOpen())
		{
			player->hudService->CloseLayoutMenu();
		}
		// Меню реплея: сущность снёс движок, бота кикнет OnRoundStart реплеев тем же хуком
		// ниже (hooks.cpp) — флаг сбрасываем здесь безусловно, иначе следующий !rpmenu
		// «закроет» несуществующее меню.
		player->hudService->CloseReplayMenu("round_start");
	}
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
// (отступ упирается в нижний край рамки канала). Alert — третий канал апстрима
// («actually more center» по апстримному описанию), рисуется движком в своём
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

// Престрейф в скобках и приписка C кибершоковской панели — ФИКСИРОВАННАЯ палитра, не
// MHUD-префы игрока: префы mhud*Color — цвета элементов panorama-худа, и сохранённое там
// экзотическое значение красило престрейф в невидимый цвет на тёмной панели
// (баг 25.07: у игрока mhudSpeedColor = чёрный ⇒ (престрейф) не виден вне перфа).
#define KZ_HUD_C_PERF    "#40FF40" // престрейф после перфа
#define KZ_HUD_C_JUMPBUG "#FFFF20" // престрейф после jumpbug/duckbug
#define KZ_HUD_C_CJ      "#71EEB8" // приписка C (crouch-jump)

Vector KZHUDService::GetDisplayVelocity(KZPlayer *src)
{
	Vector velocity, baseVelocity;
	src->GetVelocity(&velocity);
	src->GetBaseVelocity(&baseVelocity);
	velocity += baseVelocity;

	// Реплей-бот на паузе: его velocity принудительно обнулена, чтобы физика не унесла
	// замороженного бота (replays/playback.cpp), — худ показывал бы честный, но
	// бесполезный 0 вместо скорости момента, ради которого зритель и жмёт паузу.
	// Только на паузе: в движении пешке присвоена та же скорость из кадра записи.
	if (KZ::replaysystem::data::IsReplayPlaying() && KZ::replaysystem::data::GetCurrentReplay()->replayPaused)
	{
		Vector frameVelocity;
		if (KZ::replaysystem::playback::GetDisplayedFrameVelocity(src, frameVelocity))
		{
			return frameVelocity;
		}
	}
	return velocity;
}

std::string KZHUDService::GetCheckpointText(const char *language)
{
	const bool isReplay = KZ::replaysystem::IsReplayBot(this->player);
	const i32 cp = isReplay ? KZ::replaysystem::GetCurrentCpIndex() : this->player->checkpointService->GetCurrentCpIndex();
	const i32 cpCount = isReplay ? KZ::replaysystem::GetCheckpointCount() : this->player->checkpointService->GetCheckpointCount();
	// AWR-реплей: телепорты вырезаны, счётчик ТП кадра врал бы про то, что видит зритель —
	// вместо числа метка «AWR» (отдельная фраза: в этой числовой позиции строка не форматируется).
	if (isReplay && KZ::replaysystem::IsAwrMode())
	{
		return KZLanguageService::PrepareMessageWithLang(language, "HUD - Checkpoint AWR Text", cp, cpCount);
	}
	const i32 tp = isReplay ? KZ::replaysystem::GetTeleportCount() : (i32)this->player->checkpointService->GetTeleportCount();
	return KZLanguageService::PrepareMessageWithLang(language, "HUD - Checkpoint Text", cp, cpCount, tp);
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
// Явная иерархия по важности: заголовочные строки (таймер и скорость) > вторичная инфа >
// клавиши/метка стиля. ВНИМАНИЕ: имена классов — гипотезы (движок кибершока вероятно знает
// fontSize-l/m/sm/s); если класс неизвестен, размер молча откатится на дефолт — проверить
// вживую. Все в одном месте: иерархия правится одной строкой после живого теста.
// ВАЖНО: заголовочный кегль правится НЕ здесь, а через kz_hud_panel_headline ниже —
// KZ_HUD_FS_HEADLINE лишь его нулевой (дефолтный) вариант.
#define KZ_HUD_FS_HEADLINE  "fontSize-l"  // нулевой вариант ОБЕИХ заголовочных строк (по фото кибершока); разводит их kz_hud_panel_headline 3
#define KZ_HUD_FS_SECONDARY "fontSize-sm" // PB/WR, CP/TP, престрейф, Stage, координаты — вторичная инфа
#define KZ_HUD_FS_KEYS      "fontSize-m"  // клавиши (оба варианта раскладки: 2 ряда / одна строка). Был l — уменьшен на ступень по просьбе тестера; заодно меньше риск обрезки низа панели
#define KZ_HUD_FS_MINOR     "fontSize-s"  // метка стиля — наименее заметное

// Кегль ДВУХ ЗАГОЛОВОЧНЫХ строк панели (таймер и скорость) — cvar, а не константа.
// Причина: ёмкость центральной HTML-панели задана клиентом в ПИКСЕЛЯХ, а не в строках, и
// хвост, не поместившийся в рамку, движок молча не рисует. Замеры той же панели живьём:
// 9 строк кеглем fontSize-sm (vendor/mm-cs2menus .../menu_manager.cpp, kHtmlPanelLineBudget) и
// «≈9 экранных строк» у GG1 (gameops/plugins/CLAUDE.md) — причём GG1 рисует пункты кеглем
// fontSize-m и число влезающих строк ему пришлось пересчитывать: бюджет тратится КЕГЛЕМ.
// Эти две строки — самые дорогие в панели, и класс fontSize-l вдобавок под вопросом: cs2menus
// пользуется только s/sm/m, знает ли Panorama клиента класс -l, из наших исходников не следует
// (неизвестный класс = дефолтный кегль панели, а он крупный).
// Перебирать кегль пересборкой форка — час на итерацию (тот же довод, что у
// kz_hud_bottom_pad_*), поэтому подбирается по rcon на канарейке.
// 0 (деф.) — как было; 1 — обе кеглем клавиш (fontSize-m); 2 — обе кеглем вторичной инфы;
// 3 — ТОЛЬКО таймер мельче (fontSize-m), скорость остаётся крупной. Вариант 3 повторяет кегль
// ТАЙМЕРА, каким он был до 86b2357 (24.07 14:46, там таймер стал -l). Двух рядов клавиш тогда
// ещё не было (они появились через 37 минут в b0a9f4e), так что сочетание «таймер -m + два
// ряда» не существовало никогда и НЕ измерялось: влезет ли — вопрос живого прогона.
static CConVar<int> kz_hud_panel_headline("kz_hud_panel_headline", FCVAR_NONE,
										  "HUD panel headline rows font: 0 = both large, 1 = both medium, 2 = both small, 3 = timer medium only.", 0);

// Таймер и скорость ОДНОЙ строкой вместо двух. Единственный размен, который не трогает кегли
// вовсе: убирает целую строку самого дорогого класса. Тот же приём уже применён к показаниям
// под меню (см. FormatBottomText) и по той же причине. Цена — длинная строка у ЖИВОГО игрока
// (PRO + время + режим + скорость + престрейф) может не влезть по ширине и перенестись, а
// перенос съедает ту же строку обратно; у спектатора реплея строка короткая.
static CConVar<bool> kz_hud_panel_merge_head("kz_hud_panel_merge_head", FCVAR_NONE, "Draw the HUD panel timer and speed on one line instead of two.",
											 false);

// Запоминать текст панели, РЕАЛЬНО ушедший каждому получателю (см. lastPanelSent в kz_hud.h).
// Только под этим cvar'ом: иначе это копия строки на каждого получателя каждый такт. Включается
// по rcon на время разбора, `kz_hud panel` тогда печатает и отправленное, и пересобранное.
static CConVar<bool> kz_hud_panel_trace("kz_hud_panel_trace", FCVAR_NONE, "Remember the last HUD panel text actually sent, for `kz_hud panel`.",
										false);

// Класс кегля строки таймера и строки скорости. Разведены (а не одна функция), потому что
// вариант 3 мельчит только таймер.
static_function const char *TimerFontClass()
{
	switch (kz_hud_panel_headline.Get())
	{
		case 1:
		case 3:
			return KZ_HUD_FS_KEYS;
		case 2:
			return KZ_HUD_FS_SECONDARY;
		default:
			return KZ_HUD_FS_HEADLINE;
	}
}

static_function const char *SpeedFontClass()
{
	switch (kz_hud_panel_headline.Get())
	{
		case 1:
			return KZ_HUD_FS_KEYS;
		case 2:
			return KZ_HUD_FS_SECONDARY;
		default: // 0 и 3 — скорость остаётся главным акцентом
			return KZ_HUD_FS_HEADLINE;
	}
}

// Метка режима наблюдаемого ЗАГЛАВНЫМИ (CKZ/KZT/VNL): короткое имя через
// CybReplayCommon::MapMode (тот же маппинг/вайтлист, что у PB/WR-фетча в kz_timer.cpp).
// Пустая строка = кастомный режим сверх этих трёх (метку не показываем) или нет modeService.
// Используется строкой 1 кибершоковского худа, В ТОМ ЧИСЛЕ для реплей-бота (см. вызывающего).
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
	// showpos — тумблер получателя (this->player), данные наблюдаемого.
	bool showPos = !compact && this->player->optionService->GetPreferenceBool("showPos", false);
	// CP/TP — нижняя строка панели (гейт hudCpTp получателя). Компакт её НЕ убирает: у элемента
	// свой тумблер, и в прежней нижней панели он тоже переживал компакт. Без ветки masterMode
	// (в отличие от соседей выше): suppress-флага у CP/TP нет и быть не может — particle-MHUD
	// такого элемента не рисует, дублировать нечего, поэтому тумблер действует всегда.
	bool showCpTp = kz_hud_cptp_in_panel.Get() && this->IsMHUDCpTpEnabled();

	// Типографика (иерархия): таймер — TimerFontClass(), скорость — SpeedFontClass() (оба через
	// cvar kz_hud_panel_headline, дефолт KZ_HUD_FS_HEADLINE), вторичная инфа (PB/WR, CP/TP,
	// престрейф, Stage, координаты) — KZ_HUD_FS_SECONDARY, клавиши — KZ_HUD_FS_KEYS, метка стиля —
	// KZ_HUD_FS_MINOR (значения классов — в #define рядом с палитрой выше).
	// Классы вкладываем В color-теги: <font class='...'><font color='...'>X</font></font>.
	// Это прогрессивное улучшение — если движок не подхватит класс, размер молча дефолтный,
	// но цвета и раскладка остаются корректными (класс лишь меняет кегль, не текст/цвет).

	// Заголовочные строки копим, а не отправляем сразу: при kz_hud_panel_merge_head они уходят
	// ОДНОЙ строкой (экономит строку самого дорогого класса, не трогая кегли).
	const bool mergeHead = kz_hud_panel_merge_head.Get();
	std::string headTimer, headSpeed;

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
					// Часы стоят: действующей попытки нет (вошли в prac без рана и ещё не выходили
					// из стартовой зоны / ноуклип обнулил попытку). Плейсхолдер той же ширины,
					// что «MM:SS.CC», DIM — тот же идиом, что у пустых PB/WR ниже. Белое 00:00.00
					// читалось бы как «время ноль, часы вот-вот пойдут», а пойдут они только с
					// выхода из стартовой зоны или с !practp на точку со временем.
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
			// (GetModeTagUpper; пусто = кастомный режим → метки нет).
			// РЕПЛЕЙ-БОТ ТОЖЕ ПОКАЗЫВАЕТ РЕЖИМ (10.08). Прежний гейт `!isReplay` стоял на
			// предположении «своего режима у бота нет» — оно неверно: плейбек применяет боту
			// ЗАПИСАННЫЙ режим забега (`RPEVENT_MODE_CHANGE` → `SwitchToMode`, replays/events.cpp),
			// то есть `modeService` у него ровно от того рана, который смотрят. Без метки зритель
			// реплея не видел, в каком режиме забег, — это была потеря информации, а не экономия.
			// Метка есть С ПЕРВОГО ТИКА: рекордер всегда кладёт базовое событие режима на
			// serverTick 0 (recording/recorders.cpp, синтез из earliestMode), а перемотка назад
			// переигрывает события с нуля. Краевой случай: если записанный режим на этом сервере
			// не загружен, SwitchToMode тихо возвращает false и бот остаётся на режиме сервера —
			// метка тогда покажет режим СЕРВЕРА, а не забега.
			// PRO/NUB и метка стиля у реплея намеренно остаются под `!isReplay`: данные для них
			// у бота тоже есть, но это отдельное решение о составе худа, а не часть этого фикса.
			// Видимая ширина правой части («&#160;&#160;режим» + «&#160;&#160;стиль») в «символах» —
			// нужна для левого паддинга, чтобы ВРЕМЯ встало по центру экрана (см. leftPad ниже).
			int rightVisChars = 0;
			std::string modeTag;
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
			// Слитая заголовочная строка центруется целиком (время + скорость), и добивать её
			// слева под центр ВРЕМЕНИ нечем и незачем — паддинг там только сдвигал бы всё вправо.
			const int kLeftPadNbspPerChar = 2; // nbsp на 1 «символ» правой части; крутить после теста
			std::string leftPad;
			int padBudget = (mergeHead && showSpeed) ? 0 : rightVisChars - leftVisChars;
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
			V_snprintf(buf, sizeof(buf), "%s%s<font class='%s'><font color='%s'>%s</font></font>%s%s", leftPad.c_str(), proNubTag.c_str(),
					   TimerFontClass(), timerColor, tTime.c_str(), modeTag.c_str(), styleTag.c_str());
			headTimer = buf;
		}
	}

	// --- Строка 2: 2064 (784) [JB] — крупная белая скорость (m), мелкий престрейф (s) в
	//        перф/jumpbug/базовом цвете, приписка JB при строго классифицированном jumpbug
	//        (KZ_HUD_C_JUMPBUG) либо C при crouch-jump (KZ_HUD_C_CJ) — взаимоисключающе, JB
	//        приоритетнее. Всё — по текущему условию onGroundSettled (престрейф/C/JB скрыты,
	//        когда игрок осел на земле). ---
	if (showSpeed)
	{
		Vector velocity = KZHUDService::GetDisplayVelocity(dataSource);
		i32 speed = RoundFloatToInt(velocity.Length2D());

		bool onGroundSettled = (dataSource->GetPlayerPawn()->m_fFlags() & FL_ONGROUND
								&& g_pKZUtils->GetServerGlobals()->curtime - dataSource->landingTime > KZ_HUD_ON_GROUND_THRESHOLD)
							   || (dataSource->GetPlayerPawn()->m_MoveType() == MOVETYPE_LADDER && !dataSource->IsButtonPressed(IN_JUMP));

		std::string takeoff;
		if (!onGroundSettled)
		{
			// Цвет — из палитры стандартного худа (см. KZ_HUD_C_PERF рядом с ней), а НЕ из
			// mhud*Color игрока: те префы про цвета элементов panorama-худа, и чужое значение
			// делало престрейф невидимым. Зазор — nbsp, как во всех остальных строках (обычный
			// пробел схлопывается).
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
		V_snprintf(buf, sizeof(buf), "<font class='%s'><font color='" KZ_HUD_C_CYAN "'>%d</font></font>%s", SpeedFontClass(), speed, takeoff.c_str());
		headSpeed = buf;
	}

	// --- Заголовок панели: две строки (деф.) либо одна слитая (kz_hud_panel_merge_head).
	//        Слияние — единственный размен, не трогающий кегли: панель ограничена по ВЫСОТЕ, и
	//        строка самого дорогого класса стоит дороже всего остального. Разделитель — пара nbsp
	//        (обычные пробелы схлопываются). Слияние осмысленно только когда обе части есть:
	//        с одной включённой это та же одна строка. ---
	if (mergeHead && !headTimer.empty() && !headSpeed.empty())
	{
		addLine(headTimer + "&#160;&#160;" + headSpeed);
	}
	else
	{
		if (!headTimer.empty())
		{
			addLine(headTimer);
		}
		if (!headSpeed.empty())
		{
			addLine(headSpeed);
		}
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
	//        лейблы PB/WR MUTED, время PB белое, время WR зелёное. PB/WR — ЛУЧШЕЕ из
	//        платформенного api-кэша (совпадает с сайтом) и НАШЕГО локального кэша плагина
	//        (localPBCache для PB, srCache для WR): платформенный обновляется раз на карту, а
	//        локальный на каждом финише, и «платформенный первым» прятал свежий рекорд до смены
	//        карты (правка 18.08, обоснование в kz_timer.cpp). Курс — pbwrCourse (активный или
	//        главный вне старт-зоны).
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

	// --- Зазора между верхней группой (таймер/скорость/Stage/PB-WR) и нижней (клавиши/CP-TP/
	//        showpos) БОЛЬШЕ НЕТ (10.08). Панель ограничена по ВЫСОТЕ, и хвост за рамкой движок
	//        молча не рисует (репорт: на реплее видно «C W J», а «A S D» и CP/TP нет). Зазор был
	//        пустой строкой: она занимала строку целиком — пусть и мелкого кегля, то есть
	//        дешевле строки клавиш, но платить пикселями за пустоту в переполненной панели
	//        нельзя. Вернуть его можно только когда будет чем платить (kz_hud_panel_headline). ---

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
		// AWR-реплей — метка вместо числа ТП (см. GetCheckpointText).
		const bool awr = isReplay && KZ::replaysystem::IsAwrMode();
		std::string cpTpText = awr ? KZLanguageService::PrepareMessageWithLang(language, "HUD - Bottom CP/TP AWR Text", cp, cpCount)
								   : KZLanguageService::PrepareMessageWithLang(language, "HUD - Bottom CP/TP Text", cp, cpCount, tp);
		V_snprintf(buf, sizeof(buf), "<font class='" KZ_HUD_FS_SECONDARY "'><font color='" KZ_HUD_C_WHITE "'>%s</font></font>", cpTpText.c_str());
		addLine(buf);
	}

	// --- Координаты и углы (!showpos) — ДВЕ строки: «pos: x y z» и «ang: pitch yaw». Тумблер —
	//        настройка получателя (this), данные — наблюдаемого (dataSource). В компакте скрыто.
	//        Почему две: с точностью до сотых одна строка не влезала в ширину панели и рвалась
	//        движком посередине (решение пользователя 18.08 — переносить по смыслу). Цена — на
	//        одну строку больше в панели, чья высота ограничена; поэтому блок и стоит ПОСЛЕДНИМ
	//        (см. комментарий у CP/TP выше): при обрезке низа страдают отладочные координаты. ---
	if (showPos)
	{
		Vector origin;
		QAngle angles;
		dataSource->GetOrigin(&origin);
		dataSource->GetAngles(&angles);
		// Две строки, а не одна: до сотых «pos … ang …» не влезало в ширину панели и рвалось
		// движком посередине (решение пользователя 18.08 — переносить по смыслу).
		std::string posText = KZLanguageService::PrepareMessageWithLang(language, "HUD - Position Text", origin.x, origin.y, origin.z);
		V_snprintf(buf, sizeof(buf), "<font class='" KZ_HUD_FS_SECONDARY "'><font color='" KZ_HUD_C_MUTED "'>%s</font></font>", posText.c_str());
		addLine(buf);
		std::string angText = KZLanguageService::PrepareMessageWithLang(language, "HUD - Angles Text", angles.x, angles.y);
		V_snprintf(buf, sizeof(buf), "<font class='" KZ_HUD_FS_SECONDARY "'><font color='" KZ_HUD_C_MUTED "'>%s</font></font>", angText.c_str());
		addLine(buf);
	}

	return html;
}

// `kz_hud panel` / `!hud panel` — см. объявление в kz_hud.h. Живёт здесь, рядом со сборщиком
// панели и CountHtmlLines (второй счётчик строк разъехался бы с реальным при смене
// разделителя — диагностика, которая врёт, хуже её отсутствия).
// Разбор ОДНОЙ панели в консоль получателя: построчно кегль, видимая длина, длина в байтах,
// баланс тегов и сырой текст кусками. Отдельная функция, потому что печатать надо ДВЕ панели —
// реально отправленную (kz_hud_panel_trace) и пересобранную сейчас: расхождение между ними
// делит пространство поиска пополам («на проводе было другое» против «клиент не нарисовал»).
static_function void DumpPanel(CBaseEntity *controller, const char *tag, const std::string &html)
{
	utils::PrintConsole(controller, "[KZ]  --- %s: %d lines, %d bytes\n", tag, CountHtmlLines(html), (int)html.size());
	if (html.empty())
	{
		return;
	}
	// Порядок «крупности» классов; неизвестное имя считаем крупнее всех известных — движок
	// откатит его на дефолтный кегль панели, а он крупный, и ошибаться тут надо в сторону
	// «дороже», не «дешевле».
	static const char *const kRank[] = {KZ_HUD_FS_MINOR, KZ_HUD_FS_SECONDARY, KZ_HUD_FS_KEYS, KZ_HUD_FS_HEADLINE};
	int index = 0;
	for (size_t pos = 0; pos <= html.size();)
	{
		size_t br = html.find("<br>", pos);
		std::string line = html.substr(pos, (br == std::string::npos ? html.size() : br) - pos);
		index++;

		// Печатаем ОБА кегля: ПЕРВЫЙ класс в строке и МАКСИМАЛЬНЫЙ. Это не избыточность —
		// разбор 10.08 показал, что различает случаи именно ПЕРВЫЙ (строка таймера живого
		// игрока начинается с fontSize-sm от leftPad/PRO-NUB, а у реплей-бота сразу с
		// fontSize-l), а печать одного лишь максимума этот механизм и скрывала: по максимуму
		// обе строки одинаковы. «По первому» — единственная из рассмотренных моделей,
		// совместимая со всеми живыми точками; физический механизм не установлен, поэтому в
		// дампе должны быть ОБА числа, а не выбранное нами.
		char cls[32] = "default";
		char firstCls[32] = "default";
		bool haveFirst = false;
		int bestRank = -1;
		for (size_t cp = line.find("class='"); cp != std::string::npos; cp = line.find("class='", cp + 7))
		{
			char name[32];
			size_t end = line.find('\'', cp + 7);
			// Гейт по размеру ПРИЁМНИКА (name), не cls: буферы одинаковы сегодня, но разъедутся
			// молча. continue, а не break: бросив разбор на одном подозрительном вхождении, мы
			// потеряли бы крупный класс дальше по строке и отрапортовали её дешевле, чем есть.
			if (end == std::string::npos || end - (cp + 7) >= sizeof(name))
			{
				// Битое/переросшее вхождение. Помечаем его В ДАМПЕ, а не пропускаем молча:
				// «первый класс» — сейчас главная улика, и подменить его вторым значило бы
				// соврать ровно в том случае, ради которого дамп и писался.
				if (!haveFirst)
				{
					haveFirst = true;
					V_strncpy(firstCls, "<битый>", sizeof(firstCls));
				}
				continue;
			}
			V_strncpy(name, line.c_str() + cp + 7, (int)(end - (cp + 7)) + 1);
			if (!haveFirst)
			{
				haveFirst = true;
				V_strncpy(firstCls, name, sizeof(firstCls));
			}
			int rank = (int)KZ_ARRAYSIZE(kRank);
			for (int r = 0; r < (int)KZ_ARRAYSIZE(kRank); r++)
			{
				if (KZ_STREQ(name, kRank[r]))
				{
					rank = r;
					break;
				}
			}
			if (rank > bestRank)
			{
				bestRank = rank;
				V_strncpy(cls, name, sizeof(cls));
			}
		}

		// «Видимая длина» — ЗНАКИ, не ширина: тег считаем за 0, HTML-сущность за 1. Две
		// оговорки, без которых число обманет. (1) `&#160;` у́же буквы (≈0.5–0.6, см. leftPad
		// выше), поэтому у строки с паддингом длина завышена. (2) Порог переноса зависит от
		// кегля: 35 колонок намерены для fontSize-sm (kHtmlWrapChars в cs2menus), для -l он
		// заметно меньше. Число годится для сравнения строк ОДНОГО класса между собой, а не
		// для вывода «влезает / не влезает».
		int visible = 0;
		for (size_t i = 0; i < line.size(); i++)
		{
			if (line[i] == '<')
			{
				i = line.find('>', i);
				if (i == std::string::npos)
				{
					break;
				}
			}
			else if (line[i] == '&')
			{
				size_t semi = line.find(';', i);
				if (semi == std::string::npos)
				{
					break;
				}
				i = semi;
				visible++;
			}
			else if (((unsigned char)line[i] & 0xC0) != 0x80)
			{
				// Продолжающие байты UTF-8 (0b10xxxxxx) — не отдельные знаки. Без этой
				// проверки китайский перевод CP/TP считался бы втрое длиннее, чем есть,
				// а на этом числе строится рассуждение про порог переноса.
				visible++;
			}
		}

		// Баланс тегов строки: незакрытый <font> проглатывает всё, что идёт дальше, и симптом
		// «пропал хвост» неотличим от нехватки высоты. Считаем вхождения буквально —
		// '</font>' не содержит подстроки '<font', так что счётчики независимы. Баланс ПО
		// СТРОКЕ, а не по всей панели: каждый addLine самодостаточен, теги через <br> не тянутся.
		int opened = 0, closed = 0;
		for (size_t t = line.find("<font"); t != std::string::npos; t = line.find("<font", t + 5))
		{
			opened++;
		}
		for (size_t t = line.find("</font>"); t != std::string::npos; t = line.find("</font>", t + 7))
		{
			closed++;
		}
		// Длина в БАЙТАХ — самый дешёвый дискриминатор при побайтовом сравнении двух дампов:
		// хвостовые пробелы в консоли не видны, а «строка кончилась» от «кусок кончился» без
		// счётчика не отличить.
		utils::PrintConsole(controller, "[KZ]   line %d: first=%-12s max=%-12s %d bytes, %d visible chars, <font>=%d </font>=%d%s\n", index, firstCls,
							cls, (int)line.size(), visible, opened, closed, opened == closed ? "" : "  <<< НЕБАЛАНС");

		// Сырой текст кусками: буфер PrintConsole невелик, а нужны именно байты. Границу куска
		// подрезаем назад с продолжающих байтов UTF-8 (0b10xxxxxx) — рвать кодовую точку нельзя:
		// битая последовательность уедет в protobuf-поле и испортит улику ровно в локализованном
		// случае (CP/TP есть и в китайском переводе).
		const size_t kChunk = 160;
		for (size_t off = 0, part = 1; off < line.size(); part++)
		{
			size_t len = (std::min)(kChunk, line.size() - off);
			while (len > 1 && ((unsigned char)line[off + len] & 0xC0) == 0x80)
			{
				len--;
			}
			utils::PrintConsole(controller, "[KZ]   raw %d.%d: %s\n", index, (int)part, line.substr(off, len).c_str());
			off += len;
		}

		if (br == std::string::npos)
		{
			break;
		}
		pos = br + 4;
	}
}

void KZHUDService::PrintPanelDiagnostics()
{
	auto *controller = this->player->GetController();
	if (!controller)
	{
		return;
	}
	// Источник данных — как в DrawPanels: наблюдаемый при спектейте, иначе сам игрок.
	KZPlayer *dataSource = this->player->specService->GetSpectatedPlayer();
	if (!dataSource)
	{
		dataSource = this->player;
	}
	// Вид цели печатаем явно: за живым игроком и за реплей-ботом панель ведёт себя по-разному
	// (репорт 10.08), и без метки цели два дампа не сопоставить. Обычный фейк-клиент (не
	// реплей-бот) попадёт в «live player» — для этой задачи безразлично.
	const char *kind = (dataSource == this->player) ? "self" : (KZ::replaysystem::IsReplayBot(dataSource) ? "replay bot" : "live player");
	// Состояние ВСЕХ тумблеров, влияющих на состав панели: без него числа строк из разных
	// прогонов несопоставимы (в одном showpos включён, в другом нет — и «пропавшая последняя
	// строка» это разные строки).
	utils::PrintConsole(controller,
						"[KZ] HUD panel: target=%s | cvars headline=%d merge_head=%d cptp_in_panel=%d trace=%d | prefs hudTimer=%d hudSpeed=%d "
						"hudKeys=%d twoRows=%d hudPbWr=%d hudCpTp=%d showPos=%d compact=%d\n",
						kind, kz_hud_panel_headline.Get(), kz_hud_panel_merge_head.Get() ? 1 : 0, kz_hud_cptp_in_panel.Get() ? 1 : 0,
						kz_hud_panel_trace.Get() ? 1 : 0, this->IsMHUDTimerEnabled() ? 1 : 0, this->IsMHUDSpeedEnabled() ? 1 : 0,
						this->IsMHUDKeysEnabled() ? 1 : 0, this->IsMHUDKeysTwoRowsEnabled() ? 1 : 0, this->IsMHUDPbWrEnabled() ? 1 : 0,
						this->IsMHUDCpTpEnabled() ? 1 : 0, this->player->optionService->GetPreferenceBool("showPos", false) ? 1 : 0,
						this->IsCompactPanel() ? 1 : 0);

	// Отправленное: то, что реально ушло в канал последним. Доступно только при включённом
	// kz_hud_panel_trace — иначе поле пусто, и об этом надо сказать, а не молчать.
	if (!this->lastPanelSent.empty())
	{
		f64 age = g_pKZUtils->GetServerGlobals()->curtime - this->lastPanelSentTime;
		utils::PrintConsole(controller, "[KZ]  (отправлено %.2f с назад)\n", age);
		DumpPanel(controller, "SENT", this->lastPanelSent);
	}
	else if (!kz_hud_panel_trace.Get())
	{
		utils::PrintConsole(controller, "[KZ]  SENT: слепок не собирается — включите kz_hud_panel_trace 1 и повторите\n");
	}
	else
	{
		// Трейс включён, а слепка нет — панель этому получателю сейчас не отправляется вовсе.
		// Штатных причин две, обе с ранним return в DrawPanels: hudType Off, открытое
		// Html-меню (либо hudType Panorama — тогда HTML-путь не строится вовсе).
		// Валить это на cvar — врать.
		utils::PrintConsole(controller, "[KZ]  SENT: пусто — панель не отправляется (hudType Off / MHUD / открытое меню)\n");
	}

	// Пересобранное сейчас. Пешка обязательна: сборщик читает её флаги/скорость напрямую, а
	// команду, в отличие от тактового пути, можно позвать мёртвым и без цели наблюдения.
	// Меряем ИМЕННО кибершоковскую панель (Обновлённый стиль) с мастер-тумблерами — ровно то,
	// что собирает DrawPanels; текущий hudType/стиль на замер намеренно не влияет.
	if (!dataSource->GetPlayerPawn())
	{
		utils::PrintConsole(controller, "[KZ]  BUILT: нет источника данных (мёртв и никого не спектит)\n");
		return;
	}
	std::string html = this->BuildVersionCHud(dataSource, false, false, false, true, this->player->languageService->GetLanguage());
	DumpPanel(controller, "BUILT", html);
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
// из реплей-системы, как апстримный GetCheckpointText).
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
		// data/settings, что в BuildVersionCHud. До 05.08 плайн-худ под меню их не смотрел
		// вовсе и показывал выключенные элементы.
		out.showSpeed = cfg->IsMHUDSpeedEnabled();
		out.showKeys = cfg->IsMHUDKeysEnabled();
		if (out.showSpeed)
		{
			Vector velocity = KZHUDService::GetDisplayVelocity(player);
			out.speed = RoundFloatToInt(velocity.Length2D());
			// Точка отрыва в скобках — то же условие, что в GetSpeedInfo/hasPrespeed: в воздухе
			// либо сразу после приземления (сглаживание мерцания); на лестнице — только с
			// зажатым прыжком.
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
		// Часть слепка: смена режима реплея обязана перерисовать низ (см. GetCheckpointText).
		out.awr = isReplay && KZ::replaysystem::IsAwrMode();
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
		std::string line = state.awr
							   ? KZLanguageService::PrepareMessageWithLang(state.lang, "HUD - Bottom CP/TP AWR Text", state.cp, state.cpCount)
							   : KZLanguageService::PrepareMessageWithLang(state.lang, "HUD - Bottom CP/TP Text", state.cp, state.cpCount, state.tp);
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
// KZ_HUD_BOTTOM_HEARTBEAT — centre-канал надёжный (BUF_RELIABLE), слать 64/с (тик CS2)
// каждому получателю расточительно, а неизменившийся текст переотправляется как есть без
// пересборки. Меняющийся слепок при этом уходит каждый тик — это не heartbeat, а изменение.
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

void KZHUDService::DrawPanels(KZPlayer *player, KZPlayer *target)
{
	KZHUDService *cfg = target->hudService;

	// Меню реплея спектатора (layout/rpmenu.cpp) — до всех ветвлений по типу худа: у него
	// своя сущность, оно не зависит от hudType получателя и обязано тикать (ввод + рендер),
	// пока игрок наблюдает бота; само же закрывается, когда player перестал быть ботом.
	cfg->UpdateReplayMenu(player);

	bool available = KZHUDService::IsMHUDAvailable();
	// hudType: 0 = Standard (HTML-панель), 2 = Off (ничего), 3 = Panorama (custom_hud_layout
	// сущность). 1 (particle-оверлей апстрима) удалён в задаче 12 — апстрим вычистил
	// particles/* из воркшоп-аддона, путь больше не работал; GetHudType() мигрирует
	// сохранённую 1 в Panorama.
	//
	// Panorama-путь: сущность custom_hud_layout (Task 6). Текст в неё пишет СЕРВЕР по
	// конкретному слоту (UpdateLayoutElement/SetPanelText по классам схемы), а не клиентский
	// оператор, считающий экранную позицию по взгляду владельца, поэтому НЕТ гейта «только
	// живой владелец» — panorama одинаково пригодна и владельцу, и спектатору (cfg — всегда
	// settings/entity ПОЛУЧАТЕЛЯ, т.е. target, а данные берутся у player — наблюдаемого при
	// спектейте, иначе он же сам). Сущность рисуется независимо от HTML-центр-канала —
	// поэтому она обязана жить и обновляться и под открытым cs2menus-меню: ниже мы уходим с
	// раннего return'а ДО проверки Off/меню/needHtml, чтобы не завести второй путь показаний
	// тех же данных поверх panorama.
	bool usePanorama = available && cfg->GetHudType() == HUD_TYPE_PANORAMA;
	if (usePanorama)
	{
		if (cfg->UpdateHudLayout(player))
		{
			// needHtml ниже никогда не увидит usePanorama=true — html и panorama обязаны
			// оставаться взаимоисключающими (иначе получатель ловит оба худа разом).
			cfg->ClearBottomPanel();
			return;
		}
		// Сущность не создалась (см. reason в логе UpdateHudLayout) — это ОТКАЗ, а не выбор
		// игрока. Падать в тишину нельзя: усыновляем needHtml-фолбэк ниже, а не return'им.
		usePanorama = false;
	}
	else
	{
		// Ушли с panorama-типа (или он никогда не был выбран) — сущность и её кэши классов
		// не должны переживать смену типа, иначе на экране останется застывший худ.
		cfg->DestroyOwnedLayout();
	}

	// Тип Off — единственный рычаг «выключить худ целиком»: ни panorama (уже обработана
	// выше — не выбрана либо отвалилась в HTML-фолбэк), ни HTML-панель, ни нижнюю панель.
	if (cfg->GetHudType() == HUD_TYPE_OFF)
	{
		cfg->ClearBottomPanel();
		return;
	}

	// Yield the center channel while a cs2menus HTML menu is open.
	// ГЛУШЕНИЕ центр-канала — под ЛЮБЫМ Html-меню и для всех: HTML-панель одна на игрока, и
	// пока она принадлежит меню, худ обязан уступить (иначе тексты лезут друг на друга).
	// Показания «строкой в панели меню» (UpdateBottomPanel с menuOpen) больше не рисуются:
	// единственный их потребитель — cs2menus-!rpmenu — удалён 09.09 (меню реплея живёт на
	// panorama, hud/layout/rpmenu.cpp, и центр-канал не занимает). Под любым оставшимся
	// Html-меню низ просто гасится; клир гасит ИМЕННО тот приёмник, в который писали.
	if (g_pMenus && g_pMenus->GetActiveMenuType(target->GetPlayerSlot().Get()) == MenuType::Html)
	{
		cfg->ClearBottomPanel();
		return;
	}
	const char *language = target->languageService->GetLanguage();

	// К этой строке Off и живой panorama-путь уже отсеяны ранними return'ами выше, а
	// usePanorama, если panorama отказала, сброшен в false — значит здесь он ГАРАНТИРОВАННО
	// false, и единственный оставшийся путь ниже — HTML. До удаления particle-MHUD (задача 12)
	// needHtml реально мог быть false на этом месте (particle рисовал свою сущность и не
	// нуждался в HTML) — с его уходом два первых условия ниже стали недостижимым слоем поверх
	// константы; оставляем именованную переменную ради читаемости остального кода функции.
	bool needHtml = true;

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
		// "%s", а не текст форматом: KZPlayer::PrintHTMLCentre прогоняет аргумент через
		// FormatV, а utils::PrintHTMLCentre — ВТОРОЙ раз. Первый же `%` в переводимой фразе
		// (CP/TP, showpos) съел бы хвост панели — тот самый симптом, который тут и чинят.
		target->PrintHTMLCentre(false, false, "%s", htmlText.c_str());
		// Слепок отправленного — под cvar'ом, см. kz_hud_panel_trace. Пишем ПОСЛЕ отправки и
		// ровно ту строку, что ушла (после трима выше), иначе улика не про провод.
		if (kz_hud_panel_trace.Get())
		{
			cfg->lastPanelSent = htmlText;
			cfg->lastPanelSentTime = g_pKZUtils->GetServerGlobals()->curtime;
		}
	}

	// --- Нижняя панель (обычный centre-канал): строка CP/TP — ТОЛЬКО при kz_hud_cptp_in_panel 0.
	//        По умолчанию CP/TP уже нарисован последней строкой HTML-панели выше (там кегль
	//        фиксированный и накрыть себя панель не может), а этот канал остаётся пустым.
	//        Ниже — про прежнюю раскладку (cvar 0): СОПРОВОЖДАЕТ HTML-панель — преф hudCpTp
	//        всегда был про HTML-путь, а до этой точки в функции живой panorama и Off уже
	//        вышли своими return'ами (needHtml здесь всегда true, см. её объявление выше) —
	//        поэтому и спектатор (всегда HTML), и мёртвый видят низ.
	//        Отправка/пересборка — по изменению слепка + heartbeat (см. UpdateBottomPanel);
	//        переход «был текст → стало нечего» стирает остаток одноразовым клиром. Каждый
	//        получатель (владелец/спектатор) получает свой вызов DrawPanels →
	//        includeSpectators=false. ---
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
		// сервере).
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

// !hud — команда осталась от particle-пути (спека §4: "!hud остаётся как есть", §8 снимает
// только `!mhud *`), но её определение жило в particles.cpp и уехало вместе с ним (задача 12).
// Возвращаем ровно две живые роли: без аргументов — открыть настройки худа (те же, что !hudmenu:
// категория HUD реестра, layout/menu.cpp), `panel` — диагностика вместимости центральной
// HTML-панели. Остальные субкоманды particle-эпохи (offset/scale/*color/font/reset) не
// возвращаем: настройки, которые они правили, теперь пункты меню, а particle-худа больше нет.
SCMD(kz_hud, SCFL_HUD | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	if (args->ArgC() < 2)
	{
		if (player->hudService->IsLayoutMenuOpen())
		{
			player->hudService->CloseLayoutMenu();
		}
		else
		{
			player->hudService->OpenLayoutMenu("HUD - Menu Cat General");
		}
		return MRES_SUPERCEDE;
	}
	if (KZ_STREQI(args->Arg(1), "panel"))
	{
		// Отдельной субкомандой, а не в сводке: сводка печаталась ТОЛЬКО когда cs2menus не
		// загружен, то есть на живом сервере была недостижима.
		player->hudService->PrintPanelDiagnostics();
		return MRES_SUPERCEDE;
	}
	player->languageService->PrintChat(true, false, "HUD Command Usage");
	return MRES_SUPERCEDE;
}
