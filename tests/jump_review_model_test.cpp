#include "../src/kz/jumpstats/jump_review_model.h"
#include <cassert>
#include <limits>
using namespace KZ::jumpreview;

int main()
{
	std::vector<Point> p {{0, 0, 0}, {5, 50, .5}, {0, 100, 1}};
	std::vector<Span> spans {{0, .5, Shade::Good, 0}, {.5, 1, Shade::Bad, 1}};
	auto lines = Project(p, spans, 128, 370, 192);
	assert(!lines.empty());
	assert(lines.front().y > lines.back().y); // отрыв внизу, landing наверху
	bool good = false, bad = false;
	for (auto l : lines)
	{
		good |= l.shade == Shade::Good;
		bad |= l.shade == Shade::Bad;
		assert(l.x >= 0 && l.y >= 0 && l.x <= 128 && l.y <= 370);
	}
	assert(good && bad);
	assert(std::abs(lines.back().y + lines.back().dy - 4) < .01);
	// При переполнении не теряются конец/начало, смешанный цвет не становится Good.
	p.clear();
	for (int i = 0; i <= 800; i++)
	{
		p.push_back({float(i % 2) * 20, float(i), float(i) / 800});
	}
	spans.clear();
	for (int i = 0; i < 800; i++)
	{
		spans.push_back({float(i) / 800, float(i + 1) / 800, i % 2 ? Shade::Bad : Shade::Good, i % 2});
	}
	auto limited = Project(p, spans, 128, 370, 96);
	assert(!limited.empty() && limited.size() <= 96);
	for (auto l : limited)
	{
		assert(l.shade == Shade::Unknown);
	}
	assert(std::abs(limited.back().y + limited.back().dy - 4) < .01);
	// Пустые/битые данные не выходят в CSS NaN/Infinity.
	assert(Project({}, spans, 128, 370, 96).empty());
	assert(Quantize(std::numeric_limits<float>::infinity(), 2, 128) == 0);
	assert(Quantize(-12, 2, 128) == 0 && Quantize(999, 2, 128) == 128);
	// Единый масштаб по X/Y: диагональ не выпрямляется в вертикальный отрезок.
	auto diag = Project({{0, 0, 0}, {10, 40, .4}, {0, 100, 1}}, {{0, 1, Shade::Good, 0}}, 128, 370, 192);
	float scale = 362.f / 100;
	assert(std::abs(diag.front().dx / diag.front().dy + 10.f / 40) < .001);
	assert(scale > 0);
}
