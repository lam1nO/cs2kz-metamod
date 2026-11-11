#pragma once
#include "ahk_input_model_data.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <optional>
#include <vector>

namespace KZ::analytics
{
	struct InputStrafe
	{
		double width, duration;
		bool hasArStats;
		double arMedian, arAverage;
	};

	struct InputAA
	{
		int strafeIndex;
		double prevYaw, currentYaw, duration;
		uint64_t buttons;
	};

	using InputFeatureVector = std::array<double, 20>;

	namespace input_detail
	{
		inline double Mean(const std::vector<double> &v)
		{
			return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
		}

		inline double Quantile(std::vector<double> v, double p)
		{
			std::sort(v.begin(), v.end());
			double x = (v.size() - 1) * p;
			size_t i = size_t(x);
			return v[i] + (v[std::min(i + 1, v.size() - 1)] - v[i]) * (x - i);
		}

		inline double CV(const std::vector<double> &v)
		{
			double m = Mean(v), sum = 0;
			for (double x : v)
			{
				sum += (x - m) * (x - m);
			}
			return std::sqrt(sum / v.size()) / m;
		}

		inline double MAD(const std::vector<double> &v)
		{
			double m = Quantile(v, .5);
			std::vector<double> d;
			for (double x : v)
			{
				d.push_back(std::abs(x - m));
			}
			return Quantile(d, .5) / m;
		}

		inline bool Bounded(double x, double limit)
		{
			return std::isfinite(x) && std::abs(x) <= limit;
		}
	} // namespace input_detail

