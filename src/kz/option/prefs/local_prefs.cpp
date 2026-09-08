// Регистрация НАШИХ уникальных веток в реестре настроек (kz/option/menu/model.h) —
// Checkpoint/Visibility/Sound/Messages/Paint. У апстрима прямого образца нет: чекпоинты,
// paint и часть звуковых тумблеров — свои модули, апстримного дерева под них не существует
// (см. docs/design/2026-09-08-hud-options-diff.md §3/§6). Состав и ключи префов взяты
// ровно из старого cs2menus-меню !options (уже удалено) — расхождение имени
// префа тут означало бы потерянную настройку игрока.
// Фразы для пунктов и категорий — те же "Options - Menu Cat *"/"Options - Menu Label *",
// локализованные ещё под старое меню (translations/cs2kz-options-menu.phrases.txt).
//
// Три пункта (HidePlayers/HideWeapon/TimerStopSound) регистрируем через AddActionToggle,
// не AddToggle: их Toggle*()-функции держат приватный кэш-член сервиса в синхроне с префом
// (KZQuietService::hideOtherPlayers/hideWeapon, KZTimerService::shouldPlayTimerStopSound) —
// сырой AddToggle написал бы преф, но не тронул бы кэш, и поведение разошлось бы с настройкой
// до следующего реконнекта. HideLegs/CheckpointMessage/CheckpointSound/TeleportSound/
// ShowAllPaint кэша не держат (каждый раз читают преф заново) — им хватает AddToggle.
// SetItemPref рядом с каждым из трёх — не для чтения (его делает getCurrent), а чтобы у пункта
// не оставался prefKey == NULL: запись по такому ключу заводит в префах игрока член с ПУСТЫМ
// именем и флашит его в БД (см. фикс-раунд ревью, находка 1).
//
// Вызов регистрации (KZLocalOptionsMenu_Register) — в общем Init-порядке (cs2kz.cpp::Load, Task 15), между Misc
// и Jumpstats: пять per-run веток (чекпоинты/видимость/звук/сообщения/paint), используются
// часто, но не так вездесуще, как Misc.
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

// Регистрирует наши локальные ветки. Вызов — cs2kz.cpp::Load (Task 15).
void KZLocalOptionsMenu_Register()
{
	// Пять локальных страниц — ПОДКАТЕГОРИИ одного узла «Игра» (задача «дерево категорий»):
	// по одному-четырём пункта каждая, плоским списком они забивали левую колонку наравне с
	// худом. Состав пунктов не меняется, меняется только группировка. Родитель заводится
	// здесь, потому что все пять живут в этом файле: делить его между Register-ами разных
	// файлов значило бы завязать порядок подкатегорий на порядок вызовов в cs2kz.cpp.
	KZOptNode *game = KZ::menu::AddCategory("Options - Menu Cat Gameplay");

	// --- Checkpoint --------------------------------------------------------------------
	KZOptNode *checkpoint = KZ::menu::AddSub(game, "Options - Menu Cat Checkpoint");
	KZ::menu::AddButton(checkpoint, "Options - Menu Label SetStartPos", &SetStartPosOnActivate);
	KZ::menu::AddButton(checkpoint, "Options - Menu Label ClearStartPos", &ClearStartPosOnActivate);
	KZ::menu::AddToggle(checkpoint, "Options - Menu Label CheckpointMessage", "checkpointMessage", true);

	// --- Visibility ----------------------------------------------------------------------
	KZOptNode *visibility = KZ::menu::AddSub(game, "Options - Menu Cat Visibility");
	KZ::menu::AddActionToggle(visibility, "Options - Menu Label HidePlayers", &HidePlayersGetCurrent, &HidePlayersOnActivate);
	KZ::menu::SetItemPref(visibility, "hideOtherPlayers", KZOptStorage::Bool, 0);
	KZ::menu::AddActionToggle(visibility, "Options - Menu Label HideWeapon", &HideWeaponGetCurrent, &HideWeaponOnActivate);
	KZ::menu::SetItemPref(visibility, "hideWeapon", KZOptStorage::Bool, 0);
	// hideLegs дефолт НАШ: true (kz_player.cpp), апстримный false НЕ
	// переносим — см. docs/design диф §4, task-8-brief.md.
	KZ::menu::AddToggle(visibility, "Options - Menu Label HideLegs", "hideLegs", true);

	// --- Sound ----------------------------------------------------------------------------
	KZOptNode *sound = KZ::menu::AddSub(game, "Options - Menu Cat Sound");
	KZ::menu::AddToggle(sound, "Options - Menu Label CheckpointSound", "checkpointSound", true);
	KZ::menu::AddToggle(sound, "Options - Menu Label TeleportSound", "teleportSound", true);
	KZ::menu::AddActionToggle(sound, "Options - Menu Label TimerStopSound", &TimerStopSoundGetCurrent, &TimerStopSoundOnActivate);
	KZ::menu::SetItemPref(sound, "timerStopSound", KZOptStorage::Bool, 1);
	// Хранится как float 0.0-2.0 (см. kz_recordvolume, клэмп 0..2), в меню — проценты 0-200.
	KZ::menu::AddSize(sound, "Options - Menu Label RecordVolume", "recordVolume", 100, 0, 200);
	KZ::menu::SetItemUnit(sound, "%");
	KZ::menu::SetItemScale(sound, 100);

	// --- Messages --------------------------------------------------------------------------
	KZOptNode *messages = KZ::menu::AddSub(game, "Options - Menu Cat Messages");
	KZ::menu::AddToggle(messages, "Options - Menu Label MissedTime", "missedTimeAnnounce", true);
	// mapOverlay — строки "[CS2KZ] split|N|время" в КОНСОЛЬ (kz_timer.cpp, 7 мест): их читают
	// внешние оверлеи стримеров, в игре ничего не рисуется. Преф жив и переключался только
	// командой `!mapoverlay` — пункта в меню у него не было. Голый AddToggle: кэша нет, читатели
	// каждый раз зовут GetPreferenceBool. Дефолт false — тот же, что у `!mapoverlay`
	// (kz_timer.cpp:2564) и у остальных читателей (GetPreferenceBool без второго аргумента).
	KZ::menu::AddToggle(messages, "Options - Menu Label MapOverlay", "mapOverlay", false);
	KZ::menu::SetItemSubtext(messages, "Options - Menu Label MapOverlay Sub");

	// --- Paint -------------------------------------------------------------------------------
	KZOptNode *paint = KZ::menu::AddSub(game, "Options - Menu Cat Paint");
	KZ::menu::AddToggle(paint, "Options - Menu Label ShowAllPaint", "showAllPaint", false);
	// Преф хранится как упакованный int 0xRRGGBBAA (KZPaintService::SetColor/SetColorRGB) —
	// тот же формат, что и модель Color (см. model.cpp::ResetNode). Дефолт — красный, как в
	// KZPaintService::GetColor ("Default to red").
	KZ::menu::AddColor(paint, "Options - Menu Label PaintColor", "paintColor", Color(255, 0, 0, 255));
	KZ::menu::AddSize(paint, "Options - Menu Label PaintSize", "paintSize", (i32)KZPaintService::DEFAULT_PAINT_SIZE, 1, 50);
}
