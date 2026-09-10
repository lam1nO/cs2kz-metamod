#include "../src/kz/replays/awr_cut.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace KZ::replaysystem::awr;

// Строит кадр: тик, чекпоинты, позиция x (y=z=0).
static Frame F(uint32_t tick, int32_t cpIndex, int32_t cpCount, int32_t tpCount, float x)
{
	Frame f {};
	f.serverTick = tick; f.cpIndex = cpIndex; f.cpCount = cpCount; f.tpCount = tpCount; f.origin[0] = x;
	return f;
}
static const double TI = 1.0 / 64.0;

// Шаг «клетки» старых фикстур, юниты. Намеренно много больше AWR_DEST_TOLERANCE (16):
// иначе соседние кадры попадали бы в допуск друг друга и тесты проверяли бы не логику
// сшивки, а ширину допуска. Работу САМОГО допуска проверяют отдельные тесты ниже
// (cp/undo «на бегу»), у которых шаг реалистичный — 6 u за тик.
static const float CELL = 64.0f;

// Кадр по номеру клетки (старые фикстуры писались в клетках, когда допуска ещё не было).
static Frame FC(uint32_t tick, int32_t cpIndex, int32_t cpCount, int32_t tpCount, float cell)
{
	return F(tick, cpIndex, cpCount, tpCount, cell * CELL);
}

// Заполнить pre.origin: у кадра i начало тика — это конец предыдущего (у первого = свой
// конец). Ровно так их пишет рекордер: post кадра i-1 и pre кадра i — один и тот же момент.
static void FillPre(std::vector<Frame> &v)
{
	for (size_t i = 0; i < v.size(); i++)
	{
		const Frame &src = i == 0 ? v[0] : v[i - 1];
		v[i].preOrigin[0] = src.origin[0];
		v[i].preOrigin[1] = src.origin[1];
		v[i].preOrigin[2] = src.origin[2];
	}
}

// Кадр с тремя координатами: нужен фикстуре по РЕАЛЬНЫМ числам живого файла.
static Frame F3(uint32_t tick, int32_t cpIndex, int32_t cpCount, int32_t tpCount, float x, float y, float z)
{
	Frame f {};
	f.serverTick = tick;
	f.cpIndex = cpIndex;
	f.cpCount = cpCount;
	f.tpCount = tpCount;
	f.origin[0] = x;
	f.origin[1] = y;
	f.origin[2] = z;
	return f;
}

// Кадр с двумя координатами: нужен тестам ветки pre, где «поздний проход рядом» обязан
// отличаться от точки чекпоинта по второй оси, иначе кандидатов в радиусе слишком много.
static Frame F2(uint32_t tick, int32_t cpIndex, int32_t cpCount, int32_t tpCount, float x, float y)
{
	Frame f = F(tick, cpIndex, cpCount, tpCount, x);
	f.origin[1] = y;
	return f;
}

// Задать pre кадра ВРУЧНУЮ (после FillPre): рекордер пишет pre = post предыдущего кадра, но
// позиция чекпоинта снимается в середине тика, и именно её несовпадение с обоими концами
// проверяют тесты ниже.
static void SetPre(Frame &f, float x, float y)
{
	f.preOrigin[0] = x;
	f.preOrigin[1] = y;
	f.preOrigin[2] = 0.0f;
}

static void test_no_teleports()
{
	std::vector<Frame> v; for (uint32_t i = 0; i < 100; i++) v.push_back(FC(i, 0, 0, 0, (float)i));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 5000, TI, 0, v.size() - 1);
	assert(r.ok && r.dead.empty() && r.teleports == 0 && r.awrMs == 5000);
}

// cp на кадре 10 (x=10), уходим до x=30 на кадре 30, ТП на кадре 31 (x=10), дальше x растёт.
static void test_single_tp()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 30; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(31, 0, 1, 1, 10.0f));
	for (uint32_t i = 32; i < 60; i++) v.push_back(FC(i, 0, 1, 1, 10.0f + (i - 31)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 60 * 1000 / 64, TI, 0, v.size() - 1);
	assert(r.ok && r.teleports == 1 && r.dead.size() == 1);
	assert(r.dead[0].from == 11 && r.dead[0].to == 31);
	// мёртвое время = serverTick[31]-serverTick[10] = 21 тик
	assert(r.awrMs == (uint64_t)(60 * 1000 / 64) - (uint64_t)(21 * TI * 1000.0 + 0.5));
}

// Повторные ТП на один cp: dead = от постановки до ПОСЛЕДНЕГО ТП, стояние после первого ТП тоже мёртвое.
static void test_repeat_tp_same_cp()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 20; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(21, 0, 1, 1, 10.0f));                      // ТП №1
	for (uint32_t i = 22; i <= 25; i++) v.push_back(FC(i, 0, 1, 1, 10.0f)); // стоим
	for (uint32_t i = 26; i <= 35; i++) v.push_back(FC(i, 0, 1, 1, 10.0f + (i - 25)));
	v.push_back(FC(36, 0, 1, 2, 10.0f));                      // ТП №2
	for (uint32_t i = 37; i < 50; i++) v.push_back(FC(i, 0, 1, 2, 10.0f + (i - 36)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.teleports == 2 && r.dead.size() == 1 && r.dead[0].from == 11 && r.dead[0].to == 36);
}

// cp1 на 10 (x=10), cp2 на 20 (x=20), ТП на cp1 на 25, потом nextcp → cp2 на 30: участок 11..20 ЖИВОЙ.
static void test_prevcp_nextcp_keeps_middle()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 24; i++) v.push_back(FC(i, i >= 20 ? 1 : 0, i >= 20 ? 2 : (i >= 10 ? 1 : 0), 0, (float)i));
	v.push_back(FC(25, 0, 2, 1, 10.0f));                       // tp на cp1 (index 0)
	for (uint32_t i = 26; i <= 29; i++) v.push_back(FC(i, 0, 2, 1, 10.0f));
	v.push_back(FC(30, 1, 2, 2, 20.0f));                       // nextcp → cp2 (index 1)
	for (uint32_t i = 31; i < 40; i++) v.push_back(FC(i, 1, 2, 2, 20.0f + (i - 30)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.dead.size() == 1);
	assert(r.dead[0].from == 21 && r.dead[0].to == 30);       // 11..20 остались живыми
	auto live = LiveIntervals(r.dead, v.size());
	assert(live.size() == 2 && live[0].from == 0 && live[0].to == 20 && live[1].from == 31 && live[1].to == 39);
}

// undo: ТП на cp (кадр 21, x=10), undo на кадре 23 возвращает в x=20 (позиция кадра 20). Живое: 0..20, 24..
static void test_undo()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 20; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(21, 0, 1, 1, 10.0f));
	v.push_back(FC(22, 0, 1, 1, 10.0f));
	v.push_back(FC(23, 0, 1, 2, 20.0f));                       // undo: tpCount++, позиция = кадр 20
	for (uint32_t i = 24; i < 40; i++) v.push_back(FC(i, 0, 1, 2, 20.0f + (i - 23)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.dead.size() == 1 && r.dead[0].from == 21 && r.dead[0].to == 23);
}

// Пауза на КАДРАХ 15..18 внутри мёртвого интервала 11..31: эти кадры записаны (игрок
// заморожен, физика идёт), но таймер их не считал — из мёртвого времени они вычитаются.
static void test_pause_overlap_not_double_counted()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 30; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(31, 0, 1, 1, 10.0f));
	for (uint32_t i = 32; i < 40; i++) v.push_back(FC(i, 0, 1, 1, 10.0f + (i - 31)));
	Interval pause {15, 18};
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), &pause, 1, 10000, TI, 0, v.size() - 1);
	// мёртвых кадров 21 (11..31) минус 4 паузных (15..18) = 17
	assert(r.ok && r.awrMs == 10000 - (uint64_t)(17 * TI * 1000.0 + 0.5));
}

