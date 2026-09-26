// Пять элементов panorama-худа + точка сборки (Task 6). Перенесено с апстрима
// (origin/master:src/kz/hud/layout/mhud.cpp), расхождения с ним — см. заметки планирования
// задачи в отдельном (не этом) репозитории, кратко (R1-R4):
//   - MHUDElement/MHUD_ELEMENTS -> LayoutElement/LAYOUT_ELEMENTS (R1, имя было занято
//     particles.cpp — файл удалён в задаче 12 вместе с particle-MHUD);
//   - GetPrefs()/IsMHUDElementEnabled -> GetLayoutPrefs()/IsLayoutElementEnabled (R4);
//   - GetPreferenceColor -> GetMHUDColorPref, дефолтные цвета и сама функция — kz_hud.cpp (R2,
//     задача 12 подняла реализацию туда из удалённого particles.cpp);
//   - SpeedInfo больше не апстримный тип: это ОБЩИЙ метод KZHUDService::GetSpeedInfo()
//     (kz_hud.cpp) — см. R3.
//   - клавиши: транш "клавиши" (Task 5) довёл UpdateKeysElement до апстримного состава —
//     keysIdle/keysBorder/keysGlowEnabled(keysGlow)/keysFillEnabled(keysFill)/keysLetters/
//     keysSquare/keysOverlapAxis/keysOverlapGlow/keysPressed-цвет заведены (см. kz_hud.h,
//     layout/prefs.cpp); имена полей местами короче апстримных (Glow/Fill вместо
//     GlowEnabled/FillEnabled), классы разметки и дефолты — те же.
//   - ApplyCrosshair (Task 10) — реализация в отдельном layout/crosshair.cpp, вызов отсюда.
#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/panorama_tables.h" // FindColorEntry/ResolveColorClass — key-glow-N и осевая тонировка клавиш
#include "kz/language/kz_language.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "kz/lead/kz_lead.h" // GetProgressPercent — данные элемента «Прогресс»
#include "kz/replays/kz_replaysystem.h"
#include "kz/prac/kz_prac.h" // prac-часы в элементе таймера
#include "kz/mode/kz_mode.h"   // GetModeShortName — строка курса
#include "kz/style/kz_style.h" // GetStyleShortName — строка курса
#include "kz/hud/hud_format.h" // чистые форматтеры полей редактора (host-тест tests/hud_format_test.cpp)
#include "sdk/entity/ccscustomhudlayout.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "utils/logging.h"
#include "cs2kz.h"

#include "tier0/memdbgon.h"

void KZHUDService::UpdateTimerElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force)
{
	// prac: настоящий таймер на входе честно остановлен (kz_prac), поэтому GetTimerText давал
	// «(СТОП)» на время показа после стопа, а потом элемент пропадал совсем — хотя попытка идёт
	// по prac-часам. Показываем их, как стандартный (HTML) худ: идут — как живой ран, стоят —
	// плейсхолдер «--:--.--» цветом остановленного таймера (попытки нет до выхода из старта
	// или !practp на точку со временем). Флаг — от НАБЛЮДАЕМОГО (source), как и весь элемент.
	const bool replay = KZ::replaysystem::IsReplayBot(source);
	const bool inPrac = !replay && source->pracService && source->pracService->IsInPrac();
	const bool pracRunning = inPrac && source->pracService->IsPracTimeRunning();
	std::string text;
	if (pracRunning)
	{
		char timeText[128];
		utils::FormatTime(source->pracService->GetPracTime(), timeText, sizeof(timeText));
		text = timeText;
	}
	else if (inPrac)
	{
		text = this->GetLayoutPrefs().timerDetailed ? "--:--.--" : "--:--";
	}
	else
	{
		text = source->hudService->GetTimerText(this->player->languageService->GetLanguage());
	}
	if (!this->GetLayoutPrefs().timerDetailed && !(inPrac && !pracRunning)) // плейсхолдер уже нужной ширины
	{
		// Отбросить дробную часть, суффикс (STOPPED)/(PAUSED) — сохранить.
		const size_t dot = text.find('.');
		if (dot != std::string::npos)
		{
			size_t end = dot + 1;
			while (end < text.size() && V_isdigit(text[end]))
			{
				end++;
			}
			text.erase(dot, end - dot);
		}
	}

	// Ветки реплей-бота: у наблюдаемого реплей-бота нет timerService/checkpointService с
	// реальными данными, всё идёт через KZ::replaysystem (нужно для спектейта бота).
	const bool paused = replay ? KZ::replaysystem::GetPaused() : (!inPrac && source->timerService->GetPaused());
	const bool running = replay ? KZ::replaysystem::GetEndTime() == 0.0f : (inPrac ? pracRunning : source->timerService->GetTimerRunning());
	const i32 teleports = replay ? KZ::replaysystem::GetTeleportCount() : source->checkpointService->GetTeleportCount();

	const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
	Color color;
	if (paused)
	{
		color = prefs.timerPaused;
	}
	else if (!running)
	{
		color = prefs.timerStopped;
	}
	else
	{
		color = teleports > 0 ? prefs.timerTp : prefs.timerPro;
	}

	const bool show = this->IsLayoutElementEnabled(LayoutElement::Timer) && !text.empty();
	this->UpdateLayoutElement(layout, LayoutElement::Timer, show, text.c_str(), color, force);

	// === Дельта к PB/WR (спека §4.3): «+0.312» справа от таймера, по пути `!lead` ===========
	// Путь сравнения держит СВОЙ игрок и по СВОЕМУ префу: при спектейте source — наблюдаемый, и
	// заказывать путь в его сервисе по нашим настройкам нельзя (два зрителя с разным режимом
	// дергали бы его слот туда-обратно). Наблюдаемому дельта видна, если его собственный худ
	// её заказал. Вызов каждый тик — уровень, а не фронт: сервис сам решает, грузить ли
	// (кулдаун и защёлка отказа — у него, Review Focus 3).
	//
	// Желание — только при ВКЛЮЧЁННОМ своём элементе таймера: дельта живёт в его строке, и
	// выключенный таймер с hudTimerCompare != 0 иначе держал бы путь (резолв, докачку, вектор
	// вершин) ради элемента, которого нет на экране. Что худ именно panorama — гарантирует
	// вызывающий: UpdateHudLayout зовётся только на этом типе, а на остальных DrawPanels
	// отпускает путь сам (SetCompareWanted(false)).
	if (source == this->player && this->player->leadService)
	{
		const MHUDLayoutPrefs &own = this->GetOwnLayoutPrefs();
		const i32 wantMode = own.elements[(i32)LayoutElement::Timer].enabled ? own.timerCompare : 0;
		this->player->leadService->SetCompareWanted(wantMode != 0, wantMode == 2 ? CybReplayDownload::Kind::AWR : CybReplayDownload::Kind::PB);
	}
	f64 delta = 0.0;
	// Спектейт: путь сравнения — у наблюдаемого и под ЕГО вид записи (PB или AWR). Если зритель
	// хочет другой вид, число было бы подписано не тем, что он выбрал («+0.3 к AWR» под видом
	// «к PB»), — такую дельту не показываем вовсе.
	const bool kindMatches = source == this->player
							 || (source->hudService && source->hudService->GetOwnLayoutPrefs().timerCompare == prefs.timerCompare);
	// prac-часы и реплей-бот к пути сравнения отношения не имеют: там показывать нечего.
	const bool gotDelta = show && !replay && !inPrac && prefs.timerCompare != 0 && kindMatches && source->leadService
						  && source->leadService->GetCompareDeltaSeconds(delta);
	// Нет пути (новичок без PB-реплея, идёт загрузка, путь другого курса/режима) — дельта просто
	// скрыта: ни прочерка, ни сообщения (Review Focus 3).
	const bool showDelta = KZ::hudfmt::DeltaVisible(running, gotDelta, gotDelta, prefs.timerCompare);
	this->ApplyTimerDelta(layout, "", this->layoutExtra, prefs, showDelta, delta);
}

