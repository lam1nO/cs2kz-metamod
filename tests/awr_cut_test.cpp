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

// Пауза (TIMER_PAUSE на тике 15, TIMER_RESUME на тике 18) внутри мёртвого интервала 11..31 —
// её длительность из мёртвого времени вычитается. Паузы задаются в СЕРВЕРНЫХ ТИКАХ.
static void test_pause_overlap_not_double_counted()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 30; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(31, 0, 1, 1, 10.0f));
	for (uint32_t i = 32; i < 40; i++) v.push_back(FC(i, 0, 1, 1, 10.0f + (i - 31)));
	Interval pause {15, 18};
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), &pause, 1, 10000, TI, 0, v.size() - 1);
	// мёртвое 21 тик (31-10) минус пауза (18-15 = 3 тика) = 18 тиков
	assert(r.ok && r.awrMs == 10000 - (uint64_t)(18 * TI * 1000.0 + 0.5));
}

// КОРЕНЬ awr_ms = 0 на длинных гриндах: `!prac` не пишет тиков вовсе, а таймер на это время
// стоит на паузе — в файле остаётся РАЗРЫВ serverTick между двумя соседними кадрами. Пауза,
// измеренная в индексах кадров, съёживается в один кадр, и её тики оставались в мёртвом
// времени: deadMs > timeMs → awrMs = 0. Пересечение считается по тикам и разрыв покрывает.
static void test_prac_gap_pause_subtracted_by_ticks()
{
	const uint32_t GAP = 20000; // ~5 минут prac между кадрами 20 и 21
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 20; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	// После prac игрок продолжает с того же места; тики прыгнули на GAP.
	for (uint32_t i = 21; i <= 24; i++) v.push_back(FC(i + GAP, 0, 1, 0, (float)i));
	v.push_back(FC(25 + GAP, 0, 1, 1, 10.0f)); // ТП на чекпоинт кадра 10
	for (uint32_t i = 26; i <= 30; i++) v.push_back(FC(i + GAP, 0, 1, 1, 10.0f + (i - 25)));
	FillPre(v);
	// TIMER_PAUSE на тике 20, TIMER_RESUME на тике 21+GAP — ровно как их пишет рекордер.
	Interval pause {20, 21 + GAP};
	// Время рана: окно 0..(30+GAP) тиков минус пауза = 30 - 1 + 1 ... считаем по-честному.
	const uint64_t windowTicks = 30 + GAP;
	const uint64_t timeMs = (uint64_t)((double)(windowTicks - GAP) * TI * 1000.0 + 0.5);
	CutResult r = ComputeAwrCut(v.data(), v.size(), &pause, 1, timeMs, TI, 0, v.size() - 1);
	assert(r.ok && r.teleports == 1 && r.dead.size() == 1);
	assert(r.dead[0].from == 11 && r.dead[0].to == 25);
	// Мёртвое: тики 10..(25+GAP) = 15 + GAP, минус пауза GAP+1 → 14 тиков.
	assert(r.awrMs == timeMs - (uint64_t)(14 * TI * 1000.0 + 0.5));
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
	// Якорь первого прибытия — кадр 0: игрок стоял в точке чекпоинта ещё до его постановки,
	// и по каноническому правилу это стояние тоже мёртвое (самый ранний кадр пребывания).
	const uint64_t deadTicks = 601 - 0;
	const uint64_t deadMs = (uint64_t)((double)deadTicks * TI * 1000.0 + 0.5);
	CutResult r = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 20000, TI, 0, (uint32_t)v.size() - 1);
	assert(r.ok && r.teleports == 200);
	assert(r.dead.size() == 1 && r.dead[0].from == 1 && r.dead[0].to == 601);
	assert(r.awrMs == 20000 - deadMs);
}

// Неправдоподобно малый awrMs — отказ, а не строка с нулём: она выиграла бы минимум по
// (карта, курс, режим), и игрок увидел бы пустой прыжок в финиш.
static void test_awr_implausible_guard()
{
	std::vector<Frame> v = BuildChainFixture(200);
	const uint64_t deadMs = (uint64_t)((double)(601 - 0) * TI * 1000.0 + 0.5); // 9391
	// (1) мёртвого времени насчитали больше, чем длился ран → кламп в ноль запрещён.
	CutResult zero = ComputeAwrCut(v.data(), v.size(), nullptr, 0, deadMs - 375, TI, 0, (uint32_t)v.size() - 1);
	assert(!zero.ok && std::strcmp(zero.reason, "awr_implausible") == 0);
	assert(std::strstr(zero.detail, "dead_ms=") && std::strstr(zero.detail, "time_ms=") && std::strstr(zero.detail, "dead_n=1"));
	// (2) живого меньше 5 % времени рана → тот же отказ.
	CutResult tiny = ComputeAwrCut(v.data(), v.size(), nullptr, 0, deadMs + 225, TI, 0, (uint32_t)v.size() - 1);
	assert(!tiny.ok && std::strcmp(tiny.reason, "awr_implausible") == 0);
	// (3) ровно над порогом — проходит.
	CutResult okRes = ComputeAwrCut(v.data(), v.size(), nullptr, 0, deadMs + 500, TI, 0, (uint32_t)v.size() - 1);
	assert(okRes.ok && okRes.awrMs == 500);
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
// Тест обязан падать, если убрать откат якоря к самому раннему кадру пребывания в радиусе
// ИЛИ слияние смежных dead-интервалов (схлопывание держится на этой паре).
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
	// Все три попытки — ОДИН вырез до последнего прибытия (якорь очередного прибытия
	// упирается в предыдущее, смежные интервалы сливаются). from == 9, а не 11: путь (б)
	// откатывает якорь к началу пребывания в допуске, и два последних кадра плавного подхода
	// (9 — 54 u, 8 — 48 u) в него попадают; при валидном cpIndex (путь «а») было бы ровно 11 —
	// см. test_cp_index_and_scan_agree.
	assert(r.dead.size() == 1 && r.dead[0].from == 9 && r.dead[0].to == 71);
}

