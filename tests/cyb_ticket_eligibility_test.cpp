#include "../src/kz/timer/cyb_ticket_eligibility.h"
#include <cassert>

int main()
{
	using namespace CybTickets;
	assert(Eligible(true, true, true, false, 0, 0));
	assert(!Eligible(false, true, true, false, 0, 0));
	assert(!Eligible(true, false, true, false, 0, 0));
	assert(!Eligible(true, true, false, false, 0, 0));
	assert(!Eligible(true, true, true, true, 0, 0));
	assert(!Eligible(true, true, true, false, 1, 0));
	assert(!Eligible(true, true, true, false, 0, 1));
	assert(IsCampaignId("12345678-1234-4234-8234-123456789abc"));
	assert(!IsCampaignId("12345678-1234-1234-1234-123456789abc"));
	assert(!IsCampaignId(""));
	assert(!IsCampaignId("12345678-1234-1234-1234-123456789abz"));
}
