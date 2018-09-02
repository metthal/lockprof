#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <unordered_map>
#include <vector>

extern "C" {

#define ORIGCALL(func, ...) ((orig_##func)dlsym(RTLD_NEXT, #func))(__VA_ARGS__)
typedef int (*orig_pthread_mutex_init)(pthread_mutex_t*, const pthread_mutexattr_t*);
typedef int (*orig_pthread_mutex_destroy)(pthread_mutex_t*);
typedef int (*orig_pthread_mutex_lock)(pthread_mutex_t*);
typedef int (*orig_pthread_mutex_trylock)(pthread_mutex_t*);
typedef int (*orig_pthread_mutex_unlock)(pthread_mutex_t*);

}

struct ScopeSet
{
	ScopeSet(bool& value) : _value(value) { _value = true; }
	~ScopeSet() { _value = false; }

	bool& _value;
};

struct Lock
{
	Lock(pthread_mutex_t* mutex) : _mutex(mutex)
	{
		ORIGCALL(pthread_mutex_lock, _mutex);
	}

	~Lock()
	{
		ORIGCALL(pthread_mutex_unlock, _mutex);
	}

	pthread_mutex_t* _mutex;
};

class Lockprof
{
public:
	struct LockInfo
	{
		LockInfo() : LockInfo(0, {}) {}
		LockInfo(std::uint64_t id_, std::vector<std::string>&& callstack) : id(id_), initCallstack(std::move(callstack)) {}
		LockInfo(const LockInfo&) = default;
		LockInfo(LockInfo&&) noexcept = default;

		LockInfo& operator=(const LockInfo&) = default;
		LockInfo& operator=(LockInfo&&) noexcept = default;

		void lock(const std::chrono::system_clock::duration& waitTime)
		{
			waitTimes.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(waitTime).count());
		}

		void unlock()
		{
		}

		std::uint64_t lockCount() const
		{
			return waitTimes.size();
		}

		std::uint64_t instantLocks() const
		{
			return std::count_if(waitTimes.begin(), waitTimes.end(), [](std::uint64_t t) { return t == 0; });
		}

		std::uint64_t fastLocks() const
		{
			return std::count_if(waitTimes.begin(), waitTimes.end(), [](std::uint64_t t) { return t <= 2; });
		}

		std::uint64_t totalWaitTime() const
		{
			return std::accumulate(waitTimes.begin(), waitTimes.end(), 0);
		}

		double avgWaitTime() const
		{
			return static_cast<double>(totalWaitTime()) / static_cast<double>(lockCount());
		}

		double waitTimeStdDev() const
		{
			auto avg = avgWaitTime();
			std::vector<double> diff(waitTimes.size());
			std::transform(waitTimes.begin(), waitTimes.end(), diff.begin(), [avg](double x) { return x - avg; });
			auto sqSum = std::inner_product(diff.begin(), diff.end(), diff.begin(), 0.0);
			return std::sqrt(sqSum / static_cast<double>(lockCount()));
		}

