// Регистрация НАШИХ уникальных веток в реестре настроек (kz/option/menu/model.h) —
// Checkpoint/Visibility/Sound/Messages/Paint. У апстрима прямого образца нет: чекпоинты,
// paint и часть звуковых тумблеров — свои модули, апстримного дерева под них не существует
// (см. docs/design/2026-09-08-hud-options-diff.md §3/§6). Состав и ключи взяты ровно из
// текущего !options (kz_option_menu.cpp, таблицы s_cpItems/s_visItems/s_sndItems/s_msgItems/
// s_paintItems) — расхождение имени префа тут означало бы потерянную настройку игрока.
// Фразы для пунктов и категорий — те же "Options - Menu Cat *"/"Options - Menu Label *",
// уже локализованные под старое меню (translations/cs2kz-options-menu.phrases.txt),
// переиспользуем один в один.
//
// Три пункта (HidePlayers/HideWeapon/TimerStopSound) регистрируем через AddActionToggle,
// не AddToggle: их Toggle*()-функции держат приватный кэш-член сервиса в синхроне с префом
// (KZQuietService::hideOtherPlayers/hideWeapon, KZTimerService::shouldPlayTimerStopSound) —
// сырой AddToggle написал бы преф, но не тронул бы кэш, и поведение разошлось бы с настройкой
// до следующего реконнекта. HideLegs/CheckpointMessage/CheckpointSound/TeleportSound/
// ShowAllPaint кэша не держат (каждый раз читают преф заново) — им хватает AddToggle.
//
// Старое меню !options (kz_option_menu.cpp) не трогаем — оно остаётся рабочим до отдельной
// задачи переноса на реестр (см. docs/design диф §6 и брифы транша). Вызов регистрации
// (KZLocalOptionsMenu_Register) в общий Init-порядок пока никто не включает — потребитель
// дерева (KZMenuService) ещё не построен, см. тот же комментарий в jumpstats_prefs.cpp.
#include "kz/option/kz_option.h"
#include "kz/option/menu/model.h"
#include "kz/checkpoint/kz_checkpoint.h"
#include "kz/quiet/kz_quiet.h"
#include "kz/timer/kz_timer.h"
#include "kz/paint/kz_paint.h"

#include "tier0/memdbgon.h"

namespace
{
	// ------------------------------------------------------------------ Checkpoint ------
	void SetStartPosOnActivate(KZPlayer *player, i64 tag)
	{
		player->checkpointService->SetStartPosition();
	}

	void ClearStartPosOnActivate(KZPlayer *player, i64 tag)
	{
		player->checkpointService->ClearStartPosition();
	}

	// ------------------------------------------------------------------ Visibility ------
	i64 HidePlayersGetCurrent(KZPlayer *player, i64 tag)
	{
		return player->optionService->GetPreferenceBool("hideOtherPlayers", false) ? 1 : 0;
	}

	void HidePlayersOnActivate(KZPlayer *player, i64 tag)
	{
		player->quietService->ToggleHide();
	}

	i64 HideWeaponGetCurrent(KZPlayer *player, i64 tag)
	{
		return player->optionService->GetPreferenceBool("hideWeapon", false) ? 1 : 0;
	}

	void HideWeaponOnActivate(KZPlayer *player, i64 tag)
	{
		player->quietService->ToggleHideWeapon();
	}

	// ---------------------------------------------------------------------- Sound ------
	i64 TimerStopSoundGetCurrent(KZPlayer *player, i64 tag)
	{
		return player->optionService->GetPreferenceBool("timerStopSound", true) ? 1 : 0;
	}

	void TimerStopSoundOnActivate(KZPlayer *player, i64 tag)
	{
		player->timerService->ToggleTimerStopSound();
	}
} // namespace

// Регистрирует наши локальные ветки. Пока не вызывается ниоткуда — см. комментарий в шапке.
void KZLocalOptionsMenu_Register()
{
	// --- Checkpoint --------------------------------------------------------------------
	KZOptNode *checkpoint = KZ::menu::AddCategory("Options - Menu Cat Checkpoint");
	KZ::menu::AddButton(checkpoint, "Options - Menu Label SetStartPos", &SetStartPosOnActivate);
	KZ::menu::AddButton(checkpoint, "Options - Menu Label ClearStartPos", &ClearStartPosOnActivate);
	KZ::menu::AddToggle(checkpoint, "Options - Menu Label CheckpointMessage", "checkpointMessage", true);

	// --- Visibility ----------------------------------------------------------------------
	KZOptNode *visibility = KZ::menu::AddCategory("Options - Menu Cat Visibility");
	KZ::menu::AddActionToggle(visibility, "Options - Menu Label HidePlayers", &HidePlayersGetCurrent, &HidePlayersOnActivate);
	KZ::menu::AddActionToggle(visibility, "Options - Menu Label HideWeapon", &HideWeaponGetCurrent, &HideWeaponOnActivate);
	// hideLegs дефолт НАШ: true (kz_option_menu.cpp/kz_player.cpp), апстримный false НЕ
	// переносим — см. docs/design диф §4, task-8-brief.md.
	KZ::menu::AddToggle(visibility, "Options - Menu Label HideLegs", "hideLegs", true);

	// --- Sound ----------------------------------------------------------------------------
	KZOptNode *sound = KZ::menu::AddCategory("Options - Menu Cat Sound");
	KZ::menu::AddToggle(sound, "Options - Menu Label CheckpointSound", "checkpointSound", true);
	KZ::menu::AddToggle(sound, "Options - Menu Label TeleportSound", "teleportSound", true);
	KZ::menu::AddActionToggle(sound, "Options - Menu Label TimerStopSound", &TimerStopSoundGetCurrent, &TimerStopSoundOnActivate);
	// Хранится как float 0.0-2.0 (см. kz_recordvolume, клэмп 0..2), в меню — проценты 0-200.
	KZ::menu::AddSize(sound, "Options - Menu Label RecordVolume", "recordVolume", 100, 0, 200);
	KZ::menu::SetItemUnit(sound, "%");
	KZ::menu::SetItemScale(sound, 100);

	// --- Messages --------------------------------------------------------------------------
	KZOptNode *messages = KZ::menu::AddCategory("Options - Menu Cat Messages");
	KZ::menu::AddToggle(messages, "Options - Menu Label MissedTime", "missedTimeAnnounce", true);

	// --- Paint -------------------------------------------------------------------------------
	KZOptNode *paint = KZ::menu::AddCategory("Options - Menu Cat Paint");
	KZ::menu::AddToggle(paint, "Options - Menu Label ShowAllPaint", "showAllPaint", false);
	// Преф хранится как упакованный int 0xRRGGBBAA (KZPaintService::SetColor/SetColorRGB) —
	// тот же формат, что и модель Color (см. model.cpp::ResetNode). Дефолт — красный, как в
	// KZPaintService::GetColor ("Default to red").
	KZ::menu::AddColor(paint, "Options - Menu Label PaintColor", "paintColor", Color(255, 0, 0, 255));
	KZ::menu::AddSize(paint, "Options - Menu Label PaintSize", "paintSize", (i32)KZPaintService::DEFAULT_PAINT_SIZE, 1, 50);
}
