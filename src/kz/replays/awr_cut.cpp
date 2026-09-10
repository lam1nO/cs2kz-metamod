#include "awr_cut.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace KZ::replaysystem::awr
{
	// Квадрат расстояния: сравнения идут по нему (корень в скане назад — лишние такты на
	// каждый кадр), сам корень нужен только строке detail.
	static float DistSq(const float *a, const float *b)
	{
		const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
		return dx * dx + dy * dy + dz * dz;
	}

	static float Dist(const float *a, const float *b)
	{
		return std::sqrt(DistSq(a, b));
	}

	static constexpr float AWR_DEST_TOLERANCE_SQ = AWR_DEST_TOLERANCE * AWR_DEST_TOLERANCE;
	static constexpr float AWR_SAME_DEST_TOLERANCE_SQ = AWR_SAME_DEST_TOLERANCE * AWR_SAME_DEST_TOLERANCE;

	// Одна и та же точка назначения? (см. AWR_SAME_DEST_TOLERANCE — допуск тут ТУГОЙ)
	static bool SameDest(const float *a, const float *b)
	{
		return DistSq(a, b) <= AWR_SAME_DEST_TOLERANCE_SQ;
	}

	// Нормализовать интервалы пауз (индексы кадров): отсортировать и слить пересекающиеся и
	// смежные. Записанные паузы приходят упорядоченными и не вложенными (пары PAUSE→RESUME
	// разбираются по возрастанию serverTick, а CanPause запрещает паузу в паузе), но
	// полагаться на это в ПУБЛИЧНОМ контракте чистой функции незачем: две пересекающиеся
	// паузы вычлись бы дважды и завысили awrMs. Копия дешёвая — пауз единицы.
	static std::vector<Interval> NormalizePauses(const Interval *pauses, uint32_t pauseCount)
	{
		std::vector<Interval> out;
		if (!pauses || pauseCount == 0)
		{
			return out;
		}
		out.assign(pauses, pauses + pauseCount);
		std::sort(out.begin(), out.end(), [](const Interval &x, const Interval &y) { return x.from < y.from; });
		size_t merged = 0;
		for (size_t i = 0; i < out.size(); i++)
		{
			if (out[i].to < out[i].from)
			{
				continue;
			}
			if (merged > 0 && out[i].from <= out[merged - 1].to + 1)
			{
				if (out[i].to > out[merged - 1].to)
				{
					out[merged - 1].to = out[i].to;
				}
				continue;
			}
			out[merged++] = out[i];
		}
		out.resize(merged);
		return out;
	}

	// Сколько кадров в интервале [from, to] НЕ записаны на паузе. Это и есть мера мёртвого
	// времени: один записанный кадр = один тик таймера (KZTimerService::OnPhysicsSimulatePost
	// прибавляет ровно тик за тик физики, пока таймер идёт и не на паузе; рекордер пишет
	// ровно один кадр за тот же тик, кроме `!prac`, где не пишет вовсе). Мерить длину
	// интервала по serverTick НЕЛЬЗЯ: в разрыв записи попадает время, которого таймер никогда
	// не считал, и мёртвое время выходило больше времени рана (замер cyb.183).
	static uint64_t ActiveFrames(uint32_t from, uint32_t to, const std::vector<Interval> &pauses)
	{
		if (to < from)
		{
			return 0;
		}
		uint64_t count = (uint64_t)to - from + 1;
		for (const Interval &p : pauses)
		{
			const uint32_t f = std::max(from, p.from), t = std::min(to, p.to);
			if (f <= t)
			{
				count -= std::min(count, (uint64_t)t - f + 1);
			}
		}
		return count;
	}

	// Кадр постановки чекпоинта №k: первый кадр, где cpCount вырос ДО k+1 (позднейшая постановка
	// побеждает — после сброса счётчика индексы переиспользуются). Только внутри окна рана:
	// чекпоинты предзаписи к рану не относятся, а их позиции сшили бы ран с предстартовым куском.
	static std::vector<int64_t> BuildCpSetIndex(const Frame *frames, uint32_t runStart, uint32_t runEnd)
	{
		std::vector<int64_t> cpSet;
		for (uint32_t i = runStart + 1; i <= runEnd; i++)
		{
			int32_t prev = frames[i - 1].cpCount, cur = frames[i].cpCount;
			for (int32_t k = prev; k < cur; k++)
			{
				if (k < 0)
				{
					continue;
				}
				if ((size_t)k >= cpSet.size())
				{
					cpSet.resize(k + 1, -1);
				}
				cpSet[k] = i;
			}
		}
		return cpSet;
	}

	// Подходит ли кадр j как место, ОТКУДА игрок телепортировался в позицию a. Возвращает
	// индекс последнего ЖИВОГО кадра (то самое D) или -1. Кадр j при совпадении остаётся
	// ЖИВЫМ (вырез начинается с j+1) — это половина канонического правила «вырезаем всё от
	// постановки чекпоинта до последнего телепорта на него», см. DestFrame.
	//
	// Позиция чекпоинта/undo снята в СЕРЕДИНЕ тика (см. AWR_DEST_TOLERANCE), поэтому она
	// сравнивается с обоими концами кадра:
	//  - совпала с post (конец тика j) → игрок был там к концу j, последний живой кадр = j;
	//  - совпала с pre (начало тика j) → он был там ещё до симуляции j, значит последним
	//    живым надо считать j-1, иначе в живой путь попал бы кадр, уже уводящий его прочь.
	// На нижней границе окна j-1 упирается в runStart: срезать сам старт рана нельзя.
	static int64_t MatchDest(const Frame *frames, uint32_t j, const float *a, uint32_t runStart)
	{
		if (DistSq(a, frames[j].origin) <= AWR_DEST_TOLERANCE_SQ)
		{
			return (int64_t)j;
		}
		if (DistSq(a, frames[j].preOrigin) <= AWR_DEST_TOLERANCE_SQ)
		{
			// Кламп в runStart сегодня НЕДОСТИЖИМ: путь (а) зовёт нас только с кадром
			// постановки, а cpSet заполняется с runStart+1; путь (б) зажимает якорь сам, до
			// вызова. Оставлен гардом на будущего вызывающего — мутация здесь по построению
			// не ловится ни одним тестом, и это осознанно (срезать старт рана нельзя никому).
			return j > runStart ? (int64_t)j - 1 : (int64_t)runStart;
		}
		return -1;
	}

	// Разбор неудачного поиска назначения — только для строки detail в логе.
	struct DestProbe
	{
		int64_t cpFrame = -1; // S: кадр постановки чекпоинта по индексу, -1 если нет
		float cpPostDist = -1.0f;
		float cpPreDist = -1.0f;
		int64_t bestFrame = -1; // ближайший кадр скана
		float bestDistSq = -1.0f;
		char method = '-'; // чем сшили: 'a' — индекс чекпоинта, 'b' — скан назад
	};

	// Кадр назначения телепорта, прибывшего на кадре t; -1 — не нашли.
	static int64_t DestFrame(const Frame *frames, uint32_t t, const std::vector<int64_t> &cpSet, uint32_t runStart, const std::vector<char> &arrivals,
							 DestProbe *probe)
	{
		const float *arrival = frames[t].origin;

		// (а) Прямая ссылка: телепорт на чекпоинт №k, кадр его постановки известен.
		int32_t k = frames[t].cpIndex;
		if (k >= 0 && (size_t)k < cpSet.size() && cpSet[k] >= (int64_t)runStart && (uint32_t)cpSet[k] < t)
		{
			const uint32_t sFrame = (uint32_t)cpSet[k];
			if (probe)
			{
				probe->cpFrame = (int64_t)sFrame;
				probe->cpPostDist = Dist(arrival, frames[sFrame].origin);
				probe->cpPreDist = Dist(arrival, frames[sFrame].preOrigin);
			}
			int64_t d = MatchDest(frames, sFrame, arrival, runStart);
			if (d >= 0)
			{
				if (probe)
				{
					probe->method = 'a';
				}
				return d;
			}
		}

		// (б) !undo, переполнение списка чекпоинтов, сдвиг индексов: позиционный поиск. Идём
		// от t назад до первого кадра, попавшего в допуск (это ВХОД в участок пребывания,
		// ближайший к t), а якорь выбираем ниже — самым ранним кадром того же участка.
		for (int64_t j = (int64_t)t - 1; j >= (int64_t)runStart; j--)
		{
			if (probe)
			{
				const float dSq = std::min(DistSq(arrival, frames[j].origin), DistSq(arrival, frames[j].preOrigin));
				if (probe->bestFrame < 0 || dSq < probe->bestDistSq)
				{
					probe->bestFrame = j;
					probe->bestDistSq = dSq;
				}
			}
			if (MatchDest(frames, (uint32_t)j, arrival, runStart) < 0)
			{
				continue;
			}
			if (probe)
			{
				probe->method = 'b';
			}

			// ПРАВИЛО (формулировка пользователя 10.09, та же, что в спеке §4 и в CYBER.md):
			// вырезается ПЕТЛЯ «поставлен чекпоинт N → какой-то ран → телепорт на N», то есть
			// средний пункт. Вырез = отрезок от ПОСТАНОВКИ чекпоинта либо от ПРЕДЫДУЩЕГО
			// ПРИБЫТИЯ на ту же точку (что позже) до текущего прибытия. Сшивать соседние
			// вырезы можно только пока ТОЧКА НАЗНАЧЕНИЯ не менялась: как только игрок
			// сдвинулся и поставил новый чекпоинт, сшивка прерывается, и переход между
			// чекпоинтами остаётся ЖИВЫМ.
			//
			// Кадр j — лишь ВХОД в участок, лежащий в допуске от точки прибытия. Якорь
			// выбирается в два шага (ниже): участок откатывается назад по непрерывному
			// пребыванию, но НЕ через прибытие на другую точку, а затем внутри участка
			// берётся самый ПОЗДНИЙ кадр ПОСТАНОВКИ чекпоинта (роста cpCount), попавший в
			// допуск от точки прибытия. Постановки в участке нет — якорем остаётся начало
			// участка. Так путь (б) даёт тот же ответ, что путь (а) по индексу чекпоинта:
			// один и тот же ран не должен считаться по-разному в зависимости от того,
			// нашёлся ли cpIndex.
			//
			// Побочно правило схлопывает цепочку повторных ТП на ОДИН чекпоинт: якорь
			// очередного прибытия упирается в прибытие предыдущего (кадры между ними из
			// радиуса выходят), обход продолжается с него, вырезы стыкуются кадр в кадр — и
			// их СЛИВАЕТ ComputeAwrCut. Схлопывание держится именно на паре «якорь + слияние
			// смежных ПРИ СОВПАДЕНИИ ТОЧКИ»: убрать любую половину — и у чекпоинта снова
			// останутся живые прибытия либо склеятся петли разных чекпоинтов.
			//
			// ЦЕНА позиционного пути: постановку он ищет по позиции с допуском
			// AWR_DEST_TOLERANCE, поэтому может взять якорем ЧУЖУЮ постановку, оказавшуюся в
			// радиусе (например переставленный на полшага чекпоинт). Направление ошибки при
			// этом безопасное: чужая постановка ПОЗЖЕ — вырез короче, awrMs БОЛЬШЕ, время
			// хуже. Если постановки в участке нет вовсе (`!undo`, чекпоинт вне окна), живым
			// остаётся один кадр перехода, а не весь переход: кадра постановки этот путь не
			// знает. Точен только путь (а), поэтому (б) и остаётся запасным.
			//
			// Прежнее правило («самый ранний кадр непрерывного пребывания») ошибалось в
			// другую, ОПАСНУЮ сторону: якорь уезжал назад через границу между чекпоинтами
			// (соседние стоят в единицах юнитов, замер 8.9 u — это внутри 16 u), затягивая в
			// вырез и переход, и весь предыдущий кластер петель. На живом файле 01a06ca3 так
			// склеилось 21 прибытие в один вырез на 70.4 с, а стык вырезки оказался между
			// РАЗНЫМИ чекпоинтами: разрыв 18.1 u и 170.9° по yaw. Значит контроль инвариантами
			// нужен СВЕРХУ (не занизить время рана); кламп awrMs ≤ timeMs — только гард от
			// отрицательного результата, не проверка.
			//
			// Началом участка считаем j при совпадении по post и j-1 при совпадении по pre:
			// в последнем случае позиция была на конце предыдущего тика.
			int64_t first = DistSq(arrival, frames[j].origin) <= AWR_DEST_TOLERANCE_SQ ? j : j - 1;
			// Шаг 1 якоря: откат участка пребывания, но НЕ через прибытие на ДРУГУЮ точку —
			// между разными точками лежит ЖИВОЙ переход (разбор дефекта — в блоке-правиле
			// выше). Прибытие на ТУ ЖЕ точку якорь проходит свободно: на этом держится
			// схлопывание повторных попыток одного чекпоинта в один вырез.
			while (first > (int64_t)runStart && DistSq(arrival, frames[first - 1].origin) <= AWR_DEST_TOLERANCE_SQ
				   && !(arrivals[first - 1] && !SameDest(frames[first - 1].origin, arrival)))
			{
				first--;
			}
			if (first < (int64_t)runStart)
			{
				// Совпадение было по pre кадра runStart — срезать сам старт рана нельзя.
				return (int64_t)runStart;
			}

			// «Вырез начинается от ПОСТАНОВКИ чекпоинта»: если внутри участка пребывания есть
			// кадр, где вырос счётчик чекпоинтов, и он лежит в допуске от точки прибытия — это
			// и есть постановка той точки, на которую вернулся игрок. Берём САМУЮ ПОЗДНЮЮ:
			// игрок мог переставить чекпоинт на том же месте, и живым остаётся всё до
			// последней постановки. Без этого шага путь (б) отдавал бы начало пребывания, то
			// есть съедал бы и подход, и переход между чекпоинтами (путь (а) знает кадр
			// постановки точно, но он молчит на !undo и при сдвиге индексов).
			int64_t placed = -1;
			for (int64_t i = first > (int64_t)runStart ? first : (int64_t)runStart + 1; i <= j; i++)
			{
				if (frames[i].cpCount > frames[i - 1].cpCount && DistSq(arrival, frames[i].origin) <= AWR_DEST_TOLERANCE_SQ)
				{
					placed = i;
				}
			}
			return placed >= 0 ? placed : first;
		}
		return -1;
	}

	// Длина стояния в точке кадра t: сколько последующих кадров окна не ушли из допуска.
	static uint32_t StandTicks(const Frame *frames, uint32_t t, uint32_t runEnd)
	{
		uint32_t n = 0;
		for (uint32_t k = t + 1; k <= runEnd && DistSq(frames[t].origin, frames[k].origin) <= AWR_DEST_TOLERANCE_SQ; k++)
		{
			n++;
		}
		return n;
	}

	CutResult ComputeAwrCut(const Frame *frames, uint32_t count, const Interval *pauses, uint32_t pauseCount, uint64_t timeMs, double tickInterval,
							uint32_t runStart, uint32_t runEnd, std::vector<ArrivalTrace> *trace)
	{
		CutResult r;
		if (!frames || count == 0)
		{
			r.reason = "empty";
			return r;
		}
		if (runEnd >= count || runStart >= runEnd)
		{
			// Окна нет или оно вырождено — резать нечего и не по чему.
			r.reason = "no_run_window";
			return r;
		}
		// Нормализованные паузы нужны и сверке ниже, и подсчёту мёртвого времени — считаем
		// один раз, до обхода.
		const std::vector<Interval> pauseList = NormalizePauses(pauses, pauseCount);

		// ПРЯМАЯ сверка тождества, на котором стоит мера мёртвого времени (см. CutResult):
		// записанные и не паузные кадры окна против тиков, насчитанных таймером. Считается
		// всегда, отказа не вызывает — нужна, чтобы увидеть живой разброс. Стоит здесь, а не
		// у вызывающего, потому что только тут есть и окно, и нормализованные паузы, и ДО
		// обхода — чтобы она была и на отказах обхода (dest_not_found, counter_mismatch), где
		// окно уже известно: печатать там `frames=n/a` значило бы терять диагностику ровно на
		// тех файлах, которые и надо разбирать.
		{
			const uint64_t windowFramesTotal = (uint64_t)runEnd - runStart;
			uint64_t pausedInWindow = 0;
			for (const Interval &p : pauseList)
			{
				const uint32_t f = std::max(runStart + 1, p.from), t = std::min(runEnd, p.to);
				if (f <= t)
				{
					pausedInWindow += (uint64_t)t - f + 1;
				}
			}
			r.timerFramesRecorded = windowFramesTotal - std::min(windowFramesTotal, pausedInWindow);
			r.timerFramesExpected = tickInterval > 0.0 ? (uint64_t)((double)timeMs / (tickInterval * 1000.0) + 0.5) : 0;
			const uint64_t tolerance = std::max(AWR_TIMER_FRAMES_TOLERANCE_TICKS, r.timerFramesExpected / AWR_TIMER_FRAMES_TOLERANCE_DIVISOR);
			const uint64_t delta = r.timerFramesRecorded > r.timerFramesExpected ? r.timerFramesRecorded - r.timerFramesExpected
																				 : r.timerFramesExpected - r.timerFramesRecorded;
			r.timerFramesMismatch = delta > tolerance;
			r.timerFramesChecked = true;
		}

		std::vector<int64_t> cpSet = BuildCpSetIndex(frames, runStart, runEnd);

		// Прибытия ТП: кадры внутри окна, где вырос teleportCount.
		std::vector<char> arrival(count, 0);
		for (uint32_t i = runStart + 1; i <= runEnd; i++)
		{
			if (frames[i].tpCount > frames[i - 1].tpCount)
			{
				arrival[i] = 1;
				r.teleports++;
			}
		}

		// Инвариант спеки §4.5: число прибытий по кадрам == приросту teleportCount за окно.
		// Разойтись они могут, если счётчик за один кадр прыгнул больше чем на 1 (два телепорта
		// в одном тике или пропущенные кадры записи) — восстановить такую петлю нечем, а
		// молча выдавать неверную сшивку хуже, чем отказать.
		const int64_t expectedTeleports = (int64_t)frames[runEnd].tpCount - (int64_t)frames[runStart].tpCount;
		if ((int64_t)r.teleports != expectedTeleports)
		{
			r.reason = "counter_mismatch";
			std::snprintf(r.detail, sizeof(r.detail), "arrivals=%u expected=%lld window=%u..%u tp_start=%d tp_end=%d", r.teleports,
						  (long long)expectedTeleports, runStart, runEnd, frames[runStart].tpCount, frames[runEnd].tpCount);
			return r;
		}

		// Обход от финиша назад по ссылкам телепортов (спека §4), в пределах окна рана.
		// deadDest — точка назначения прибытия, породившего вырез (индексы совпадают с r.dead
		// до нормализации). Нужна слиянию: разные точки сшивать нельзя.
		struct DestPoint
		{
			float x, y, z;
		};

		std::vector<DestPoint> deadDest;
		int64_t cursor = (int64_t)runEnd;
		while (cursor > (int64_t)runStart)
		{
			if (!arrival[cursor])
			{
				cursor--;
				continue;
			}
			// probe считаем только под трассу: на обычном пути диагностика не нужна.
			DestProbe walkProbe;
			int64_t d = DestFrame(frames, (uint32_t)cursor, cpSet, runStart, arrival, trace ? &walkProbe : nullptr);
			if (trace)
			{
				const Frame &a = frames[cursor];
				ArrivalTrace tr;
				tr.frame = (uint32_t)cursor;
				tr.serverTick = a.serverTick;
				tr.cpIndex = a.cpIndex;
				tr.cpCount = a.cpCount;
				tr.tpCount = a.tpCount;
				tr.origin[0] = a.origin[0];
				tr.origin[1] = a.origin[1];
				tr.origin[2] = a.origin[2];
				tr.dest = d;
				tr.method = d >= 0 ? walkProbe.method : '-';
				tr.cpFrame = walkProbe.cpFrame;
				tr.cpPostDist = walkProbe.cpPostDist;
				tr.cpPreDist = walkProbe.cpPreDist;
				tr.standTicks = StandTicks(frames, (uint32_t)cursor, runEnd);
				if (d >= 0)
				{
					tr.deadFrom = (uint32_t)d + 1;
					tr.deadTo = (uint32_t)cursor;
				}
				trace->push_back(tr);
			}
			if (d < 0)
			{
				r.reason = "dest_not_found";
				r.dead.clear();
				deadDest.clear();
				if (trace)
				{
					// Трасса собиралась в порядке обхода (от финиша назад) — для чтения
					// глазами упорядочиваем по кадру, как и на успешном пути.
					std::sort(trace->begin(), trace->end(), [](const ArrivalTrace &x, const ArrivalTrace &y) { return x.frame < y.frame; });
				}
				// Второй проход — только ради строки detail. Считать дистанции на КАЖДОМ кадре
				// удачного скана незачем: успешный путь обычно обрывается на первых кадрах, а
				// неудачный и так уже прошёл всё окно — лишний проход платится один раз за файл.
				DestProbe probe;
				DestFrame(frames, (uint32_t)cursor, cpSet, runStart, arrival, &probe);
				const Frame &a = frames[cursor];
				const float bestDist = probe.bestDistSq >= 0.0f ? std::sqrt(probe.bestDistSq) : -1.0f;
				std::snprintf(
					r.detail, sizeof(r.detail),
					"t=%lld tick=%u arrival=%.1f/%.1f/%.1f cp_index=%d cp_frame=%lld cp_post_d=%.1f cp_pre_d=%.1f best_frame=%lld best_d=%.1f",
					(long long)cursor, a.serverTick, a.origin[0], a.origin[1], a.origin[2], a.cpIndex, (long long)probe.cpFrame, probe.cpPostDist,
					probe.cpPreDist, (long long)probe.bestFrame, bestDist);
				return r;
			}
			r.dead.push_back({(uint32_t)d + 1, (uint32_t)cursor});
			// Точка назначения выреза — для слияния: сшивать соседние вырезы можно ТОЛЬКО
			// пока точка не менялась (правило пользователя). Держим параллельно, а не в
			// Interval, чтобы публичный тип разреза не тащил геометрию.
			deadDest.push_back({frames[cursor].origin[0], frames[cursor].origin[1], frames[cursor].origin[2]});
			cursor = d;
		}
		// Порядок здесь — от финиша к старту; в возрастающий его приводит нормализация ниже.

		if (trace)
		{
			// Обход прыгает с прибытия на его назначение, поэтому прибытия, оказавшиеся
			// ВНУТРИ уже объявленного выреза, он не разбирает. Для диагностики их всё равно
			// надо видеть: помечаем method='s' и упорядочиваем трассу по кадру.
			for (uint32_t i = runStart + 1; i <= runEnd; i++)
			{
				if (!arrival[i])
				{
					continue;
				}
				bool seen = false;
				for (const ArrivalTrace &tr : *trace)
				{
					if (tr.frame == i)
					{
						seen = true;
						break;
					}
				}
				if (!seen)
				{
					const Frame &a = frames[i];
					ArrivalTrace tr;
					tr.frame = i;
					tr.serverTick = a.serverTick;
					tr.cpIndex = a.cpIndex;
					tr.cpCount = a.cpCount;
					tr.tpCount = a.tpCount;
					tr.origin[0] = a.origin[0];
					tr.origin[1] = a.origin[1];
					tr.origin[2] = a.origin[2];
					tr.standTicks = StandTicks(frames, i, runEnd);
					trace->push_back(tr);
				}
			}
			std::sort(trace->begin(), trace->end(), [](const ArrivalTrace &x, const ArrivalTrace &y) { return x.frame < y.frame; });
		}

		// НОРМАЛИЗАЦИЯ списка вырезов: отсортировать и слить смежные, пересекающиеся и
		// вложенные — но ТОЛЬКО пока точка назначения не менялась. Обход даёт интервалы по
		// построению непересекающимися и по убыванию (курсор строго убывает: DestFrame всегда
		// возвращает d < t), но подсчёт мёртвого времени суммирует интервалы, и любая будущая
		// ошибка в сшивке, породившая перекрытие, вычла бы перекрытый участок дважды.
		// Нормализация делает подсчёт корректным по построению и не зависящим от инвариантов
		// обхода.
		//
		// Слияние смежных (from <= to + 1) нужно и по существу: повторные попытки на ОДНОМ
		// чекпоинте дают цепочку вырезов, стыкующихся кадр в кадр ([D+1,T1], [T1+1,T2], …) —
		// сшивка ведёт каждый следующий обход ровно к прибытию предыдущего. Но как только
		// точка назначения сменилась, сшивка обязана прерваться: между разными чекпоинтами
		// лежит ЖИВОЙ переход, и склейка через него давала разрыв позиции и угла на стыке
		// (разбор и цифры — в блоке-правиле у пути (б) в DestFrame). Мёртвое время от слияния
		// не меняется: длительности считаются в кадрах и складываются.
		{
			struct DeadCut
			{
				Interval iv;
				DestPoint dest;
			};

			std::vector<DeadCut> cuts;
			cuts.reserve(r.dead.size());
			for (size_t i = 0; i < r.dead.size(); i++)
			{
				cuts.push_back({r.dead[i], i < deadDest.size() ? deadDest[i] : DestPoint {0.0f, 0.0f, 0.0f}});
			}
			std::sort(cuts.begin(), cuts.end(), [](const DeadCut &x, const DeadCut &y) { return x.iv.from < y.iv.from; });
			size_t merged = 0;
			for (size_t i = 0; i < cuts.size(); i++)
			{
				// ПЕРЕСЕЧЕНИЕ (и вложенность) сливаем ВСЕГДА, точка назначения тут не при чём:
				// сумма мёртвых кадров считается по интервалам, и оставленное перекрытие
				// вычлось бы дважды — то есть awrMs занизился бы, а это как раз направление
				// «незаслуженно быстрый рекорд». СТЫК кадр в кадр — только при совпадении
				// точки: там перекрытия нет, а есть сшивка, и её правило рвёт на смене точки.
				// Сверка точки на СТЫКЕ достижима: она срабатывает, когда вырез следующего
				// кластера начинается ровно на кадре после предыдущего — то есть когда
				// прибытие на новую точку случилось сразу за прибытием на старую, без живых
				// кадров между ними. Фикстура и числа — в тесте
				// test_adjacent_cuts_with_different_dest_not_merged (числа держим там, чтобы
				// комментарий не разъезжался с ассертами). На awrMs это не влияет (кадры те
				// же), но влияет на ЧИСЛО вырезов и на разброс точек внутри выреза
				// (maxChainDestOffset) — то есть на инвариант правила.
				//
				// Ветка ПЕРЕСЕЧЕНИЯ сегодня недостижима и стоит страховкой, это осознанно:
				// обход даёт интервалы непересекающимися (курсор строго убывает), поэтому
				// мутация «не сливать пересечения» host-тестами не ловится — как и мутация
				// awr_interval_overflow. Убирать её нельзя: она держит арифметику
				// (перекрытие вычлось бы дважды, awrMs занизился бы).
				const bool overlap = merged > 0 && cuts[i].iv.from <= cuts[merged - 1].iv.to;
				const bool adjacent = merged > 0 && cuts[i].iv.from == cuts[merged - 1].iv.to + 1;
				if (overlap || (adjacent && SameDest(&cuts[i].dest.x, &cuts[merged - 1].dest.x)))
				{
					// ИНВАРИАНТ: член цепочки обязан лежать в допуске от её ЯКОРЯ (см.
					// CutResult::maxChainDestOffset). Пишем максимум здесь, потому что
					// сравнение якоря с членом происходит ровно в этой строке — O(вырезов),
					// без прохода по кадрам.
					const float offset = Dist(&cuts[i].dest.x, &cuts[merged - 1].dest.x);
					if (offset > r.maxChainDestOffset)
					{
						r.maxChainDestOffset = offset;
						r.maxChainDestOffsetCut = (uint32_t)(merged - 1);
					}
					if (cuts[i].iv.to > cuts[merged - 1].iv.to)
					{
						cuts[merged - 1].iv.to = cuts[i].iv.to;
					}
					continue;
				}
				cuts[merged++] = cuts[i];
			}
			cuts.resize(merged);
			// С этого места ноль в метрике правдив (см. CutResult::maxChainDestOffsetMeasured).
			r.maxChainDestOffsetMeasured = true;
			r.dead.clear();
			r.dead.reserve(cuts.size());
			for (const DeadCut &cut : cuts)
			{
				r.dead.push_back(cut.iv);
			}
		}

		// Мёртвое время = ЧИСЛО ЗАПИСАННЫХ КАДРОВ внутри вырезов, минус кадры, записанные на
		// паузе (пауза в time_ms уже не входит, вычесть её дважды нельзя).
		//
		// Почему кадры, а не длина интервала по serverTick (так было до 10.09): time_ms рана
		// — это ТИКИ ТАЙМЕРА, а таймер тикает только пока игрок жив, таймер идёт и не на
		// паузе (KZTimerService::OnPhysicsSimulatePost: `currentTime +=
		// ENGINE_FIXED_TICK_INTERVAL`). Рекордер за тот же тик пишет ровно один кадр — кроме
		// `!prac`, где не пишет вовсе. Значит промежуток, где записи не было, для таймера не
		// существует, а по serverTick он в вырез попадал целиком: мёртвое время оказывалось
		// БОЛЬШЕ времени рана. Живой замер на канарейке cyb.183: 32 файла из 40 отвергнуты,
		// у одного time_ms=706539 против dead_ms=720094 (awr_ms клампился в ноль). В кадрах
		// такой промежуток не даёт вклада ни в одну из величин, и класс отказов исчезает.
		uint64_t deadFrames = 0;
		for (const Interval &d : r.dead)
		{
			deadFrames += ActiveFrames(d.from, d.to, pauseList);

			// Разрывы записи внутри выреза — считаем ВСЕГДА, но теперь только как диагностику
			// (на мёртвое время они больше не влияют): по ним видно, сколько времени внутри
			// вырезов не подтверждено кадрами. Цена — проход по кадрам выреза; зовут разрез с
			// трёх сторон: рабочий поток бэкфилла (cyb_awr_backfill), загрузка реплея на
			// главном потоке (commands.cpp) и `!lead` (kz_lead.cpp) — везде разовая работа на
			// файл, в игровом такте разреза нет.
			for (uint32_t i = d.from > 0 ? d.from : 1; i <= d.to; i++)
			{
				const uint32_t prev = frames[i - 1].serverTick, cur = frames[i].serverTick;
				const uint64_t gap = cur > prev ? (uint64_t)(cur - prev) : 0;
				if (gap > r.maxRecordGapTicks)
				{
					r.maxRecordGapTicks = gap;
					r.maxRecordGapFrame = i;
				}
			}
		}
		// Метрика разрывов посчитана — с этого места ноль в ней правдив (см. CutResult).
		r.maxRecordGapMeasured = true;

		// Инвариант арифметики: мёртвых кадров не может быть больше, чем кадров в окне рана.
		// Держится это на нормализации выше (интервалы не пересекаются и лежат внутри окна),
		// а проверка ловит любую будущую ошибку подсчёта до того, как она уедет в api
		// заниженным awr_ms.
		const uint64_t windowFrames = (uint64_t)runEnd - runStart;
		if (deadFrames > windowFrames)
		{
			r.reason = "awr_interval_overflow";
			std::snprintf(r.detail, sizeof(r.detail), "dead_frames=%llu window_frames=%llu dead_n=%zu window=%u..%u", (unsigned long long)deadFrames,
						  (unsigned long long)windowFrames, r.dead.size(), runStart, runEnd);
			return r;
		}

		// Разрыв записи на результат больше не влияет, но час дыры внутри одного рана — это
		// уже не пауза, а битый или склеенный файл (см. AWR_MAX_RECORD_GAP_TICKS).
		if (r.maxRecordGapTicks > AWR_MAX_RECORD_GAP_TICKS)
		{
			r.reason = "awr_record_gap";
			std::snprintf(r.detail, sizeof(r.detail), "max_gap=%llu@%u gap_s=%.1f dead_frames=%llu time_ms=%llu dead_n=%zu",
						  (unsigned long long)r.maxRecordGapTicks, r.maxRecordGapFrame, (double)r.maxRecordGapTicks * tickInterval,
						  (unsigned long long)deadFrames, (unsigned long long)timeMs, r.dead.size());
			return r;
		}

		const uint64_t deadMs = (uint64_t)((double)deadFrames * tickInterval * 1000.0 + 0.5);
		r.awrMs = timeMs > deadMs ? timeMs - deadMs : 0;

		// СЕТЬ: результат абсурден по величине (см. AWR_MIN_LIVE_FRACTION_DIVISOR). Инвариант
		// «awrMs == 0 при timeMs > 0 — всегда отказ» держит первое слагаемое: кламп в ноль сам
		// по себе означает, что мёртвого времени насчитали больше, чем длился ран.
		if (timeMs > 0 && (deadMs >= timeMs || r.awrMs * AWR_MIN_LIVE_FRACTION_DIVISOR < timeMs))
		{
			r.reason = "awr_implausible";
			std::snprintf(r.detail, sizeof(r.detail), "dead_ms=%llu time_ms=%llu dead_n=%zu first=%u..%u last=%u..%u max_gap=%llu@%u",
						  (unsigned long long)deadMs, (unsigned long long)timeMs, r.dead.size(), r.dead.empty() ? 0 : r.dead.front().from,
						  r.dead.empty() ? 0 : r.dead.front().to, r.dead.empty() ? 0 : r.dead.back().from, r.dead.empty() ? 0 : r.dead.back().to,
						  (unsigned long long)r.maxRecordGapTicks, r.maxRecordGapFrame);
			return r;
		}
		r.ok = true;
		r.reason = "";
		return r;
	}

	uint64_t DeadFramesUpTo(const Interval *dead, uint32_t deadCount, const Interval *pauses, uint32_t pauseCount, uint32_t targetFrame)
	{
		if (!dead || deadCount == 0)
		{
			return 0;
		}
		const std::vector<Interval> pauseList = NormalizePauses(pauses, pauseCount);
		uint64_t total = 0;
		for (uint32_t i = 0; i < deadCount; i++)
		{
			// Обрезаем по цели: перемотка могла приземлиться и на середину выреза (сегодня
			// SnapSeekTargetOutOfPause этого не допускает, но полагаться на это незачем).
			if (dead[i].from > targetFrame)
			{
				continue;
			}
			total += ActiveFrames(dead[i].from, std::min(dead[i].to, targetFrame), pauseList);
		}
		return total;
	}

	std::vector<Interval> LiveIntervals(const std::vector<Interval> &dead, uint32_t count)
	{
		std::vector<Interval> live;
		if (count == 0)
		{
			return live;
		}
		uint32_t start = 0;
		for (const Interval &d : dead)
		{
			if (d.from > start)
			{
				live.push_back({start, d.from - 1});
			}
			start = d.to + 1;
		}
		if (start <= count - 1)
		{
			live.push_back({start, count - 1});
		}
		return live;
	}
} // namespace KZ::replaysystem::awr
