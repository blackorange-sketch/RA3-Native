#pragma once
// Recursive lock for hot paths (queued GL commands, guest critical sections). Uncontended lock/unlock are an
// inline compare-and-swap and a swap, no libc call: the profile showed pthread mutex calls at several % of the
// game thread. Contended: a spin, then the thread sleeps on a futex; the unlocking thread makes the wake syscall
// only while somebody actually sleeps.
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
	std::atomic<int> st{0};              // 0 free, 1 locked
	std::atomic<int> sleepers{0};        // threads blocked in futex_wait on st
	std::atomic<pthread_t> owner{0};
	int depth = 0;

	bool tryAcquire()
	{
		int c = 0;
		return st.compare_exchange_strong(c, 1, std::memory_order_seq_cst, std::memory_order_relaxed);
	}
	void acquireSlow()
	{
		// spin first (tens of microseconds): a waiter that sleeps makes the owner pay a wake syscall on unlock,
		// and the game's critical sections are held only briefly
		for (int i = 0; i < 4000; i++)
		{
			if (st.load(std::memory_order_relaxed) == 0 && tryAcquire()) return;
			cpuRelax();
		}
		sleepers.fetch_add(1, std::memory_order_seq_cst);
		while (!tryAcquire())
			syscall(SYS_futex, (int*)&st, FUTEX_WAIT_PRIVATE, 1, nullptr, nullptr, 0);
		sleepers.fetch_sub(1, std::memory_order_relaxed);
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
		st.store(0, std::memory_order_seq_cst);
		// wake only when someone really sleeps (the three-state mutex kept waking after the waiters were gone)
		if (sleepers.load(std::memory_order_seq_cst))
			syscall(SYS_futex, (int*)&st, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
	}
};
}
