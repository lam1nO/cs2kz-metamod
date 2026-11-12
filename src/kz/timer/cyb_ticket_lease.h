#pragma once
#include "cyb_ticket_eligibility.h"
#include <cstdint>
#include <limits>

namespace CybTickets
{
	class Lease
	{
		std::string campaign;
		std::int64_t issued {}, expires {};

	public:
		bool Update(const std::string &id, std::int64_t ttl, std::int64_t now, int slot)
		{
			if (slot != -1)
			{
				return false; // Client commands cannot renew or clear a trusted lease.
			}
			campaign.clear();
			issued = expires = 0;
			if (id == "clear" && ttl == 0)
			{
				return true;
			}
			if (!IsCampaignId(id) || ttl <= 0 || ttl > 90000 || now < 0 || now > (std::numeric_limits<std::int64_t>::max)() - ttl)
			{
				return false;
			}
			campaign = id;
			issued = now;
			expires = now + ttl;
			return true;
		}

		std::string Capture(std::int64_t now) const
		{
			return now >= issued && now < expires ? campaign : std::string {};
		}

		std::int64_t Remaining(std::int64_t now) const
		{
			return Capture(now).empty() ? 0 : expires - now;
		}
	};
} // namespace CybTickets
