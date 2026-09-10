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

// Пауза 15..18 внутри мёртвого интервала 11..31 — её длительность из мёртвого времени вычитается.
static void test_pause_overlap_not_double_counted()
{
	std::vector<Frame> v;
	for (uint32_t i = 0; i <= 30; i++) v.push_back(FC(i, 0, i >= 10 ? 1 : 0, 0, (float)i));
	v.push_back(FC(31, 0, 1, 1, 10.0f));
	for (uint32_t i = 32; i < 40; i++) v.push_back(FC(i, 0, 1, 1, 10.0f + (i - 31)));
	Interval pause {15, 18};
	FillPre(v);
	CutResult r = ComputeAwrCut(v.data(), v.size(), &pause, 1, 10000, TI, 0, v.size() - 1);
	// мёртвое 21 тик минус пауза (18-14 = 4 тика) = 17 тиков
	assert(r.ok && r.awrMs == 10000 - (uint64_t)(17 * TI * 1000.0 + 0.5));
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
	// pre кадра 21 = 120, в 1 юните от 121 → последний живой кадр 20, мёртвое 21..31.
	assert(r.dead[0].from == 21 && r.dead[0].to == 31);
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

int main()
{
	test_no_teleports(); test_single_tp(); test_repeat_tp_same_cp(); test_prevcp_nextcp_keeps_middle();
	test_undo(); test_pause_overlap_not_double_counted(); test_dest_not_found();
	test_tail_teleport_after_run_end(); test_prerecord_teleport_before_run_start(); test_counter_mismatch();
	test_no_run_window();
	test_cp_set_while_running(); test_cp_matches_pre_side(); test_undo_mid_tick();
	test_dest_not_found_detail(); test_counter_mismatch_detail();
	std::puts("awr_cut: all tests passed");
	return 0;
}