// КОРЕНЬ отказов замера cyb.183: `!prac` не пишет тиков вовсе, а таймер на это время стоит
// — в файле остаётся РАЗРЫВ serverTick между соседними кадрами. Мёртвое время измеряется
// числом ЗАПИСАННЫХ КАДРОВ, поэтому неписанный промежуток не даёт вклада ни в time_ms, ни в
// мёртвое. По длине интервала в серверных тиках вклад был бы 20015 тиков вместо 15 кадров, и
// мёртвое выходило БОЛЬШЕ времени рана (живой пример: time_ms=706539, dead_ms=720094).
static void test_dead_counts_frames_not_server_ticks()
{
	const uint32_t GAP = 20000; // ~5 минут prac между кадрами 20 и 21
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 20; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	for (uint32_t i = 21; i <= 24; i++) v.push_back(FC(i + GAP, 0, 1, 0, (float)i));
	v.push_back(FC(25 + GAP, 0, 1, 1, 10.0f)); // ТП на чекпоинт кадра 10
	for (uint32_t i = 26; i <= 30; i++) v.push_back(FC(i + GAP, 0, 1, 1, 10.0f + (i - 25)));
	FillPre(v);
	// Пауз НЕТ вовсе: в prac кадров не писали, и паре PAUSE/RESUME в кадрах соответствовать
	// нечему — вклад всё равно обязан быть равен числу кадров выреза.
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.teleports == 1 && r.dead.size() == 1);
	assert(r.dead[0].from == 11 && r.dead[0].to == 25);
	assert(r.awrMs == 10000 - (uint64_t)(15 * TI * 1000.0 + 0.5));
	// Разрыв виден в метрике, но отказом больше не является.
	assert(r.maxRecordGapTicks == GAP + 1 && r.maxRecordGapFrame == 21);
}

// Тридцать секунд prac внутри выреза (медиана живого замера) — законная история, проходит.
static void test_prac_sized_gap_allowed()
{
	const uint32_t GAP = 64 * 31; // ~31 с
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 20; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	for (uint32_t i = 21; i <= 24; i++) v.push_back(FC(i + GAP, 0, 1, 0, (float)i));
	v.push_back(FC(25 + GAP, 0, 1, 1, 10.0f));
	for (uint32_t i = 26; i <= 30; i++) v.push_back(FC(i + GAP, 0, 1, 1, 10.0f + (i - 25)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.maxRecordGapTicks == GAP + 1);
	assert(r.awrMs == 10000 - (uint64_t)(15 * TI * 1000.0 + 0.5));
}

// Разрыв длиннее часа игрового времени — уже не пауза, а битый/склеенный файл: отказ.
// Порог существует ТОЛЬКО для этого случая.
static void test_absurd_record_gap_refused()
{
	const uint32_t GAP = 64 * 60 * 60 + 100; // час с лишним
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 20; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	for (uint32_t i = 21; i <= 24; i++) v.push_back(FC(i + GAP, 0, 1, 0, (float)i));
	v.push_back(FC(25 + GAP, 0, 1, 1, 10.0f));
	for (uint32_t i = 26; i <= 30; i++) v.push_back(FC(i + GAP, 0, 1, 1, 10.0f + (i - 25)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(!r.ok && std::strcmp(r.reason, "awr_record_gap") == 0);
	assert(std::strstr(r.detail, "gap_s=") && std::strstr(r.detail, "dead_frames="));
}

// Флаг «метрика разрывов посчитана»: на РАННЕМ отказе её печатать нельзя (ноль читался бы
// как «разрыва нет»), а на успехе без телепортов ноль правдив.
static void test_max_gap_measured_flag()
{
	// (1) окна нет — до подсчёта не дошли.
	std::vector<Frame> plain;
	for (uint32_t i = 0; i < 20; i++) plain.push_back(FC(i, 0, 0, 0, (float)i));
	FillPre(plain);
	CutResult noWindow = ComputeAwrCut(plain.data(), plain.size(), nullptr, 0, 10000, TI, 0, 0);
	assert(!noWindow.ok && !noWindow.maxRecordGapMeasured && noWindow.maxRecordGapTicks == 0);

	// (2) счётчик ТП прыгнул на 2 — тоже ранний отказ.
	std::vector<Frame> bad;
	for (uint32_t i = 0; i <= 20; i++) bad.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	bad.push_back(FC(21, 0, 1, 2, 10.0f));
	for (uint32_t i = 22; i < 30; i++) bad.push_back(FC(i, 0, 1, 2, 10.0f + (i - 21)));
	FillPre(bad);
	CutResult mismatch = ComputeAwrCut(bad.data(), bad.size(), nullptr, 0, 10000, TI, 0, bad.size() - 1);
	assert(!mismatch.ok && !mismatch.maxRecordGapMeasured);

	// (3) успех без телепортов: вырезов нет, значит и разрывов внутри них — ноль ПРАВДИВ.
	CutResult ok = ComputeAwrCut(plain.data(), plain.size(), nullptr, 0, 5000, TI, 0, plain.size() - 1);
	assert(ok.ok && ok.maxRecordGapMeasured && ok.maxRecordGapTicks == 0);
}

// Пересекающиеся/неупорядоченные паузы нормализуются: пересечение не вычитается дважды.
static void test_overlapping_pauses_not_double_subtracted()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 30; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(31, 0, 1, 1, 10.0f));
	for (uint32_t i = 32; i < 40; i++) v.push_back(FC(i, 0, 1, 1, 10.0f + (i - 31)));
	FillPre(v);
	// Две пересекающиеся паузы (кадры 14..20 и 18..24, в обратном порядке) = одна 14..24,
	// то есть 11 паузных кадров; мёртвых кадров 21 (11..31) → 10.
	const Interval pauses[2] = {{18, 24}, {14, 20}};
	CutResult r = ComputeAwrCut(v.data(), v.size(), pauses, 2, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.awrMs == 10000 - (uint64_t)(10 * TI * 1000.0 + 0.5));
	// Без нормализации вычлось бы 7+7=14 кадров вместо 11 → мёртвое 7, awrMs больше.
}

// Фикстура формы живого дефекта: N прибытий подряд, каждое сшивается к предыдущему, стояний
// нет. Проверяет, что цепочка вырезов схлопывается в ОДИН интервал и что мёртвое время
// считается один раз (а не по 3 тика на каждое прибытие).
static std::vector<Frame> BuildChainFixture(uint32_t arrivals)
{
	std::vector<Frame> v;
	v.push_back(F(0, -1, 0, 0, 0.0f));
	v.push_back(F(1, -1, 1, 0, 0.0f)); // cp поставлен стоя; cpIndex = -1 → путь (б)
	for (uint32_t k = 0; k < arrivals; k++)
	{
		v.push_back(F(2 + 3 * k, -1, 1, (int32_t)k, 40.0f));
		v.push_back(F(3 + 3 * k, -1, 1, (int32_t)k, 80.0f));
		v.push_back(F(4 + 3 * k, -1, 1, (int32_t)k + 1, 0.0f)); // прибытие
	}
	const uint32_t last = 4 + 3 * (arrivals - 1);
	v.push_back(F(last + 1, -1, 1, (int32_t)arrivals, 40.0f));
	v.push_back(F(last + 2, -1, 1, (int32_t)arrivals, 80.0f));
	FillPre(v);
	return v;
}

static void test_many_arrivals_chain_no_double_count()
{
	std::vector<Frame> v = BuildChainFixture(200);
	// Якорь первого прибытия — кадр ПОСТАНОВКИ чекпоинта (1), а не кадр 0: правило режет
	// «от постановки чекпоинта до прибытия», поэтому стояние ДО постановки остаётся живым.
	const uint64_t deadTicks = 601 - 1;
	const uint64_t deadMs = (uint64_t)((double)deadTicks * TI * 1000.0 + 0.5);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 20000, TI, 0, (uint32_t)v.size() - 1);
	assert(r.ok && r.teleports == 200);
	assert(r.dead.size() == 1 && r.dead[0].from == 2 && r.dead[0].to == 601);
	assert(r.awrMs == 20000 - deadMs);
}

