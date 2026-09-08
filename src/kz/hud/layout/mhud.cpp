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
#include "kz/replays/kz_replaysystem.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "utils/logging.h"

#include "tier0/memdbgon.h"

void KZHUDService::UpdateTimerElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force)
{
	std::string text = source->hudService->GetTimerText(this->player->languageService->GetLanguage());
	if (!this->GetLayoutPrefs().timerDetailed)
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
	const bool replay = KZ::replaysystem::IsReplayBot(source);
	const bool paused = replay ? KZ::replaysystem::GetPaused() : source->timerService->GetPaused();
	const bool running = replay ? KZ::replaysystem::GetEndTime() == 0.0f : source->timerService->GetTimerRunning();
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

void KZHUDService::UpdatePrespeedElement(CCSCustomHudLayout *layout, const SpeedInfo &info, bool force)
{
	// Отдельного prespeedPrecise у нас нет (Task 5 не заводил) — форматируем тем же
	// speedPrecise, что и саму скорость; без скобок — апстримный prespeedBrackets тоже
	// отсутствует как преф.
	const MHUDLayoutPrefs &prefs = this->GetLayoutPrefs();
	char text[16];
	V_snprintf(text, sizeof(text), prefs.speedPrecise ? "%.2f" : "%.0f", info.prespeed.Length2D());
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
	// info.hasPrespeed — тот же useTakeoff, что и в particle-пути (общий GetSpeedInfo, R3):
	// без взлёта престрейфу нечего показывать, апстримный prespeedHideWalkOff у нас не заведён.
	const bool show = this->IsLayoutElementEnabled(LayoutElement::Prespeed) && info.hasPrespeed;
	this->UpdateLayoutElement(layout, LayoutElement::Prespeed, show, text, color, force);
}

// Порядок соответствует кнопкам в mhud.vxml (чужой аддон 3469155349, путь общий с апстримом
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

	const char *const keysPanel = LAYOUT_ELEMENTS[(i32)LayoutElement::Keys].panelId;

	// Тумблеры на весь контейнер клавиш — каждый ставится И снимается по значению префа
	// (залипание оформления при выключении уже ловилось ревью на другом месте, см. брифинг).
	const i32 idle = prefs.keysIdle;
	if (this->layoutKeys.idle != idle)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "hide-idle", idle == 1);
		SetKeysHasClass(layout, this->player, keysPanel, "keys-underscore", idle == 2);
		this->layoutKeys.idle = idle;
	}
	const i32 noBorder = prefs.keysBorder ? 0 : 1;
	if (this->layoutKeys.noBorder != noBorder)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "keys-noborder", noBorder != 0);
		this->layoutKeys.noBorder = noBorder;
	}
	const i32 noGlow = prefs.keysGlow ? 0 : 1;
	if (this->layoutKeys.noGlow != noGlow)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "keys-noglow", noGlow != 0);
		this->layoutKeys.noGlow = noGlow;
	}
	const i32 noFill = prefs.keysFill ? 0 : 1;
	if (this->layoutKeys.noFill != noFill)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "keys-nofill", noFill != 0);
		this->layoutKeys.noFill = noFill;
	}
	const i32 letters = prefs.keysLetters ? 1 : 0;
	if (this->layoutKeys.letters != letters)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "keys-letters", letters != 0);
		this->layoutKeys.letters = letters;
	}
	const i32 square = prefs.keysSquare ? 1 : 0;
	if (this->layoutKeys.square != square)
	{
		SetKeysHasClass(layout, this->player, keysPanel, "keys-square", square != 0);
		this->layoutKeys.square = square;
	}

	// key-glow-N (keys.css, 160 записей) — та же палитра, что pal-fg-N/pal-bg-N в
	// panorama_tables.cpp (значения сверены построчно, см. отчёт задачи): индекс через
	// FindColorEntry и клэмп в [0, 159] — тот же приём, что и остальной проект (fallback на
	// дефолт при "нашёл, но не туда"); градиент сюда не долетает — SetItemSolidOnly (hud_prefs.cpp)
	// не даёт выбрать его в пикере обоих цветов ниже.
	auto resolveGlowIndex = [](const Color &c)
	{
		return Clamp(panorama::FindColorEntry(c), 0, 159);
	};
	const i32 glow = resolveGlowIndex(prefs.keysPressed);
	const i32 glowOverlap = overlap ? resolveGlowIndex(prefs.keysOverlapGlow) : glow;
	const char *overlapClass = prefs.keysOverlapAxis ? panorama::ResolveColorClass(prefs.keysOverlap) : NULL;
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(KEY_PANELS); i++)
	{
		// Цвет контейнера наследуется детьми — класс здесь его переопределяет для одной кнопки.
		this->SetLayoutClass(layout, KEY_PANELS[i], this->layoutKeys.overlapClass[i], overlapped[i] ? overlapClass : NULL);

		const i32 wanted = overlapped[i] ? glowOverlap : glow;
		if (this->layoutKeys.glow[i] == wanted)
		{
			continue;
		}
		char glowClass[32];
		if (this->layoutKeys.glow[i] >= 0)
		{
			V_snprintf(glowClass, sizeof(glowClass), "key-glow-%i", this->layoutKeys.glow[i]);
			layout->SetHasClass(KEY_PANELS[i], glowClass, k_eHudPanelClassStatus_DoesNotHaveClass);
		}
		V_snprintf(glowClass, sizeof(glowClass), "key-glow-%i", wanted);
		if (!layout->SetHasClass(KEY_PANELS[i], glowClass, k_eHudPanelClassStatus_HasClass))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n", KEY_PANELS[i],
						glowClass, this->player->GetPlayerSlot().Get());
		}
		this->layoutKeys.glow[i] = wanted;
	}

	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(KEY_PANELS); i++)
	{
		if (this->layoutKeys.pressed[i] == keys[i])
		{
			continue;
		}
		this->layoutKeys.pressed[i] = keys[i];
		if (!layout->SetHasClass(KEY_PANELS[i], "pressed", keys[i] ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=pressed slot=%i\n",
						KEY_PANELS[i], this->player->GetPlayerSlot().Get());
		}
	}

	// Размер элемента (уже clamped/snapped к LAYOUT_SIZE_MIN/MAX в RefreshLayoutPrefs) — на
	// контейнер (масштаб кнопок) и на каждую кнопку отдельно (масштаб глифа): в panorama
	// font-size не наследуется детьми через класс родителя, апстрим тоже дублирует явно.
	const MHUDLayoutPrefs::Element &cached = prefs.elements[(i32)LayoutElement::Keys];
	const i32 size = cached.size;

	if (this->layoutKeys.boxSize != size)
	{
		char className[32];
		if (this->layoutKeys.boxSize != INT_MIN)
		{
			V_snprintf(className, sizeof(className), "key-size--%i", this->layoutKeys.boxSize);
			layout->SetHasClass(keysPanel, className, k_eHudPanelClassStatus_DoesNotHaveClass);
		}
		V_snprintf(className, sizeof(className), "key-size--%i", size);
		if (!layout->SetHasClass(keysPanel, className, k_eHudPanelClassStatus_HasClass))
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n", keysPanel,
						className, this->player->GetPlayerSlot().Get());
		}
		this->layoutKeys.boxSize = size;
	}

	if (this->layoutKeys.fontSize != size)
	{
		char className[32];
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(KEY_PANELS); i++)
		{
			if (this->layoutKeys.fontSize != INT_MIN)
			{
				V_snprintf(className, sizeof(className), "font-size--%ipx", this->layoutKeys.fontSize);
				layout->SetHasClass(KEY_PANELS[i], className, k_eHudPanelClassStatus_DoesNotHaveClass);
			}
			V_snprintf(className, sizeof(className), "font-size--%ipx", size);
			if (!layout->SetHasClass(KEY_PANELS[i], className, k_eHudPanelClassStatus_HasClass))
			{
				KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n",
							KEY_PANELS[i], className, this->player->GetPlayerSlot().Get());
			}
		}
		this->layoutKeys.fontSize = size;
	}

	// fontClass — уже резолвленный css-класс (RefreshLayoutPrefs зовёт ResolveFontClass один
	// раз), как и у остальных элементов (см. entity.cpp/UpdateLayoutElement).
	if (this->layoutKeys.fontClass != cached.fontClass)
	{
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(KEY_GLYPHS); i++)
		{
			if (this->layoutKeys.fontClass)
			{
				layout->SetHasClass(KEY_GLYPHS[i], this->layoutKeys.fontClass, k_eHudPanelClassStatus_DoesNotHaveClass);
			}
			if (!layout->SetHasClass(KEY_GLYPHS[i], cached.fontClass, k_eHudPanelClassStatus_HasClass))
			{
				KZ_LOG_WARN(LogChannel::General, "[cyb] panorama_hud_class_dropped reason=intern_limit panel=%s class=%s slot=%i\n",
							KEY_GLYPHS[i], cached.fontClass, this->player->GetPlayerSlot().Get());
			}
		}
		this->layoutKeys.fontClass = cached.fontClass;
	}
}