// Дельта в строке таймера — общая для настоящего худа (prefix "") и реплики редактора !hud
// (prefix "x_", свой кэш extra): один код цвета/кегля/показа на оба.
void KZHUDService::ApplyTimerDelta(CCSCustomHudLayout *layout, const char *prefix, LayoutExtraState &extra, const MHUDLayoutPrefs &prefs, bool show,
								   f64 delta)
{
	char idBuf[64];
	const char *deltaPanel = PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "mhud_delta");
	if (show)
	{
		char deltaText[24];
		KZ::hudfmt::FormatDelta(delta, prefs.timerDetailed, deltaText, sizeof(deltaText));
		this->SetLayoutVar(layout, deltaPanel, "delta", extra.deltaText, deltaText);

		// Цвет: при дефолтных цветах — классы d-ahead/d-behind (в mhud.css точные #4CC38A/
		// #FF5C5C дизайна), при своих — ближайший класс палитры (pal-fg-N/grad-N). Одновременно
		// оба не ставим: `.delta.d-ahead` специфичнее `.pal-fg-N` и перебил бы цвет игрока.
		const bool ahead = delta < 0.0;
		const Color &wanted = ahead ? prefs.deltaAhead : prefs.deltaBehind;
		const Color &fallback = ahead ? MHUD_DEF_DELTA_AHEAD_COLOR : MHUD_DEF_DELTA_BEHIND_COLOR;
		const auto pack = [](const Color &c) { return ((u32)c.r() << 24) | ((u32)c.g() << 16) | ((u32)c.b() << 8) | (u32)c.a(); };
		const u32 packed = pack(wanted);
		const bool isDefault = packed == pack(fallback);
		const char *colorClass = NULL;
		if (!isDefault)
		{
			if (!extra.deltaColorValid || extra.deltaColorPacked != packed)
			{
				extra.deltaColorValid = true;
				extra.deltaColorPacked = packed;
				extra.deltaColorComputed = panorama::ResolveColorClass(wanted);
			}
			colorClass = extra.deltaColorComputed;
		}
		this->SetLayoutClass(layout, deltaPanel, extra.deltaStateClass, isDefault ? (ahead ? "d-ahead" : "d-behind") : NULL);
		this->SetLayoutClass(layout, deltaPanel, extra.deltaColorClass, colorClass);

		// Кегль — 0.65 кегля таймера (половина при таймере 24px давала нечитаемые 12px), в пределах
		// таблицы font-size.css (8..100 px): мельче восьми классов нет, дельта упирается в минимум.
		const i32 timerSize = prefs.elements[(i32)LayoutElement::Timer].size;
		const i32 deltaSize = panorama::SnapToStep((timerSize * 65 + 50) / 100, LAYOUT_SIZE_MIN, LAYOUT_SIZE_MAX);
		this->SetLayoutValueClass(layout, deltaPanel, extra.deltaFontSize, deltaSize, "font-size", false);
		// Шрифт — тот же, что у таймера: дельта стоит в его строке, и другое начертание рядом
		// читалось бы как чужой элемент (дизайн: одна строка, дельта мельче — 0.65×, — но тем же шрифтом).
		this->SetLayoutClass(layout, deltaPanel, extra.deltaFontClass, prefs.elements[(i32)LayoutElement::Timer].fontClass);
	}
	// hidden — последним: сначала текст/цвет/кегль, потом показ, чтобы не мелькнул прошлый кадр.
	this->SetLayoutBoolClass(layout, deltaPanel, "hidden", extra.deltaHidden, !show);
}

void KZHUDService::UpdateSpeedElement(CCSCustomHudLayout *layout, const SpeedInfo &info, bool force)
{
	const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
	char text[16];
	V_snprintf(text, sizeof(text), prefs.speedPrecise ? "%.2f" : "%.0f", info.velocity.Length2D());
	const Color color = info.crouchJump ? prefs.speedCj : prefs.speed;
	const bool show = this->IsLayoutElementEnabled(LayoutElement::Speed);
	this->UpdateLayoutElement(layout, LayoutElement::Speed, show, text, color, force);
}

