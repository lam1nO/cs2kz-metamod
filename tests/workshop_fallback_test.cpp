#include "../src/kz/misc/workshop_fallback.h"

#include <cassert>

using KZ::misc::customchangemap::WorkshopFallback;

int main()
{
	WorkshopFallback fallback;
	assert(!fallback.ShouldRequest(45));
	assert(fallback.ShouldRequest(61));
	fallback.NoConnection();
	assert(fallback.ShouldRequest(0));

	// При отсутствии службы заявка сразу возвращается в штатный путь и не дёргает сокет повторно.
	assert(!fallback.Request(3844450818, "gymstrike-workshop-fallback-test-absent"));
	assert(!fallback.ShouldRequest(61));
	fallback.Reset();
	assert(!fallback.ShouldRequest(0));

#ifdef _LINUX
	constexpr const char *address = "gymstrike-workshop-fallback-test";
	sockaddr_un endpoint = WorkshopFallback::Endpoint(address);
	int listener = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(listener >= 0);
	assert(bind(listener, reinterpret_cast<sockaddr *>(&endpoint), WorkshopFallback::EndpointSize(address)) == 0);
	assert(listen(listener, 2) == 0);

	assert(!fallback.Request(3844450818, address, geteuid() + 1));
	int rejected = accept(listener, nullptr, nullptr);
	assert(rejected >= 0);
	char ignored;
	assert(recv(rejected, &ignored, 1, 0) == 0);
	close(rejected);
	fallback.Reset();

	assert(fallback.Request(3844450818, address, geteuid()));
	int peer = accept(listener, nullptr, nullptr);
	assert(peer >= 0);
	char item[32];
	auto size = recv(peer, item, sizeof(item), 0);
	assert(size == 11 && std::string_view(item, size) == "3844450818\n");
	assert(fallback.Check() == WorkshopFallback::Result::Pending);
	assert(send(peer, "rea", 3, 0) == 3);
	assert(fallback.Check() == WorkshopFallback::Result::Pending);
	assert(send(peer, "dy\n", 3, 0) == 3);
	assert(fallback.Check() == WorkshopFallback::Result::Ready);
	assert(fallback.Check() == WorkshopFallback::Result::Idle);
	close(peer);

	fallback.Reset();
	assert(fallback.Request(3844450818, address, geteuid()));
	peer = accept(listener, nullptr, nullptr);
	assert(peer >= 0);
	fallback.Reset(); // смена вручную, новая заявка или её таймаут
	assert(fallback.Check() == WorkshopFallback::Result::Idle);
	close(peer);

	assert(fallback.Request(3844450818, address, geteuid()));
	peer = accept(listener, nullptr, nullptr);
	assert(peer >= 0);
	assert(recv(peer, item, sizeof(item), 0) == 11);
	assert(send(peer, "failed\n", 7, 0) == 7);
	assert(fallback.Check() == WorkshopFallback::Result::Failed);
	close(peer);

	fallback.Reset();
	assert(fallback.Request(3844450818, address, geteuid()));
	peer = accept(listener, nullptr, nullptr);
	assert(peer >= 0);
	assert(recv(peer, item, sizeof(item), 0) == 11);
	assert(send(peer, "unavailable\n", 12, 0) == 12);
	assert(fallback.Check() == WorkshopFallback::Result::Unavailable);
	close(peer);

	fallback.Reset();
	assert(fallback.Request(3844450818, address, geteuid()));
	peer = accept(listener, nullptr, nullptr);
	assert(peer >= 0);
	assert(recv(peer, item, sizeof(item), 0) == 11);
	close(peer);
	assert(fallback.Check() == WorkshopFallback::Result::Unavailable);
	close(listener);
#endif
}
