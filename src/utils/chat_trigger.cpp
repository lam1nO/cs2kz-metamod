#include "chat_trigger.h"

#include <cstring>

namespace chattrigger
{
	static const char *const kBareDenied[] = {"nc", "b", "o", "hm"};

	static char LowerAscii(char c)
	{
		return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
	}

	static bool EqualsNoCase(const char *a, const char *b)
	{
		for (; *a && *b; a++, b++)
		{
			if (LowerAscii(*a) != LowerAscii(*b))
			{
				return false;
			}
		}
		return *a == *b;
	}

	bool IsBareDenied(const char *word)
	{
		for (const char *denied : kBareDenied)
		{
			if (EqualsNoCase(word, denied))
			{
				return true;
			}
		}
		return false;
	}

	bool Rewrite(const char *text, IsKnownFn isKnown, void *ctx, char *out, size_t outSize)
	{
		if (!text || !text[0] || text[0] == '!' || text[0] == '/')
		{
			return false;
		}
		if (strchr(text, '"'))
		{
			return false;
		}
		const Form form = text[0] == '.' ? FORM_DOT : FORM_BARE;
		const char *body = form == FORM_DOT ? text + 1 : text;

		char word[64];
		size_t len = strcspn(body, " \t");
		if (len == 0 || len >= sizeof(word))
		{
			return false;
		}
		memcpy(word, body, len);
		word[len] = '\0';

		if (form == FORM_BARE && IsBareDenied(word))
		{
			return false;
		}
		if (!isKnown(word, form, ctx))
		{
			return false;
		}

		// «!» + слово + остаток + '\0'
		const size_t restLen = strlen(body + len);
		if (1 + len + restLen + 1 > outSize)
		{
			return false;
		}
		out[0] = '!';
		for (size_t i = 0; i < len; i++)
		{
			// Только ASCII: CSSharp ищет css_<слово> как есть, а кириллицу (русская раскладка)
			// дальше перекодируют сами обработчики «!»-строк.
			out[1 + i] = LowerAscii(word[i]);
		}
		memcpy(out + 1 + len, body + len, restLen + 1);
		return true;
	}
} // namespace chattrigger