	// Same ordered features as the replay-trained frozen v2 model. Durations are seconds.
	inline std::optional<InputFeatureVector> InputFeatures(const std::vector<InputStrafe> &s, const std::vector<InputAA> &aa, bool valid = true,
														   bool inPrac = false)
	{
		using namespace input_detail;
		if (!valid || inPrac || s.size() < 5 || s.size() > 64 || aa.empty() || aa.size() > 65536)
		{
			return std::nullopt;
		}
		for (const auto &x : s)
		{
			if (!Bounded(x.width, 1000000) || x.width < 0 || !Bounded(x.duration, 3600) || x.duration < 0
				|| (x.hasArStats && (!Bounded(x.arMedian, 1000000) || !Bounded(x.arAverage, 1000000))))
			{
				return std::nullopt;
			}
		}
		for (const auto &x : aa)
		{
			if (x.buttons & 0x180 || x.strafeIndex < 0 || size_t(x.strafeIndex) >= s.size() || !Bounded(x.prevYaw, 1000000)
				|| !Bounded(x.currentYaw, 1000000) || !Bounded(x.duration, 3600) || x.duration < 0)
			{
				return std::nullopt;
			}
		}
		std::vector<double> rates, dur, width, arsMedian, arsAverage;
		for (size_t i = 1; i + 1 < s.size(); ++i)
		{
			if (s[i].duration <= 0)
			{
				return std::nullopt;
			}
			rates.push_back(s[i].width / s[i].duration);
			dur.push_back(s[i].duration);
			width.push_back(s[i].width);
			if (s[i].hasArStats)
			{
				arsMedian.push_back(std::abs(s[i].arMedian));
				arsAverage.push_back(std::abs(s[i].arAverage));
			}
		}
		double mean = Mean(rates);
		if (!std::isfinite(mean) || mean < 1)
		{
			return std::nullopt;
		}
		double slopeNum = 0, slopeDen = 0, center = (rates.size() - 1) / 2.0, adjacent = 0;
		for (size_t i = 0; i < rates.size(); ++i)
		{
			slopeNum += (i - center) * (rates[i] - mean);
			slopeDen += (i - center) * (i - center);
			if (i)
			{
				adjacent += std::abs(rates[i] - rates[i - 1]);
			}
		}
		std::vector<const InputAA *> selected;
		std::vector<double> vel;
		for (const auto &x : aa)
		{
			if (x.strafeIndex > 0 && size_t(x.strafeIndex) + 1 < s.size() && x.duration > 1e-6)
			{
				double delta = std::fmod(x.currentYaw - x.prevYaw + 180, 360);
				if (delta < 0)
				{
					delta += 360;
				}
				selected.push_back(&x);
				vel.push_back(std::abs(delta - 180) / x.duration);
			}
		}
		if (vel.size() < 20)
		{
			return std::nullopt;
		}
		double vmean = Mean(vel), vmed = Quantile(vel, .5);
		if (vmean <= 1 || vmed <= 1)
		{
			return std::nullopt;
		}
		size_t zero = 0, dominant = 0;
		std::map<long long, size_t> bins;
		for (double v : vel)
		{
			zero += v < 1; // nearbyint implements NumPy's ties-to-even rounding in the default rounding mode.
			dominant = std::max(dominant, ++bins[static_cast<long long>(std::nearbyint(v / vmed * 50))]);
		}
		std::vector<double> per, distances;
		std::vector<std::array<double, 12>> shapes;
		for (size_t i = 1; i + 1 < s.size(); ++i)
		{
			std::vector<double> vs, pos;
			double elapsed = 0;
			for (size_t k = 0; k < selected.size(); ++k)
			{
				if (selected[k]->strafeIndex == int(i))
				{
					elapsed += selected[k]->duration;
					pos.push_back(elapsed);
					vs.push_back(vel[k]);
				}
			}
			if (vs.size() < 4 || Mean(vs) <= 1)
			{
				continue;
			}
			per.push_back(CV(vs));
			double start = pos.front();
			for (double &p : pos)
			{
				p -= start;
			}
			if (pos.back() <= 0)
			{
				continue;
			}
			std::array<double, 12> shape {};
			double vm = Mean(vs);
			for (size_t k = 0; k < 12; ++k)
			{
				double target = pos.back() * k / 11.;
				auto next = std::upper_bound(pos.begin(), pos.end(), target);
				size_t hi = next - pos.begin();
				double value = vs.back();
				if (hi < pos.size())
				{
					size_t lo = hi ? hi - 1 : 0;
					value = vs[lo] + (vs[hi] - vs[lo]) * (target - pos[lo]) / (pos[hi] - pos[lo]);
				}
				shape[k] = value / vm;
			}
			shapes.push_back(shape);
		}
		for (size_t i = 1; i < shapes.size(); ++i)
		{
			double d = 0;
			for (size_t k = 0; k < 12; ++k)
			{
				d += std::abs(shapes[i][k] - shapes[i - 1][k]);
			}
			distances.push_back(d / 12);
		}
		std::vector<std::pair<int, double>> collapsed;
		for (size_t i = 0; i < vel.size(); ++i)
		{
			if (collapsed.empty() || selected[i]->strafeIndex != collapsed.back().first || std::abs(vel[i] - collapsed.back().second) > .01)
			{
				collapsed.emplace_back(selected[i]->strafeIndex, vel[i]);
			}
		}
		std::vector<double> diffs;
		for (size_t i = 1; i < collapsed.size(); ++i)
		{
			if (collapsed[i].first == collapsed[i - 1].first)
			{
				diffs.push_back(std::abs(collapsed[i].second - collapsed[i - 1].second));
			}
		}
		InputFeatureVector f {{diffs.empty() ? 0 : Quantile(diffs, .5) / vmed,
							   double(dominant) / vel.size(),
							   CV(vel),
							   (Quantile(vel, .75) - Quantile(vel, .25)) / vmed,
							   MAD(vel),
							   distances.empty() ? 1 : Quantile(distances, .5),
							   per.empty() ? 1 : Quantile(per, .5),
							   double(collapsed.size()) / vel.size(),
							   double(zero) / vel.size(),
							   arsAverage.empty() ? 1 : Mean(arsAverage),
							   arsMedian.empty() ? 1 : Mean(arsMedian),
							   CV(dur),
							   MAD(dur),
							   adjacent / (rates.size() - 1) / mean,
							   CV(rates),
							   MAD(rates),
							   (*std::max_element(rates.begin(), rates.end()) - *std::min_element(rates.begin(), rates.end())) / mean,
							   std::abs(slopeNum / slopeDen) * rates.size() / mean,
							   CV(width),
							   MAD(width)}};
		for (double x : f)
		{
			if (!Bounded(x, 1000000) || x < 0)
			{
				return std::nullopt;
			}
		}
		return f;
	}

	inline double InputScore(const InputFeatureVector &features)
	{
		std::array<float, 20> f {};
		for (size_t i = 0; i < f.size(); ++i)
		{
			if (!input_detail::Bounded(features[i], 1000000) || features[i] < 0)
			{
				return 0;
			}
			f[i] = static_cast<float>(features[i]);
		}
		double score = 0;
		for (int root : AHK_INPUT_ROOTS)
		{
			int i = root;
			while (AHK_INPUT_NODES[i].left >= 0)
			{
				const auto &node = AHK_INPUT_NODES[i];
				i = f[node.feature] <= node.threshold ? node.left : node.right;
			}
			score += AHK_INPUT_NODES[i].positiveWeight;
		}
		return score / AHK_INPUT_ROOTS.size();
	}
} // namespace KZ::analytics