void KZHUDService::UpdatePrespeedElement(CCSCustomHudLayout *layout, KZPlayer *source, const SpeedInfo &info, bool force)
{
	// Точность/скобки/скрытие при уходе с края — порт с апстрима один-в-один
	// (origin/master:src/kz/hud/layout/mhud.cpp:63-84): свои префы mhudPrespeedPrecise/
	// mhudPrespeedBrackets/mhudPrespeedHideWalkOff, а не общий с самой скоростью speedPrecise.
	const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
	char text[16];
	const char *format = prefs.prespeedBrackets ? (prefs.prespeedPrecise ? "(%.2f)" : "(%.0f)") : (prefs.prespeedPrecise ? "%.2f" : "%.0f");
	V_snprintf(text, sizeof(text), format, info.prespeed.Length2D());
	Color color;
	if (info.jumpbug)
	{
		color = prefs.prespeedJumpbug;
	}
	else if (info.perfing)
	{
		color = prefs.prespeedPerf;
	}
	else
	{
		color = prefs.prespeed;
	}
	// info.hasPrespeed — тот же useTakeoff/showTakeoff, что и в остальных ветках худа (общий
	// GetSpeedInfo, R3): без взлёта престрейфу нечего показывать. walkedOff — уход с края
	// вместо прыжка: с mhudPrespeedHideWalkOff такой «престрейф» не показываем (апстрим:
	// origin/master:src/kz/hud/layout/mhud.cpp:82).
	const bool show = this->IsLayoutElementEnabled(LayoutElement::Prespeed) && info.hasPrespeed && !(prefs.prespeedHideWalkOff && info.walkedOff);

	// Бейджи PERF/JB/CJ живут ровно столько же, сколько число престрейфа их взлёта. perfing
	// (inPerf) и jumpbug (fromDuckbug) сбрасываются на приземлении, а престрейф висит ещё
	// KZ_HUD_ON_GROUND_THRESHOLD — поэтому флаги защёлкиваются: новый взлёт (другой takeoffTime
	// или другой источник при спектейте) берёт их заново, в воздухе они обновляются (KZT
	// выставляет перф сам и не обязательно в тик отрыва), на земле — заморожены. JB важнее PERF,
	// как и в цвете престрейфа выше: одновременно горят максимум два (JB/PERF + CJ).
	LayoutExtraState &extra = this->layoutExtra;
	const i32 slot = source->GetPlayerSlot().Get();
	if (!show)
	{
		extra.indLatchValid = false;
	}
	else if (!extra.indLatchValid || extra.indLatchSlot != slot || extra.indLatchTakeoff != info.takeoffTime || !info.onGround)
	{
		extra.indLatchValid = true;
		extra.indLatchSlot = slot;
		extra.indLatchTakeoff = info.takeoffTime;
		extra.indLatch[0] = info.perfing && !info.jumpbug;
		extra.indLatch[1] = info.jumpbug;
		extra.indLatch[2] = info.crouchJump;
	}
	this->ApplyJumpIndicators(layout, "", extra, show && prefs.indicators, extra.indLatch, prefs.elements[(i32)LayoutElement::Prespeed].size);
	this->UpdateLayoutElement(layout, LayoutElement::Prespeed, show, text, color, force);
}

// Общая для худа (prefix "") и реплики редактора ("x_") запись бейджей прыжка: hidden на каждом
// бейдже и на полосе mhud_ind (пустая полоса не должна держать место под престрейфом), кегль
// элемента — на каждом бейдже (font-size ребёнку через класс корня не наследуется). Без анимаций:
// бейдж появляется и гаснет в тот же тик, что и число.
static_global const char *const JUMP_IND_IDS[3] = {"ind_perf", "ind_jb", "ind_cj"};

void KZHUDService::ApplyJumpIndicators(CCSCustomHudLayout *layout, const char *prefix, LayoutExtraState &extra, bool show, const bool (&lit)[3], i32 size)
{
	char idBuf[64];
	bool any = false;
	for (i32 i = 0; i < 3; i++)
	{
		const char *id = PrefixLayoutId(idBuf, sizeof(idBuf), prefix, JUMP_IND_IDS[i]);
		this->SetLayoutValueClass(layout, id, extra.indSize[i], size, "font-size", false);
		this->SetLayoutBoolClass(layout, id, "hidden", extra.indHidden[i], !(show && lit[i]));
		any |= show && lit[i];
	}
	this->SetLayoutBoolClass(layout, PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "mhud_ind"), "hidden", extra.indStripHidden, !any);
}

// Порядок соответствует кнопкам в mhud.vxml (наш аддон, путь и содержимое общие с апстримом
// — см. KZ_MHUD_LAYOUT): верхний ряд C W J, нижний A S D.
static_global const char *KEY_PANELS[] = {"mhud_key_c", "mhud_key_w", "mhud_key_j", "mhud_key_a", "mhud_key_s", "mhud_key_d"};

// Глифы-подписи внутри кнопок: ребёнок в panorama перекрашивается, только когда класс стоит
// именно на нём, поэтому шрифт-класс кладём сюда, а не на сами кнопки (как у апстрима).
static_global const char *KEY_GLYPHS[] = {"mhud_kg_c_main",   "mhud_kg_c_idle", "mhud_kg_w_main",   "mhud_kg_w_letter",
										  "mhud_kg_w_idle",   "mhud_kg_j_main", "mhud_kg_j_idle",   "mhud_kg_a_main",
										  "mhud_kg_a_letter", "mhud_kg_a_idle", "mhud_kg_s_main",   "mhud_kg_s_letter",
										  "mhud_kg_s_idle",   "mhud_kg_d_main", "mhud_kg_d_letter", "mhud_kg_d_idle"};

// Ось движения каждой кнопки — только чтобы при keysOverlapAxis подсветить именно две
// конфликтующие клавиши, а не весь контейнер. Индексы под тем же порядком, что KEY_PANELS.
enum KeyAxis
{
	KEY_AXIS_NONE = -1,
	KEY_AXIS_FORWARD_BACK,
	KEY_AXIS_LEFT_RIGHT,
};

static_global const i32 KEY_AXES[] = {KEY_AXIS_NONE,       KEY_AXIS_FORWARD_BACK, KEY_AXIS_NONE,
									   KEY_AXIS_LEFT_RIGHT, KEY_AXIS_FORWARD_BACK, KEY_AXIS_LEFT_RIGHT};

// Обёртка над SetHasClass с логом отказа (интерн-лимит 1024, см. LogHudInternFailure в
// entity.cpp) — та же проверка, что уже была у "pressed"/размерных классов ниже, но теперь
// нужна в нескольких местах разом (idle/border/glow/fill/letters/square), поэтому вынесена.
static_function void SetKeysHasClass(CCSCustomHudLayout *layout, KZPlayer *player, const char *panelId, const char *className, bool has)
{
	if (!layout->SetHasClass(panelId, className, has ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n", panelId, className,
					player->GetPlayerSlot().Get());
	}
}

void KZHUDService::UpdateKeysElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force)
{
	// Состояние клавиш игрока — тот же источник, что у нижней панели (KPF_*/particle-путь):
	// второй не заводим. Порядок под KEY_PANELS — C W J A S D.
	const bool duck = source->IsButtonPressed(IN_DUCK);
	const bool forward = source->IsButtonPressed(IN_FORWARD);
	const bool jump = source->hudService->jumpedThisTick || source->IsButtonPressed(IN_JUMP);
	const bool left = source->IsButtonPressed(IN_MOVELEFT);
	const bool back = source->IsButtonPressed(IN_BACK);
	const bool right = source->IsButtonPressed(IN_MOVERIGHT);
	const bool keys[] = {duck, forward, jump, left, back, right};

	const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
	const bool overlap = ((forward && back) || (left && right)) && prefs.keysOverlapEnabled;
	// keysOverlapAxis: контейнер остаётся на базовом цвете, а тонировку получают только две
	// конфликтующие кнопки (ниже, per-key overlapClass) — иначе (как раньше) красится целиком.
	bool overlapped[KZ_ARRAYSIZE(KEY_PANELS)] {};
	for (i32 i = 0; overlap && i < (i32)KZ_ARRAYSIZE(KEY_PANELS); i++)
	{
		overlapped[i] = !prefs.keysOverlapAxis || (KEY_AXES[i] == KEY_AXIS_FORWARD_BACK && forward && back)
						|| (KEY_AXES[i] == KEY_AXIS_LEFT_RIGHT && left && right);
	}
	const Color color = overlap && !prefs.keysOverlapAxis ? prefs.keysOverlap : prefs.keys;
	const bool show = this->IsLayoutElementEnabled(LayoutElement::Keys);
	this->UpdateLayoutElement(layout, LayoutElement::Keys, show, NULL, color, force);

	if (force)
	{
		// force — сущность только что создана, кэш кнопок обязан сброситься вместе с ней
		// (см. LayoutKeysState в kz_hud.h и DestroyOwnedLayout).
		this->layoutKeys = LayoutKeysState();
	}
	if (!show)
	{
		return;
	}
	this->ApplyKeysLook(layout, "", this->layoutKeys, prefs, keys, overlap, overlapped);
}

