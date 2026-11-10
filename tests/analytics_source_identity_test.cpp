#include "../src/kz/analytics/source_identity.h"
#include <cassert>
#include <fstream>
#include <iterator>

int main()
{
	using namespace KZ::analytics;
	assert(SourceServerId("srv-5", "srv-3") == "srv-5");
	assert(SourceServerId(nullptr, "srv-3") == "srv-3");
	assert(SourceServerId("", "srv-3") == "srv-3");
	assert(SourceServerId("srv_5", "srv-3") == "srv-3");
	assert(SourceServerId("srv-5\r\nInjected: yes", "srv-3") == "srv-3");
	assert(SourceServerId("123456789012345678901234567890123", "srv-3") == "srv-3");
	assert(SourceServerId("srv-5", "") == "srv-5");
	assert(SourceServerId(nullptr, "legacy_opaque_server") == "legacy_opaque_server");
	assert(SourceServerId(nullptr, nullptr).empty());

	assert(MovementSourceHeader(R"({"events":[{"type":"kz.ahk_suspected","serverId":"srv-5"}]})") == "srv-5");
	assert(MovementSourceHeader(R"({"events":[{"type":"kz.movement_run_finished","serverId":"srv-5"}]})") == "srv-5");
	assert(MovementSourceHeader(R"({"events":[{"type":"kz.run_finished","serverId":"srv-5"}]})").empty());
	assert(MovementSourceHeader(R"({"events":[]})").empty());
	assert(MovementSourceHeader("not json").empty());
	assert(MovementSourceHeader("kz.ahk_suspected {not json").empty());
	assert(MovementSourceHeader(R"({"events":[{"type":"kz.ahk_suspected"}]})").empty());
	assert(MovementSourceHeader(R"({"events":[{"type":"kz.run_finished","serverId":"srv-5","data":{"name":"kz.ahk_suspected"}}]})").empty());
	assert(MovementSourceHeader(R"({"events":[{"type":"kz.ahk_suspected","serverId":"srv_5"}]})").empty());
	assert(MovementSourceHeader(R"({"events":[{"type":"kz.ahk_suspected","serverId":"srv-5\r\nInjected: yes"}]})").empty());
	assert(MovementSourceHeader(R"({"events":[{"type":"kz.ahk_suspected","serverId":"srv-5"},{"type":"kz.ahk_suspected","serverId":"srv-6"}]})").empty());

	std::ifstream fixture("tests/fixtures/kz_jump_analytics_v1.json");
	assert(fixture.good());
	const std::string batch(std::istreambuf_iterator<char>{fixture}, {});
	// У многоэлементного fixture отсутствует единый movement-заголовок.
	assert(MovementSourceHeader(batch).empty());
}
