#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Чистая геометрия UI. Не изменяет физику/джампстаты и не содержит SDK/ABI.
namespace KZ::jumpreview
{
	enum class Shade
	{
		Unknown,
		Good,
		Bad,
		Overlap,
		Dead
	};

	struct Point
	{
		float x = 0, y = 0, time = 0;
	};

	struct Span
	{
		float start = 0, end = 0;
		Shade shade = Shade::Unknown;
		int strafe = -1;
	};

	struct Line
	{
		float x = 0, y = 0, dx = 0, dy = 0;
		Shade shade = Shade::Unknown;
		int strafe = -1;
	};

	inline bool Finite(const Point &p)
	{
		return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.time);
	}

	inline Point At(const std::vector<Point> &points, float t)
	{
		if (points.empty())
		{
			return {};
		}
		if (t <= points.front().time)
		{
			return points.front();
		}
		auto it = std::upper_bound(points.begin(), points.end(), t, [](float v, const Point &p) { return v < p.time; });
		if (it == points.end())
		{
			return points.back();
		}
		const auto &b = *it, &a = *(it - 1);
		float f = (t - a.time) / (b.time - a.time);
		return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, t};
	}

	inline std::vector<Line> Project(std::vector<Point> points, const std::vector<Span> &spans, float width, float height, int limit)
	{
		std::vector<Line> result;
		points.erase(std::remove_if(points.begin(), points.end(), [](const Point &p) { return !Finite(p); }), points.end());
		std::stable_sort(points.begin(), points.end(), [](const Point &a, const Point &b) { return a.time < b.time; });
		points.erase(std::unique(points.begin(), points.end(), [](const Point &a, const Point &b) { return a.time == b.time; }), points.end());
		if (points.size() < 2 || limit < 4 || width < 8 || height < 8)
		{
			return result;
		}
		const float start = points.front().time, end = points.back().time;
		if (end <= start)
		{
			return result;
		}
		float fx = points.back().x - points.front().x, fy = points.back().y - points.front().y;
		float distance = std::hypot(fx, fy);
		if (distance < .01f)
		{
			fx = 0;
			fy = 1;
		}
		else
		{
			fx /= distance;
			fy /= distance;
		}
		float minx = 0, maxx = 0, miny = 0, maxy = 0;
		auto local = [&](const Point &p)
		{
			float x = p.x - points.front().x, y = p.y - points.front().y;
			return Point {x * fy - y * fx, x * fx + y * fy, p.time};
		};
		for (const auto &p : points)
		{
			auto q = local(p);
			minx = std::min(minx, q.x);
			maxx = std::max(maxx, q.x);
			miny = std::min(miny, q.y);
			maxy = std::max(maxy, q.y);
		}
		float scale = std::min((width - 8) / std::max(maxx - minx, .01f), (height - 8) / std::max(maxy - miny, .01f));
		auto screen = [&](const Point &p)
		{
			auto q = local(p);
			return Point {width / 2 + (q.x - (minx + maxx) / 2) * scale, height - 4 - (q.y - miny) * scale, p.time};
		};
		std::vector<float> cuts;
		for (const auto &p : points)
		{
			cuts.push_back(p.time);
		}
		for (const auto &s : spans)
		{
			if (s.start > start && s.start < end)
			{
				cuts.push_back(s.start);
			}
			if (s.end > start && s.end < end)
			{
				cuts.push_back(s.end);
			}
		}
		std::sort(cuts.begin(), cuts.end());
		cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
		auto shadeAt = [&](float t)
		{
			Span found;
			for (const auto &s : spans)
			{
				if (t >= s.start && t < s.end)
				{
					found = s;
					break;
				}
			}
			return found;
		};
		auto build = [&](const std::vector<float> &times, bool reduced)
		{
			std::vector<Line> lines;
			for (size_t i = 1; i < times.size(); i++)
			{
				float a = times[i - 1], b = times[i];
				auto state = shadeAt((a + b) / 2);
				float covered = 0;
				if (reduced)
				{
					for (const auto &s : spans)
					{
						if (s.start < b && s.end > a)
						{
							covered += std::max(0.f, std::min(b, s.end) - std::max(a, s.start));
							if (s.shade != state.shade || s.strafe != state.strafe)
							{
								state.shade = Shade::Unknown;
								state.strafe = -1;
							}
						}
					}
					if (covered < b - a - .00001f)
					{
						state.shade = Shade::Unknown;
						state.strafe = -1;
					}
				}
				auto p = screen(At(points, a)), q = screen(At(points, b));
				float len = std::hypot(q.x - p.x, q.y - p.y);
				if (len < .15f)
				{
					continue;
				}
				int pieces = std::max(1, static_cast<int>(std::ceil(len / 120)));
				for (int k = 0; k < pieces; k++)
				{
					lines.push_back({p.x + (q.x - p.x) * k / pieces, p.y + (q.y - p.y) * k / pieces, (q.x - p.x) / pieces, (q.y - p.y) / pieces,
									 state.shade, state.strafe});
				}
			}
			return lines;
		};
		// Не строим квадратично дорогую полную линию для очень долгого полёта.
		const bool oversized = cuts.size() > static_cast<size_t>(limit) * 8;
		if (!oversized)
		{
			result = build(cuts, false);
		}
		if (oversized || result.size() > static_cast<size_t>(limit))
		{
			// Сохраняем оба конца. Подбираем число временных интервалов под бюджет,
			// учитывая деление длинных отрезков; смешанные интервалы нейтральны.
			int count = limit;
			while (count > 0)
			{
				cuts.clear();
				for (int i = 0; i <= count; i++)
				{
					cuts.push_back(start + (end - start) * i / count);
				}
				result = build(cuts, true);
				if (result.size() <= static_cast<size_t>(limit))
				{
					break;
				}
				count = static_cast<int>(count * static_cast<size_t>(limit) / result.size()) - 1;
			}
			if (result.size() > static_cast<size_t>(limit))
			{
				return {}; // иной canvas: fail closed
			}
		}
		return result;
	}

	inline int Quantize(float v, float step, int max)
	{
		if (!std::isfinite(v))
		{
			return 0;
		}
		return std::max(0, std::min(max, static_cast<int>(std::lround(v / step))));
	}
} // namespace KZ::jumpreview