// Неправдоподобно малый awrMs — отказ, а не строка с нулём: она выиграла бы минимум по
// (карта, курс, режим), и игрок увидел бы пустой прыжок в финиш.
static void test_awr_implausible_guard()
{
	std::vector<Frame> v = BuildChainFixture(200);
	const uint64_t deadMs = (uint64_t)((double)(601 - 1) * TI * 1000.0 + 0.5); // 9375
	// (1) мёртвого времени насчитали больше, чем длился ран → кламп в ноль запрещён.
	CutResult zero = ComputeAwrCut(v.data(), v.size(), nullptr, 0, deadMs - 375, TI, 0, (uint32_t)v.size() - 1);
	assert(!zero.ok && std::strcmp(zero.reason, "awr_implausible") == 0);
	assert(std::strstr(zero.detail, "dead_ms=") && std::strstr(zero.detail, "time_ms=") && std::strstr(zero.detail, "dead_n=1"));
	// (2) живого меньше 1 % времени рана (порог — сеть, не основной триггер) → тот же отказ.
	CutResult tiny = ComputeAwrCut(v.data(), v.size(), nullptr, 0, deadMs + 90, TI, 0, (uint32_t)v.size() - 1);
	assert(!tiny.ok && std::strcmp(tiny.reason, "awr_implausible") == 0);
	// (3) над порогом — проходит. 5 % прежнего порога легитимный гринд бы не прошёл.
	CutResult okRes = ComputeAwrCut(v.data(), v.size(), nullptr, 0, deadMs + 100, TI, 0, (uint32_t)v.size() - 1);
	assert(okRes.ok && okRes.awrMs == 100);
	CutResult grind = ComputeAwrCut(v.data(), v.size(), nullptr, 0, deadMs + 500, TI, 0, (uint32_t)v.size() - 1);
	assert(grind.ok && grind.awrMs == 500);
}

// Позиция прибытия не совпадает ни с одним прежним кадром → отказ, не ложная сшивка.
static void test_dest_not_found()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 20; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(21, 0, 1, 1, 999.0f));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(!r.ok && std::strcmp(r.reason, "dest_not_found") == 0);
}

// Хвост записи: RunRecorder пишет ~4 с ПОСЛЕ финиша, и «!r» в этом окне даёт прибытие ТП
// в хвосте. Без окна рана DestFrame нашёл бы кадр стояния на старте и объявил мёртвым весь
// ран (awrMs≈0 — и такая строка выиграла бы минимум у api).
static void test_tail_teleport_after_run_end()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 30; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i)); // ран, финиш на кадре 30
	v.push_back(FC(31, 0, 1, 1, 10.0f));                                                    // хвост: !r на чекпоинт
	for (uint32_t i = 32; i < 40; i++) v.push_back(FC(i, 0, 1, 1, 10.0f));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, 30);
	assert(r.ok && r.dead.empty() && r.teleports == 0 && r.awrMs == 10000);
}

// Предзапись: RunRecorder пишет 5 с ДО старта, там могут быть свои чекпоинты и телепорты.
// Учитываться должна только петля внутри окна, а сшивка — не уходить в предстартовые кадры.
static void test_prerecord_teleport_before_run_start()
{
	std::vector<Frame> v;
	// Предзапись 0..9: cp на кадре 2 и ТП на кадре 5 (позиция кадра 2).
	for (uint32_t i = 0; i <= 4; i++) v.push_back(FC(i, 0, i >= 2 ? 1 : 0, 0, (float)i));
	v.push_back(FC(5, 0, 1, 1, 2.0f));
	for (uint32_t i = 6; i <= 9; i++) v.push_back(FC(i, 0, 1, 1, 2.0f + (i - 5)));
	// Старт рана на кадре 10 — счётчики сброшены; cp на 20, ТП на 30.
	for (uint32_t i = 10; i <= 29; i++) v.push_back(FC(i, 0, i >= 20 ? 1 : 0, 0, (float)i));
	v.push_back(FC(30, 0, 1, 1, 20.0f));
	for (uint32_t i = 31; i <= 40; i++) v.push_back(FC(i, 0, 1, 1, 20.0f + (i - 30)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 10, 40);
	assert(r.ok && r.teleports == 1 && r.dead.size() == 1);
	assert(r.dead[0].from == 21 && r.dead[0].to == 30);
}

// Счётчик прыгнул на 2 за кадр: одну петлю по кадрам не восстановить — отказ, не сшивка.
static void test_counter_mismatch()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 20; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(21, 0, 1, 2, 10.0f)); // tpCount 0 -> 2 одним кадром
	for (uint32_t i = 22; i < 30; i++) v.push_back(FC(i, 0, 1, 2, 10.0f + (i - 21)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(!r.ok && std::strcmp(r.reason, "counter_mismatch") == 0);
}

// Окна нет (в реплее не нашлось TIMER_START/TIMER_END) — отказ, а не разрез «по всему файлу».
static void test_no_run_window()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i < 20; i++) v.push_back(FC(i, 0, 0, 0, (float)i));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, 0);
	assert(!r.ok && std::strcmp(r.reason, "no_run_window") == 0);
	CutResult r2 = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size());
	assert(!r2.ok && std::strcmp(r2.reason, "no_run_window") == 0);
}

// --- Допуск сопоставления: позиция чекпоинта/undo снята в СЕРЕДИНЕ тика ------------------
// Шаг кадра здесь реалистичный (6 u за тик ≈ 384 u/s), позиции — в юнитах, без клеток.

// cp поставлен НА БЕГУ: записанная позиция чекпоинта лежит между pre и post своего кадра,
// побитового совпадения с кадром нет вообще (ровно это убило первый живой прогон бэкфилла).
static void test_cp_set_while_running()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 9; i++) v.push_back(F(i, 0, 0, 0, 6.0f * i));     // 0..54
	v.push_back(F(10, 0, 1, 0, 60.0f));                                          // !cp в середине тика: 57
	for (uint32_t i = 11; i <= 40; i++) v.push_back(F(i, 0, 1, 0, 60.0f + 6.0f * (i - 10)));
	v.push_back(F(41, 0, 1, 1, 57.0f));                                          // прибытие ТП = позиция cp
	for (uint32_t i = 42; i < 60; i++) v.push_back(F(i, 0, 1, 1, 57.0f + 6.0f * (i - 41)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	// 57 в 3 юнитах от post кадра 10 (60) — сшивка по прямой ссылке на чекпоинт.
	assert(r.ok && r.teleports == 1 && r.dead.size() == 1);
	assert(r.dead[0].from == 11 && r.dead[0].to == 41);
}

// Тот же случай, но позиция cp ближе к НАЧАЛУ своего кадра (игрок в этот тик разогнался):
// совпало с pre кадра 10 → последним живым обязан стать кадр 9, иначе в живой путь попал бы
// кадр, который уже уводит игрока прочь от чекпоинта.
static void test_cp_matches_pre_side()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 9; i++) v.push_back(F(i, 0, 0, 0, 6.0f * i));      // ..54
	v.push_back(F(10, 0, 1, 0, 90.0f));                                           // рывок 54 -> 90, cp снят в 56
	for (uint32_t i = 11; i <= 30; i++) v.push_back(F(i, 0, 1, 0, 90.0f + 6.0f * (i - 10)));
	v.push_back(F(31, 0, 1, 1, 56.0f));
	for (uint32_t i = 32; i < 40; i++) v.push_back(F(i, 0, 1, 1, 56.0f + 6.0f * (i - 31)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.dead.size() == 1);
	assert(r.dead[0].from == 10 && r.dead[0].to == 31); // D = 10 - 1 = 9
}

