#pragma once

#include <cstdint>

#ifdef _LINUX
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace KZ::misc::customchangemap
{
class WorkshopFallback
{
public:
	enum class Result { Idle, Pending, Ready, Failed, Unavailable };

	WorkshopFallback() = default;
	WorkshopFallback(const WorkshopFallback &) = delete;
	WorkshopFallback &operator=(const WorkshopFallback &) = delete;
	~WorkshopFallback() { Close(); }

	bool ShouldRequest(double secondsWithoutProgress) const
	{
		return !attempted && (noConnection || secondsWithoutProgress >= 60.0);
	}

	void NoConnection() { noConnection = true; }

	void Reset()
	{
		Close();
		attempted = false;
		noConnection = false;
	}

	bool Request(uint64_t item, const char *name = "gymstrike-workshop-fallback-v1", uint32_t workerUid = 0)
	{
		if (attempted)
		{
			return false;
		}
		attempted = true;
#ifdef _LINUX
		sockaddr_un endpoint = Endpoint(name);
		if (EndpointSize(name) > sizeof(endpoint))
		{
			return false;
		}
		fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if (fd < 0)
		{
			return false;
		}
		ucred peer{};
		socklen_t peerSize = sizeof(peer);
		std::string payload = std::to_string(item) + '\n';
		if (connect(fd, reinterpret_cast<sockaddr *>(&endpoint), EndpointSize(name)) == 0
			&& getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peerSize) == 0 && peer.uid == workerUid
			&& send(fd, payload.data(), payload.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(payload.size()))
		{
			return true;
		}
		Close();
#else
		(void)item;
		(void)name;
		(void)workerUid;
#endif
		return false;
	}

	bool Active() const { return fd >= 0; }

	Result Check()
	{
#ifdef _LINUX
		if (Active())
		{
			auto size = recv(fd, response + used, sizeof(response) - used, 0);
			if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			{
				return Result::Pending;
			}
			if (size > 0)
			{
				used += size;
				if (std::string_view(response, used) == "ready\n")
				{
					Close();
					return Result::Ready;
				}
				if (std::string_view(response, used) == "unavailable\n")
				{
					Close();
					return Result::Unavailable;
				}
				if (std::string_view(response, used) == "failed\n")
				{
					Close();
					return Result::Failed;
				}
				if (used < sizeof(response) && response[used - 1] != '\n')
				{
					return Result::Pending;
				}
			}
			Close();
			return Result::Unavailable;
		}
#endif
		return Result::Idle;
	}

#ifdef _LINUX
	static socklen_t EndpointSize(const char *name)
	{
		return offsetof(sockaddr_un, sun_path) + 1 + std::strlen(name);
	}

	static sockaddr_un Endpoint(const char *name)
	{
		sockaddr_un endpoint{};
		endpoint.sun_family = AF_UNIX;
		if (EndpointSize(name) <= sizeof(endpoint))
		{
			std::memcpy(endpoint.sun_path + 1, name, std::strlen(name));
		}
		return endpoint;
	}
#endif

private:
	void Close()
	{
#ifdef _LINUX
		if (Active())
		{
			close(fd);
			fd = -1;
			used = 0;
		}
#endif
	}

	int fd = -1;
	bool attempted = false;
	bool noConnection = false;
#ifdef _LINUX
	char response[12]{};
	size_t used = 0;
#endif
};
} // namespace KZ::misc::customchangemap
