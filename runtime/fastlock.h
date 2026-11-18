#pragma once
// Recursive lock for hot paths (queued GL commands, guest critical sections). Uncontended lock/unlock are an
// inline compare-and-swap and a swap, no libc call: the profile showed pthread mutex calls at several % of the
// game thread. Contended: a short spin, then the thread sleeps on a futex (Drepper's three-state mutex), and
// the unlocking thread makes the wake syscall only when somebody actually sleeps.
#include <atomic>
#include <pthread.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>

namespace rt
{
inline void cpuRelax()
{
#if defined(__aarch64__)
	__asm__ __volatile__("yield");
#elif defined(__x86_64__)
	__asm__ __volatile__("pause");
#endif
}

struct FastRecLock
{
	std::atomic<int> st{0};              // 0 free, 1 locked, 2 locked with sleepers
	std::atomic<pthread_t> owner{0};
	int depth = 0;

	bool tryAcquire()
	{
		int c = 0;
		return st.compare_exchange_strong(c, 1, std::memory_order_acquire, std::memory_order_relaxed);
	}
	void acquireSlow()
	{
		for (int i = 0; i < 100; i++)
		{
			if (st.load(std::memory_order_relaxed) == 0 && tryAcquire()) return;
			cpuRelax();
		}
		int c = st.exchange(2, std::memory_order_acquire);
		while (c != 0)
		{
			syscall(SYS_futex, (int*)&st, FUTEX_WAIT_PRIVATE, 2, nullptr, nullptr, 0);
			c = st.exchange(2, std::memory_order_acquire);
		}
	}
	void lock()
	{
		pthread_t me = pthread_self();
		if (owner.load(std::memory_order_relaxed) == me) { depth++; return; }
		if (!tryAcquire()) acquireSlow();
		owner.store(me, std::memory_order_relaxed);
		depth = 1;
	}
	bool try_lock()
	{
		pthread_t me = pthread_self();
		if (owner.load(std::memory_order_relaxed) == me) { depth++; return true; }
		if (!tryAcquire()) return false;
		owner.store(me, std::memory_order_relaxed);
		depth = 1;
		return true;
	}
	void unlock()
	{
		if (owner.load(std::memory_order_relaxed) != pthread_self()) return;   // unlock without lock: ignore
		if (--depth) return;
		owner.store(0, std::memory_order_relaxed);
		if (st.exchange(0, std::memory_order_release) == 2)
			syscall(SYS_futex, (int*)&st, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
	}
};
}