// !undo: позиция возврата снята в момент команды !tp, то есть между pre и post кадра
// ПРИБЫТИЯ предыдущего телепорта. Сшивка обязана попасть на кадр перед тем прибытием.
static void test_undo_mid_tick()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 9; i++) v.push_back(F(i, 0, 0, 0, 6.0f * i));      // ..54
	v.push_back(F(10, 0, 1, 0, 60.0f));                                           // cp ровно в 60
	for (uint32_t i = 11; i <= 20; i++) v.push_back(F(i, 0, 1, 0, 60.0f + 6.0f * (i - 10))); // ..120
	v.push_back(F(21, 0, 1, 1, 60.0f));                                           // ТП №1 на cp
	// Уходим НАЗАД (в минус), чтобы рядом с точкой undo не оказалось других кадров.
	for (uint32_t i = 22; i <= 30; i++) v.push_back(F(i, 0, 1, 1, 60.0f - 6.0f * (i - 21)));
	v.push_back(F(31, 0, 1, 2, 121.0f));                                          // undo: позиция за миг до ТП №1
	for (uint32_t i = 32; i < 45; i++) v.push_back(F(i, 0, 1, 2, 121.0f + 6.0f * (i - 31)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.teleports == 2 && r.dead.size() == 1);
	// Вход в участок — pre кадра 21 (120, в 1 юните от 121), но якорь откатывается к НАЧАЛУ
	// пребывания в допуске: кадры 20 (120), 19 (114), 18 (108) — все в 16 u от 121, кадр 17
	// (102) уже нет. Значит последний живой — 18. Это заявленная цена позиционного якоря:
	// последние кадры подхода уходят в вырез (см. комментарий в DestFrame).
	assert(r.dead[0].from == 19 && r.dead[0].to == 31);
}

// Прибытие дальше допуска от всего маршрута — отказ, и в detail есть чем разбираться.
static void test_dest_not_found_detail()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 20; i++) v.push_back(F(i, 0, i >= 10 ? 1 : 0, 0, 6.0f * i));
	v.push_back(F(21, 0, 1, 1, 9999.0f));
	for (uint32_t i = 22; i < 30; i++) v.push_back(F(i, 0, 1, 1, 9999.0f));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(!r.ok && std::strcmp(r.reason, "dest_not_found") == 0);
	assert(r.detail[0] != '\0');
	assert(std::strstr(r.detail, "t=21") && std::strstr(r.detail, "cp_frame=10") && std::strstr(r.detail, "best_frame="));
}

// У counter_mismatch detail тоже обязан быть заполнен: без него живой прогон снова
// потребовал бы отдельного цикла сборка → канарейка.
static void test_counter_mismatch_detail()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 20; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(21, 0, 1, 2, 10.0f));
	for (uint32_t i = 22; i < 30; i++) v.push_back(FC(i, 0, 1, 2, 10.0f + (i - 21)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(!r.ok && std::strcmp(r.reason, "counter_mismatch") == 0);
	assert(std::strstr(r.detail, "arrivals=1") && std::strstr(r.detail, "expected=2"));
}

// Ветка pre, отличимая от скана по post. Сценарий: cp снят на рывке — записанная позиция
// совпадает ТОЛЬКО с pre кадра постановки (post того же кадра уже в 34 u), а позже маршрут
// проходит в 15 u от той же точки. Без ветки pre скан (б) зачёл бы живым почти всю петлю
// (dead начался бы у позднего прохода), с ней — сшивка идёт к кадру постановки.
// Тест обязан падать при: удалении ветки pre, замене j-1 на j, TOL=0.
static void test_pre_branch_beats_later_pass()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 9; i++) v.push_back(F2(i, 0, 0, 0, 6.0f * i, 0.0f));      // ..54
	v.push_back(F2(10, 0, 1, 0, 90.0f, 0.0f));                                          // рывок; cp снят в (56, 0)
	for (uint32_t i = 11; i <= 39; i++) v.push_back(F2(i, 0, 1, 0, 90.0f, 6.0f * (i - 10)));
	v.push_back(F2(40, 0, 1, 0, 56.0f, 15.0f));                                         // поздний проход в 15 u
	for (uint32_t i = 41; i <= 50; i++) v.push_back(F2(i, 0, 1, 0, 56.0f, 15.0f + 20.0f * (i - 40)));
	v.push_back(F2(51, 0, 1, 1, 56.0f, 0.0f));                                          // прибытие = позиция cp
	for (uint32_t i = 52; i < 60; i++) v.push_back(F2(i, 0, 1, 1, 56.0f + 6.0f * (i - 51), 0.0f));
	FillPre(v);
	SetPre(v[10], 55.0f, 0.0f); // начало тика рывка: игрок ещё в 1 u от точки чекпоинта
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.teleports == 1 && r.dead.size() == 1);
	// post кадра 10 — в 34 u (мимо), pre — в 1 u: последний живой кадр 9, мёртвое 10..51.
	// Без ветки pre скан нашёл бы кадр 40 и вернул dead = {41, 51}.
	assert(r.dead[0].from == 10 && r.dead[0].to == 51);
}

// Клампа на нижней границе окна: совпадение по pre кадра runStart не имеет права дать
// D = runStart - 1 — это кадр ПРЕДЗАПИСИ, вне окна, и стартовый кадр рана оказался бы мёртвым.
// Тест обязан падать при мутации `return (int64_t)j - 1` без клампы.
static void test_pre_match_at_run_start_clamped()
{
	std::vector<Frame> v;
	// Предзапись 0..9 — далеко в стороне, чтобы в скан не попала.
	for (uint32_t i = 0; i <= 9; i++) v.push_back(F2(i, 0, 0, 0, -500.0f + 6.0f * i, 0.0f));
	v.push_back(F2(10, 0, 0, 0, 140.0f, 0.0f));                                         // runStart, рывок из (100, 0)
	for (uint32_t i = 11; i <= 30; i++) v.push_back(F2(i, 0, 0, 0, 140.0f, 6.0f * (i - 10)));
	v.push_back(F2(31, 0, 0, 1, 100.0f, 0.0f));                                         // прибытие = начало тика 10
	for (uint32_t i = 32; i < 40; i++) v.push_back(F2(i, 0, 0, 1, 100.0f - 6.0f * (i - 31), 0.0f));
	FillPre(v);
	SetPre(v[10], 100.0f, 0.0f);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 10, v.size() - 1);
	assert(r.ok && r.teleports == 1 && r.dead.size() == 1);
	// D зажат в runStart = 10, поэтому мёртвое начинается с 11; без клампы было бы 10.
	assert(r.dead[0].from == 11 && r.dead[0].to == 31);
}

// Три ТП на ОДИН чекпоинт со стоянием после каждого прибытия, cpIndex невалиден (-1) —
// путь (а) выключен, работает запасной скан. Живое наблюдение на канарейке: без сшивки к
// прибытию вырезался только забег между двумя ТП, а само прибытие и стояние после него
// оставались живыми — бот «дёргался» у чекпоинта, а секунды стояния оставались в awrMs.
// Тест обязан падать, если убрать якорь по кадру ПОСТАНОВКИ, откат участка пребывания ИЛИ
// слияние смежных вырезов: схлопывание держится на этой связке, а точка назначения у всех
// трёх прибытий одна, поэтому сшивка не рвётся.
static void test_repeat_tp_collapses_to_one_dead()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 9; i++) v.push_back(F(i, -1, 0, 0, 6.0f * i));               // подход, ..54
	v.push_back(F(10, -1, 1, 0, 60.0f));                                                    // !cp и сразу побежал
	for (uint32_t i = 11; i <= 30; i++) v.push_back(F(i, -1, 1, 0, 60.0f + 6.0f * (i - 10)));
	v.push_back(F(31, -1, 1, 1, 60.0f));                                                    // ТП №1
	for (uint32_t i = 32; i <= 35; i++) v.push_back(F(i, -1, 1, 1, 60.0f));                 // стоит
	for (uint32_t i = 36; i <= 50; i++) v.push_back(F(i, -1, 1, 1, 60.0f + 6.0f * (i - 35)));
	v.push_back(F(51, -1, 1, 2, 60.0f));                                                    // ТП №2
	for (uint32_t i = 52; i <= 55; i++) v.push_back(F(i, -1, 1, 2, 60.0f));                 // стоит
	for (uint32_t i = 56; i <= 70; i++) v.push_back(F(i, -1, 1, 2, 60.0f + 6.0f * (i - 55)));
	v.push_back(F(71, -1, 1, 3, 60.0f));                                                    // ТП №3
	for (uint32_t i = 72; i < 86; i++) v.push_back(F(i, -1, 1, 3, 60.0f + 6.0f * (i - 71)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.teleports == 3);
	// Все три попытки — ОДИН вырез до последнего прибытия (точка назначения у всех одна,
	// якорь очередного прибытия упирается в предыдущее, смежные интервалы сливаются).
	// from == 11: путь (б) берёт якорем кадр ПОСТАНОВКИ чекпоинта (10) внутри участка
	// пребывания. Раньше здесь было 9 — якорь откатывался к началу пребывания в допуске и
	// съедал два последних кадра плавного подхода; теперь оба пути дают один ответ (ср.
	// test_cp_index_and_scan_agree), а подход остаётся живым.
	assert(r.dead.size() == 1 && r.dead[0].from == 11 && r.dead[0].to == 71);
}

