#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <vector>
#include <memory>
#include <thread>
using f32 = float;

struct UUID_t
{
};

constexpr float ENGINE_FIXED_TICK_INTERVAL = 1.f / 64;

struct Recorder
{
	UUID_t uuid;
	int totalTicksRecorded = 1;

	bool WriteToMemory(std::vector<char> &v)
	{
		v.push_back('x');
		return true;
	}

	bool WriteToFile()
	{
		return true;
	}
};

using BufferSuccessCallback = std::function<void(const UUID_t &, f32, std::vector<char> &&)>;
using DiskWriteSuccessCallback = std::function<void(const UUID_t &, f32)>;
using WriteFailureCallback = std::function<void(const char *)>;

class ReplayFileWriter
{
public:
	ReplayFileWriter();
	~ReplayFileWriter();

	// Drain active threads — blocks until all in-flight serializations finish.
	void Stop();

	// Spawn a thread to serialize recorder to memory; delivers buffer to onSuccess on the main thread.
	void QueueWrite(std::unique_ptr<Recorder> recorder, BufferSuccessCallback onSuccess, WriteFailureCallback onFailure);

	// Spawn a thread to write recorder to disk; calls onSuccess/onFailure on the main thread (both optional).
	void QueueWriteToFile(std::unique_ptr<Recorder> recorder, DiskWriteSuccessCallback onSuccess = nullptr, WriteFailureCallback onFailure = nullptr);

	// Произвольная тяжёлая работа на отдельном потоке (разбор куска рана из бэкапа —
	// kz/savedrun/kz_partial_replay.cpp): work выполняется на потоке и возвращает колбэк, который
	// зовётся на ГЛАВНОМ потоке из RunFrame (пустой колбэк допустим). Stop() ждёт и эти потоки.
	void QueueTask(std::function<std::function<void()>()> work);

	// Invoke pending callbacks — call once per game frame from the main thread.
	void RunFrame();

private:
	template<typename F>
	void SpawnThread(F &&work);

	std::atomic<int> m_activeThreads {0};
	std::mutex m_shutdownMutex;
	std::condition_variable m_shutdownCV;

	std::queue<std::function<void()>> m_completedCallbacks;
	std::mutex m_completedLock;
};
