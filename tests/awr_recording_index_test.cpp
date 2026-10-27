#include "../src/kz/replays/awr_cut.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace KZ::replaysystem::awr;

// Input numbers are the on-disk GetCurrentCpIndex() values, not Frame indices.
static Frame Recorded(uint32_t tick, int32_t number, int32_t count, int32_t teleports, float x)
{
	Frame f {};
	f.serverTick = tick;
	f.cpIndex = CheckpointIndexFromRecording(number);
	f.cpCount = count;
	f.tpCount = teleports;
	f.origin[0] = x;
	return f;
}

static void FillPre(std::vector<Frame> &v)
{
	for (size_t i = 0; i < v.size(); i++)
	{
		v[i].preOrigin[0] = v[i ? i - 1 : 0].origin[0];
	}
}

// Regression: an attempt revisits the CP's 16-unit radius before teleporting.
// Coordinate fallback would keep the failed attempt up to that late visit.
static void CheckFailedAttempt(bool mapTeleport)
{
	std::vector<Frame> v;
	v.push_back(Recorded(0, 0, 0, 0, -100.0f));
	v.push_back(Recorded(1, 1, 1, 0, 0.0f)); // actual checkpoint placement
	v.push_back(Recorded(2, 1, 1, 0, 100.0f));
	v.push_back(Recorded(3, 1, 1, 0, mapTeleport ? 300.0f : 100.0f));
	v.push_back(Recorded(4, 1, 1, 0, 12.0f)); // late near pass or map teleport exit
	v.push_back(Recorded(5, 1, 1, 0, 100.0f));
	v.push_back(Recorded(6, 1, 1, 1, 0.0f)); // return to the recorded CP #1
	v.push_back(Recorded(7, 1, 1, 1, 100.0f));
	FillPre(v);
	std::vector<ArrivalTrace> trace;
	auto cut = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 125, 1.0 / 64, 0, 7, &trace);
	assert(cut.ok && cut.dead.size() == 1);
	assert(cut.dead[0].from == 2 && cut.dead[0].to == 6);
	assert(cut.awrMs == 47); // 125 - round(5 * 1000 / 64)
	assert(trace.size() == 1 && trace[0].method == 'a' && trace[0].cpFrame == 1);
}

static void CheckRepeatedReturns()
{
	std::vector<Frame> v {
		Recorded(0, 0, 0, 0, -100), Recorded(1, 1, 1, 0, 0), Recorded(2, 1, 1, 0, 100),
		Recorded(3, 1, 1, 1, 0), Recorded(4, 1, 1, 1, 12), Recorded(5, 1, 1, 1, 100),
		Recorded(6, 1, 1, 2, 0), Recorded(7, 1, 1, 2, 100),
	};
	FillPre(v);
	auto cut = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 125, 1.0 / 64, 0, 7);
	assert(cut.ok && cut.dead.size() == 1 && cut.dead[0].from == 2 && cut.dead[0].to == 6);
}

static void CheckMissingCheckpointFallback()
{
	std::vector<Frame> v {Recorded(0, 0, 0, 0, -100), Recorded(1, 0, 0, 0, 0),
		Recorded(2, 0, 0, 0, 100), Recorded(3, 0, 0, 1, 0), Recorded(4, 0, 0, 1, 100)};
	FillPre(v);
	std::vector<ArrivalTrace> trace;
	auto cut = ComputeAwrCut(v.data(), v.size(), nullptr, 0, 78, 1.0 / 64, 0, 4, &trace);
	assert(cut.ok && trace.size() == 1 && trace[0].method == 'b');
	assert(cut.dead.size() == 1 && cut.dead[0].from == 2 && cut.dead[0].to == 3);
}

int main()
{
	// Missing subtraction breaks both consumer regressions and these boundary checks.
	CheckFailedAttempt(false);
	CheckFailedAttempt(true);
	CheckRepeatedReturns();
	CheckMissingCheckpointFallback();
	assert(CheckpointIndexFromRecording(1) == 0);
	assert(CheckpointIndexFromRecording(35) == 34);
	assert(CheckpointIndexFromRecording(224) == 223);
	assert(CheckpointIndexFromRecording(0) == -1);
	assert(CheckpointIndexFromRecording(-1) == -1);
	assert(CheckpointIndexFromRecording(std::numeric_limits<int32_t>::min()) == -1);
	assert(CheckpointIndexFromRecording(std::numeric_limits<int32_t>::max()) == std::numeric_limits<int32_t>::max() - 1);
	std::puts("awr_recording_index: all tests passed");
}
