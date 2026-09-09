#include "kz/option/pref_registry.h"
#include "kz/option/kz_option.h"
// Шрифт валидируется таблицей панорамы: неизвестный слаг читатель молча заменяет дефолтом
// (panorama::ResolveFontClass, hud/layout/prefs.cpp), то есть чужой шрифт «применился бы» и не
// применился. Отказ обязан быть громким.
#include "kz/hud/layout/panorama_tables.h"

#include "tier1/strtools.h"

#include "tier0/memdbgon.h"

static_global std::vector<KZ::prefs::Entry> registry;
static_global bool registryBuilt = false;

static_function void AddEntry(std::vector<KZ::prefs::Entry> &out, const char *key, const KZOptItem &item, bool isY)
{
	if (!key || item.storage == KZOptStorage::None)
	{
		return;
	}
	for (const KZ::prefs::Entry &entry : out)
	{
		if (V_strcmp(entry.key, key) == 0)
		{
			return;
		}
	}
	out.push_back({key, item.storage, &item, isY});
}

static_function void CollectNode(std::vector<KZ::prefs::Entry> &out, KZOptNode *node)
{
	if (!node)
	{
		return;
	}
	for (const KZOptItem &item : node->items)
	{
		AddEntry(out, item.prefKey, item, false);
		AddEntry(out, item.yKey, item, true);
	}
	for (KZOptNode *sub : node->subs)
	{
		CollectNode(out, sub);
	}
}

const std::vector<KZ::prefs::Entry> &KZ::prefs::GetRegistry()
{
	if (!registryBuilt)
	{
		registryBuilt = true;
		for (KZOptNode *category : KZ::menu::GetTree())
		{
			CollectNode(registry, category);
		}
	}
	return registry;
}

const KZ::prefs::Entry *KZ::prefs::FindEntry(const char *key)
{
	for (const KZ::prefs::Entry &entry : KZ::prefs::GetRegistry())
	{
		if (V_strcmp(entry.key, key) == 0)
		{
			return &entry;
		}
	}
	return NULL;
}

bool KZ::prefs::CollectCategory(const char *categoryPhraseKey, std::vector<KZ::prefs::Entry> &out)
{
	out.clear();
	if (!categoryPhraseKey)
	{
		return false;
	}
	// Поиск по phraseKey, а не по индексу: индекс едет при первой же смене порядка Register()
	// в cs2kz.cpp (тот же довод, что у SCMD kz_hudmenu, hud/layout/menu.cpp).
	for (KZOptNode *category : KZ::menu::GetTree())
	{
		if (category->phraseKey && V_strcmp(category->phraseKey, categoryPhraseKey) == 0)
		{
			CollectNode(out, category);
			return true;
		}
	}
	return false;
}

void KZ::prefs::Cleanup()
{
	registry.clear();
	registryBuilt = false;
}

// The value the preference reads as while the player has never set it, as the same token
// ReadValue writes. Exporting these is what lets a snapshot replace a recipient's whole set
// instead of only the keys the sender happened to touch.
bool KZ::prefs::ReadDefaultValue(const KZ::prefs::Entry &entry, char *out, i32 outLen)
{
	const KZOptItem *item = entry.item;
	if (!item)
	{
		return false;
	}
	switch (item->type)
	{
		case KZOptItemType::Color:
		{
			// Упакованный int — тот же формат, что у GetMHUDColorPref/SetMHUDColorPref
			// (hud/kz_hud.cpp, hud/layout/menu.cpp) и у KZ::menu::ResetNode.
			const Color &color = item->cdef;
			i64 packed = ((i64)color.r() << 24) | ((i64)color.g() << 16) | ((i64)color.b() << 8) | (i64)color.a();
			V_snprintf(out, outLen, "%lli", (long long)packed);
			return true;
		}
		case KZOptItemType::Position:
			V_snprintf(out, outLen, "%i", entry.isY ? item->iydef : item->idef);
			return true;
		case KZOptItemType::Vector:
			V_snprintf(out, outLen, "%f %f %f", (f32)item->idef, (f32)item->iydef, (f32)item->izdef);
			return true;
		case KZOptItemType::Size:
			if (item->storage == KZOptStorage::Int)
			{
				V_snprintf(out, outLen, "%i", item->idef);
			}
			else
			{
				V_snprintf(out, outLen, "%g", (f64)item->idef / MAX(1, item->scale));
			}
			return true;
		default:
			break;
	}
	switch (entry.storage)
	{
		case KZOptStorage::Bool:
			V_snprintf(out, outLen, "%i", item->idef != 0 ? 1 : 0);
			return true;
		case KZOptStorage::Int:
			V_snprintf(out, outLen, "%i", item->idef);
			return true;
		case KZOptStorage::Float:
			V_snprintf(out, outLen, "%i", item->idef);
			return true;
		case KZOptStorage::Str:
			if (!item->sdef || !item->sdef[0])
			{
				return false;
			}
			V_snprintf(out, outLen, "%s", item->sdef);
			return true;
		default:
			return false;
	}
}

bool KZ::prefs::ReadValue(KZPlayer *player, const KZ::prefs::Entry &entry, char *out, i32 outLen)
{
	if (!player || !player->optionService)
	{
		return false;
	}
	auto *opts = player->optionService;
	if (!opts->HasPreference(entry.key))
	{
		return KZ::prefs::ReadDefaultValue(entry, out, outLen);
	}
	switch (entry.storage)
	{
		case KZOptStorage::Bool:
			V_snprintf(out, outLen, "%i", opts->GetPreferenceBool(entry.key) ? 1 : 0);
			return true;
		case KZOptStorage::Int:
			V_snprintf(out, outLen, "%lli", (long long)opts->GetPreferenceInt(entry.key));
			return true;
		case KZOptStorage::Float:
			V_snprintf(out, outLen, "%g", opts->GetPreferenceFloat(entry.key));
			return true;
		case KZOptStorage::Str:
		{
			const char *value = opts->GetPreferenceStr(entry.key);
			V_snprintf(out, outLen, "%s", value ? value : "");
			return true;
		}
		case KZOptStorage::Vector:
		{
			const Vector value = opts->GetPreferenceVector(entry.key);
			V_snprintf(out, outLen, "%f %f %f", value.x, value.y, value.z);
			return true;
		}
		default:
			return false;
	}
}

