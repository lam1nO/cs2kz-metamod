#include "../src/kz/analytics/jump_analytics.h"
#include <cassert>
#include <iostream>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
using namespace KZ::analytics;

JumpRecord Wave(int n)
{
	JumpRecord j;
	j.id = "synthetic-" + std::to_string(n);
	j.valid = true;
	j.endTick = 100 + n * 64;
	j.distance = 250;
	j.airtimeMs = 600;
	j.sync = 90;
	j.takeoff = {double(n * 64), 0, 0};
	j.landing = {double(n * 64 + 250), 0, 0};
	j.takeoffYaw = 45;
	j.landingYaw = 50;
	j.widthsDeg = {8, 12, 8};
	j.durationsMs = {200, 200, 200};
	for (int k = 0; k < 32; ++k)
	{
		j.yawSamples[k] = std::sin(k * 0.4) * 8;
	}
	return j;
}

JumpRecord FixtureJump(int event, int index)
{
	JumpRecord jump = Wave(index);
	jump.id = "synthetic-" + std::to_string(event * 6 + index);
	jump.endTick += event * 1000;
	return jump;
}

void Fixture()
{
	const std::string meta = "\"playerName\":\"Synthetic "
							 "jumper\",\"mapName\":\"kz_synthetic\",\"mapHash\":\"11111111111111111111111111111111\",\"physicsHash\":"
							 "\"22222222222222222222222222222222\",\"mode\":\"ckz\",\"styles\":[]";
	std::cout << "{\"events\":[";
	for (int event = 0; event < 4; ++event)
	{
		if (event)
		{
			std::cout << ',';
		}
		std::cout << "{\"v\":1,\"id\":\"019a6d01-0000-7000-8000-00000000000" << event + 1
				  << "\",\"type\":" << JsonString(event == 0 ? "kz.ahk_suspected" : "kz.movement_run_finished")
				  << ",\"typeV\":1,\"at\":\"2026-11-10T10:00:00Z\",\"serverId\":\"srv-5\",\"actor\":{\"steamId64\":"
				  << JsonString(event == 3 ? "76561198000000002" : "76561198000000001") << "},\"data\":{" << meta;
		if (event == 0)
		{
			std::cout << ",\"detectorVersion\":\"ahk-jump-v1\"";
		}
		else
		{
			std::cout << ",\"runId\":\"019a6d02-0000-7000-8000-00000000000" << event
					  << "\",\"course\":0,\"timeMs\":12000,\"finished\":true,\"usedPrac\":false,\"truncated\":false";
		}
		std::cout << ",\"jumps\":[";
		for (int i = 0; i < 6; ++i)
		{
			if (i)
			{
				std::cout << ',';
			}
			std::cout << JumpJson(FixtureJump(event, i));
		}
		std::cout << "]}}";
	}
	std::cout << "]}\n";
}

int main(int argc, char **argv)
{
	InputFeatureVector positive {};
	bool foundPositive = false;
	// Use the fixture's actual vector to preserve provenance for decision tests.
	std::ifstream in("tests/fixtures/ahk_input_v2.json");
	nlohmann::json fixture;
	in >> fixture;
	for (const auto &c : fixture["cases"])
	{
		if (c["flagged"].get<bool>())
		{
			positive = c["features"].get<InputFeatureVector>();
			foundPositive = true;
			break;
		}
	}
	assert(foundPositive);
	auto jump = [&](int n)
	{
		auto j = Wave(n);
		j.inputFeatures = positive;
		j.widthsDeg.resize(5, 8);
		j.durationsMs.resize(5, 100);
		return j;
	};
	State shortJumps;
	auto shortFirst = jump(100);
	auto shortSecond = jump(101);
	shortFirst.airtimeMs = shortSecond.airtimeMs = 150;
	shortJumps.Add(shortFirst);
	shortJumps.Add(shortSecond);
	assert(shortJumps.ShouldAlert(100));
	State state;
	state.Start();
	state.Add(jump(0));
	assert(!state.ShouldAlert(100));
	state.Add(jump(0));
	assert(!state.ShouldAlert(100)); // Duplicate callback cannot supply a second physical jump.
	auto duplicateTick = jump(0);
	duplicateTick.id = "different-id-same-physical-jump";
	state.Add(duplicateTick);
	assert(state.recent.size() == 1 && state.run.size() == 1 && !state.ShouldAlert(100));
	state.Teleport();
	assert(state.recent.size() == 1);
	assert(state.segment == 1);
	state.OmitJump();
	assert(state.recent.size() == 1 && state.truncated);
	auto bad = jump(10);
	bad.valid = false;
	state.Add(bad);
	assert(state.recent.size() == 1);
	auto second = jump(1);
	second.takeoff = state.recent.front().takeoff;
	state.Add(second);
	assert(state.ShouldAlert(100));
	assert(!state.ShouldAlert(101));
	assert(!state.ShouldAlert(1899));
	assert(state.ShouldAlert(1900));
	assert(state.run[0].segment == 0 && state.run.back().segment == 1);
	auto legacy = JumpJson(jump(0));
	assert(legacy.find("inputFeatures") == std::string::npos);
	auto v2 = JumpJson(jump(0), true);
	assert(v2.find("inputFeatures") != std::string::npos);
	assert(nlohmann::json::parse(v2)["inputFeatures"].get<InputFeatureVector>() == positive);
	auto noFeatures = Wave(0);
	assert(JumpJson(noFeatures, true).find("\"inputFeatures\":null") != std::string::npos);
	state.OmitJump();
	state.Add(jump(20), false);
	assert(state.truncated && state.recent.size() == 3);
	assert(state.run.back().id != "synthetic-20");
	state.EnterPrac();
	assert(state.usedPrac && state.recent.empty());
	assert(state.Finish());
	assert(!state.Finish());
	state.Reset();
	state.Start();
	for (int i = 0; i < 1025; ++i)
	{
		state.Add(jump(i));
	}
	assert(state.run.size() == 1024 && state.truncated && state.recent.size() == 20);
	state.Stop();
	assert(!state.Finish() && state.run.empty());
	state.Reset();
	state.Add(jump(0));
	for (int i = 1; i <= 20; ++i)
	{
		state.Add(Wave(i));
	}
	assert(!state.ShouldAlert(3000));
	state.Reset();
	auto turn = jump(0);
	turn.hasTurnBind = true;
	state.Add(turn);
	state.Add(jump(1));
	assert(!state.ShouldAlert(3000));
	auto prac = jump(2);
	prac.inPrac = true;
	state.Add(prac);
	assert(state.recent.empty());
	auto wide = jump(0);
	wide.widthsDeg.resize(18, 8);
	wide.durationsMs.resize(18, 100);
	assert(Complete(wide));
	assert(WrapYaw(181) == -179 && WrapYaw(-181) == 179);
	if (argc == 2 && std::strcmp(argv[1], "--fixture") == 0)
	{
		Fixture();
	}
	else
	{
		std::cout << "jump analytics tests passed\n";
	}
}
