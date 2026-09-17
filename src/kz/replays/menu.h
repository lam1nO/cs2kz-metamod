#ifndef KZ_REPLAYMENU_H
#define KZ_REPLAYMENU_H

#include "sdk/datatypes.h" // u64 в SearchHit, f64 в ReplayMenuStatus

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
	// семантика и тексты слотов карточки; ввод и запись страницы — в rpmenu.cpp.
	// Пункты меню реплея. Порядок ЗДЕСЬ — только словарь семантики: на карточке строки идут в
	// своём порядке (таблица RPMENU_ROWS в rpmenu.cpp), по нему же ходят W/S. Бинды в подписях
	// НЕ пишутся — их показывает контекстная подсказка под списком (GetReplayMenuHintText), а у
	// регулируемой строки ещё и подсвеченные чипы A/D (класс focused, IsReplayMenuLineAdjustable).
	enum class ReplayMenuLine
	{
		Pause,    // E — пауза/продолжить
		Step,     // A/D — шаг на тик записи назад/вперёд (шаг сам ставит паузу)
		Seek,     // A/D — точная перемотка ±RPMENU_SEEK_STEP_FINE сек (menu.cpp)
		SeekFast, // A/D — быстрая перемотка ±RPMENU_SEEK_STEP_FAST сек
		Restart,  // E — с начала
		Speed,    // A/D — пресет скорости
		End,      // E — остановить плейбек и убрать бота
		Count
	};

	// constexpr — таблица строк карточки (RPMENU_ROWS) проверяет себя static_assert'ом.
	constexpr bool IsReplayMenuLineAdjustable(ReplayMenuLine line)
	{
		return line == ReplayMenuLine::Step || line == ReplayMenuLine::Seek || line == ReplayMenuLine::SeekFast
			   || line == ReplayMenuLine::Speed;
	}

	enum class ReplayMenuInput
	{
		Select, // E
		Dec,    // A
		Inc     // D
	};

	// Применить ввод к строке; здесь же живут пресеты скорости (см. RPMENU_SPEEDS в menu.cpp).
	// Сообщения игроку — как у чат-команд (пауза/продолжить объявляются, шаг и скорость — нет:
	// значение и так видно на карточке).
	void ApplyReplayMenuInput(KZPlayer *player, ReplayMenuLine line, ReplayMenuInput input);

	// Текст строки panorama-меню на языке игрока, с живыми значениями (пауза/скорость), без
	// биндов: их показывают чипы строки и подсказка.
	std::string GetReplayMenuLineText(KZPlayer *player, ReplayMenuLine line);

	// Живое состояние плейбека для карточки: позиция и длительность в ОДНОЙ шкале (см.
	// ReplayMenuPositionTime/ReplayMenuTotalTime в menu.cpp), текст скорости и пауза. Собранной
	// строки состояния больше нет: своя страница раскладывает эти значения по разным слотам
	// (время, длительность, скорость и пилюля состояния — четыре независимых слота).
	// Скорость и пауза общие на сервер — игрок здесь не нужен.
	struct ReplayMenuStatus
	{
		f64 position {};
		f64 total {}; // 0 — длительность неизвестна (реплей не играет)
		bool paused {};
		char speed[16] {};
	};

	void GetReplayMenuStatus(ReplayMenuStatus &out);
	// Идёт ли реплей в режиме AWR (телепорты вырезаны) — бейдж на карточке.
	bool IsReplayMenuAwr();
	// Реплей рана (есть заголовок run с временем) против записи без таймера (джамп/ручной):
	// от этого зависит подпись большого поля времени — «время рана» или «позиция в записи».
	bool IsReplayMenuRunReplay();
	// Ник автора записи для шапки карточки; пусто — в заголовке нет игрока.
	std::string GetReplayMenuAuthorName();
	// Подстрочник шапки: «карта · курс · режим» из заголовка записи. Разделители и порядок —
	// спека (§4, слот meta); перевода здесь нет, это имена из файла реплея. Пусто — данных нет.
	std::string GetReplayMenuMetaText();

	// Шаг перемотки в секундах — подпись чипов A/D на карточке берётся отсюда, чтобы не
	// разъехаться с самой перемоткой (RPMENU_SEEK_STEP_* в menu.cpp). fast — строка быстрой
	// перемотки (ReplayMenuLine::SeekFast).
	int GetReplayMenuSeekStepSeconds(bool fast);

	// Метка типа реплея в шапке карточки (слот badge_type). Заполняется, только когда плейбек
	// запущен через резолв `!replay pb/wr/awr/...`: тип — свойство ЗАПРОСА, в самом файле его
	// нет (docs/design/2026-09-11-replay-player-panorama/data-availability.md §3). Пустой
	// text — бейдж скрыть. cls — класс расцветки страницы («pb», «wr», «other»).
	struct ReplayMenuBadge
	{
		std::string text;
		const char *cls {"other"};
	};

	// Язык не нужен: PB/WR/AWR — одинаковые сокращения во всех локалях.
	void GetReplayMenuBadge(ReplayMenuBadge &out);

	// Контекстная подсказка биндов под списком — для ВЫБРАННОЙ строки.
	std::string GetReplayMenuHintText(KZPlayer *player, ReplayMenuLine line);

} // namespace KZ::replaysystem::menu

#endif // KZ_REPLAYMENU_H
