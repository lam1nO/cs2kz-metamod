#include "../src/kz/profile/cyb_rank_summary.h"

#include <cassert>

int main()
{
	using CybRankSummary::Parse;

	// На боевой лестнице это Master, хотя старая таблица VNL давала Legend с 6000.
	const auto master = Parse(R"({"totalPoints":36611,"rankIndex":21,"rankName":"Master","rankNextPoints":41300})");
	assert(master && master->points == 36611 && master->index == 21 && master->name == "Master");
	assert(master->nextPoints == 41300);

	const auto newPlayer = Parse(R"({"totalPoints":0,"rankIndex":0,"rankName":"New","rankNextPoints":1})");
	assert(newPlayer && newPlayer->points == 0 && newPlayer->index == 0 && newPlayer->name == "New");
	assert(newPlayer->nextPoints == 1);

	const auto legend = Parse(R"({"totalPoints":50000,"rankIndex":22,"rankName":"Legend","rankNextPoints":null})");
	assert(legend && legend->index == 22 && !legend->nextPoints);

	const auto oldSummary = Parse(R"({"totalPoints":1,"rankIndex":1,"rankName":"Beginner-"})");
	assert(oldSummary && oldSummary->name == "Beginner-" && !oldSummary->nextPoints);

	assert(!Parse(R"({"points":36611})"));
	assert(!Parse("null"));
	assert(!Parse("{"));
	assert(!Parse(R"({"totalPoints":-1,"rankIndex":1,"rankName":"Beginner-"})"));
	assert(!Parse(R"({"totalPoints":2147483648,"rankIndex":1,"rankName":"Beginner-"})"));
	assert(!Parse(R"({"totalPoints":1,"rankIndex":1.5,"rankName":"Beginner-"})"));
	assert(!Parse(R"({"totalPoints":1,"rankIndex":23,"rankName":"Beginner-"})"));
	assert(!Parse(R"({"totalPoints":1,"rankIndex":1,"rankName":"{red}Beginner-"})"));
	assert(!Parse(R"({"totalPoints":1,"rankIndex":1,"rankName":"Beginner-","rankNextPoints":1})"));
	assert(!Parse(R"({"totalPoints":50000,"rankIndex":22,"rankName":"Legend","rankNextPoints":50001})"));
}
