#pragma once
#include "kz/kz.h"
#include "kz/option/menu/model.h"

#include <vector>

// Обобщённое чтение/запись ОДНОГО значения настройки по её описанию в реестре меню
// (kz/option/menu/model.h). Порт апстримного src/kz/option/pref_registry.{h,cpp}
// (origin/master), расхождения — намеренные:
//
//  1. Userinfo-транспорт (kzp_*-конвары, prefs_transfer.cpp) НЕ портирован: у нас обмен идёт
//     коротким кодом через общую MySQL (спека 2026-09-09-hud-share-design.md §4), а `setinfo`
//     клиент фильтрует на workshop-картах — а все наши карты workshop. Поэтому нет ни Init(),
//     ни RegisterMenu(), ни OnClientDisconnect().
//  2. ReadValue НЕ кавычит строки и векторы: у апстрима значение уезжает строкой консольной
//     команды, где кавычки обязательны, у нас — полем снимка со своим разделителем.
//  3. ValidateValue — наша добавка. Апстримный ApplyValue проверяет только ТИП
//     (pref_registry.cpp:167-223: «диапазонов, whitelist'ов значений — нет»), из-за чего
//     «применилось 3 из 65» игрок не отличает от «применилось 65». Спека §5.5 требует
//     проверку по описанию пункта и видимый отказ с машинно-читаемым reason.
namespace KZ::prefs
{
	struct Entry
	{
		const char *key {};
		KZOptStorage storage {};
		const KZOptItem *item {}; // where the default comes from; stable once every service has registered
		bool isY {};              // Position: this entry is the y key, not the x key
	};

	// Built from KZ::menu::GetTree() on first use, so registration order does not matter.
	const std::vector<Entry> &GetRegistry();
	const Entry *FindEntry(const char *key);

	// Обход ОДНОЙ категории дерева по её phraseKey (апстриму не нужен — ему нужен весь реестр
	// целиком для переноса игрока между серверами; нам нужна ровно подветка HUD, спека §5.1:
	// «источник истины по составу — реестр, а не рукописный список»). Возвращает false, если
	// категории с таким ключом в дереве нет (нечего обменивать — это отказ, не пустой успех).
	bool CollectCategory(const char *categoryPhraseKey, std::vector<Entry> &out);

	// Один токен значения: сохранённое значение игрока, а при его отсутствии —
	// зарегистрированный дефолт пункта. false — отдать нечего (дефолта тоже нет).
	bool ReadValue(KZPlayer *player, const Entry &entry, char *out, i32 outLen);
	bool ReadDefaultValue(const Entry &entry, char *out, i32 outLen);

	// Тип И диапазон по описанию пункта. reason — машинно-читаемая причина отказа
	// (type_bool/type_int/type_float/range_position/range_size/range_color/range_toggle/
	// choice_unknown/font_unknown/storage_unsupported/empty/no_item), для лога и чата.
	bool ValidateValue(KZPlayer *player, const Entry &entry, const char *value, const char *&reason);

	// Пишет значение в префы игрока. НЕ валидирует — вызывающий обязан сперва позвать
	// ValidateValue (разделение сделано, чтобы применение было атомарным: сперва проверяем
	// весь снимок, потом пишем).
	bool ApplyValue(KZPlayer *player, const Entry &entry, const char *value);

	// Реестр держит указатели на KZOptItem внутри узлов KZ::menu, а те удаляются в
	// KZ::menu::Cleanup() на выгрузке плагина — кэш обязан обнулиться в той же точке.
	void Cleanup();
} // namespace KZ::prefs
