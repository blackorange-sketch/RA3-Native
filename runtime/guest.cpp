#include "guest.h"

#include <sys/mman.h>
#include <cstdio>
#include <unistd.h>

namespace guest
{
uint8_t* base;

bool init()
{
	void* p = mmap(nullptr, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED)
	{
		perror("reserve 4 GiB guest window");
		return false;
	}

	base = static_cast<uint8_t*>(p);
	return true;
}

bool commit(uint32_t addr, uint32_t size)
{
	static const uint64_t pg = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));   // 4 KiB, or 16 KiB on newer Android kernels
	uint64_t lo = addr & ~(pg - 1), hi = (static_cast<uint64_t>(addr) + size + pg - 1) & ~(pg - 1);
	return mprotect(base + lo, hi - lo, PROT_READ | PROT_WRITE) == 0;
}
}
