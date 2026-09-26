// src/kz/hud/hud_format.cpp — чистые функции без зависимостей от SDK: гоняются host-тестом.
#include "hud_format.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace KZ::hudfmt
{
	void FormatDelta(double seconds, bool detailed, char *out, size_t outLen)
	{
		const char sign = seconds < 0 ? '-' : '+';
		double a = std::fabs(seconds);
		// Округление до показываемой точности ДО разбиения на минуты: иначе 59.9996 даст "0:60.000".
		const double q = detailed ? 1000.0 : 100.0;
		a = std::floor(a * q + 0.5) / q;
		const int mins = (int)(a / 60.0);
		const double secs = a - mins * 60.0;
		if (mins > 0)
		{
			snprintf(out, outLen, detailed ? "%c%d:%06.3f" : "%c%d:%05.2f", sign, mins, secs);
		}
		else
		{
			snprintf(out, outLen, detailed ? "%c%.3f" : "%c%.2f", sign, secs);
		}
	}

	void FormatPos(float x, float y, float z, char *out, size_t outLen)
	{
		snprintf(out, outLen, "%.2f %.2f %.2f", x, y, z);
	}

	void FormatAng(float pitch, float yaw, char *out, size_t outLen)
	{
		snprintf(out, outLen, "%.2f %.2f", pitch, yaw);
	}

	// snprintf возвращает длину, которую ХОТЕЛ записать, а не записанную: без зажима n после
	// обрезки уходил за буфер, и следующий out + n / outLen - n (size_t) писал мимо него.
	static size_t ClampWritten(size_t n, int written, size_t outLen)
	{
		if (written > 0)
		{
			n += (size_t)written;
		}
		return n < outLen ? n : outLen - 1;
	}

	void FormatCourseLine(const char *course, const char *modeShort, const char *const *styles, int styleCount, char *out, size_t outLen)
	{
		if (!out || outLen == 0)
		{
			return;
		}
		size_t n = 0;
		for (const char *p = course ? course : ""; *p && n + 1 < outLen; p++)
		{
			out[n++] = (char)toupper((unsigned char)*p);
		}
		out[n] = 0;
		n = ClampWritten(n, snprintf(out + n, outLen - n, " \xC2\xB7 %s", modeShort ? modeShort : ""), outLen);
		if (styleCount <= 0)
		{
			snprintf(out + n, outLen - n, " \xC2\xB7 NRM");
			return;
		}
		for (int i = 0; i < styleCount && n + 1 < outLen; i++)
		{
			n = ClampWritten(n, snprintf(out + n, outLen - n, " \xC2\xB7 %s", styles[i]), outLen);
		}
	}

	bool GridCellToPercent(const char *buttonId, int cols, int rows, int &xPct, int &yPct)
	{
		if (!buttonId || buttonId[0] != 'g' || !isdigit((unsigned char)buttonId[1]))
		{
			return false;
		}
		char *end = nullptr;
		long c = strtol(buttonId + 1, &end, 10);
		if (!end || *end != '_')
		{
			return false;
		}
		long r = strtol(end + 1, &end, 10);
		if (!end || *end != 0 || c < 0 || c >= cols || r < 0 || r >= rows)
		{
			return false;
		}
		xPct = (int)std::lround((c + 0.5) * 100.0 / cols);
		yPct = (int)std::lround((r + 0.5) * 100.0 / rows);
		return true;
	}

	const char *NoTimePlaceholder(bool detailed)
	{
		return detailed ? "--:--.---" : "--:--.--";
	}

	bool DeltaVisible(bool timerRunning, bool pathLoaded, bool keyMatches, int compareMode)
	{
		return timerRunning && pathLoaded && keyMatches && compareMode != 0;
	}
} // namespace KZ::hudfmt
