#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <iostream>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <vector>

extern "C" {

#define ORIGCALL(func, ...) ((orig_##func)dlsym(RTLD_NEXT, #func))(__VA_ARGS__)
#define HOOK(ret, func, ...) typedef ret (*orig_##func)(__VA_ARGS__)

HOOK(int, pthread_mutex_init, pthread_mutex_t*, const pthread_mutexattr_t*);
HOOK(int, pthread_mutex_destroy, pthread_mutex_t*);
HOOK(int, pthread_mutex_lock, pthread_mutex_t*);
HOOK(int, pthread_mutex_trylock, pthread_mutex_t*);
HOOK(int, pthread_mutex_unlock, pthread_mutex_t*);
HOOK(sem_t*, sem_open, const char*, int, ...);
HOOK(int, sem_close, sem_t*);
HOOK(int, sem_wait, sem_t*);
HOOK(int, sem_post, sem_t*);

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
	enum class LockType
	{
		Mutex,
		Semaphore
	};

	template <LockType LT>
	struct LockInfo
	{
		LockInfo() : LockInfo(0, {}) {}
		LockInfo(std::uint64_t id_, std::vector<std::string>&& callstack) : id(id_), initCallstack(std::move(callstack)) {}
		LockInfo(const LockInfo&) = default;
		LockInfo(LockInfo&&) noexcept = default;

		LockInfo& operator=(const LockInfo&) = default;
		LockInfo& operator=(LockInfo&&) noexcept = default;

		std::string dump() const
		{
			std::ostringstream ss;
			if (LT == LockType::Mutex)
				ss << "Mutex";
			else if (LT == LockType::Semaphore)
				ss << "Semaphore";
			else
				ss << "Unknown";

			ss << ' ' << std::hex << std::showbase << id << std::dec << std::noshowbase << '\n'
				<< "  Lock Count = " << lockCount() << '\n'
				<< "  Total Wait Time = " << totalWaitTime() << " ms" << '\n'
				<< "  Avg Wait Time = " << avgWaitTime() << " ms" << '\n'
				<< "  Wait Time Std. Dev. = " << waitTimeStdDev() << " ms" << '\n'
				<< "  Instant Locks = " << instantLocks() << '\n'
				<< "  Fast Locks (<3ms) = " << fastLocks() << '\n'
				<< "  Initialization\n";

			for (const auto& frame : initCallstack)
				ss << "    " << frame << '\n';
			ss << '\n';

			return ss.str();
		}

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

	struct MutexInfo : public LockInfo<LockType::Mutex> { using LockInfo::LockInfo; };
	struct SemaphoreInfo : public LockInfo<LockType::Semaphore> { using LockInfo::LockInfo; };

	Lockprof() : _mutexStats(), _semStats(), _mutexHistory(), _semHistory()
	{
		ORIGCALL(pthread_mutex_init, &_mutexMutex, NULL);
		ORIGCALL(pthread_mutex_init, &_semMutex, NULL);
	}

	~Lockprof()
	{
		for (auto&& mutexInfo : _mutexStats)
			_mutexHistory.push_back(std::move(mutexInfo.second));

		for (auto&& semInfo : _semStats)
			_semHistory.push_back(std::move(semInfo.second));

		std::cout << std::endl;
		for (const auto& info : _mutexHistory)
			std::cout << info.dump();
		for (const auto& info : _semHistory)
			std::cout << info.dump();

		ORIGCALL(pthread_mutex_destroy, &_mutexMutex);
		ORIGCALL(pthread_mutex_destroy, &_semMutex);
	}

	void init(pthread_mutex_t* mutex)
	{
		_init(mutex, _mutexStats, _mutexHistory, &_mutexMutex);
	}

	void init(sem_t* sem)
	{
		_init(sem, _semStats, _semHistory, &_semMutex);
	}

	void destroy(pthread_mutex_t* mutex)
	{
		_destroy(mutex, _mutexStats, _mutexHistory, &_mutexMutex);
	}

	void destroy(sem_t* sem)
	{
		_destroy(sem, _semStats, _semHistory, &_semMutex);
	}

	void lock(pthread_mutex_t* mutex, const std::chrono::system_clock::duration& waitTime)
	{
		_lock(mutex, _mutexStats, _mutexHistory, &_mutexMutex, waitTime);
	}

	void lock(sem_t* sem, const std::chrono::system_clock::duration& waitTime)
	{
		_lock(sem, _semStats, _semHistory, &_semMutex, waitTime);
	}

	void unlock(pthread_mutex_t* mutex)
	{
		_unlock(mutex, _mutexStats, _mutexHistory, &_mutexMutex);
	}

	void unlock(sem_t* sem)
	{
		_unlock(sem, _semStats, _semHistory, &_semMutex);
	}

private:
	template <typename PtrT, typename LockInfoT>
	void _init(PtrT obj, std::unordered_map<PtrT, LockInfoT>& stats, std::vector<LockInfoT>& /*history*/, pthread_mutex_t* lock)
	{
		std::vector<std::string> callstack;
		void* trace[1024];
		auto traceSize = backtrace(trace, 1024);
		auto traceSymbols = backtrace_symbols(trace, traceSize);
		for (int i = 3; i < traceSize; ++i)
		{
			callstack.emplace_back(traceSymbols[i]);
		}

		Lock l{lock};
		//std::cout << static_cast<void*>(mutex) << " init" << std::endl;
		stats[obj] = LockInfoT{reinterpret_cast<std::uint64_t>(obj), std::move(callstack)};
	}

	template <typename PtrT, typename LockInfoT>
	void _destroy(PtrT obj, std::unordered_map<PtrT, LockInfoT>& stats, std::vector<LockInfoT>& history, pthread_mutex_t* lock)
	{
		Lock l{lock};
		//std::cout << static_cast<void*>(mutex) << " destroy" << std::endl;

		auto itr = stats.find(obj);
		if (itr == stats.end())
		{
			std::cout << "Destroying but not in initialized" << std::endl;
			throw "Destroying but not in initialized";
		}

		history.emplace_back(std::move(itr->second));
		stats.erase(itr);
	}

	template <typename PtrT, typename LockInfoT>
	void _lock(PtrT obj, std::unordered_map<PtrT, LockInfoT>& stats, std::vector<LockInfoT>& /*history*/, pthread_mutex_t* lock, const std::chrono::system_clock::duration& waitTime)
	{
		Lock l{lock};
		//std::cout << static_cast<void*>(mutex) << " lock" << std::endl;

		auto itr = stats.find(obj);
		if (itr == stats.end())
			return;

		itr->second.lock(waitTime);
	}

	template <typename PtrT, typename LockInfoT>
	void _unlock(PtrT obj, std::unordered_map<PtrT, LockInfoT>& stats, std::vector<LockInfoT>& /*history*/, pthread_mutex_t* lock)
	{
		Lock l{lock};
		//std::cout << static_cast<void*>(mutex) << " unlock" << std::endl;

		auto itr = stats.find(obj);
		if (itr == stats.end())
			return;

		itr->second.unlock();
	}

private:
	std::unordered_map<pthread_mutex_t*, MutexInfo> _mutexStats;
	std::unordered_map<sem_t*, SemaphoreInfo> _semStats;
	std::vector<MutexInfo> _mutexHistory;
	std::vector<SemaphoreInfo> _semHistory;
	pthread_mutex_t _mutexMutex, _semMutex;
};

static Lockprof lockprof;

extern "C" {

int pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attr)
{
	thread_local bool isHooked = false;
	if (isHooked)
		return ORIGCALL(pthread_mutex_init, mutex, attr);

	ScopeSet set{isHooked};
	lockprof.init(mutex);
	return ORIGCALL(pthread_mutex_init, mutex, attr);
}

int pthread_mutex_destroy(pthread_mutex_t* mutex)
{
	thread_local bool isHooked = false;
	if (isHooked)
		return ORIGCALL(pthread_mutex_destroy, mutex);

	ScopeSet set{isHooked};
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

sem_t* sem_open(const char* name, int oflag, ...)
{
	va_list args;
	va_start(args, oflag);
	mode_t mode = va_arg(args, mode_t);
	unsigned int value = va_arg(args, unsigned int);
	va_end(args);

	thread_local bool isHooked = false;
	if (isHooked)
		return ORIGCALL(sem_open, name, oflag, mode, value);

	ScopeSet set{isHooked};
	auto result = ORIGCALL(sem_open, name, oflag, mode, value);
	lockprof.init(result);
	return result;
}

int sem_close(sem_t* sem)
{
	thread_local bool isHooked = false;
	if (isHooked)
		return ORIGCALL(sem_close, sem);

	ScopeSet set{isHooked};
	lockprof.destroy(sem);
	return ORIGCALL(sem_close, sem);
}

int sem_wait(sem_t* sem)
{
	thread_local bool isHooked = false;
	if (isHooked)
		return ORIGCALL(sem_wait, sem);

	ScopeSet set{isHooked};
	thread_local std::chrono::system_clock::time_point lockTime;
	lockTime = std::chrono::system_clock::now();
	auto result = ORIGCALL(sem_wait, sem);
	lockprof.lock(sem, std::chrono::system_clock::now() - lockTime);
	return result;
}

int sem_post(sem_t* sem)
{
	thread_local bool isHooked = false;
	if (isHooked)
		return ORIGCALL(sem_post, sem);

	ScopeSet set{isHooked};
	lockprof.unlock(sem);
	return ORIGCALL(sem_post, sem);
}

}