// Вид блока клавиш (тумблеры контейнера, свечение/тонировка, «нажато», кегль и шрифт) — общий
// для настоящего худа (prefix "") и реплики редактора !hud (prefix "x_", свой кэш state).
void KZHUDService::ApplyKeysLook(CCSCustomHudLayout *layout, const char *prefix, LayoutKeysState &state, const MHUDLayoutPrefs &prefs,
								 const bool (&keys)[MHUD_KEY_COUNT], bool overlap, const bool (&overlapped)[MHUD_KEY_COUNT])
{
	char idBuf[64];
	const char *const keysPanel = PrefixLayoutId(idBuf, sizeof(idBuf), prefix, LAYOUT_ELEMENTS[(i32)LayoutElement::Keys].panelId);
	char keyIds[MHUD_KEY_COUNT][48];
	const char *keyPanels[MHUD_KEY_COUNT];
	for (i32 i = 0; i < MHUD_KEY_COUNT; i++)
	{
		keyPanels[i] = PrefixLayoutId(keyIds[i], sizeof(keyIds[i]), prefix, KEY_PANELS[i]);
	}

	// Тумблеры на весь контейнер клавиш — каждый ставится И снимается по значению префа
	// (залипание оформления при выключении уже ловилось ревью на другом месте, см. брифинг).
	const i32 idle = prefs.keysIdle;
	if (state.idle != idle)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "hide-idle", idle == 1);
		SetKeysHasClass(layout, this->player, keysPanel, "keys-underscore", idle == 2);
		state.idle = idle;
	}
	const i32 noBorder = prefs.keysBorder ? 0 : 1;
	if (state.noBorder != noBorder)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "keys-noborder", noBorder != 0);
		state.noBorder = noBorder;
	}
	const i32 noGlow = prefs.keysGlow ? 0 : 1;
	if (state.noGlow != noGlow)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "keys-noglow", noGlow != 0);
		state.noGlow = noGlow;
	}
	const i32 noFill = prefs.keysFill ? 0 : 1;
	if (state.noFill != noFill)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "keys-nofill", noFill != 0);
		state.noFill = noFill;
	}
	const i32 letters = prefs.keysLetters ? 1 : 0;
	if (state.letters != letters)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "keys-letters", letters != 0);
		state.letters = letters;
	}
	const i32 square = prefs.keysSquare ? 1 : 0;
	if (state.square != square)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "keys-square", square != 0);
		state.square = square;
	}
	// Интервал клавиш: один класс key-gap--G на корне (mhud.css), прошлый снимаем. -1 «авто» —
	// без класса, остаётся пропорциональный отступ keys.css.
	const i32 gap = prefs.keysGap;
	if (state.gap != gap)
	{
		char gapClass[32];
		if (state.gap >= 0)
		{
			V_snprintf(gapClass, sizeof(gapClass), "key-gap--%i", state.gap);
			layout->SetHasClass(keysPanel, gapClass, k_eHudPanelClassStatus_DoesNotHaveClass);
		}
		if (gap >= 0)
		{
			V_snprintf(gapClass, sizeof(gapClass), "key-gap--%i", gap);
			SetKeysHasClass(layout, this->player, keysPanel, gapClass, true);
		}
		state.gap = gap;
	}

	// key-glow-N (keys.css, 160 записей) — та же палитра, что pal-fg-N/pal-bg-N в
	// panorama_tables.cpp. Градиентов в keys.css нет: у градиента FindColorEntry отдаёт запись
	// за хвостом сплошных (или -1), и такой преф уводим на дефолт опции — как апстрим. Клэмп в
	// последнюю сплошную покрасил бы клавишу чужим цветом. Пикер градиент не даёт (SetItemSolidOnly
	// в hud_prefs.cpp), но преф мог прийти из БД, от импорта или мимо меню.
	auto resolveGlowIndex = [](const Color &c, const Color &fallback)
	{
		const i32 entry = panorama::FindColorEntry(c);
		return (entry >= 0 && entry < panorama::GetSolidColorCount()) ? entry : panorama::FindColorEntry(fallback);
	};
	const i32 glow = resolveGlowIndex(prefs.keysPressed, MHUD_DEF_KEYS_PRESSED_COLOR);
	const i32 glowOverlap = overlap ? resolveGlowIndex(prefs.keysOverlapGlow, MHUD_DEF_KEYS_OVERLAP_GLOW_COLOR) : glow;
	const char *overlapClass = prefs.keysOverlapAxis ? panorama::ResolveColorClass(prefs.keysOverlap) : NULL;
	for (i32 i = 0; i < MHUD_KEY_COUNT; i++)
	{
		// Цвет контейнера наследуется детьми — класс здесь его переопределяет для одной кнопки.
		this->SetLayoutClass(layout, keyPanels[i], state.overlapClass[i], overlapped[i] ? overlapClass : NULL);

		const i32 wanted = overlapped[i] ? glowOverlap : glow;
		if (state.glow[i] == wanted)
		{
			continue;
		}
		char glowClass[32];
		if (state.glow[i] >= 0)
		{
			V_snprintf(glowClass, sizeof(glowClass), "key-glow-%i", state.glow[i]);
			layout->SetHasClass(keyPanels[i], glowClass, k_eHudPanelClassStatus_DoesNotHaveClass);
		}
		V_snprintf(glowClass, sizeof(glowClass), "key-glow-%i", wanted);
		if (!layout->SetHasClass(keyPanels[i], glowClass, k_eHudPanelClassStatus_HasClass))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n", keyPanels[i],
						glowClass, this->player->GetPlayerSlot().Get());
		}
		state.glow[i] = wanted;
	}

	for (i32 i = 0; i < MHUD_KEY_COUNT; i++)
	{
		if (state.pressed[i] == keys[i])
		{
			continue;
		}
		state.pressed[i] = keys[i];
		if (!layout->SetHasClass(keyPanels[i], "pressed", keys[i] ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=pressed slot=%i\n",
						keyPanels[i], this->player->GetPlayerSlot().Get());
		}
	}

	// Размер элемента (уже clamped/snapped к LAYOUT_SIZE_MIN/MAX в RefreshLayoutPrefs) и шрифт
	// глифов — общим хелпером с меню реплея (layout/rpmenu.cpp).
	const MHUDLayoutPrefs::Element &cached = prefs.elements[(i32)LayoutElement::Keys];
	this->ApplyKeysSizing(layout, state, cached.size, cached.fontClass, prefix);
}