void KZHUDService::UpdateCheckpointElement(CCSCustomHudLayout *layout, KZPlayer *source, bool force)
{
	std::string text = source->hudService->GetCheckpointText(this->player->languageService->GetLanguage());
	const Color color = this->GetLayoutPrefs().checkpoint;
	const bool show = this->IsLayoutElementEnabled(LayoutElement::Checkpoint) && !text.empty();
	this->UpdateLayoutElement(layout, LayoutElement::Checkpoint, show, text.c_str(), color, force);
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
	// IsShowingPanel() здесь НЕ читаем: преф showPanel выведен из оборота (TogglePanel()
	// не вызывается ниоткуда, !panel переключает hudType Off↔Standard) — у игрока со старым
	// showPanel=false в БД panorama иначе гасла бы целиком и молча, без лога и без способа
	// включить обратно. Единственный переключатель видимости panorama — сам hudType, а он уже
	// проверен вызывающим (usePanorama в DrawPanels) — здесь элементы всегда показываются.
	//
	// Крестик независим от элементов ниже: у него свой тумблер (mhudCrosshair), и применяется
	// он тем же вызовом — самостоятельная настройка, не часть видимости элементов.
	this->ApplyCrosshair(layout, /* show */ true, force);

	// SpeedInfo — общий расчёт (Task 6/R3): копировать расчёт сюда запрещено (см.
	// KZHUDService::GetSpeedInfo в kz_hud.h/kz_hud.cpp) — иначе показания скорости panorama-
	// пути разошлись бы с остальными ветками худа.
	const SpeedInfo info = source->hudService->GetSpeedInfo();
	this->UpdateTimerElement(layout, source, force);
	this->UpdateSpeedElement(layout, info, force);
	this->UpdatePrespeedElement(layout, info, force);
	this->UpdateKeysElement(layout, source, force);
	this->UpdateCheckpointElement(layout, source, force);
	return true;
}
