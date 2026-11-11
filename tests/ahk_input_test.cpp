#include "../src/kz/analytics/ahk_input.h"
#include <nlohmann/json.hpp>
#include <cassert>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstring>
using namespace KZ::analytics;

int main(int argc, char **argv)
{
	std::ifstream in("tests/fixtures/ahk_input_v2.json");
	nlohmann::json fixture;
	in >> fixture;
	std::vector<std::pair<std::vector<InputStrafe>, std::vector<InputAA>>> benchmarkCases;
	for (const auto &c : fixture["cases"])
	{
		std::vector<InputStrafe> s;
		std::vector<InputAA> aa;
		for (const auto &x : c["strafes"])
		{
			s.push_back({x["width"], x["duration"], x["hasArStats"], x["arMedian"], x["arAverage"]});
		}
		for (const auto &x : c["aaCalls"])
		{
			aa.push_back({x["strafeIndex"], x["prevYaw"], x["currentYaw"], x["duration"], x["buttons"][0].get<uint64_t>()});
		}
		benchmarkCases.emplace_back(s, aa);
		auto f = InputFeatures(s, aa);
		if (!f)
		{
			std::cerr << c["name"] << " rejected\n";
			return 1;
		}
		for (size_t i = 0; i < 20; ++i)
		{
			if (std::abs((*f)[i] - c["features"][i].get<double>()) > 1e-6)
			{
				std::cerr << c["name"] << " feature " << i << "\n";
				return 1;
			}
		}
		assert(std::abs(InputScore(*f) - c["score"].get<double>()) < 1e-10);
		assert((InputScore(*f) >= AHK_INPUT_THRESHOLD) == c["flagged"].get<bool>());
		aa[0].buttons = 0x80;
		assert(!InputFeatures(s, aa));
		aa[0].buttons = 0;
		assert(InputFeatures(s, aa)); // No movement button is not a turn bind.
		aa[0].buttons = 16;
		assert(InputFeatures(s, aa)); // -W/backward is not a turn bind.
		assert(!InputFeatures(s, aa, false));
		assert(!InputFeatures(s, aa, true, true));
		aa[0].currentYaw = std::nan("");
		assert(!InputFeatures(s, aa));
	}
	assert(!InputFeatures({}, {}));
	std::vector<InputStrafe> linearStrafes(5, {10, .1, false, 0, 0});
	std::vector<InputAA> linearCalls;
	for (int strafe = 1; strafe < 4; ++strafe)
	{
		for (int tick = 0; tick < 10; ++tick)
		{
			linearCalls.push_back({strafe, double(tick), double(tick + 1), .01, 0x80});
		}
	}
	assert(!InputFeatures(linearStrafes, linearCalls)); // Sustained linear legal turn-bind input is excluded.
	for (auto &call : linearCalls)
	{
		call.buttons = 0;
	}
	assert(InputFeatures(linearStrafes, linearCalls));
	linearCalls.resize(19);
	assert(!InputFeatures(linearStrafes, linearCalls));
	linearStrafes.resize(4);
	assert(!InputFeatures(linearStrafes, linearCalls));
	InputFeatureVector malformed {};
	malformed[0] = std::nan("");
	assert(InputScore(malformed) == 0);

	if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0)
	{
		volatile double checksum = 0;
		for (size_t i = 0; i < benchmarkCases.size(); ++i)
		{
			const auto &c = benchmarkCases[i];
			auto start = std::chrono::steady_clock::now();
			for (int repeat = 0; repeat < 10000; ++repeat)
			{
				auto f = InputFeatures(c.first, c.second);
				assert(f);
				checksum = checksum + InputScore(*f);
			}
			double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 10000;
			std::cout << "benchmark " << fixture["cases"][i]["name"] << " strafes=" << c.first.size() << " aa=" << c.second.size()
					  << " iterations=10000 mean_us=" << us << '\n';
		}
		std::cout << "benchmark checksum=" << checksum << '\n';
	}
	std::cout << fixture["cases"].size() << " AHK input golden cases passed\n";
}