// Кегль блока клавиш — на контейнер (масштаб кнопок, key-size--N) и на каждую кнопку отдельно
// (масштаб глифа, font-size--Npx): в panorama font-size не наследуется детьми через класс
// родителя, апстрим тоже дублирует явно. Шрифт-класс — на глифы (см. KEY_GLYPHS). state —
// диф-кэш ТОЙ сущности, на которую пишем (layoutKeys у худа, editorKeys у реплики редактора).
void KZHUDService::ApplyKeysSizing(CCSCustomHudLayout *layout, LayoutKeysState &state, i32 size, const char *fontClass, const char *prefix)
{
	char idBuf[64];
	const char *const keysPanel = PrefixLayoutId(idBuf, sizeof(idBuf), prefix, LAYOUT_ELEMENTS[(i32)LayoutElement::Keys].panelId);
	char keyBuf[48];

	if (state.boxSize != size)
	{
		char className[32];
		if (state.boxSize != INT_MIN)
		{
			V_snprintf(className, sizeof(className), "key-size--%i", state.boxSize);
			layout->SetHasClass(keysPanel, className, k_eHudPanelClassStatus_DoesNotHaveClass);
		}
		V_snprintf(className, sizeof(className), "key-size--%i", size);
		if (!layout->SetHasClass(keysPanel, className, k_eHudPanelClassStatus_HasClass))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n", keysPanel,
						className, this->player->GetPlayerSlot().Get());
		}
		state.boxSize = size;
	}

	if (state.fontSize != size)
	{
		char className[32];
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(KEY_PANELS); i++)
		{
			const char *keyPanel = PrefixLayoutId(keyBuf, sizeof(keyBuf), prefix, KEY_PANELS[i]);
			if (state.fontSize != INT_MIN)
			{
				V_snprintf(className, sizeof(className), "font-size--%ipx", state.fontSize);
				layout->SetHasClass(keyPanel, className, k_eHudPanelClassStatus_DoesNotHaveClass);
			}
			V_snprintf(className, sizeof(className), "font-size--%ipx", size);
			if (!layout->SetHasClass(keyPanel, className, k_eHudPanelClassStatus_HasClass))
			{
				KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n", keyPanel,
							className, this->player->GetPlayerSlot().Get());
			}
		}
		state.fontSize = size;
	}

	// fontClass — уже резолвленный css-класс (RefreshLayoutPrefs зовёт ResolveFontClass один
	// раз), как и у остальных элементов (см. entity.cpp/UpdateLayoutElement).
	if (state.fontClass != fontClass)
	{
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(KEY_GLYPHS); i++)
		{
			const char *glyph = PrefixLayoutId(keyBuf, sizeof(keyBuf), prefix, KEY_GLYPHS[i]);
			if (state.fontClass)
			{
				layout->SetHasClass(glyph, state.fontClass, k_eHudPanelClassStatus_DoesNotHaveClass);
			}
			if (!layout->SetHasClass(glyph, fontClass, k_eHudPanelClassStatus_HasClass))
			{
				KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n", glyph,
							fontClass, this->player->GetPlayerSlot().Get());
			}
		}
		state.fontClass = fontClass;
	}
}

void KZHUDService::UpdateCheckpointElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force)
{
	std::string text = source->hudService->GetCheckpointText(this->player->languageService->GetLanguage());
	const Color color = this->GetLayoutPrefs().checkpoint;
	// Под открытым меню реплея (layout/rpmenu.cpp) строка CP/TP гаснет: её место по смыслу
	// занимает меню (решение пользователя 09.09 — «вместо cp/tp»), а у бота эти счётчики и
	// так вторичны. Остальные элементы худа живут как есть.
	const bool show = this->IsLayoutElementEnabled(LayoutElement::Checkpoint) && !text.empty() && !this->IsReplayMenuOpen();
	this->UpdateLayoutElement(layout, LayoutElement::Checkpoint, show, text.c_str(), color, force);
}

// === Поля редактора `!hud`: PB/WR, showpos, курс, тип рана (спека 2026-09-26, §4.2) ==========

// Курс, по которому показываем PB/WR и строку курса: активный, а до входа в старт-зону (только
// зашёл, стоит вне зоны) — главный курс карты (cyber 0). Тот же выбор, что у строки PB/WR
// HTML-худа (kz_hud.cpp, pbwrCourse): показания двух путей худа не должны расходиться.
// Строки showpos игрока: «x y z» и «pitch yaw» (форматтеры — host-тест tests/hud_format_test.cpp).
// false — пешки нет (GetOrigin/GetAngles без неё выходят молча, ничего не записав): строки не
// тронуты, вызывающий их не показывает. Раньше origin оставался неинициализированным, и в худ
// уходил мусор со стека.
bool FormatShowPos(KZPlayer *source, char *pos, u32 posLen, char *ang, u32 angLen)
{
	if (!source || !source->GetPlayerPawn())
	{
		return false;
	}
	Vector origin = vec3_origin;
	QAngle angles(0.0f, 0.0f, 0.0f);
	source->GetOrigin(&origin);
	source->GetAngles(&angles);
	KZ::hudfmt::FormatPos(origin.x, origin.y, origin.z, pos, posLen);
	KZ::hudfmt::FormatAng(angles.x, angles.y, ang, angLen);
	return true;
}

const KZCourseDescriptor *GetHudDisplayCourse(KZPlayer *source)
{
	const KZCourseDescriptor *course = source->timerService->GetCourse();
	return course ? course : KZ::course::GetCourseByCyberNumber(0);
}

// Время ячейки PB/WR в точности таймера: с hudTimerDetail — тысячные (utils::FormatTime,
// mm:ss.mmm), без — сотые (FormatTimeHud, mm:ss.cc). Ширина совпадает с плейсхолдером
// KZ::hudfmt::NoTimePlaceholder, поэтому ячейки не прыгают, когда время появляется.
static_function void FormatHudRecordTime(f64 time, bool detailed, char *out, u32 length)
{
	if (detailed)
	{
		utils::FormatTime(time, out, length, true);
	}
	else
	{
		FormatTimeHud(time, out, length);
	}
}

