// tests/chat_trigger_test.cpp
#include "../src/utils/chat_trigger.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <strings.h>
using namespace chattrigger;

// Известны: maptop, tp, nc, r, ws (css_ws), rtv (только как ConCommand — для точки).
static bool Known(const char *word, Form form, void *)
{
	const char *always[] = {"maptop", "tp", "nc", "r", "ws", "kzt"};
	for (const char *w : always)
	{
		if (!strcasecmp(word, w))
		{
			return true;
		}
	}
	return form == FORM_DOT && !strcasecmp(word, "rtv");
}

static const char *R(const char *text)
{
	static char out[512];
	return Rewrite(text, Known, nullptr, out, sizeof(out)) ? out : nullptr;
}

int main()
{
	// Голое слово и точка — в «!», регистр имени команды опускается, аргументы как есть.
	assert(!strcmp(R("maptop"), "!maptop"));
	assert(!strcmp(R("MAPTOP"), "!maptop"));
	assert(!strcmp(R(".tp"), "!tp"));
	assert(!strcmp(R(".KZT"), "!kzt"));
	assert(!strcmp(R("maptop Kz_Kiwit NUB"), "!maptop Kz_Kiwit NUB"));
	assert(!strcmp(R(".ws"), "!ws"));
	// «!» и «/» не трогаем — они и так работают.
	assert(!R("!maptop"));
	assert(!R("/maptop"));
	// Обычный чат.
	assert(!R("привет"));
	assert(!R("gg wp"));
	assert(!R("..."));
	assert(!R(". tp"));
	assert(!R(""));
	assert(!R(" tp"));
	// Опасные голые слова — только с префиксом.
	assert(!R("nc"));
	assert(!R("NC"));
	assert(!strcmp(R(".nc"), "!nc"));
	assert(!strcmp(R("r"), "!r"));
	// Точка шире: голое rtv GG1 ловит сам, а «.rtv» надо довезти до его ConCommand.
	assert(!R("rtv"));
	assert(!strcmp(R(".rtv"), "!rtv"));
	// Кавычку в say "..." обратно не собрать.
	assert(!R("tp \"x\""));
	// Не влезает в буфер — не трогаем.
	{
		char small[4];
		assert(!Rewrite("maptop", Known, nullptr, small, sizeof(small)));
		char exact[8];
		assert(Rewrite("maptop", Known, nullptr, exact, sizeof(exact)) && !strcmp(exact, "!maptop"));
	}
	puts("chat_trigger_test: ok");
	return 0;
}
