#pragma once
#include <cstddef>
namespace KZ::hudfmt
{
	void FormatDelta(double seconds, bool detailed, char *out, size_t outLen);
	void FormatPos(float x, float y, float z, char *out, size_t outLen);
	void FormatAng(float pitch, float yaw, char *out, size_t outLen);
	void FormatCourseLine(const char *course, const char *modeShort, const char *const *styles, int styleCount, char *out, size_t outLen);
	// Строка CP/TP panorama-худа: «CP 3 · TP 2» (U+00B7), teleports < 0 — «CP 3 · AWR».
	void FormatCheckpointLine(int cpIndex, int teleports, char *out, size_t outLen);
	bool GridCellToPercent(const char *buttonId, int cols, int rows, int &xPct, int &yPct);
	// Позиция элемента в редакторе (проценты от центра) — в видимую область -50..50. Старое окно
	// позволяло ±100: сохранённое (-51,-51) уводит элемент за край экрана и из-под сетки.
	int ClampEditorPos(int pct);
	const char *NoTimePlaceholder(bool detailed);
	// Видимость дельты к PB/WR в таймере: нужен идущий таймер, загруженный путь `!lead`,
	// совпадение ключа (карта/курс/режим/стиль) и включённый режим сравнения (0 = Off).
	bool DeltaVisible(bool timerRunning, bool pathLoaded, bool keyMatches, int compareMode);
} // namespace KZ::hudfmt