// Стиль дочернего лейбла элемента (ячейки PB/WR, строки showpos): font-size/шрифт/цвет ставятся
// на САМ лейбл — в panorama font-size через класс предка ребёнку не наследуется (как у клавиш), а
// разметка больше не задаёт им размер/цвет/шрифт в css. colorClass NULL — класс цвета снят
// (подписи PB/WR держат свой статичный цвет из css).
void KZHUDService::ApplyChildLabelStyle(CCSCustomHudLayout *layout, const char *panelId, LayoutChildStyleState &state, i32 size, const char *fontClass,
										const char *colorClass)
{
	this->SetLayoutValueClass(layout, panelId, state.fontSize, size, "font-size", false);
	this->SetLayoutClass(layout, panelId, state.fontClass, fontClass);
	this->SetLayoutClass(layout, panelId, state.colorClass, colorClass);
}

// Порядок ячеек: PB NUB, PB PRO, WR NUB, WR PRO — i < 2 это PB, нечётный индекс — PRO.
static_global const char *const PBWR_CELL_IDS[4] = {"pw_pb_nub", "pw_pb_pro", "pw_wr_nub", "pw_wr_pro"};
static_global const char *const PBWR_TIME_IDS[4] = {"pw_t_pb_nub", "pw_t_pb_pro", "pw_t_wr_nub", "pw_t_wr_pro"};
static_global const char *const PBWR_CAP_IDS[4] = {"pw_c_pb_nub", "pw_c_pb_pro", "pw_c_wr_nub", "pw_c_wr_pro"};
static_global const char *const PBWR_VARS[4] = {"pb_nub", "pb_pro", "wr_nub", "wr_pro"};

// Ячейки PB/WR — общая запись для настоящего худа (prefix "") и реплики редактора (prefix "x_").
// texts[i] — готовое время ячейки; cells[i] — включена ли ячейка.
void KZHUDService::ApplyPbWrCells(CCSCustomHudLayout *layout, const char *prefix, LayoutExtraState &extra, const char *const (&texts)[4],
								  const bool (&cells)[4], const MHUDLayoutPrefs::Element &style, const Color &color)
{
	char idBuf[64];
	const char *colorClass = extra.pwColor.Get(color);
	for (i32 i = 0; i < 4; i++)
	{
		// Переменная — на самом лейбле времени (у лейблов с 26.09 есть id pw_t_*), без расчёта на
		// наследование переменных от предка.
		const char *timeId = PrefixLayoutId(idBuf, sizeof(idBuf), prefix, PBWR_TIME_IDS[i]);
		this->SetLayoutVar(layout, timeId, PBWR_VARS[i], extra.pbwrText[i], texts[i]);
		this->ApplyChildLabelStyle(layout, timeId, extra.pwTime[i], style.size, style.fontClass, colorClass);
		// Подпись PB/WR: только кегль — цвет статичный из css (WR — янтарный), шрифт — бейджевый.
		const char *capId = PrefixLayoutId(idBuf, sizeof(idBuf), prefix, PBWR_CAP_IDS[i]);
		this->SetLayoutValueClass(layout, capId, extra.pwCapSize[i], style.size, "font-size", false);
		this->SetLayoutBoolClass(layout, PrefixLayoutId(idBuf, sizeof(idBuf), prefix, PBWR_CELL_IDS[i]), "hidden", extra.pbwrCellHidden[i], !cells[i]);
	}
	// Бейджи NUB/PRO — тот же кегль, что у времени: font-size ребёнку через класс корня не
	// наследуется (см. ApplyChildLabelStyle), без класса бейдж не растёт вместе с элементом.
	this->SetLayoutValueClass(layout, PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "pw_b_nub"), extra.pwBadgeSize[0], style.size, "font-size", false);
	this->SetLayoutValueClass(layout, PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "pw_b_pro"), extra.pwBadgeSize[1], style.size, "font-size", false);
	// Строка гаснет, когда обе её ячейки выключены: иначе от неё остался бы бейдж NUB/PRO.
	this->SetLayoutBoolClass(layout, PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "pw_nub"), "hidden", extra.pbwrRowHidden[0], !(cells[0] || cells[2]));
	this->SetLayoutBoolClass(layout, PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "pw_pro"), "hidden", extra.pbwrRowHidden[1], !(cells[1] || cells[3]));
}

void KZHUDService::UpdatePbWrElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force)
{
	const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
	const bool cells[4] = {prefs.pbNub, prefs.pbPro, prefs.wrNub, prefs.wrPro};
	// Все четыре ячейки выключены — элемент гаснет целиком (Review Focus 5), иначе на экране
	// висела бы пустая рамка элемента, а в редакторе — пустышка, которую нечем наполнить.
	const bool any = cells[0] || cells[1] || cells[2] || cells[3];
	// Реплей-бот: у него нет своих PB/WR кэшей по курсу — как и HTML-худ, строку не показываем.
	const bool replay = KZ::replaysystem::IsReplayBot(source);
	const KZCourseDescriptor *course = (any && !replay && this->IsLayoutElementEnabled(LayoutElement::PbWr)) ? GetHudDisplayCourse(source) : NULL;
	const bool show = course != NULL;
	if (show)
	{
		char buf[4][32];
		const char *texts[4];
		this->BuildPbWrTexts(source, course, prefs.timerDetailed, NULL, buf, texts);
		this->ApplyPbWrCells(layout, "", this->layoutExtra, texts, cells, prefs.elements[(i32)LayoutElement::PbWr], prefs.pbwrColor);
	}
	// text = NULL: у корня (Panel) своей переменной нет, всё содержимое — в ячейках выше.
	this->UpdateLayoutElement(layout, LayoutElement::PbWr, show, NULL, prefs.pbwrColor, force);
}

// Тексты четырёх ячеек PB/WR. Нет времени — placeholders[i] (редактор) или прочерк нужной ширины.
void KZHUDService::BuildPbWrTexts(KZPlayer *source, const KZCourseDescriptor *course, bool detailed, const char *const *placeholders,
								  char (&buf)[4][32], const char *(&texts)[4])
{
	for (i32 i = 0; i < 4; i++)
	{
		f64 time = 0.0;
		const bool pro = (i & 1) != 0;
		const bool has = source && course
						 && (i < 2 ? source->timerService->GetHudPBTime(time, course, pro) : source->timerService->GetHudWorldRecordTime(time, course, pro));
		if (has)
		{
			FormatHudRecordTime(time, detailed, buf[i], sizeof(buf[i]));
		}
		else
		{
			V_strncpy(buf[i], placeholders ? placeholders[i] : KZ::hudfmt::NoTimePlaceholder(detailed), sizeof(buf[i]));
		}
		texts[i] = buf[i];
	}
}

