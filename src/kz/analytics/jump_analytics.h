#pragma once
#include "ahk_input.h"
#include <array>
#include <vector>
#include <deque>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <locale>

namespace KZ::analytics
{
	struct JumpRecord
	{
		std::string id;
		int endTick {}, segment {};
		double distance {}, airtimeMs {}, sync {}, takeoffYaw {}, landingYaw {};
		std::array<double, 3> takeoff {}, landing {};
		std::vector<double> widthsDeg, durationsMs;
		std::array<double, 32> yawSamples {};
		bool hasTurnBind {}, valid {}, inPrac {};
		std::optional<InputFeatureVector> inputFeatures;
	};

	inline double WrapYaw(double yaw)
	{
		yaw = std::fmod(yaw + 180.0, 360.0);
		if (yaw < 0)
		{
			yaw += 360.0;
		}
		return yaw - 180.0;
	}

	inline bool Complete(const JumpRecord &j)
	{
		auto bounded = [](double n, double limit) { return std::isfinite(n) && std::abs(n) <= limit; };
		if (j.id.empty() || j.id.size() > 96 || j.endTick < 0 || j.segment < 0 || !bounded(j.distance, 1000000) || j.distance < 0
			|| !bounded(j.airtimeMs, 3600000) || j.airtimeMs < 0 || !bounded(j.sync, 100) || j.sync < 0 || !bounded(j.takeoffYaw, 180)
			|| !bounded(j.landingYaw, 180) || j.widthsDeg.empty() || j.widthsDeg.size() > 64 || j.widthsDeg.size() != j.durationsMs.size())
		{
			return false;
		}
		for (double n : j.takeoff)
		{
			if (!bounded(n, 1000000))
			{
				return false;
			}
		}
		for (double n : j.landing)
		{
			if (!bounded(n, 1000000))
			{
				return false;
			}
		}
		for (double n : j.yawSamples)
		{
			if (!bounded(n, 180))
			{
				return false;
			}
		}
		for (double n : j.widthsDeg)
		{
			if (!bounded(n, 1000000) || n < 0)
			{
				return false;
			}
		}
		for (double n : j.durationsMs)
		{
			if (!bounded(n, 3600000) || n < 0)
			{
				return false;
			}
		}
		return true;
	}

	inline bool Informative(const JumpRecord &j)
	{
		if (!Complete(j) || !j.valid || j.inPrac || j.hasTurnBind || j.widthsDeg.size() < 3 || j.airtimeMs < 200)
		{
			return false;
		}
		auto range = std::minmax_element(j.yawSamples.begin(), j.yawSamples.end());
		if (*range.second - *range.first < 3)
		{
			return false;
		}
		double minDelta = 360, maxDelta = -360;
		for (size_t i = 1; i < j.yawSamples.size(); ++i)
		{
			double delta = WrapYaw(j.yawSamples[i] - j.yawSamples[i - 1]);
			minDelta = std::min(minDelta, delta);
			maxDelta = std::max(maxDelta, delta);
		}
		return maxDelta - minDelta > 0.015;
	}

	inline bool Matches(const JumpRecord &a, const JumpRecord &b)
	{
		if (a.widthsDeg.size() != b.widthsDeg.size())
		{
			return false;
		}
		for (size_t i = 0; i < a.widthsDeg.size(); ++i)
		{
			if (std::abs(a.widthsDeg[i] - b.widthsDeg[i]) > 0.015 || std::abs(a.durationsMs[i] - b.durationsMs[i]) > 2)
			{
				return false;
			}
		}
		for (size_t i = 0; i < 32; ++i)
		{
			if (std::abs(WrapYaw(a.yawSamples[i] - b.yawSamples[i])) > 0.015)
			{
				return false;
			}
		}
		return true;
	}

	inline bool Separated(const JumpRecord &a, const JumpRecord &b)
	{
		double squared = 0;
		for (size_t i = 0; i < 3; ++i)
		{
			squared += (a.takeoff[i] - b.takeoff[i]) * (a.takeoff[i] - b.takeoff[i]);
		}
		return squared >= 32 * 32;
	}