// Стояние на чекпоинте ПОСЛЕ его постановки и перед первой попыткой вырезается: правило
// режет всё от постановки чекпоинта до телепорта на него, и стояние на месте постановки в
// этот промежуток входит. Здесь работает путь (б) (cpIndex = -1), и он берёт якорем кадр
// ПОСТАНОВКИ внутри участка пребывания, поэтому подход к чекпоинту и сама постановка
// остаются ЖИВЫМИ — прежде якорь уходил к началу пребывания и съедал два кадра подхода.
static void test_standing_before_first_tp_is_cut()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 9; i++) v.push_back(F(i, -1, 0, 0, 6.0f * i));               // подход, ..54
	v.push_back(F(10, -1, 1, 0, 60.0f));                                                    // !cp
	for (uint32_t i = 11; i <= 20; i++) v.push_back(F(i, -1, 1, 0, 60.0f));                 // стоит ДО попытки
	for (uint32_t i = 21; i <= 40; i++) v.push_back(F(i, -1, 1, 0, 60.0f + 6.0f * (i - 20)));
	v.push_back(F(41, -1, 1, 1, 60.0f));                                                    // ТП №1
	for (uint32_t i = 42; i < 56; i++) v.push_back(F(i, -1, 1, 1, 60.0f + 6.0f * (i - 41)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.teleports == 1 && r.dead.size() == 1);
	// from == 11: вырез идёт от кадра ПОСТАНОВКИ (10), сама постановка и подход к ней живые,
	// а стояние ПОСЛЕ постановки (11..20) — мёртвое, это и проверяет тест. Раньше было 9:
	// якорь откатывался к началу пребывания в допуске и съедал два кадра подхода.
	assert(r.dead[0].from == 11 && r.dead[0].to == 41);
	auto live = LiveIntervals(r.dead, v.size());
	assert(live.size() == 2 && live[0].from == 0 && live[0].to == 10);
}

// Пути (а) и (б) обязаны давать ОДИН И ТОТ ЖЕ вырез и одно awrMs: иначе один и тот же ран
// считался бы по-разному в зависимости от того, нашёлся ли индекс чекпоинта. Подход здесь
// намеренно резкий (кадр 9 в 40 u от точки — уже за допуском), чтобы изолировать РАВЕНСТВО
// правил; погрешность позиционного якоря на плавном подходе проверяет тест выше.
static void BuildSymmetricFixture(std::vector<Frame> &v, int32_t cpIndexOnArrival)
{
	v.clear();
	for (uint32_t i = 0; i <= 9; i++) v.push_back(F(i, cpIndexOnArrival, 0, 0, 40.0f - 20.0f * (9 - i))); // резкий подход, кадр 9 = 40
	v.push_back(F(10, cpIndexOnArrival, 1, 0, 60.0f));                                                     // !cp
	for (uint32_t i = 11; i <= 20; i++) v.push_back(F(i, cpIndexOnArrival, 1, 0, 60.0f));                  // стоит
	for (uint32_t i = 21; i <= 40; i++) v.push_back(F(i, cpIndexOnArrival, 1, 0, 60.0f + 6.0f * (i - 20)));
	v.push_back(F(41, cpIndexOnArrival, 1, 1, 60.0f));                                                     // ТП №1
	for (uint32_t i = 42; i <= 60; i++) v.push_back(F(i, cpIndexOnArrival, 1, 1, 60.0f + 6.0f * (i - 41)));
	v.push_back(F(61, cpIndexOnArrival, 1, 2, 60.0f));                                                     // ТП №2
	for (uint32_t i = 62; i < 76; i++) v.push_back(F(i, cpIndexOnArrival, 1, 2, 60.0f + 6.0f * (i - 61)));
	FillPre(v);
}

static void test_cp_index_and_scan_agree()
{
	std::vector<Frame> byIndex, byScan;
	BuildSymmetricFixture(byIndex, 0);  // валидный cpIndex → путь (а)
	BuildSymmetricFixture(byScan, -1);  // cpIndex невалиден → путь (б)
	CutResult a = ComputeAwrCut(byIndex.data(), byIndex.size(), nullptr, 0, 10000, TI, 0, byIndex.size() - 1);
	CutResult b = ComputeAwrCut(byScan.data(), byScan.size(), nullptr, 0, 10000, TI, 0, byScan.size() - 1);
	assert(a.ok && b.ok && a.teleports == 2 && b.teleports == 2);
	assert(a.dead.size() == 1 && b.dead.size() == 1);
	// Вырез — от кадра ПОСЛЕ постановки до последнего ТП; стояние 11..20 внутри.
	assert(a.dead[0].from == 11 && a.dead[0].to == 61);
	assert(b.dead[0].from == a.dead[0].from && b.dead[0].to == a.dead[0].to);
	assert(a.awrMs == b.awrMs && a.awrMs > 0);
}

// Трасса прибытий (её печатает kz_awr_debug): по записи на каждое прибытие окна, в порядке
// кадров, с методом сшивки и объявленным вырезом.
static void test_trace_reports_every_arrival()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 9; i++) v.push_back(F(i, -1, 0, 0, 6.0f * i));
	v.push_back(F(10, -1, 1, 0, 60.0f));
	for (uint32_t i = 11; i <= 30; i++) v.push_back(F(i, -1, 1, 0, 60.0f + 6.0f * (i - 10)));
	v.push_back(F(31, -1, 1, 1, 60.0f));
	for (uint32_t i = 32; i <= 35; i++) v.push_back(F(i, -1, 1, 1, 60.0f));
	for (uint32_t i = 36; i <= 50; i++) v.push_back(F(i, -1, 1, 1, 60.0f + 6.0f * (i - 35)));
	v.push_back(F(51, -1, 1, 2, 60.0f));
	for (uint32_t i = 52; i <= 55; i++) v.push_back(F(i, -1, 1, 2, 60.0f));
	for (uint32_t i = 56; i <= 70; i++) v.push_back(F(i, -1, 1, 2, 60.0f + 6.0f * (i - 55)));
	v.push_back(F(71, -1, 1, 3, 60.0f));
	for (uint32_t i = 72; i < 86; i++) v.push_back(F(i, -1, 1, 3, 60.0f + 6.0f * (i - 71)));
	FillPre(v);
	std::vector<ArrivalTrace> trace;
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1, &trace);
	assert(r.ok && trace.size() == 3);
	assert(trace[0].frame == 31 && trace[1].frame == 51 && trace[2].frame == 71);
	// cpIndex невалиден — сшивка только сканом; каждое следующее прибытие сшито к предыдущему.
	assert(trace[0].method == 'b' && trace[1].method == 'b' && trace[2].method == 'b');
	// dest первого прибытия — кадр ПОСТАНОВКИ (10), а не начало пребывания в допуске (8):
	// правило режет от постановки. Второе и третье сшиты к предыдущему прибытию — точка
	// назначения та же, поэтому цепочка не рвётся.
	assert(trace[0].dest == 10 && trace[1].dest == 31 && trace[2].dest == 51);
	assert(trace[0].deadFrom == 11 && trace[0].deadTo == 31);
	// standTicks мерится тем же допуском, поэтому включает и пару тиков разгона — важно
	// лишь, что стояние после прибытия видно.
	assert(trace[0].standTicks >= 4 && trace[2].cpFrame == -1);
}