// Обе строки showpos (позиция и углы) — переменные и стиль на самих лейблах mhud_pos/mhud_ang.
void KZHUDService::ApplyShowPosLines(CCSCustomHudLayout *layout, const char *prefix, LayoutExtraState &extra, const char *pos, const char *ang,
									 const MHUDLayoutPrefs::Element &style, const Color &color)
{
	char idBuf[64];
	const char *colorClass = extra.posColor.Get(color);
	const char *posId = PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "mhud_pos");
	this->SetLayoutVar(layout, posId, "pos", extra.posText, pos);
	this->ApplyChildLabelStyle(layout, posId, extra.posLine[0], style.size, style.fontClass, colorClass);
	const char *angId = PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "mhud_ang");
	this->SetLayoutVar(layout, angId, "ang", extra.angText, ang);
	this->ApplyChildLabelStyle(layout, angId, extra.posLine[1], style.size, style.fontClass, colorClass);
	// Подписи pos/ang: только кегль — цвет и шрифт у них свои из css (как у подписей PB/WR).
	this->SetLayoutValueClass(layout, PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "mhud_pos_cap"), extra.posCapSize[0], style.size, "font-size", false);
	this->SetLayoutValueClass(layout, PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "mhud_ang_cap"), extra.posCapSize[1], style.size, "font-size", false);
}

void KZHUDService::UpdateShowPosElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force)
{
	// Тумблер — настройка получателя (эффективный набор), координаты — наблюдаемого (source),
	// как у строк !showpos HTML-худа.
	bool show = this->IsLayoutElementEnabled(LayoutElement::ShowPos);
	const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
	char pos[64];
	char ang[48];
	// Без пешки координат нет — элемент гаснет, а не показывает нули.
	show = show && FormatShowPos(source, pos, sizeof(pos), ang, sizeof(ang));
	if (show)
	{
		this->ApplyShowPosLines(layout, "", this->layoutExtra, pos, ang, prefs.elements[(i32)LayoutElement::ShowPos], prefs.showPosColor);
	}
	// text = NULL: строки пишутся в свои лейблы выше, корень — только позиция/показ.
	this->UpdateLayoutElement(layout, LayoutElement::ShowPos, show, NULL, prefs.showPosColor, force);
}

void KZHUDService::UpdateCourseElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force)
{
	// Реплей-бот: курс и стили живого игрока к записи не относятся — строку не показываем.
	const bool replay = KZ::replaysystem::IsReplayBot(source);
	const KZCourseDescriptor *course = (!replay && this->IsLayoutElementEnabled(LayoutElement::Course)) ? GetHudDisplayCourse(source) : NULL;
	char line[128] = "";
	if (course)
	{
		// Полные имена стилей (AutoBhop, LegacyJump) — по дизайну; FormatCourseLine поднимает регистр.
		const char *styles[8];
		i32 styleCount = 0;
		for (i32 i = 0; i < source->styleServices.Count() && styleCount < (i32)KZ_ARRAYSIZE(styles); i++)
		{
			styles[styleCount++] = source->styleServices[i]->GetStyleName();
		}
		KZ::hudfmt::FormatCourseLine(course->name, source->modeService->GetModeShortName(), styles, styleCount, line, sizeof(line));
	}
	this->UpdateLayoutElement(layout, LayoutElement::Course, course != NULL, course ? line : NULL, this->GetLayoutPrefs().courseColor, force);
}

// PRAC / PRO / NUB — тот же критерий, что у метки слева от времени в HTML-худе (kz_hud.cpp):
// prac важнее всего (настоящий таймер в prac остановлен), PRO — ран без телепортов
// (GetTeleportCount() == 0, как KZTimerService::GetCurrentTimeType). Таймер стоит и не prac —
// NULL, элемент скрыт. cls — класс расцветки бейджа (строковый литерал: SetLayoutClass
// сравнивает указатели).
static_function const char *GetRunTypeText(KZPlayer *source, const char *&cls)
{
	cls = NULL;
	if (source->pracService && source->pracService->IsInPrac())
	{
		cls = "rt-prac";
		return "PRAC";
	}
	if (!source->timerService->GetTimerRunning() || !source->checkpointService)
	{
		return NULL;
	}
	if (source->checkpointService->GetTeleportCount() > 0)
	{
		cls = "rt-nub";
		return "NUB";
	}
	cls = "rt-pro";
	return "PRO";
}

void KZHUDService::UpdateRunTypeElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force)
{
	const char *cls = NULL;
	// Реплей-бот — как HTML-худ, метки нет (его timerService/checkpointService не про запись).
	const char *text = KZ::replaysystem::IsReplayBot(source) ? NULL : GetRunTypeText(source, cls);
	const bool show = text != NULL && this->IsLayoutElementEnabled(LayoutElement::RunType);
	if (show)
	{
		// Скрытый элемент класс не трогает: прошлый rt-* под hidden никому не мешает, а снятие
		// и возврат на каждом старте/стопе были бы лишними пересылками.
		this->SetLayoutClass(layout, LAYOUT_ELEMENTS[(i32)LayoutElement::RunType].panelId, this->layoutExtra.runTypeClass, cls);
	}
	this->UpdateLayoutElement(layout, LayoutElement::RunType, show, text, MHUD_DEF_BASE_COLOR, force);
}

// === Элемент «Прогресс» по маршруту `!lead` (LayoutElement::LeadProgress) ==================
// Панель mhud_progress на сущности худа (дизайн, борд 1): подпись «ПРОГРЕСС» слева, процент
// справа, под ними полоса 4px. Позиция/прозрачность/показ — на корне (общий UpdateLayoutElement),
// процент — переменная progress, подпись и ширина заливки — здесь.

