#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <functional>
#include <utility>
#include <memory>
#include <vector>
#include <algorithm>

namespace KZ::analytics::evidence
{
	constexpr std::size_t maxBytes = 4u * 1024u * 1024u;
	constexpr std::int64_t ttlSeconds = 60LL * 24 * 60 * 60;
	constexpr std::size_t maxEncodedBytes = ((maxBytes + 2) / 3) * 4;

	inline bool CanStore(std::size_t bytes, int endTick)
	{
		return bytes > 0 && bytes <= maxBytes && endTick >= 0;
	}

	// Serializer/DB callbacks execute on the main thread. They own the frozen event
	// and resolve it once: only a durable commit may attach a replay reference.
	class Delivery
	{
		std::function<void(const std::string &)> emit;
		bool completed = false;

	public:
		explicit Delivery(std::function<void(const std::string &)> callback) : emit(std::move(callback)) {}

		bool IsComplete() const
		{
			return completed;
		}

		void Complete(const std::string &uuid, bool committed)
		{
			if (completed)
			{
				return;
			}
			completed = true;
			emit(committed ? uuid : std::string());
		}
	};

	// Main-thread ownership spans recording, serialization and DB transaction callbacks.
	// Timeout and unload resolve fallback while the outbox is still alive.
	class PendingDeliveries
	{
		struct Pending
		{
			std::shared_ptr<Delivery> delivery;
			double deadline;
		};

		std::vector<Pending> entries;
		bool closing = false;

	public:
		static constexpr std::size_t maxPending = 128;
		static constexpr double timeoutSeconds = 30;

		void Poll(double now)
		{
			for (auto &pending : entries)
			{
				if (now >= pending.deadline)
				{
					pending.delivery->Complete("", false);
				}
			}
			entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Pending &pending) { return pending.delivery->IsComplete(); }),
						  entries.end());
		}

		void Add(std::shared_ptr<Delivery> delivery, double now)
		{
			if (closing)
			{
				delivery->Complete("", false);
				return;
			}
			Poll(now);
			if (entries.size() >= maxPending)
			{
				entries.front().delivery->Complete("", false);
				entries.erase(entries.begin());
			}
			entries.push_back({std::move(delivery), now + timeoutSeconds});
		}

		void Flush()
		{
			closing = true;
			for (auto &pending : entries)
			{
				pending.delivery->Complete("", false);
			}
			entries.clear();
		}
	};

	// UUID must already be parsed and canonicalized by UUID_t.
	inline std::string FetchSQL(const std::string &uuid, std::int64_t now)
	{
		return "SELECT Data, ExpiresAt FROM KzMovementEvidenceReplays WHERE ReplayUUID = '" + uuid + "' AND ExpiresAt > " + std::to_string(now);
	}

	inline std::string ExpireSQL(std::int64_t now)
	{
		return "DELETE FROM KzMovementEvidenceReplays WHERE ReplayUUID IN (SELECT ReplayUUID FROM "
			   "(SELECT ReplayUUID FROM KzMovementEvidenceReplays WHERE ExpiresAt <= "
			   + std::to_string(now) + " ORDER BY ExpiresAt LIMIT 64) AS expired)";
	}
} // namespace KZ::analytics::evidence