// --- Перемотка: сколько вырезанных КАДРОВ лежит до целевого кадра ------------------------
// Отображаемое время бота при сике = сырое время от старта рана до цели, минус записанные
// паузы, минус вырезы. Паузы вычитает аккумулятор плейбека, поэтому DeadFramesUpTo обязана
// исключить кадры, записанные на паузе — иначе двойной вычет.
static void test_dead_frames_up_to()
{
	const Interval dead[2] = {{150, 300}, {400, 500}};   // индексы кадров, включительно
	const Interval pauses[2] = {{200, 250}, {600, 650}}; // первая внутри выреза, вторая в живом

	// Цель за всеми вырезами: 151 - 51 + 101 = 201 кадр.
	assert(DeadFramesUpTo(dead, 2, pauses, 2, 1000) == 201);
	// Без пауз паузные кадры не вычитаются: 151 + 101 = 252.
	assert(DeadFramesUpTo(dead, 2, nullptr, 0, 1000) == 252);
	// Цель В СЕРЕДИНЕ второго выреза — обрезаем по цели: (151 - 51) + (450-400+1) = 151.
	assert(DeadFramesUpTo(dead, 2, pauses, 2, 450) == 151);
	// Цель до всех вырезов — ноль; ровно на первом кадре выреза — один кадр.
	assert(DeadFramesUpTo(dead, 2, pauses, 2, 149) == 0);
	assert(DeadFramesUpTo(dead, 2, pauses, 2, 150) == 1);
	assert(DeadFramesUpTo(dead, 2, pauses, 2, 0) == 0);
	assert(DeadFramesUpTo(nullptr, 0, pauses, 2, 1000) == 0);
	// Пауза, целиком накрывшая вырез, обнуляет его вклад.
	const Interval bigPause[1] = {{140, 320}};
	assert(DeadFramesUpTo(dead, 1, bigPause, 1, 1000) == 0);
}

// Тождество: живые кадры от старта рана до цели == все кадры минус паузные минус вырезанные.
// Правую часть считаем формулой перемотки, левую — прямым перебором кадров.
static void test_seek_live_frames_identity()
{
	const uint32_t runStart = 100, target = 1000;
	const Interval dead[2] = {{150, 300}, {400, 500}};
	const Interval pauses[2] = {{200, 250}, {600, 650}};

	uint64_t liveDirect = 0;
	for (uint32_t f = runStart + 1; f <= target; f++)
	{
		bool skipped = false;
		for (const Interval &d : dead)
		{
			if (f >= d.from && f <= d.to) skipped = true;
		}
		for (const Interval &p : pauses)
		{
			if (f >= p.from && f <= p.to) skipped = true;
		}
		if (!skipped) liveDirect++;
	}

	uint64_t pausedUpTo = 0;
	for (const Interval &p : pauses)
	{
		const uint32_t to = p.to < target ? p.to : target;
		if (to >= p.from) pausedUpTo += to - p.from + 1;
	}
	const uint64_t all = target - runStart;
	const uint64_t formula = all - pausedUpTo - DeadFramesUpTo(dead, 2, pauses, 2, target);
	// Абсолютный якорь: без него тест сверяет формулу с самой собой при пустых фикстурах.
	assert(liveDirect == 597 && formula == liveDirect);
}

// Шкала перемотки и шкала разреза — ОДНА: DeadFramesUpTo(…, runEnd) обязана дать ровно то
// число кадров, которое разрез вычел из времени рана. Разъедутся — таймер бота после сика
// разойдётся с awr_ms (тот самый дефект «отмотал назад — вернулись вырезы»).
static void test_seek_scale_matches_cut_scale()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 30; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(31, 0, 1, 1, 10.0f));
	for (uint32_t i = 32; i < 40; i++) v.push_back(FC(i, 0, 1, 1, 10.0f + (i - 31)));
	FillPre(v);
	const Interval pause {15, 18};
	const uint32_t runEnd = (uint32_t)v.size() - 1;
	CutResult r = ComputeAwrCut(v.data(), v.size(), &pause, 1, 10000, TI, 0, runEnd);
	assert(r.ok && r.dead.size() == 1);
	const uint64_t seekFrames = DeadFramesUpTo(r.dead.data(), (uint32_t)r.dead.size(), &pause, 1, runEnd);
	assert(seekFrames == 17);
	// И то же самое через сам awrMs: (timeMs - awrMs) — это ровно seekFrames тиков.
	assert(10000 - r.awrMs == (uint64_t)(seekFrames * TI * 1000.0 + 0.5));
}

// Сверка «кадры окна против тиков таймера»: на согласованной фикстуре расхождения нет, а
// завышенное time_ms (модель порванной паузной пары) метрика обязана заметить — не отказом.
static void test_timer_frames_check()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 30; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(31, 0, 1, 1, 10.0f));
	for (uint32_t i = 32; i < 40; i++) v.push_back(FC(i, 0, 1, 1, 10.0f + (i - 31)));
	FillPre(v);
	const uint32_t runEnd = (uint32_t)v.size() - 1;
	const Interval pause {15, 18};

	// Окно 0..39 = 39 кадров, из них 4 паузных → таймер должен был насчитать 35 тиков.
	const uint64_t honestMs = (uint64_t)(35 * TI * 1000.0 + 0.5);
	CutResult okRes = ComputeAwrCut(v.data(), v.size(), &pause, 1, honestMs, TI, 0, runEnd);
	assert(okRes.ok && okRes.timerFramesChecked);
	assert(okRes.timerFramesRecorded == 35 && okRes.timerFramesExpected == 35 && !okRes.timerFramesMismatch);

	// Без паузы кадры окна не вычитаются: 39 записанных против 35 ожидаемых — 4 тика, это
	// внутри допуска (16), метрика молчит.
	CutResult inTol = ComputeAwrCut(v.data(), v.size(), nullptr, 0, honestMs, TI, 0, runEnd);
	assert(inTol.ok && inTol.timerFramesRecorded == 39 && !inTol.timerFramesMismatch);

	// time_ms завышено на 2 секунды (128 тиков) — расхождение сверх допуска.
	CutResult bad = ComputeAwrCut(v.data(), v.size(), &pause, 1, honestMs + 2000, TI, 0, runEnd);
	assert(bad.timerFramesChecked && bad.timerFramesMismatch);
	assert(bad.timerFramesRecorded == 35 && bad.timerFramesExpected == 35 + 128);
	// И это НЕ отказ: метрика только предупреждает.
	assert(bad.ok);

	// ОПАСНОЕ направление: пара PAUSE/RESUME потеряна (например порвана посторонним
	// TIMER_START), паузные кадры не вычлись → recorded > expected сверх допуска. Мёртвое
	// время при этом ЗАВЫШЕНО, awr_ms занижен — это и есть класс «правдоподобный чужой
	// рекорд», ради которого метрика заведена.
	std::vector<Frame> longPause;
	for (uint32_t i = 0; i <= 30; i++) longPause.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	longPause.push_back(FC(31, 0, 1, 1, 10.0f));
	// 40 кадров стояния на месте — модель записанной паузы, которую разрез не увидел.
	for (uint32_t i = 32; i <= 71; i++) longPause.push_back(FC(i, 0, 1, 1, 10.0f));
	for (uint32_t i = 72; i < 80; i++) longPause.push_back(FC(i, 0, 1, 1, 10.0f + (i - 71)));
	FillPre(longPause);
	// Таймер за паузу не тикал: ожидание 79 - 40 = 39 тиков, записано 79 кадров.
	const uint64_t lostPauseMs = (uint64_t)(39 * TI * 1000.0 + 0.5);
	CutResult lost = ComputeAwrCut(longPause.data(), longPause.size(), nullptr, 0, lostPauseMs, TI, 0, (uint32_t)longPause.size() - 1);
	assert(lost.timerFramesChecked && lost.timerFramesMismatch);
	assert(lost.timerFramesRecorded == 79 && lost.timerFramesExpected == 39);
	// Отказа нет — только метрика; ловить такое обязан читатель лога.
	assert(lost.ok);

	// На раннем отказе сверка не выполнялась — печатать её нельзя.
	std::vector<Frame> plain;
	for (uint32_t i = 0; i < 20; i++) plain.push_back(FC(i, 0, 0, 0, (float)i));
	FillPre(plain);
	CutResult noWindow = ComputeAwrCut(plain.data(), plain.size(), nullptr, 0, 10000, TI, 0, 0);
	assert(!noWindow.ok && !noWindow.timerFramesChecked);

	// А вот на отказе ОБХОДА (окно уже известно) сверка обязана быть посчитана: именно эти
	// файлы и разбирают по логу.
	std::vector<Frame> lostDest;
	for (uint32_t i = 0; i <= 20; i++) lostDest.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	lostDest.push_back(FC(21, 0, 1, 1, 999.0f));
	FillPre(lostDest);
	CutResult destFail = ComputeAwrCut(lostDest.data(), lostDest.size(), nullptr, 0, 10000, TI, 0, lostDest.size() - 1);
	assert(!destFail.ok && std::strcmp(destFail.reason, "dest_not_found") == 0 && destFail.timerFramesChecked);
}