// Подпись, ширина заливки и стиль процента — общая запись для худа (prefix "") и реплики
// редактора (prefix "x_", плейсхолдер 38%).
void KZHUDService::ApplyLeadProgressParts(CCSCustomHudLayout *layout, const char *prefix, LayoutExtraState &extra, i32 percent,
										  const MHUDLayoutPrefs::Element &style, const Color &color)
{
	char idBuf[64];
	// Подпись — переменная на САМОМ лейбле mhud_progress_cap (не полагаемся на наследование
	// dialog-переменных от родителя), шлётся раз на сущность:
	// диф-кэш по тексту фразы, язык игрока меняется редко.
	const char *lang = this->player->languageService->GetLanguage();
	const std::string cap = KZLanguageService::PrepareMessageWithLang(lang, "Lead - Hud Progress Caption");
	this->SetLayoutVar(layout, PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "mhud_progress_cap"), "progress_cap", extra.progressCap, cap.c_str());

	// Ширина заливки — класс w-p--N (mhud.css, 0..100 шагом 1). Интерн-строк за ран — до 101
	// (по одной на каждый увиденный процент), это в пределах пула сущности худа.
	const i32 fill = Clamp(percent, 0, 100);
	if (extra.progressFill != fill)
	{
		const char *fillId = PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "mhud_progress_fill");
		char className[16];
		if (extra.progressFill != INT_MIN)
		{
			V_snprintf(className, sizeof(className), "w-p--%i", extra.progressFill);
			layout->SetHasClass(fillId, className, k_eHudPanelClassStatus_DoesNotHaveClass);
		}
		V_snprintf(className, sizeof(className), "w-p--%i", fill);
		if (!layout->SetHasClass(fillId, className, k_eHudPanelClassStatus_HasClass))
		{
			LogHudInternFailure(this->player, fillId, className);
		}
		extra.progressFill = fill;
	}

	// Кегль/шрифт/цвет — на самом лейбле процента: в panorama font-size через класс корня
	// ребёнку не наследуется (как у ячеек PB/WR), а корень элемента — Panel.
	this->ApplyChildLabelStyle(layout, PrefixLayoutId(idBuf, sizeof(idBuf), prefix, "mhud_progress_pct"), extra.progressPct, style.size,
							   style.fontClass, extra.progressColor.Get(color));
}

void KZHUDService::UpdateLeadProgressElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force)
{
	// Тумблер — из ЭФФЕКТИВНОГО набора (как у остальных элементов: при mhudMimicSpec это
	// набор наблюдаемого), данные — у наблюдаемого (source->leadService): процент показываем
	// ЕГО, по его же маршруту.
	const bool enabled = this->IsLayoutElementEnabled(LayoutElement::LeadProgress);
	const i32 percent = (enabled && source->leadService) ? source->leadService->GetProgressPercent() : -1;
	// Пути нет (нет AWR-реплея на карте, идёт загрузка, путь не сошёлся) либо элемент выключен —
	// элемент скрыт. Прочерка и сообщений в чат тут нет намеренно (решение дизайна).
	const bool show = percent >= 0;
	char text[8] = "";
	if (show)
	{
		const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
		this->ApplyLeadProgressParts(layout, "", this->layoutExtra, percent, prefs.elements[(i32)LayoutElement::LeadProgress],
									 prefs.leadProgressColor);
		V_snprintf(text, sizeof(text), "%i%%", Clamp(percent, 0, 100));
	}
	// Цвет — свой преф элемента (mhudLeadProgressColor, правится в редакторе !hud), дефолт белый.
	this->UpdateLayoutElement(layout, LayoutElement::LeadProgress, show, show ? text : NULL, this->GetLayoutPrefs().leadProgressColor, force);
}

bool KZHUDService::UpdateHudLayout(KZPlayer *source)
{
	bool created = false;
	CCSCustomHudLayout *layout = this->EnsureOwnedLayout(created);
	if (!layout)
	{
		// Сущность не создалась (схема разъехалась после апдейта CS2 / MultiAddonManager нет)
		// — это ОТКАЗ, а не «худа нет». Вызывающий уходит на HTML, здесь громко пишем причину
		// (правило проекта: отказ логируется с машинно-читаемым reason, молчаливое исчезновение
		// худа недопустимо; META_CONPRINTF из апстрима у нас не используется нигде — общий
		// канал логов проекта KZ_LOG_*, см. utils/logging.h).
		KZ_LOG_ERROR(LogChannel::General, "[cyb] panorama_hud_unavailable reason=layout_entity_failed slot=%i\n",
					 this->player->GetPlayerSlot().Get());
		return false;
	}
	// created=true — сущность только что заспавнена, кэши классов элементов и клавиш пусты
	// (см. LayoutElementState/LayoutKeysState), поэтому первый проход обязан выставить всё
	// принудительно (иначе первый кадр уедет с дефолтными классами схемы).
	const bool force = created;
	if (force)
	{
		// Свежая сущность — кэш дочерних панелей полей редактора обязан сброситься вместе с ней
		// (см. LayoutExtraState). Здесь, а не в каждом Update*: дельту таймера и ячейки PB/WR
		// пишут разные функции, а сброс нужен ровно один, до первой из них.
		this->layoutExtra = LayoutExtraState();
	}
	// IsShowingPanel() здесь НЕ читаем: преф showPanel выведен из оборота (TogglePanel()
	// не вызывается ниоткуда, !panel переключает hudType Off↔Standard) — у игрока со старым
	// showPanel=false в БД panorama иначе гасла бы целиком и молча, без лога и без способа
	// включить обратно. Единственный переключатель видимости panorama — сам hudType, а он уже
	// проверен вызывающим (usePanorama в DrawPanels) — здесь элементы всегда показываются.
	//
	// Крестик независим от элементов ниже: у него свой тумблер (mhudCrosshair), и применяется
	// он тем же вызовом — самостоятельная настройка, не часть видимости элементов.
	//
	// В спектейте крестик гасим ВСЕГДА, отдельным префом это не закрывается. Панель рисует
	// реплику cl_crosshair* ПОЛУЧАТЕЛЯ (this->crosshair наполняется опросом его клиента), а не
	// наблюдаемого, и в наблюдении игра своего прицела не рисует вовсе — то есть на чужом
	// экране висел бы заведомо посторонний крестик. Мимикрия (mhudMimicSpec) тут не помощник:
	// она подменяет ПРЕФЫ (GetLayoutPrefs), а конвары клиента подменить нечем, так что
	// «крестик наблюдаемого» всё равно вышел бы своей формы — поэтому крестик из мимикрии
	// исключён и решает только факт наблюдения.
	// source != this->player — это ровно «получатель наблюдает за другим» (см. DrawPanels:
	// source — наблюдаемый при спектейте, иначе сам получатель). Мёртвый без цели наблюдения
	// сюда не доходит: там DestroyOwnedLayout (kz_player.cpp).
	this->ApplyCrosshair(layout, /* show */ source == this->player, force);

	// SpeedInfo — общий расчёт (Task 6/R3): копировать расчёт сюда запрещено (см.
	// KZHUDService::GetSpeedInfo в kz_hud.h/kz_hud.cpp) — иначе показания скорости panorama-
	// пути разошлись бы с остальными ветками худа.
	const SpeedInfo info = source->hudService->GetSpeedInfo();
	this->UpdateTimerElement(layout, source, force);
	this->UpdateSpeedElement(layout, info, force);
	this->UpdatePrespeedElement(layout, source, info, force);
	this->UpdateKeysElement(layout, source, force);
	this->UpdateCheckpointElement(layout, source, force);
	this->UpdatePbWrElement(layout, source, force);
	this->UpdateShowPosElement(layout, source, force);
	this->UpdateCourseElement(layout, source, force);
	this->UpdateRunTypeElement(layout, source, force);
	this->UpdateLeadProgressElement(layout, source, force);
	return true;
}