// Стояние на чекпоинте ПЕРЕД первой попыткой тоже вырезается: каноническое правило —
// «вырезаем всё от постановки чекпоинта до последнего телепорта на него», и стояние на месте
// постановки в этот промежуток входит (решение пользователя 10.09). Здесь работает путь (б)
// (cpIndex = -1), и видна его цена: якорь уходит к началу пребывания в радиусе, поэтому в
// вырез попадают ещё два кадра подхода (9 — 54 u, 8 — 48 u; кадр 7 в 42 u уже за допуском).
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
	assert(r.dead[0].from == 9 && r.dead[0].to == 41);
	auto live = LiveIntervals(r.dead, v.size());
	assert(live.size() == 2 && live[0].from == 0 && live[0].to == 8);
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
	assert(trace[0].dest == 8 && trace[1].dest == 31 && trace[2].dest == 51);
	assert(trace[0].deadFrom == 9 && trace[0].deadTo == 31);
	// standTicks мерится тем же допуском, поэтому включает и пару тиков разгона — важно
	// лишь, что стояние после прибытия видно.
	assert(trace[0].standTicks >= 4 && trace[2].cpFrame == -1);
}

// --- Перемотка: сколько вырезанного времени лежит до целевого тика --------------------------
// Отображаемое время бота при сике = сырое время от старта рана до цели, минус записанные
// паузы, минус вырезы. Паузы вычитает аккумулятор плейбека, поэтому DeadTicksUpTo обязана
// исключить их из выреза — иначе двойной вычет (та же ловушка, что в подсчёте мёртвого).
static void test_dead_ticks_up_to()
{
	const Interval dead[2] = {{150, 300}, {400, 500}}; // полуинтервалы (from, to] в ТИКАХ
	const Interval pauses[2] = {{200, 250}, {600, 650}}; // первая внутри выреза, вторая в живом

	// Цель за всеми вырезами: (300-150) - 50 + (500-400) = 200.
	assert(DeadTicksUpTo(dead, 2, pauses, 2, 1000) == 200);
	// Без пауз пересечение не вычитается: 150 + 100 = 250.
	assert(DeadTicksUpTo(dead, 2, nullptr, 0, 1000) == 250);
	// Цель В СЕРЕДИНЕ второго выреза — обрезаем по цели: 100 + (450-400) = 150.
	assert(DeadTicksUpTo(dead, 2, pauses, 2, 450) == 150);
	// Цель до всех вырезов — ноль (и на границе входа в вырез тоже).
	assert(DeadTicksUpTo(dead, 2, pauses, 2, 150) == 0);
	assert(DeadTicksUpTo(dead, 2, pauses, 2, 0) == 0);
	assert(DeadTicksUpTo(nullptr, 0, pauses, 2, 1000) == 0);
	// Пауза, целиком накрывшая вырез (prac внутри петли), обнуляет его вклад.
	const Interval bigPause[1] = {{140, 320}};
	assert(DeadTicksUpTo(dead, 1, bigPause, 1, 1000) == 0);
}

// Тождество: живые тики от старта рана до цели == сырое - паузы - вырезы. Считаем правую
// часть формулой, левую — прямым перебором тиков, и сверяем.
static void test_seek_live_time_identity()
{
	const uint32_t runStartTick = 100, targetTick = 1000;
	const Interval dead[2] = {{150, 300}, {400, 500}};
	const Interval pauses[2] = {{200, 250}, {600, 650}};

	uint64_t liveDirect = 0;
	for (uint32_t t = runStartTick + 1; t <= targetTick; t++)
	{
		bool skipped = false;
		for (const Interval &d : dead)
		{
			if (t > d.from && t <= d.to) skipped = true;
		}
		for (const Interval &p : pauses)
		{
			if (t > p.from && t <= p.to) skipped = true;
		}
		if (!skipped) liveDirect++;
	}

	uint64_t pausedUpTo = 0;
	for (const Interval &p : pauses)
	{
		const uint32_t to = p.to < targetTick ? p.to : targetTick;
		if (to > p.from) pausedUpTo += to - p.from;
	}
	const uint64_t raw = targetTick - runStartTick;
	const uint64_t formula = raw - pausedUpTo - DeadTicksUpTo(dead, 2, pauses, 2, targetTick);
	assert(liveDirect == 600 && formula == liveDirect);
}

int main()
{
	test_no_teleports(); test_single_tp(); test_repeat_tp_same_cp(); test_prevcp_nextcp_keeps_middle();
	test_undo(); test_pause_overlap_not_double_counted(); test_dest_not_found();
	test_prac_gap_pause_subtracted_by_ticks(); test_many_arrivals_chain_no_double_count(); test_awr_implausible_guard();
	test_tail_teleport_after_run_end(); test_prerecord_teleport_before_run_start(); test_counter_mismatch();
	test_no_run_window();
	test_cp_set_while_running(); test_cp_matches_pre_side(); test_undo_mid_tick();
	test_dest_not_found_detail(); test_counter_mismatch_detail();
	test_pre_branch_beats_later_pass(); test_pre_match_at_run_start_clamped();
	test_repeat_tp_collapses_to_one_dead(); test_standing_before_first_tp_is_cut(); test_cp_index_and_scan_agree();
	test_trace_reports_every_arrival();
	test_dead_ticks_up_to(); test_seek_live_time_identity();
	std::puts("awr_cut: all tests passed");
	return 0;
}
