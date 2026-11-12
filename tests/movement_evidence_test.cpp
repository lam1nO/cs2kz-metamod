#include "../src/kz/analytics/movement_evidence.h"
#include "../src/kz/analytics/jump_analytics.h"
#include "../src/kz/db/queries/jumptop.h"
#include <cassert>
#include <iostream>
#include <memory>

int main()
{
	using namespace KZ::analytics;
	using namespace KZ::analytics::evidence;
	assert(!CanStore(0, 1));
	assert(CanStore(maxBytes, 0));
	assert(!CanStore(maxBytes + 1, 1));
	assert(!CanStore(1, -1));
	assert(ttlSeconds == 5184000);

	InputFeatureVector features {};
	for (auto &feature : features)
	{
		assert(bool(std::cin >> feature));
	}
	assert(InputScore(features) >= AHK_INPUT_THRESHOLD);
	JumpRecord first;
	first.id = "non-pb";
	first.endTick = 100;
	first.valid = true;
	first.distance = 220; // Below the leaderboard tier; no PB eligibility dependency.
	first.airtimeMs = 600;
	first.widthsDeg.resize(5, 8);
	first.durationsMs.resize(5, 100);
	first.inputFeatures = features;
	assert(PositiveInputEvidence(first));
	auto second = first;
	second.id = "second-non-pb";
	second.endTick = 200;
	State state;
	state.Add(first, false);
	state.Add(second, false);
	assert(state.ShouldAlert(0));
	assert(!state.ShouldAlert(1799));
	JumpRecord normal = second;
	normal.id = "normal-current";
	normal.endTick = 300;
	normal.inputFeatures.reset();
	state.Add(normal, false);
	assert(state.ShouldAlert(1800)); // Old positives remain, current clip MUST NOT be linked.
	assert(!PositiveInputEvidence(normal));
	first.hasTurnBind = true;
	assert(!PositiveInputEvidence(first));
	first.hasTurnBind = false;
	first.inPrac = true;
	assert(!PositiveInputEvidence(first));

	// Deferred callbacks own immutable identity after player/slot state disappears.
	std::vector<std::string> delivered;
	std::shared_ptr<Delivery> pending;
	{
		std::string player = "original-player/map/alert";
		pending = std::make_shared<Delivery>([player, &delivered](const std::string &uuid) { delivered.push_back(player + ":" + uuid); });
		player = "reused-slot";
	}
	pending->Complete("uncommitted-uuid", false); // DB failure or serialization failure.
	pending->Complete("late-uuid", true);         // Duplicate completion cannot advertise another event.
	assert(delivered.size() == 1 && delivered[0] == "original-player/map/alert:");
	Delivery committed([&](const std::string &uuid) { delivered.push_back(uuid); });
	committed.Complete("durable-uuid", true);
	assert(delivered.back() == "durable-uuid");

	// Exercise the production SQL against SQLite in the Python driver.
	std::cout << sqlite_movement_evidence_create << '\x1e' << sqlite_movement_evidence_index << '\x1e'
			  << FetchSQL("019abcde-1234-7000-8000-000000000001", 1000) << '\x1e' << ExpireSQL(1000);
}
