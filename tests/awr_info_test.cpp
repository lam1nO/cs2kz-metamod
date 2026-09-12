// Host-тест таблицы решений `!awr` (src/kz/replays/cyb_awr_info.h). В сборку плагина не входит,
// гоняется руками — команда в CYBER.md рядом с awr_cut_test.
//
// Проверяется ровно одно: по каким статусам двух резолвов какая фраза уходит игроку. Регресс
// нужен потому, что ошибка здесь невидима — игрок получит «записей нет» вместо «ещё не
// посчитан» и просто не вернётся за ответом.
#include "../src/kz/replays/cyb_awr_info.h"
#include <cassert>
#include <cstdio>

using CybAwrInfo::Classify;
using CybAwrInfo::Outcome;

int main()
{
	// --- AWR нашёлся -------------------------------------------------------------------------
	assert(Classify(200, true, 0) == Outcome::Found);
	// 2xx целиком, не только 200: api за прокси может ответить 203/204-подобным кодом.
	assert(Classify(299, true, 0) == Outcome::Found);
	// wrStatus на успехе не смотрится вовсе — второго запроса в этой ветке не было.
	assert(Classify(200, true, 500) == Outcome::Found);

	// --- 2xx без времени: расхождение с контрактом, но НЕ «нашли ноль» ------------------------
	// api не отдаёт по type=awr строк с пустым awr_ms, так что сюда мы попасть не должны;
	// если попали — это «ещё не посчитан», показывать игроку 00:00.000 нельзя.
	assert(Classify(200, false, 0) == Outcome::NotComputed);

	// --- 404 по awr: решает ВТОРОЙ резолв -----------------------------------------------------
	// Файл рекордсмена на ключе есть → пул не пуст, разрез просто не дошёл.
	assert(Classify(404, false, 200) == Outcome::NotComputed);
	assert(Classify(404, false, 204) == Outcome::NotComputed);
	// На ключе нет ничего.
	assert(Classify(404, false, 404) == Outcome::NoRecord);
	// Второй резолв сам не доехал — «записей нет» утверждать нечем.
	assert(Classify(404, false, 0) == Outcome::Unavailable);
	assert(Classify(404, false, -1) == Outcome::Unavailable);
	assert(Classify(404, false, 500) == Outcome::Unavailable);
	assert(Classify(404, false, 503) == Outcome::Unavailable);

	// --- отказ НАШЕЙ стороны на первом же резолве ---------------------------------------------
	assert(Classify(0, false, 0) == Outcome::Unavailable);   // сеть/нечитаемое тело
	assert(Classify(-1, false, 0) == Outcome::Unavailable);  // запроса не было (резолв выключен)
	assert(Classify(500, false, 0) == Outcome::Unavailable); // api сломался
	assert(Classify(401, false, 0) == Outcome::Unavailable); // токен сервера протух
	assert(Classify(400, false, 200) == Outcome::Unavailable);
	// 4xx, кроме 404, второй резолв НЕ уточняет: 400 значит «мы спросили неправильно», и
	// ответ «записей нет» тут был бы ложью о содержимом платформы.
	assert(Classify(403, false, 404) == Outcome::Unavailable);

	printf("awr_info_test: OK\n");
	return 0;
}
