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

	// Меню управления реплеем живёт ТОЛЬКО на panorama (hud/layout/rpmenu.cpp) и всегда открыто,
	// пока игрок наблюдает реплей-бота; команды открытия нет, `!rpmenu` лишь напоминает об этом.
	// Старое cs2menus-меню удалено 09.09 (решение пользователя). Здесь — словарь пунктов, их
	// семантика и тексты строк карточки; ввод и рендер — в rpmenu.cpp.
	// Пункты меню реплея. Порядок = порядок строк карточки. Бинды в подписях НЕ пишутся — их
	// показывает контекстная подсказка под списком (GetReplayMenuHintText); регулируемые
	// строки обрамляются «< >» в рендере, когда выбраны (IsReplayMenuLineAdjustable).
	enum class ReplayMenuLine
	{
		Pause,   // E — пауза/продолжить
		Step,    // A/D — шаг на тик записи назад/вперёд (шаг сам ставит паузу), E — шаг вперёд
		Seek,    // A/D — перемотка ±RPMENU_SEEK_STEP_10 сек (menu.cpp)
		Restart, // E — с начала
		Speed,   // A/D — пресет скорости, E — сброс на 1x
		End,     // E — остановить плейбек и убрать бота
		Count
	};

	inline bool IsReplayMenuLineAdjustable(ReplayMenuLine line)
	{
		return line == ReplayMenuLine::Step || line == ReplayMenuLine::Seek || line == ReplayMenuLine::Speed;
	}

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

	// Текст строки panorama-меню на языке игрока, с живыми значениями (пауза/скорость), без
	// биндов и без обрамления — их добавляет рендер.
	std::string GetReplayMenuLineText(KZPlayer *player, ReplayMenuLine line);
	// Заголовок карточки: «РЕПЛЕЙ · <ник автора рана>».
	std::string GetReplayMenuTitleText(KZPlayer *player);
	// Строка состояния: «<скорость>x · <время> / <длительность>[ · пауза]» — живая.
	std::string GetReplayMenuStatusText(KZPlayer *player);
	// Контекстная подсказка биндов под списком — для ВЫБРАННОЙ строки.
	std::string GetReplayMenuHintText(KZPlayer *player, ReplayMenuLine line);

} // namespace KZ::replaysystem::menu

#endif // KZ_REPLAYMENU_H