// --- Смена точки назначения рвёт сшивку ---------------------------------------------------
// ФИКСТУРА ПО РЕАЛЬНЫМ ЧИСЛАМ живого файла 01a06ca3-0570-7c54-a732-eba372716594
// (kz_slide_arid main ckz, AWR 42 281 мс, вырез №9). Кадры и позиции — из разбора файла,
// индексы сдвинуты на -30087, то есть кадр 0 здесь это кадр 30087 в файле:
//   30087 последний живой   494.9 / 1042.6 / 83.5
//   30142 постановка cp19   510.4 / 1049.5 / 80.0
//   30168 постановка cp20   504.3 / 1054.0 / 80.0
//   30325, 30414, 30571, 30725 — четыре прибытия ТП на точку cp20
//   30769 cp21 500.5/1055.9, 30783 cp22 498.2/1059.7, 30795 cp23 497.8/1060.3
//   31006 постановка cp24   497.9 / 1060.2 / 80.0
//   … прибытия на точку cp24 …  34595 последнее, 34596 первый живой
// Соседние чекпоинты стоят в 8.9 u друг от друга — это ВНУТРИ AWR_DEST_TOLERANCE (16), и
// именно поэтому прежний якорь склеивал два кластера в один вырез на 70.4 с.
static void test_real_file_dest_change_breaks_chain()
{
	const float P1x = 504.3f, P1y = 1054.0f;  // точка cp20
	const float P2x = 497.9f, P2y = 1060.2f;  // точка cp24
	std::vector<Frame> v;
	// 0..54: подход от последнего живого кадра к cp19.
	for (uint32_t i = 0; i <= 54; i++)
	{
		const float t = (float)i / 55.0f;
		v.push_back(F3(i, -1, 18, 0, 494.9f + t * 15.5f, 1042.6f + t * 6.9f, 83.5f - t * 3.5f));
	}
	v.push_back(F3(55, -1, 19, 0, 510.4f, 1049.5f, 80.0f));  // 30142: cp19
	for (uint32_t i = 56; i <= 80; i++)
	{
		const float t = (float)(i - 55) / 26.0f;
		v.push_back(F3(i, -1, 19, 0, 510.4f - t * 6.1f, 1049.5f + t * 4.5f, 80.0f));
	}
	v.push_back(F3(81, -1, 20, 0, P1x, P1y, 80.0f));  // 30168: cp20
	// Четыре попытки на cp20: уход (слайд, далеко) и прибытие в ТУ ЖЕ точку.
	const uint32_t p1Arrivals[4] = {238, 327, 484, 638}; // 30325, 30414, 30571, 30725
	// Прибытия на ОДНУ точку разнесены на доли юнита (0.0 / 0.6 / 1.2 / 1.8): в живом файле
	// они совпали до 0.1 u, но битовое равенство в фикстуре не пинало бы
	// AWR_SAME_DEST_TOLERANCE снизу вовсе — его можно было бы молча обнулить, и тест не
	// заметил бы. Разброс меньше допуска (2 u) и больше половины допуска, поэтому мутации
	// «допуск 0» и «допуск 0.5» ломают именно этот тест.
	const float p1Jitter[4] = {0.0f, 0.6f, 1.2f, 1.8f};
	uint32_t frame = 82;
	int32_t tp = 0;
	for (uint32_t a = 0; a < 4; a++)
	{
		for (; frame < p1Arrivals[a]; frame++)
		{
			v.push_back(F3(frame, -1, 20, tp, P1x + 90.0f, P1y + 70.0f, 80.0f));
		}
		tp++;
		v.push_back(F3(frame, -1, 20, tp, P1x + p1Jitter[a], P1y, 80.0f));
		frame++;
	}
	// Переход к новой точке: 0.7 с шага (639..681), потом три постановки и стояние до cp24.
	for (; frame <= 681; frame++)
	{
		const float t = (float)(frame - 639) / 43.0f;
		v.push_back(F3(frame, -1, 20, tp, P1x - t * 3.8f, P1y + t * 1.9f, 80.0f));
	}
	v.push_back(F3(682, -1, 21, tp, 500.5f, 1055.9f, 80.0f));  // 30769: cp21
	for (frame = 683; frame <= 695; frame++) v.push_back(F3(frame, -1, 21, tp, 500.5f, 1055.9f, 80.0f));
	v.push_back(F3(696, -1, 22, tp, 498.2f, 1059.7f, 80.0f));  // 30783: cp22
	for (frame = 697; frame <= 707; frame++) v.push_back(F3(frame, -1, 22, tp, 498.2f, 1059.7f, 80.0f));
	v.push_back(F3(708, -1, 23, tp, 497.8f, 1060.3f, 80.0f));  // 30795: cp23
	for (frame = 709; frame <= 918; frame++) v.push_back(F3(frame, -1, 23, tp, 497.8f, 1060.3f, 80.0f));
	v.push_back(F3(919, -1, 24, tp, P2x, P2y, 80.0f));         // 31006: cp24
	// Петли на cp24 (в файле их 17, здесь три — правило от их числа не зависит).
	const uint32_t p2Arrivals[3] = {1500, 3000, 4508}; // последнее = 34595
	frame = 920;
	for (uint32_t a = 0; a < 3; a++)
	{
		for (; frame < p2Arrivals[a]; frame++)
		{
			v.push_back(F3(frame, -1, 24, tp, P2x + 90.0f, P2y + 70.0f, 80.0f));
		}
		tp++;
		v.push_back(F3(frame, -1, 24, tp, P2x, P2y, 80.0f));
		frame++;
	}
	// 34596 и далее — живой хвост до финиша.
	for (; frame <= 4600; frame++) v.push_back(F3(frame, -1, 24, tp, P2x + 6.0f * (frame - 4509), P2y, 80.0f));
	FillPre(v);

	const uint32_t runEnd = (uint32_t)v.size() - 1;
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 520000, TI, 0, runEnd);
	assert(r.ok && r.teleports == 7);
	// ДВА выреза, а не один: точка назначения сменилась.
	assert(r.dead.size() == 2);
	// Четыре петли cp20 схлопнулись в один вырез — от постановки cp20 (81) до последнего
	// прибытия (638).
	assert(r.dead[0].from == 82 && r.dead[0].to == 638);
	// Петли cp24 — свой вырез, от постановки cp24 (919) до последнего прибытия (4508).
	assert(r.dead[1].from == 920 && r.dead[1].to == 4508);
	// Переход между чекпоинтами ЖИВОЙ, и постановки cp21/22/23 видны.
	auto live = LiveIntervals(r.dead, v.size());
	assert(live.size() == 3);
	assert(live[0].from == 0 && live[0].to == 81);
	assert(live[1].from == 639 && live[1].to == 919);
	assert(live[2].from == 4509 && live[2].to == runEnd);
	// Сумма живых кадров выросла ровно на длину перехода: прежнее правило давало ОДИН вырез
	// с 82 по 4508 (в файле — 30088..34595), то есть переход 639..919 был мёртвым.
	const uint64_t liveNow = (uint64_t)(81 - 0 + 1) + (919 - 639 + 1) + (runEnd - 4509 + 1);
	const uint64_t liveBefore = (uint64_t)(81 - 0 + 1) + (runEnd - 4509 + 1);
	assert(liveNow - liveBefore == 919 - 639 + 1);
}