// Диапазон по описанию пункта. Числовое значение уже разобрано вызывающим (одна проверка на
// Int и Float — границы в модели целые, см. model.h: lo/hi).
static_function bool CheckNumericRange(KZPlayer *player, const KZ::prefs::Entry &entry, f64 value, const char *&reason)
{
	const KZOptItem *item = entry.item;
	switch (item->type)
	{
		case KZOptItemType::Color:
			// Цвет хранится упакованным u32 (RGBA). Всё, что вне — не цвет.
			if (value < 0.0 || value > 4294967295.0)
			{
				reason = "range_color";
				return false;
			}
			return true;
		case KZOptItemType::Position:
			if (value < (f64)item->lo || value > (f64)item->hi)
			{
				reason = "range_position";
				return false;
			}
			return true;
		case KZOptItemType::Size:
			if (value < (f64)item->lo || value > (f64)item->hi)
			{
				reason = "range_size";
				return false;
			}
			return true;
		case KZOptItemType::Toggle:
			// Тумблер с int-хранением (AddActionToggle + SetItemPref(..., Int, ...)) — всё
			// равно два состояния.
			if (value != 0.0 && value != 1.0)
			{
				reason = "range_toggle";
				return false;
			}
			return true;
		case KZOptItemType::Choice:
			// Диапазона у Choice нет вовсе — есть СПИСОК, и он runtime (getChoices). Спрашиваем
			// его же, что показывает игроку меню: иначе mhudKeysIdle=99 «применился» бы и был
			// молча зажат читателем (layout/prefs.cpp: Clamp(0,2)).
			if (item->getChoices)
			{
				std::vector<KZChoice> choices;
				item->getChoices(player, item->tag, choices);
				for (const KZChoice &choice : choices)
				{
					if ((f64)choice.id == value)
					{
						return true;
					}
				}
				reason = "choice_unknown";
				return false;
			}
			return true;
		default:
			return true;
	}
}

bool KZ::prefs::ValidateValue(KZPlayer *player, const KZ::prefs::Entry &entry, const char *value, const char *&reason)
{
	reason = "";
	if (!value || !value[0])
	{
		reason = "empty";
		return false;
	}
	const KZOptItem *item = entry.item;
	if (!item)
	{
		reason = "no_item";
		return false;
	}
	if (item->type == KZOptItemType::Font)
	{
		// NULL-фолбэк: ResolveFontSlug возвращает его на неизвестном слаге (panorama_tables.cpp).
		if (!panorama::ResolveFontSlug(value, NULL))
		{
			reason = "font_unknown";
			return false;
		}
		return true;
	}
	switch (entry.storage)
	{
		case KZOptStorage::Bool:
		{
			bool parsed = false;
			if (!V_StringToValue<bool>(value, parsed))
			{
				reason = "type_bool";
				return false;
			}
			return true;
		}
		case KZOptStorage::Int:
		{
			int64 parsed = 0;
			if (!V_StringToValue<int64>(value, parsed))
			{
				reason = "type_int";
				return false;
			}
			return CheckNumericRange(player, entry, (f64)parsed, reason);
		}
		case KZOptStorage::Float:
		{
			float64 parsed = 0.0;
			if (!V_StringToValue<float64>(value, parsed))
			{
				reason = "type_float";
				return false;
			}
			return CheckNumericRange(player, entry, (f64)parsed, reason);
		}
		case KZOptStorage::Str:
			// Не Font-пункт: свободная строка. В обмене худа таких нет (см. hud/share/hud_share.cpp,
			// белый список), поэтому дополнительной проверки здесь не заводим.
			return true;
		default:
			reason = "storage_unsupported";
			return false;
	}
}

bool KZ::prefs::ApplyValue(KZPlayer *player, const KZ::prefs::Entry &entry, const char *value)
{
	if (!player || !player->optionService || !value || !value[0])
	{
		return false;
	}
	auto *opts = player->optionService;
	switch (entry.storage)
	{
		case KZOptStorage::Bool:
		{
			// Takes "true"/"false" as well as the "1"/"0" the snapshot writes.
			bool parsed = false;
			if (!V_StringToValue<bool>(value, parsed))
			{
				return false;
			}
			opts->SetPreferenceBool(entry.key, parsed);
			return true;
		}
		case KZOptStorage::Int:
		{
			int64 parsed = 0;
			if (!V_StringToValue<int64>(value, parsed))
			{
				return false;
			}
			opts->SetPreferenceInt(entry.key, parsed);
			return true;
		}
		case KZOptStorage::Float:
		{
			float64 parsed = 0.0;
			if (!V_StringToValue<float64>(value, parsed))
			{
				return false;
			}
			opts->SetPreferenceFloat(entry.key, parsed);
			return true;
		}
		case KZOptStorage::Str:
			opts->SetPreferenceStr(entry.key, value);
			return true;
		case KZOptStorage::Vector:
		{
			Vector parsed;
			if (!V_StringToValue<Vector>(value, parsed))
			{
				return false;
			}
			opts->SetPreferenceVector(entry.key, parsed);
			return true;
		}
		default:
			return false;
	}
}
