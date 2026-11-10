#include "../src/kz/analytics/jump_analytics.h"
#include <cassert>
#include <iostream>
#include <cstring>
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
	for (int event = 2; event <= 3; ++event)
	{
		for (int index = 0; index < 6; ++index)
		{
			auto original = FixtureJump(1, index);
			auto copy = FixtureJump(event, index);
			assert(original.takeoff == copy.takeoff && original.landing == copy.landing);
			assert(original.takeoffYaw == copy.takeoffYaw && original.landingYaw == copy.landingYaw);
			assert(original.widthsDeg == copy.widthsDeg && original.durationsMs == copy.durationsMs);
			assert(original.yawSamples == copy.yawSamples && original.airtimeMs == copy.airtimeMs);
			assert(original.id != copy.id && original.endTick != copy.endTick);
		}
	}
	std::deque<JumpRecord> wave;
	for (int i = 0; i < 6; ++i)
	{
		wave.push_back(Wave(i));
	}
	assert(Suspicious(wave));
	auto reject = [&](auto change)
	{
		auto negative = wave;
		for (auto &j : negative)
		{
			change(j);
		}
		assert(!Suspicious(negative));
	};
	reject([](auto &j) { j.yawSamples.fill(0); });
	reject(
		[](auto &j)
		{
			for (int k = 0; k < 32; ++k)
			{
				j.yawSamples[k] = k * 0.25;
			}
		});
	reject([](auto &j) { j.hasTurnBind = true; });
	reject([](auto &j) { j.valid = false; });
	reject([](auto &j) { j.inPrac = true; });
	reject([](auto &j) { j.takeoff = {0, 0, 0}; });
	reject(
		[](auto &j)
		{
			j.widthsDeg.resize(17, 8);
			j.durationsMs.resize(17, 200);
		});
	reject([](auto &j) { j.airtimeMs = 199; });
	auto differing = wave;
	differing.back().widthsDeg[0] += .016;
	assert(!Suspicious(differing));
	differing = wave;
	differing.back().durationsMs[0] += 2.01;
	assert(!Suspicious(differing));
	differing = wave;
	differing.back().yawSamples[5] += .016;
	assert(!Suspicious(differing));
	State state;
	state.Add(Wave(0));
	assert(!state.Finish());
	state.Start();
	state.Add(Wave(0));
	state.Teleport();
	state.Add(Wave(1));
	assert(state.run[0].segment == 0 && state.run[1].segment == 1);
	state.EnterPrac();
	assert(state.usedPrac);
	assert(state.Finish());
	assert(!state.Finish());
	state.Start();
	for (int i = 0; i < 1025; ++i)
	{
		state.Add(Wave(i));
	}
	assert(state.run.size() == 1024 && state.truncated && state.recent.size() == 20);
	state.Stop();
	assert(!state.Finish() && state.run.empty());
	state.Start();
	state.OmitJump();
	assert(state.truncated);
	state.Reset();
	assert(state.run.empty() && state.recent.empty() && !state.truncated && !state.active);
	state.Start();
	for (int i = 0; i < 6; ++i)
	{
		state.Add(Wave(i));
	}
	assert(state.ShouldAlert(100));
	assert(!state.ShouldAlert(101));
	assert(!state.ShouldAlert(1899));
	assert(state.ShouldAlert(1900));
	state.Start();
	state.Add(Wave(0), false);
	assert(state.run.empty() && state.active);
	auto malformed = Wave(0);
	malformed.sync = std::nan("");
	state.Add(malformed);
	assert(state.run.empty() && state.truncated);
	state.Reset();
	for (int i = 0; i < 5; ++i)
	{
		state.Add(Wave(i));
	}
	assert(!state.ShouldAlert(5000));
	for (int i = 0; i < 21; ++i)
	{
		auto j = Wave(i);
		j.yawSamples.fill(0);
		state.Add(j);
	}
	assert(state.recent.size() == 20 && !state.ShouldAlert(6000));
	assert(WrapYaw(181) == -179 && WrapYaw(-181) == 179);
	assert(JsonString("a\n\t\\\"\x01") == "\"a\\n\\t\\\\\\\"\\u0001\"");
	if (argc == 2 && std::strcmp(argv[1], "--fixture") == 0)
	{
		Fixture();
	}
	else
	{
		std::cout << "jump analytics tests passed\n";
	}
}