// Тот же разрыв, но БЕЗ постановки чекпоинта у второй точки (модель `!undo` и сдвига
// индексов): якорь второго кластера обязан упереться в прибытие первого, а не пройти сквозь
// него. Точки в 8.9 u друг от друга — внутри AWR_DEST_TOLERANCE, то есть прежний якорь
// склеил бы кластеры в один вырез.
static void test_dest_change_breaks_chain_without_placement()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 9; i++) v.push_back(F3(i, -1, 0, 0, 500.0f + 6.0f * i, 1000.0f, 80.0f));
	v.push_back(F3(10, -1, 1, 0, 560.0f, 1000.0f, 80.0f)); // постановка первой точки
	for (uint32_t i = 11; i <= 20; i++) v.push_back(F3(i, -1, 1, 0, 560.0f + 10.0f * (i - 10), 1060.0f, 80.0f));
	v.push_back(F3(21, -1, 1, 1, 560.0f, 1000.0f, 80.0f)); // прибытие на первую точку
	// Игрок сдвинулся на 8.9 u и оказался на второй точке (чекпоинт не ставился).
	for (uint32_t i = 22; i <= 25; i++) v.push_back(F3(i, -1, 1, 1, 566.4f, 1006.2f, 80.0f));
	for (uint32_t i = 26; i <= 40; i++) v.push_back(F3(i, -1, 1, 1, 566.4f + 10.0f * (i - 25), 1106.0f, 80.0f));
	v.push_back(F3(41, -1, 1, 2, 566.4f, 1006.2f, 80.0f)); // прибытие на ВТОРУЮ точку
	for (uint32_t i = 42; i < 60; i++) v.push_back(F3(i, -1, 1, 2, 566.4f + 10.0f * (i - 41), 1006.2f, 80.0f));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.teleports == 2);
	// ДВА выреза, а не один: якорь второго кластера упёрся в прибытие первого (кадр 21) и
	// дальше не пошёл, хотя 8.9 u — это внутри AWR_DEST_TOLERANCE. Прежнее правило склеивало
	// оба кластера в один вырез 11..41.
	assert(r.dead.size() == 2);
	assert(r.dead[0].from == 11 && r.dead[0].to == 21);
	assert(r.dead[1].from == 23 && r.dead[1].to == 41);
	// Между вырезами есть живой кадр 22 — тот самый момент, когда игрок оказался на новой
	// точке. Постановки чекпоинта здесь нет, поэтому живым остаётся один кадр, а не весь
	// переход: путь (а) знает кадр постановки точно, путь (б) — только начало пребывания.
	auto live = LiveIntervals(r.dead, v.size());
	assert(live.size() == 3 && live[1].from == 22 && live[1].to == 22);
}

// Стык КАДР В КАДР при РАЗНОЙ точке назначения: вырез второго кластера начинается ровно на
// следующем кадре после прибытия первого (живых кадров между ними нет), и слияние обязано
// это НЕ сшить. На awrMs не влияет — кадры те же, — но влияет на число вырезов, а по нему
// принимают пересчёт и читают трассу. Фикстура ревью: реальный код даёт [11,21]+[22,41],
// мутант «сливать стык без сверки точки» — [11,41].
static void test_adjacent_cuts_with_different_dest_not_merged()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 9; i++) v.push_back(F3(i, -1, 0, 0, 500.0f + 6.0f * i, 1000.0f, 80.0f));
	v.push_back(F3(10, -1, 1, 0, 560.0f, 1000.0f, 80.0f)); // постановка точки A
	for (uint32_t i = 11; i <= 20; i++) v.push_back(F3(i, -1, 1, 0, 560.0f + 30.0f * (i - 10), 1200.0f, 80.0f));
	v.push_back(F3(21, -1, 1, 1, 560.0f, 1000.0f, 80.0f)); // прибытие на A
	// Сразу уходим далеко: скан второго прибытия не найдёт кадра в допуске от точки B
	// раньше, чем дойдёт до кадра 21 (точки A и B в 8.9 u — внутри AWR_DEST_TOLERANCE).
	for (uint32_t i = 22; i <= 40; i++) v.push_back(F3(i, -1, 1, 1, 560.0f + 30.0f * (i - 21), 1400.0f, 80.0f));
	v.push_back(F3(41, -1, 1, 2, 566.4f, 1006.2f, 80.0f)); // прибытие на B (не чекпоинт: undo)
	for (uint32_t i = 42; i < 60; i++) v.push_back(F3(i, -1, 1, 2, 566.4f + 30.0f * (i - 41), 1006.2f, 80.0f));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.teleports == 2);
	assert(r.dead.size() == 2);
	assert(r.dead[0].from == 11 && r.dead[0].to == 21);
	assert(r.dead[1].from == 22 && r.dead[1].to == 41);
	// Именно стык: между вырезами нет живых кадров, и всё равно это ДВА выреза.
	assert(r.dead[0].to + 1 == r.dead[1].from);
}

// `!undo` по-прежнему снимает и телепорт, и его вырез: игрок возвращается туда, откуда
// телепортировался, и живой маршрут продолжается как будто телепорта не было.
static void test_undo_cancels_teleport_and_its_cut()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 20; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(21, 0, 1, 1, 10.0f));                                     // ТП на чекпоинт
	for (uint32_t i = 22; i <= 22; i++) v.push_back(FC(i, 0, 1, 1, 10.0f));  // стоит на нём
	v.push_back(FC(23, 0, 1, 2, 20.0f));                                     // !undo — назад в кадр 20
	for (uint32_t i = 24; i < 40; i++) v.push_back(FC(i, 0, 1, 2, 20.0f + (i - 23)));
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 10000, TI, 0, v.size() - 1);
	assert(r.ok && r.teleports == 2 && r.dead.size() == 1);
	// Вырезано ровно то, что создал телепорт: кадры 21..23. Точка undo — НЕ точка чекпоинта,
	// поэтому цепочка не сшивается с вырезом чекпоинта, а маршрут до кадра 20 и после 23
	// остаётся живым.
	assert(r.dead[0].from == 21 && r.dead[0].to == 23);
	auto live = LiveIntervals(r.dead, v.size());
	assert(live.size() == 2 && live[0].from == 0 && live[0].to == 20 && live[1].from == 24);
}

int main()
{
	test_no_teleports(); test_single_tp(); test_repeat_tp_same_cp(); test_prevcp_nextcp_keeps_middle();
	test_undo(); test_pause_overlap_not_double_counted(); test_dest_not_found();
	test_dead_counts_frames_not_server_ticks(); test_prac_sized_gap_allowed(); test_absurd_record_gap_refused();
	test_many_arrivals_chain_no_double_count(); test_awr_implausible_guard(); test_max_gap_measured_flag();
	test_overlapping_pauses_not_double_subtracted();
	test_tail_teleport_after_run_end(); test_prerecord_teleport_before_run_start(); test_counter_mismatch();
	test_no_run_window();
	test_cp_set_while_running(); test_cp_matches_pre_side(); test_undo_mid_tick();
	test_dest_not_found_detail(); test_counter_mismatch_detail();
	test_pre_branch_beats_later_pass(); test_pre_match_at_run_start_clamped();
	test_repeat_tp_collapses_to_one_dead(); test_standing_before_first_tp_is_cut(); test_cp_index_and_scan_agree();
	test_trace_reports_every_arrival();
	test_real_file_dest_change_breaks_chain(); test_dest_change_breaks_chain_without_placement();
	test_adjacent_cuts_with_different_dest_not_merged(); test_undo_cancels_teleport_and_its_cut();
	test_dead_frames_up_to(); test_seek_live_frames_identity(); test_seek_scale_matches_cut_scale();
	test_timer_frames_check();
	std::puts("awr_cut: all tests passed");
	return 0;
}
