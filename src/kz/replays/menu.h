#ifndef KZ_REPLAYMENU_H
#define KZ_REPLAYMENU_H

#include "sdk/datatypes.h" // u64 в SearchHit

#include <string>
#include <vector>

class KZPlayer;

namespace KZ::replaysystem::menu
{
	// Один кандидат для меню выбора реплея по нику (ответ
	// GET /v1/kz/maps/:map/replays/search, схема kzReplaySearchResultSchema).
	struct SearchHit
	{
		std::string nickname;
		std::string replayUuid;
		// PB держателя в миллисекундах — им же отсортирован ответ api (лучший первым);
		// здесь только показываем, повторно не сортируем.
		u64 pbTimeMs {};
	};

	// Меню выбора реплея, когда подстрока ника совпала с несколькими держателями PB
	// (паттерн !spec: одноразовый выбор, закрытие по клику). Возвращает false, если меню
	// показать нечем или некому (cs2menus не загружен, пустой список, плохой слот) — тогда
	// вызывающий сам решает, что делать, и грузит первого кандидата.
	bool OpenReplaySearchMenu(KZPlayer *player, const std::vector<SearchHit> &hits);

	// Меню управления реплеем (!rpmenu). Два бэкенда с одной семантикой пунктов:
	//   - panorama (KZHUDService::OpenReplayMenu, hud/layout/rpmenu.cpp) — когда игрок
	//     наблюдает реплей-бота и есть аддон: список у левого края, W/S/E/A/D без курсора;
	//   - cs2menus (ниже в этом файле) — во всех остальных случаях (нет аддона, игрок не
	//     смотрит бота); тихий no-op, если и cs2menus не загружен.
	// Повторный вызов при открытом panorama-меню ЗАКРЫВАЕТ его (тумблер, как !hudmenu).
	// Если спектейт бота ещё применяется (сразу после `!replay`), запрос откладывается —
	// KZHUDService::RequestReplayMenu, открытие произойдёт с первого тика фактического
	// спектейта, фолбэк на cs2menus по таймауту с reason в логе.
	void OpenReplayControlsMenu(KZPlayer *player);

	// Явно cs2menus-бэкенд (без выбора бэкенда); его зовут селектор OpenReplayControlsMenu и
	// KZHUDService::TickReplayMenuPending как фолбэк. Тихий no-op без cs2menus.
	void OpenReplayControlsMenuCs2menus(KZPlayer *player);
	// Закрыть cs2menus-!rpmenu игрока, если оно сейчас активно (перед открытием panorama).
	void CancelReplayControlsMenuCs2menus(KZPlayer *player);

	// Пункты меню реплея — общий словарь обоих бэкендов. Порядок = порядок строк panorama-меню.
	enum class ReplayMenuLine
	{
		PauseStep, // E — пауза/продолжить, A/D — шаг на тик записи (шаг сам ставит паузу)
		Seek,      // A/D — перемотка ±RPMENU_SEEK_STEP_10 сек (menu.cpp), E — с начала
		Speed,     // A/D — пресет скорости, E — сброс на 1x
		End,       // E — остановить плейбек и убрать бота
		Count
	};

	enum class ReplayMenuInput
	{
		Select, // E
		Dec,    // A
		Inc     // D
	};

	// Применить ввод к строке. Семантика одна на оба бэкенда, здесь же живут пресеты
	// скорости (см. RPMENU_SPEEDS в menu.cpp). Сообщения игроку — как у чат-команд
	// (пауза/продолжить объявляются, шаг и скорость — нет: значение видно в строке).
	void ApplyReplayMenuInput(KZPlayer *player, ReplayMenuLine line, ReplayMenuInput input);

	// Текст строки panorama-меню на языке игрока, с живыми значениями (пауза/скорость) и
	// подсказками клавиш. Без маркера выбора — его ставит рендер.
	std::string GetReplayMenuLineText(KZPlayer *player, ReplayMenuLine line);

	// Открыто ли у слота ИМЕННО !rpmenu (а не любое другое cs2menus-меню). Сравнение по
	// ХЭНДЛУ созданного нами меню — заголовок для этого не годится: он переводится и
	// правится. false, если cs2menus нет, меню закрыто или открыто чужое.
	// Зовётся из худа только когда меню реально открыто (GetActiveMenu берёт мьютекс cs2menus).
	bool IsReplayControlsMenuOpen(int slot);
} // namespace KZ::replaysystem::menu

#endif // KZ_REPLAYMENU_H