		std::uint64_t id;
		std::vector<std::uint64_t> waitTimes;
		std::vector<std::string> initCallstack;
	};

	Lockprof() : _lockStats()
	{
		ORIGCALL(pthread_mutex_init, &_mutex, NULL);
	}

	~Lockprof()
	{
		for (auto&& mutexInfo : _lockStats)
			_history.push_back(std::move(mutexInfo.second));

		std::cout << std::endl;
		for (const auto& info : _history)
		{
			std::cout << "Mutex " << std::hex << std::showbase << info.id << std::dec << std::noshowbase << '\n'
				<< "  Lock Count = " << info.lockCount() << '\n'
				<< "  Total Wait Time = " << info.totalWaitTime() << " ms" << '\n'
				<< "  Avg Wait Time = " << info.avgWaitTime() << " ms" << '\n'
				<< "  Wait Time Std. Dev. = " << info.waitTimeStdDev() << " ms" << '\n'
				<< "  Instant Locks = " << info.instantLocks() << '\n'
				<< "  Fast Locks (<3ms) = " << info.fastLocks() << '\n'
				<< "  Initialization\n";

			for (const auto& frame : info.initCallstack)
				std::cout << "    " << frame << '\n';

			std::cout << std::endl;
		}

		ORIGCALL(pthread_mutex_destroy, &_mutex);
	}

	void init(pthread_mutex_t* mutex)
	{
		thread_local bool isHooked = false;

		if (isHooked)
			return;

		ScopeSet set{isHooked};
		std::vector<std::string> callstack;

		void* trace[1024];
		auto traceSize = backtrace(trace, 1024);
		auto traceSymbols = backtrace_symbols(trace, traceSize);
		for (int i = 2; i < traceSize; ++i)
		{
			callstack.emplace_back(traceSymbols[i]);
		}

		Lock lock{&_mutex};
		//std::cout << static_cast<void*>(mutex) << " init" << std::endl;
		_lockStats[mutex] = LockInfo{reinterpret_cast<std::uint64_t>(mutex), std::move(callstack)};
	}

	void destroy(pthread_mutex_t* mutex)
	{
		Lock lock{&_mutex};
		//std::cout << static_cast<void*>(mutex) << " destroy" << std::endl;

		auto itr = _lockStats.find(mutex);
		if (itr == _lockStats.end())
		{
			std::cout << "Destroying but not in initialized" << std::endl;
			throw "Destroying but not in initialized";
		}

		_history.emplace_back(std::move(itr->second));
		_lockStats.erase(itr);
	}

	void lock(pthread_mutex_t* mutex, const std::chrono::system_clock::duration& waitTime)
	{
		Lock lock{&_mutex};
		//std::cout << static_cast<void*>(mutex) << " lock" << std::endl;

		auto itr = _lockStats.find(mutex);
		if (itr == _lockStats.end())
			return;

		itr->second.lock(waitTime);
	}

	void unlock(pthread_mutex_t* mutex)
	{
		Lock lock{&_mutex};
		//std::cout << static_cast<void*>(mutex) << " unlock" << std::endl;

		auto itr = _lockStats.find(mutex);
		if (itr == _lockStats.end())
			return;

		itr->second.unlock();
	}

private:
	std::unordered_map<pthread_mutex_t*, LockInfo> _lockStats;
	std::vector<LockInfo> _history;
	pthread_mutex_t _mutex;
};

static Lockprof lockprof;

extern "C" {

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr)
{
	lockprof.init(mutex);
	return ORIGCALL(pthread_mutex_init, mutex, attr);
}

int pthread_mutex_destroy(pthread_mutex_t* mutex)
{
	lockprof.destroy(mutex);
	return ORIGCALL(pthread_mutex_destroy, mutex);
}

int pthread_mutex_lock(pthread_mutex_t* mutex)
{
	thread_local bool isHooked = false;
	if (isHooked)
		return ORIGCALL(pthread_mutex_lock, mutex);

	ScopeSet set{isHooked};

	thread_local std::chrono::system_clock::time_point lockTime;
	lockTime = std::chrono::system_clock::now();
	auto result = ORIGCALL(pthread_mutex_lock, mutex);
	lockprof.lock(mutex, std::chrono::system_clock::now() - lockTime);
	return result;
}

int pthread_mutex_trylock(pthread_mutex_t* mutex)
{
	return ORIGCALL(pthread_mutex_trylock, mutex);
}

int pthread_mutex_unlock(pthread_mutex_t* mutex)
{
	thread_local bool isHooked = false;
	if (isHooked)
		return ORIGCALL(pthread_mutex_unlock, mutex);

	ScopeSet set{isHooked};

	lockprof.unlock(mutex);
	return ORIGCALL(pthread_mutex_unlock, mutex);
}

}
