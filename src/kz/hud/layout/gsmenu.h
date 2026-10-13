#pragma once
#include "kz/kz.h"

#include <functional>
#include <string>
#include <vector>

// Общее panorama-окно GYMSTRIKE (спека монорепы docs/design/2026-10-13-panorama-menus.md):
// окно-таблица gw (!maptop, !jumptop), боковой список gs, сетка карт gg, голосование gv — одна
// разметка cyber/gsmenu.xml аддона GYMSTRIKE-KZ (генератор tools/gen_gsmenu_xml.py) и ЧЕТВЁРТАЯ
// персональная сущность custom_hud_layout игрока (после худа, !options и карточки реплея).
//
// Валидатор CustomHud клиента пускает только Panel/Label/Button без скриптов (проверено 13.10):
// всё состояние — классы и строковые переменные, клик — id кнопки. Захват курсора — только у
// окон gw/gg: он отнимает движение и клавиши, поэтому боковые панели живут без него.
#define KZ_GSMENU_LAYOUT "panorama/layout/custom_game/cyber/gsmenu.vxml_c"

// Ёмкости разметки — контракт с tools/gen_gsmenu_xml.py аддона, менять только вместе.
#define KZ_GSMENU_TABLE_ROWS    10
#define KZ_GSMENU_TABLE_COLS    8
#define KZ_GSMENU_ROW_ACTIONS   3
#define KZ_GSMENU_FILTER_GROUPS 3
#define KZ_GSMENU_FILTER_CHIPS  8
#define KZ_GSMENU_LIST_ROWS     10
// Пересоздание сущности на открытии, когда её интерн-таблица (лимит 1024) подошла к краю:
// классы обложек mc-<карта> уникальны и копятся за жизнь сущности.
#define KZ_GSMENU_INTERN_RECYCLE 900

class KZPlayer;
class ICS2Menus;

namespace KZ::gsmenu
{
	struct Chip
	{
		std::string label;
		bool on = false;
	};

	struct Row
	{
		std::string cells[KZ_GSMENU_TABLE_COLS];
		std::string initial; // буква на квадрате вместо аватара (аватарки валидатор не пускает)
		u32 colour = 0;      // класс avc0..avc7
		bool wr = false;     // золото
		bool me = false;     // своя строка — акцент
		std::string actions[KZ_GSMENU_ROW_ACTIONS]; // пустая — кнопки нет
		std::string note;
	};

	struct Table
	{
		std::string title, sub;
		std::string cover; // имя карты для класса mc-<карта>; пусто — заглушка
		i32 tier = 0;      // 0 — без бейджа тира
		const char *cols = "cols-maptop";
		bool noTp = false;
		std::vector<Chip> filters[KZ_GSMENU_FILTER_GROUPS];
		std::string headers[KZ_GSMENU_TABLE_COLS];
		std::vector<Row> rows;
		std::string page, hint;
		bool prev = false, next = false;
	};

	// Боковой список gs (без захвата курсора) — меню cs2menus (gsmenu_shim.cpp).
	struct ListRow
	{
		std::string label, value;
		bool sel = false, dim = false, adj = false, cap = false, tog = false, on = false, grp = false;
	};

	struct List
	{
		std::string title, sub;
		std::vector<ListRow> rows; // не больше KZ_GSMENU_LIST_ROWS
		bool adjHint = false;      // подсказка A/D в подвале
		std::string selectHint = "выбрать";
		std::string exitHint = "F"; // у спектатора выход — Shift
	};

	// Клик по кнопке окна: id как в разметке (gw_close, gf1_0, gr3, gr3_go, gr3_a0, gw_next…).
	// Строки (gr{i}) раскрывает/сворачивает само окно — колбэк их не получает.
	using ClickFn = std::function<void(KZPlayer *player, const char *id)>;

	// Открыть или перерисовать окно-таблицу. Захват курсора включается.
	bool ShowTable(KZPlayer *player, const Table &table, ClickFn onClick);
	void Close(KZPlayer *player, const char *reason);
	bool ShowList(KZPlayer *player, const List &list);
	void HideList(KZPlayer *player);
	bool IsOpen(KZPlayer *player);

	// Проводка из KZHUDService.
	bool Owns(KZPlayer *player, CEntityHandle handle);
	bool OnClick(KZPlayer *player, uint32 packedHandle, const char *id); // true — клик наш
	void OnSlotReset(KZPlayer *player);                                  // дисконнект
	void OnRoundStart(KZPlayer *player);                                 // смена карты снесла сущность
	void Cleanup();                                                      // выгрузка плагина

	// Прослойка над cs2menus (gsmenu_shim.cpp): меню форка ведёт сама и рисует боковым списком;
	// без панорамы всё уходит в cs2menus как раньше.
	ICS2Menus *WrapMenus(ICS2Menus *real);
	void ShimTick(KZPlayer *player);         // ввод и перерисовка, каждый такт (DrawPanels)
	void ShimDisconnect(KZPlayer *player);
	void ShimRedrawAll(); // сущность пересоздана/снесена — перерисовать открытые списки
	bool ShimClosedRecently(KZPlayer *player); // меню прослойки закрылось в этом/прошлом такте
	// Заголовок группы перед пунктом item меню (только панорама; cs2menus его не видит).
	void SetItemGroup(uint32_t menu, int item, const char *title);

	// Панорама-меню доступны (аддон с разметкой подключён, сущность создаётся).
	bool Available(KZPlayer *player);

	// Буква для квадрата: первый символ UTF-8, латиница — заглавная.
	std::string Initial(const char *name);
} // namespace KZ::gsmenu
