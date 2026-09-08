// Пять элементов panorama-худа + точка сборки (Task 6). Перенесено с апстрима
// (origin/master:src/kz/hud/layout/mhud.cpp), расхождения с ним — см. заметки планирования
// задачи в отдельном (не этом) репозитории, кратко (R1-R4):
//   - MHUDElement/MHUD_ELEMENTS -> LayoutElement/LAYOUT_ELEMENTS (R1, имя занято particles.cpp);
//   - GetPrefs()/IsMHUDElementEnabled -> GetLayoutPrefs()/IsLayoutElementEnabled (R4);
//   - GetPreferenceColor -> GetMHUDColorPref, дефолтные цвета уже подняты в kz_hud.h (R2);
//   - SpeedInfo больше не апстримный тип: это ОБЩИЙ метод KZHUDService::GetSpeedInfo(),
//     который также использует particle-путь (particles.cpp/UpdateMHUDSpeed) — см. R3.
//   - клавиши: апстримные keysIdle/keysBorder/keysGlowEnabled/keysFillEnabled/keysLetters/
//     keysSquare/keysOverlapAxis/keysOverlapGlow/keysPressed-цвет НЕ переносим — таких префов
//     Task 5 не заводил (в нашей базе их нет). Оставлено то, что опирается на существующие
//     префы: нажатие клавиши, общий цвет/оверлап (hudKeysOverlap/mhudKeysOverlapColor),
//     размер и шрифт-класс контейнера Keys.
//   - ApplyCrosshair НЕ переносим — крестик отдельная задача (10), место оставлено комментарием.
#include "kz/hud/layout/layout.h"
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
	// Как в particle-пути (UpdateMHUDKeys): без раздельного per-axis режима (такого префа у
	// нас нет) — оверлап красит контейнер целиком в hudKeysOverlap/mhudKeysOverlapColor.
	const bool overlap = ((forward && back) || (left && right)) && prefs.keysOverlapEnabled;
	const Color color = overlap ? prefs.keysOverlap : prefs.keys;
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

	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(KEY_PANELS); i++)
	{
		if (this->layoutKeys.pressed[i] == keys[i])
		{
			continue;
		}
		this->layoutKeys.pressed[i] = keys[i];
		layout->SetHasClass(KEY_PANELS[i], "pressed", keys[i] ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass);
	}

	// Размер элемента (уже clamped/snapped к LAYOUT_SIZE_MIN/MAX в RefreshLayoutPrefs) — на
	// контейнер (масштаб кнопок) и на каждую кнопку отдельно (масштаб глифа): в panorama
	// font-size не наследуется детьми через класс родителя, апстрим тоже дублирует явно.
	const MHUDLayoutPrefs::Element &cached = prefs.elements[(i32)LayoutElement::Keys];
	const i32 size = cached.size;
	const char *const keysPanel = LAYOUT_ELEMENTS[(i32)LayoutElement::Keys].panelId;

	if (this->layoutKeys.boxSize != size)
	{
		char className[32];
		if (this->layoutKeys.boxSize != INT_MIN)
		{
			V_snprintf(className, sizeof(className), "key-size--%i", this->layoutKeys.boxSize);
			layout->SetHasClass(keysPanel, className, k_eHudPanelClassStatus_DoesNotHaveClass);
		}
		V_snprintf(className, sizeof(className), "key-size--%i", size);
		layout->SetHasClass(keysPanel, className, k_eHudPanelClassStatus_HasClass);
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
			layout->SetHasClass(KEY_PANELS[i], className, k_eHudPanelClassStatus_HasClass);
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
			layout->SetHasClass(KEY_GLYPHS[i], cached.fontClass, k_eHudPanelClassStatus_HasClass);
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
	const bool show = this->IsShowingPanel();

	// Крестик — задача 10, независим от коллапса элементов ниже. Место для
	// ApplyCrosshair(layout, show, force) оставлено здесь намеренно, вызов не добавляем.

	if (!show)
	{
		// show=false — применяем "hidden" всем элементам и выходим: текст/цвет ниже
		// проигнорированы (см. UpdateLayoutElement), скрытый элемент значения не теряет.
		for (i32 i = 0; i < (i32)LayoutElement::Count; i++)
		{
			this->UpdateLayoutElement(layout, (LayoutElement)i, false, NULL, MHUD_DEF_BASE_COLOR, force);
		}
		return true;
	}

	// SpeedInfo — общий расчёт (Task 6/R3): panorama-путь ПЯТАЯ ветвь показа скорости, копия
	// расчёта сюда запрещена (см. KZHUDService::GetSpeedInfo в kz_hud.h/particles.cpp).
	const SpeedInfo info = source->hudService->GetSpeedInfo();
	this->UpdateTimerElement(layout, source, force);
	this->UpdateSpeedElement(layout, info, force);
	this->UpdatePrespeedElement(layout, info, force);
	this->UpdateKeysElement(layout, source, force);
	this->UpdateCheckpointElement(layout, source, force);
	return true;
}