	inline bool Diverse(const std::vector<const JumpRecord *> &cluster)
	{
		for (size_t a = 0; a < cluster.size(); ++a)
		{
			for (size_t b = a + 1; b < cluster.size(); ++b)
			{
				for (size_t c = b + 1; c < cluster.size(); ++c)
				{
					if (Separated(*cluster[a], *cluster[b]) && Separated(*cluster[a], *cluster[c]) && Separated(*cluster[b], *cluster[c]))
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	inline bool Suspicious(const std::deque<JumpRecord> &window)
	{
		std::vector<std::string> positiveIds;
		std::vector<int> positiveTicks;
		for (const auto &j : window)
		{
			if (!Complete(j) || !j.valid || j.inPrac || j.hasTurnBind || j.widthsDeg.size() < 5 || j.airtimeMs <= 0 || j.airtimeMs > 60000
				|| !j.inputFeatures || InputScore(*j.inputFeatures) < AHK_INPUT_THRESHOLD
				|| std::find(positiveIds.begin(), positiveIds.end(), j.id) != positiveIds.end()
				|| std::find(positiveTicks.begin(), positiveTicks.end(), j.endTick) != positiveTicks.end())
			{
				continue;
			}
			positiveIds.push_back(j.id);
			positiveTicks.push_back(j.endTick);
			if (positiveIds.size() >= 2)
			{
				return true;
			}
		}
		return false;
	}

	struct State
	{
		std::deque<JumpRecord> recent;
		std::vector<JumpRecord> run;
		bool active {}, usedPrac {}, truncated {};
		int segment {};
		double lastAlert = -1;

		void Reset()
		{
			*this = State {};
		}

		void Start()
		{
			run.clear();
			active = true;
			usedPrac = false;
			truncated = false;
			segment = 0;
		}

		void Stop()
		{
			active = false;
			run.clear();
		}

		void EnterPrac()
		{
			if (active)
			{
				usedPrac = true;
			}
			recent.clear();
		}

		void Teleport()
		{
			++segment;
			// Retry teleports separate MMR segments; valid input evidence survives.
		}

		void OmitJump()
		{
			if (active)
			{
				truncated = true;
			}
			// Invalid attempts do not erase the bounded last-20-valid-jump input window.
		}

		void Add(JumpRecord j, bool captureRun = true)
		{
			if (!Complete(j))
			{
				OmitJump();
				return;
			}
			if (std::any_of(recent.begin(), recent.end(), [&](const JumpRecord &old) { return old.id == j.id || old.endTick == j.endTick; }))
			{
				return;
			}
			j.segment = segment;
			if (active && captureRun)
			{
				usedPrac = usedPrac || j.inPrac;
				if (run.size() < 1024)
				{
					run.push_back(j);
				}
				else
				{
					truncated = true;
				}
			}
			if (!j.valid || j.inPrac)
			{
				if (j.inPrac)
				{
					recent.clear();
				}
				return;
			}
			recent.push_back(std::move(j));
			if (recent.size() > 20)
			{
				recent.pop_front();
			}
		}

		bool Finish()
		{
			bool finished = active;
			active = false;
			return finished;
		}

		bool ShouldAlert(double now)
		{
			if ((lastAlert >= 0 && now - lastAlert < 1800) || !Suspicious(recent))
			{
				return false;
			}
			lastAlert = now;
			return true;
		}
	};

	inline std::string JsonString(const std::string &value)
	{
		std::string result = "\"";
		for (unsigned char c : value)
		{
			switch (c)
			{
				case '"':
					result += "\\\"";
					break;
				case '\\':
					result += "\\\\";
					break;
				case '\n':
					result += "\\n";
					break;
				case '\r':
					result += "\\r";
					break;
				case '\t':
					result += "\\t";
					break;
				default:
					if (c < 32)
					{
						const char *hex = "0123456789abcdef";
						result += "\\u00";
						result += hex[c >> 4];
						result += hex[c & 15];
					}
					else
					{
						result += char(c);
					}
			}
		}
		return result + "\"";
	}

	template<class Values>
	inline void Numbers(std::ostream &out, const Values &values)
	{
		out << '[';
		bool first = true;
		for (double n : values)
		{
			if (!first)
			{
				out << ',';
			}
			first = false;
			out << n;
		}
		out << ']';
	}

	inline std::string JumpJson(const JumpRecord &j, bool includeInputFeatures = false)
	{
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out << std::setprecision(includeInputFeatures ? 17 : 9) << "{\"id\":" << JsonString(j.id) << ",\"endTick\":" << j.endTick
			<< ",\"segment\":" << j.segment << ",\"distance\":" << j.distance << ",\"airtimeMs\":" << j.airtimeMs << ",\"sync\":" << j.sync
			<< ",\"takeoff\":";
		Numbers(out, j.takeoff);
		out << ",\"landing\":";
		Numbers(out, j.landing);
		out << ",\"takeoffYaw\":" << j.takeoffYaw << ",\"landingYaw\":" << j.landingYaw << ",\"widthsDeg\":";
		Numbers(out, j.widthsDeg);
		out << ",\"durationsMs\":";
		Numbers(out, j.durationsMs);
		out << ",\"yawSamples\":";
		Numbers(out, j.yawSamples);
		out << ",\"hasTurnBind\":" << (j.hasTurnBind ? "true" : "false") << ",\"valid\":" << (j.valid ? "true" : "false")
			<< ",\"inPrac\":" << (j.inPrac ? "true" : "false");
		if (includeInputFeatures)
		{
			out << ",\"inputFeatures\":";
			if (j.inputFeatures)
			{
				Numbers(out, *j.inputFeatures);
			}
			else
			{
				out << "null";
			}
		}
		out << '}';
		return out.str();
	}
} // namespace KZ::analytics
