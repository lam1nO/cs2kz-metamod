#include "src/kz/analytics/movement_evidence.h"
#include <cassert>
#include <iostream>

int main()
{
	using namespace KZ::analytics::evidence;
	PendingDeliveries pending;
	const auto mainThread = std::this_thread::get_id();
	std::vector<std::string> events;
	auto emit = [&](const std::string &uuid)
	{
		assert(std::this_thread::get_id() == mainThread);
		events.push_back(uuid);
	};
	auto serializing = std::make_shared<Delivery>(emit);
	pending.Add(serializing, 0);
	auto *writer = new ReplayFileWriter;
	writer->QueueWrite(
		std::make_unique<Recorder>(), [serializing](const UUID_t &, f32, std::vector<char> &&) { serializing->Complete("durable", true); },
		[serializing](const char *) { serializing->Complete("", false); });
	// Plugin unload resolves captured alerts BEFORE touching writer/DB/outbox teardown.
	pending.Flush();
	writer->Stop();
	delete writer;
	assert(events.size() == 1 && events[0].empty());
	serializing->Complete("late-commit", true);
	assert(events.size() == 1);

	PendingDeliveries dbPending;
	auto transaction = std::make_shared<Delivery>(emit);
	dbPending.Add(transaction, 0);
	dbPending.Flush();
	transaction->Complete("late-db-success", true);
	assert(events.size() == 2 && events.back().empty());

	PendingDeliveries timeout;
	auto lostCallback = std::make_shared<Delivery>(emit);
	timeout.Add(lostCallback, 100);
	timeout.Poll(129);
	assert(events.size() == 2);
	timeout.Poll(130);
	assert(events.size() == 3 && events.back().empty());
	lostCallback->Complete("late-timeout-success", true);
	assert(events.size() == 3);
	auto committed = std::make_shared<Delivery>(emit);
	timeout.Add(committed, 200);
	committed->Complete("committed-before-timeout", true);
	timeout.Poll(230);
	assert(events.size() == 4 && events.back() == "committed-before-timeout");
	timeout.Flush();
	auto duringShutdown = std::make_shared<Delivery>(emit);
	timeout.Add(duringShutdown, 240);
	assert(events.size() == 5 && events.back().empty());

	PendingDeliveries capacity;
	for (std::size_t i = 0; i < PendingDeliveries::maxPending + 1; ++i)
	{
		capacity.Add(std::make_shared<Delivery>(emit), 0);
	}
	assert(events.size() == 6); // Overflow resolves oldest, never drops it.
	capacity.Flush();
	assert(events.size() == 6 + PendingDeliveries::maxPending);
	std::cout << "PASS actual filewriter shutdown, DB pending, timeout, exactly-once, main-thread fallback and bounded registry\n";
}
