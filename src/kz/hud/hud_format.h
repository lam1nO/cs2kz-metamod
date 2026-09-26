#pragma once
#include <cstddef>
namespace KZ::hudfmt
{
	void FormatDelta(double seconds, bool detailed, char *out, size_t outLen);
	void FormatPos(float x, float y, float z, char *out, size_t outLen);
	void FormatAng(float pitch, float yaw, char *out, size_t outLen);
	void FormatCourseLine(const char *course, const char *modeShort, const char *const *styles, int styleCount, char *out, size_t outLen);
	bool GridCellToPercent(const char *buttonId, int cols, int rows, int &xPct, int &yPct);
	const char *NoTimePlaceholder(bool detailed);
	// Видимость дельты к PB/WR в таймере: нужен идущий таймер, загруженный путь `!lead`,
	// совпадение ключа (карта/курс/режим/стиль) и включённый режим сравнения (0 = Off).
	bool DeltaVisible(bool timerRunning, bool pathLoaded, bool keyMatches, int compareMode);
} // namespace KZ::hudfmt
