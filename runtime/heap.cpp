#include <map>
#include <mutex>

#include "rt.h"

namespace rt
{
namespace
{
uint32_t watchAddr = getenv("RA3_WATCH") ? (uint32_t)strtoul(getenv("RA3_WATCH"), nullptr, 16) : 0;
void watchLog(const char* what, uint32_t p, uint32_t sz)
{
	CPU* c = thisThread().cpu;
	log("heap %s %08x size %u (ret %08x)", what, p, sz, c ? guest::load<uint32_t>(c->r[ESP]) : 0);
}
// Block header lives 16 bytes before the payload: {size, magic, pad...}. Size classes are powers of
// two from 32 bytes; larger blocks come straight from the arena and are recycled by exact size.
constexpr uint32_t Magic = 0xa110ca7e;
std::mutex mu;
uint32_t bump = HeapBase, committedTo = HeapBase, lastReq = 0;
uint64_t liveBytes = 0, freeBytes = 0;
std::map<uint32_t, std::vector<uint32_t>> freeBySize;   // class size -> payload addresses

uint32_t classOf(uint32_t n)
{
	// Tight classes keep fragmentation and waste low: powers of two up to 256 B, quarter-octave steps up to
	// 1 MiB, 64 KiB multiples above (big stream buffers).
	if (n <= 256) { uint32_t s = 32; while (s < n) s <<= 1; return s; }
	if (n > (1u << 20)) return (n + 0xffffu) & ~0xffffu;
	uint32_t s = 256;
	while (s * 2 < n) s <<= 1;
	uint32_t step = s / 4;
	return (n + step - 1) / step * step;
}

void commitTo(uint32_t end)
{
	if (end <= committedTo) return;
	uint32_t newEnd = (end + 0xfffff) & ~0xfffffu;   // 1 MiB steps
	if (newEnd > HeapEnd) fatal("guest heap exhausted (request %u bytes, live %llu MiB, free-listed %llu MiB)", lastReq, (unsigned long long)(liveBytes >> 20), (unsigned long long)(freeBytes >> 20));
	if (!guest::commit(committedTo, newEnd - committedTo)) fatal("cannot commit guest heap");
	committedTo = newEnd;
}
}

void heapInit() {}

uint32_t gmalloc(uint32_t size, uint32_t align)
{
	std::lock_guard<std::mutex> l(mu);
	uint32_t cls = classOf(size ? size : 1);
	if (align > 16) cls = classOf(cls + align);
	lastReq = size;
	auto& fl = freeBySize[cls];
	uint32_t block;
	if (!fl.empty()) { block = fl.back(); fl.pop_back(); freeBytes -= cls; }
	else
	{
		block = (bump + 15) & ~15u;
		bump = block + 16 + cls;
		commitTo(bump);
		block += 16;
	}
	uint32_t payload = block;
	if (align > 16)
	{
		payload = (block + align - 1) & ~(align - 1);
		if (payload == block) payload += align;   // keep room for the header
	}
	liveBytes += cls;
	guest::store<uint32_t>(payload - 16, cls);
	guest::store<uint32_t>(payload - 12, Magic);
	guest::store<uint32_t>(payload - 8, block);   // original block start (differs when over-aligned)
	guest::store<uint32_t>(payload - 4, size);
	if (watchAddr && payload <= watchAddr && watchAddr < payload + cls) watchLog("alloc", payload, size);
	return payload;
}

void gfree(uint32_t p)
{
	if (!p) return;
	std::lock_guard<std::mutex> l(mu);
	if (guest::load<uint32_t>(p - 12) != Magic) { log("free of a foreign pointer %08x", p); return; }
	uint32_t cls = guest::load<uint32_t>(p - 16), block = guest::load<uint32_t>(p - 8);
	if (watchAddr && p <= watchAddr && watchAddr < p + guest::load<uint32_t>(p - 16)) watchLog("free", p, 0);
	guest::store<uint32_t>(p - 12, 0);
	freeBySize[cls].push_back(block);
	liveBytes -= cls; freeBytes += cls;
}

uint32_t gsize(uint32_t p) { return p ? guest::load<uint32_t>(p - 4) : 0; }

uint32_t grealloc(uint32_t p, uint32_t size)
{
	if (!p) return gmalloc(size);
	uint32_t old = gsize(p);
	uint32_t cls = guest::load<uint32_t>(p - 16);
	if (size <= cls - 0 && size <= cls && guest::load<uint32_t>(p - 8) == p) { guest::store<uint32_t>(p - 4, size); return p; }
	uint32_t n = gmalloc(size);
	memcpy(guest::host(n), guest::host(p), std::min(old, size));
	gfree(p);
	return n;
}

uint32_t vmAlloc(uint32_t size)
{
	static std::mutex vmu;
	static uint32_t next = VmBase;
	std::lock_guard<std::mutex> l(vmu);
	uint32_t sz = (size + 0xffff) & ~0xffffu;   // allocation granularity
	uint32_t a = next;
	if (a + sz > VmEnd) return 0;
	next += sz;
	if (!guest::commit(a, sz)) return 0;
	return a;
}
}
