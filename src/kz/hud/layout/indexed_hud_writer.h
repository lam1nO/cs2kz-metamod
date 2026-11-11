#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <unordered_map>

// Индексы живут вне SDK-overlay; AddToTail может переместить элементы коллекции.
template<class Key>
struct IndexedHudLookup
{
	std::unordered_map<Key, int> indices;
	int count = -1;

	template<class Collection, class KeyOf>
	int Find(Collection &collection, const Key &key, KeyOf keyOf)
	{
		const int actual = collection.Count();
		if (count != actual)
		{
			indices.clear();
			for (int i = 0; i < actual; ++i)
			{
				indices.emplace(keyOf(collection.ElementUnchecked(i)), i);
			}
			count = actual;
		}
		auto found = indices.find(key);
		return found == indices.end() ? -1 : found->second;
	}

	void Appended(const Key &key, int index)
	{
		indices.emplace(key, index);
		count = index + 1;
	}
};

struct IndexedHudCache
{
	IndexedHudLookup<std::string> panels, classes, variables;
	IndexedHudLookup<uint32_t> classEntries, variableEntries;
	const void *owner = nullptr;
	const void *state = nullptr;

	void Reset()
	{
		*this = IndexedHudCache {};
	}
};

// Шаблон использует тот же интерфейс коллекций и уведомлений, что SDK; host-тест проверяет настоящий writer.
template<class Layout>
class IndexedHudWriter
{
	Layout &layout;
	IndexedHudCache &cache;
	std::function<void()> ownerChanged;

	template<class Collection>
	int Intern(Collection &&strings, IndexedHudLookup<std::string> &lookup, const char *value)
	{
		const std::string key = value ? value : "";
		int index = lookup.Find(strings, key, [](auto *entry) { return std::string(entry->Get()); });
		if (index >= 0)
		{
			return index;
		}
		const int count = strings.Count();
		if (count >= 1024)
		{
			return -1;
		}
		auto *added = strings.AddToTail();
		if (!added)
		{
			return -1;
		}
		*added = key.c_str();
		lookup.Appended(key, count);
		ownerChanged();
		return count;
	}

	static uint32_t Pair(int panel, int name)
	{
		return (uint32_t(panel) << 16) | uint32_t(name);
	}

	auto State()
	{
		auto *state = layout.GetGlobalLayoutState();
		if (cache.owner != &layout || cache.state != state)
		{
			cache.Reset();
			cache.owner = &layout;
			cache.state = state;
		}
		return state;
	}

public:
	IndexedHudWriter(Layout &layout, IndexedHudCache &cache, std::function<void()> changed)
		: layout(layout), cache(cache), ownerChanged(std::move(changed))
	{
	}

	template<class Status>
	bool SetHasClass(const char *panel, const char *name, Status status)
	{
		auto *state = State();
		if (!state)
		{
			return false;
		}
		const int p = Intern(layout.m_vecPanelIds(), cache.panels, panel);
		const int n = Intern(layout.m_vecClassNames(), cache.classes, name);
		if (p < 0 || n < 0)
		{
			return false;
		}
		auto &&entries = state->m_vecHasClasses();
		const auto key = Pair(p, n);
		const int index =
			cache.classEntries.Find(entries, key, [](auto *entry) { return Pair(*entry->m_nPanelIdIndex(), *entry->m_nClassNameIndex()); });
		auto *entry = index >= 0 ? entries.ElementUnchecked(index) : entries.AddToTail();
		if (!entry)
		{
			return false;
		}
		*entry->m_eClassStatus() = status;
		if (index >= 0)
		{
			state->MarkHasClassChanged(index);
		}
		else
		{
			*entry->m_nPanelIdIndex() = p;
			*entry->m_nClassNameIndex() = n;
			cache.classEntries.Appended(key, entries.Count() - 1);
			state->MarkFullChanged();
		}
		return true;
	}

	bool SetDialogVariableString(const char *panel, const char *name, const char *value)
	{
		auto *state = State();
		if (!state)
		{
			return false;
		}
		const int p = Intern(layout.m_vecPanelIds(), cache.panels, panel);
		const int n = Intern(layout.m_vecDialogVariableNames(), cache.variables, name);
		if (p < 0 || n < 0)
		{
			return false;
		}
		auto &&entries = state->m_vecDialogVariableStrings();
		const auto key = Pair(p, n);
		const int index =
			cache.variableEntries.Find(entries, key, [](auto *entry) { return Pair(*entry->m_nPanelIdIndex(), *entry->m_nDialogVariableIndex()); });
		if (index < 0 && !value)
		{
			return false;
		}
		auto *entry = index >= 0 ? entries.ElementUnchecked(index) : entries.AddToTail();
		if (!entry)
		{
			return false;
		}
		if (value)
		{
			*entry->m_sValue() = value;
		}
		*entry->m_bIsSet() = value != nullptr;
		if (index >= 0)
		{
			state->MarkDialogVarChanged(index);
		}
		else
		{
			*entry->m_nPanelIdIndex() = p;
			*entry->m_nDialogVariableIndex() = n;
			cache.variableEntries.Appended(key, entries.Count() - 1);
			state->MarkFullChanged();
		}
		return true;
	}
};
