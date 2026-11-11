#include "../src/kz/hud/layout/indexed_hud_writer.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

struct String
{
	std::string value;

	const char *Get() const
	{
		return value.c_str();
	}

	void operator=(const char *s)
	{
		value = s;
	}
};

struct Class
{
	unsigned short panel = 0, name = 0;
	int status = -1;

	auto m_nPanelIdIndex()
	{
		return &panel;
	}

	auto m_nClassNameIndex()
	{
		return &name;
	}

	auto m_eClassStatus()
	{
		return &status;
	}
};

struct Var
{
	unsigned short panel = 0, name = 0;
	String value;
	bool set = false;

	auto m_nPanelIdIndex()
	{
		return &panel;
	}

	auto m_nDialogVariableIndex()
	{
		return &name;
	}

	auto m_sValue()
	{
		return &value;
	}

	auto m_bIsSet()
	{
		return &set;
	}
};

static size_t visits = 0;

template<class T>
struct Collection
{
	std::vector<T> data;
	bool rejectAppend = false;

	int Count() const
	{
		return data.size();
	}

	T *ElementUnchecked(int i)
	{
		++visits;
		return &data[i];
	}

	T *AddToTail()
	{
		if (rejectAppend)
		{
			return nullptr;
		}
		data.emplace_back();
		return &data.back();
	}
};

struct State
{
	Collection<Class> classes;
	Collection<Var> vars;
	int full = 0, changedClass = -1, changedVar = -1;

	auto &m_vecHasClasses()
	{
		return classes;
	}

	auto &m_vecDialogVariableStrings()
	{
		return vars;
	}

	void MarkFullChanged()
	{
		++full;
	}

	void MarkHasClassChanged(int i)
	{
		changedClass = i;
	}

	void MarkDialogVarChanged(int i)
	{
		changedVar = i;
	}
};

struct Layout
{
	Collection<String> panels, classes, vars;
	State state;
	int dirty = 0;

	auto &m_vecPanelIds()
	{
		return panels;
	}

	auto &m_vecClassNames()
	{
		return classes;
	}

	auto &m_vecDialogVariableNames()
	{
		return vars;
	}

	State *GetGlobalLayoutState()
	{
		return &state;
	}
};

int main()
{
	Layout layout;
	IndexedHudCache cache;
	auto dirty = [&] { ++layout.dirty; };
	IndexedHudWriter<Layout> writer(layout, cache, dirty);
	assert(writer.SetHasClass("p", "c", 0));
	assert(layout.state.classes.data[0].status == 0 && layout.state.full == 1 && layout.dirty == 2);
	assert(writer.SetHasClass("p", "c", 1));
	assert(layout.state.changedClass == 0 && layout.state.full == 1);
	assert(writer.SetDialogVariableString("p", "v", ""));
	assert(layout.state.vars.data[0].set && layout.state.vars.data[0].value.value.empty());
	assert(writer.SetDialogVariableString("p", "v", nullptr));
	assert(!layout.state.vars.data[0].set && layout.state.changedVar == 0);
	assert(!writer.SetDialogVariableString("p", "missing", nullptr));
	assert(layout.state.vars.Count() == 1);
	assert(writer.SetDialogVariableString("p", "v", "retained"));
	assert(writer.SetDialogVariableString("p", "v", nullptr));
	assert(layout.state.vars.data[0].value.value == "retained");
	// Ошибка добавления не портит ожидаемый размер и допускает следующий успешный вызов.
	layout.state.vars.rejectAppend = true;
	assert(!writer.SetDialogVariableString("p", "retry", "value"));
	layout.state.vars.rejectAppend = false;
	assert(writer.SetDialogVariableString("p", "retry", "value"));
	assert(layout.state.vars.Count() == 2);
	// NULL в имени интернируется как пустая строка, как в старом SDK.
	assert(writer.SetHasClass(nullptr, nullptr, 0));
	assert(writer.SetHasClass("", "", 1));
	assert(layout.panels.data.back().value.empty());
	// Внешние добавления и дубли: выбираем первую пару/строку, как линейный путь SDK.
	*layout.panels.AddToTail() = "p";
	auto duplicate = layout.state.classes.AddToTail();
	duplicate->panel = 0;
	duplicate->name = 0;
	duplicate->status = -1;
	assert(writer.SetHasClass("p", "c", 0));
	assert(layout.state.classes.data[0].status == 0 && layout.state.classes.data.back().status == -1);
	auto external = layout.panels.AddToTail();
	*external = "external";
	assert(writer.SetDialogVariableString("external", "v", "value"));
	assert(layout.state.vars.data.back().panel == 3);
	// Пересоздание по тому же адресу: явный Reset обязателен даже при одинаковых размерах.
	layout = Layout {};
	cache.Reset();
	assert(writer.SetHasClass("new", "other", -1));
	assert(layout.panels.data[0].value == "new" && layout.state.classes.data[0].status == -1);
	auto run = [&](int count)
	{
		layout = Layout {};
		cache.Reset();
		visits = 0;
		for (int i = 0; i < count; ++i)
		{
			auto p = "gt" + std::to_string(i);
			assert(writer.SetHasClass(p.c_str(), "done", 0));
			assert(writer.SetDialogVariableString(p.c_str(), "pb", "1"));
		}
		for (int i = 0; i < count; ++i)
		{
			auto p = "gt" + std::to_string(i);
			assert(writer.SetHasClass(p.c_str(), "done", 1));
			assert(writer.SetDialogVariableString(p.c_str(), "pb", "2"));
		}
		for (const auto &v : layout.state.vars.data)
		{
			assert(v.set && v.value.value == "2");
		}
		return visits;
	};
	auto small = run(200), large = run(400);
	assert(large <= small * 2 + 10 && large <= 2000);
	std::cout << "indexed_hud_writer: visits 200=" << small << " 400=" << large << '\n';
	// Лимит интернов, поиск существующих строк после достижения лимита.
	layout = Layout {};
	cache.Reset();
	for (int i = 0; i < 1024; ++i)
	{
		assert(writer.SetHasClass(std::to_string(i).c_str(), "c", 0));
	}
	assert(!writer.SetHasClass("overflow", "c", 0));
	assert(writer.SetHasClass("0", "c", 1));
	assert(layout.panels.Count() == 1024);
	std::cout << "indexed_hud_writer: semantics, rebuild, reset, realloc, limit PASS\n";
}
