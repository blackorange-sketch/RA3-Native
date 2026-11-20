// GL command recorder: queue and render thread (see glrec.h).
#ifdef __ANDROID__
#include "glrec.h"
#include "rt.h"
#include <chrono>
#include <cstdlib>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

void profThread(uint32_t guestTid);   // prof.cpp
namespace d3d
{
namespace rec
{
bool gOn = false;
pthread_t gRenderThread = 0;
RecLock gProducer;
GLint gUnpackAlign = 4;

namespace
{
struct Chunk { uint8_t* buf; size_t cap, used; };
const size_t kChunk = 64u << 10;
RecLock& pm = gProducer;                      // producers (game threads)
std::mutex qm;                                 // queue
std::condition_variable qcv, doneCv;
std::deque<Chunk*> ready, freeList;
Chunk* cur = nullptr;
uint64_t submitted = 0, executed = 0;          // chunks (guarded by qm)
uint64_t framesSub = 0, framesDone = 0;
bool renderWaiting = false;                    // render thread blocked on qcv (guarded by qm)
int doneWaiters = 0;                           // producers blocked on doneCv (guarded by qm)
std::atomic<uint64_t> nSubmitted{0};
uint64_t executedSeen = 0;                     // chunks taken by the render thread (render thread only)           // mirror of `submitted` for the render thread's lock-free poll
std::atomic<uint64_t> busyUs{0};
std::atomic<int> gRenderTid{0};

inline uint64_t nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

Chunk* getChunk(size_t min)
{
	{
		std::lock_guard<std::mutex> l(qm);
		if (min <= kChunk && !freeList.empty()) { Chunk* c = freeList.front(); freeList.pop_front(); c->used = 0; return c; }
	}
	Chunk* c = new Chunk;
	c->cap = std::max(min, kChunk);
	c->buf = (uint8_t*)malloc((c->cap + 15) & ~(size_t)15);   // 16-byte aligned on arm64 (aligned_alloc needs API 28)
	c->used = 0;
	return c;
}

void publishLocked()   // producer lock held
{
	if (!cur) return;
	if (!cur->used) return;
	bool wake;
	{
		std::lock_guard<std::mutex> l(qm);
		ready.push_back(cur);
		submitted++;
		wake = renderWaiting;
	}
	// published after qm is released: a polling render thread that saw it while we still held qm would block on
	// qm, and our unlock would then cost a futex wake
	nSubmitted.fetch_add(1, std::memory_order_release);
	if (wake) qcv.notify_one();
	cur = nullptr;
}

void renderLoop()
{
	for (;;)
	{
		Chunk* c;
		// poll very briefly before sleeping (catches chunks published back to back). It used to poll for 1 ms, which
		// kept a core busy all the time: noticeably more heat and battery drain for a few % of the game thread.
		{
			uint64_t t0 = nowUs();
			for (int i = 0; nSubmitted.load(std::memory_order_acquire) == executedSeen; i++)
			{
				if ((i & 63) == 63 && nowUs() - t0 > 50) break;
				__asm__ __volatile__("yield");
			}
		}
		{
			std::unique_lock<std::mutex> l(qm);
			if (ready.empty()) { renderWaiting = true; qcv.wait(l, [] { return !ready.empty(); }); renderWaiting = false; }
			c = ready.front(); ready.pop_front();
			executedSeen++;
		}
		uint64_t t0 = nowUs();
		size_t off = 0;
		while (off < c->used)
		{
			Cmd* cmd = (Cmd*)(c->buf + off);
			off += cmd->total;
			cmd->run(cmd + 1);
		}
		busyUs += nowUs() - t0;
		bool wake;
		{
			std::lock_guard<std::mutex> l(qm);
			executed++;
			if (c->cap <= kChunk && freeList.size() < 64) freeList.push_back(c);
			else { free(c->buf); delete c; }
			wake = doneWaiters > 0;
		}
		if (wake) doneCv.notify_all();
	}
}
}   // namespace


void* reserve(size_t payload, void (*run)(void*))
{
	size_t need = (sizeof(Cmd) + payload + 15) & ~(size_t)15;
	if (!cur || cur->used + need > cur->cap)
	{
		publishLocked();
		cur = getChunk(need);
	}
	Cmd* c = (Cmd*)(cur->buf + cur->used);
	c->run = run;
	c->total = (uint32_t)need;
	cur->used += need;
	// publish small batches early so the render thread starts on a frame before the game finishes it
	return c + 1;
}

void publish()
{
	std::lock_guard<RecLock> l(pm);
	publishLocked();
}

void waitIdle()
{
	if (!active()) return;
	uint64_t target;
	{
		std::lock_guard<RecLock> l(pm);
		publishLocked();
		std::lock_guard<std::mutex> q2(qm);
		target = submitted;
	}
	std::unique_lock<std::mutex> l(qm);
	doneWaiters++;
	doneCv.wait(l, [&] { return executed >= target; });
	doneWaiters--;
}

void endFrame()
{
	if (!active()) return;
	q([] { std::lock_guard<std::mutex> l(qm); framesDone++; });
	uint64_t mine;
	{
		std::lock_guard<RecLock> l(pm);
		publishLocked();
		std::lock_guard<std::mutex> q2(qm);
		mine = ++framesSub;
	}
	// at most one frame queued behind the one being rendered
	std::unique_lock<std::mutex> l(qm);
	doneWaiters++;
	doneCv.wait(l, [&] { return framesDone + 1 >= mine; });
	doneWaiters--;
}

void start(void (*threadInit)())
{
	std::thread([threadInit] {
		gRenderThread = pthread_self();
		pthread_setname_np(pthread_self(), "ra3-render");
		setpriority(PRIO_PROCESS, 0, -10);
		gRenderTid = (int)syscall(SYS_gettid);
		rt::log("GL: render thread tid %d", gRenderTid.load());
		::profThread(1);   // sampled with /sdcard/ra3_prof (prof.cpp: tag 1 is the render thread)
		threadInit();
		renderLoop();
	}).detach();
}

uint64_t renderBusyUs() { return busyUs.exchange(0); }
int renderTid() { return gRenderTid.load(); }

size_t texBytes(GLenum format, GLenum type, GLsizei w, GLsizei h, GLsizei d)
{
	size_t px;   // bytes per pixel
	switch (type)
	{
	case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: px = 2; break;
	case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV: case GL_UNSIGNED_INT_24_8: px = 4; break;
	case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: px = 8; break;
	default:
	{
		size_t comp = 4;
		switch (format)
		{
		case GL_RED: case GL_RED_INTEGER: case GL_LUMINANCE: case GL_ALPHA: case GL_DEPTH_COMPONENT: comp = 1; break;
		case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA: comp = 2; break;
		case GL_RGB: case GL_RGB_INTEGER: comp = 3; break;
		default: comp = 4; break;
		}
		size_t ts = 1;
		switch (type)
		{
		case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: ts = 2; break;
		case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: ts = 4; break;
		default: ts = 1; break;
		}
		px = comp * ts;
	}
	}
	size_t a = gUnpackAlign > 0 ? (size_t)gUnpackAlign : 4;
	size_t row = ((size_t)w * px + a - 1) / a * a;
	size_t slice = row * (h > 0 ? (size_t)h - 1 : 0) + (size_t)w * px;   // the last row needs no padding
	size_t sliceFull = row * (size_t)h;
	return d > 1 ? sliceFull * ((size_t)d - 1) + slice : slice;
}
}   // namespace rec
}   // namespace d3d
#endif
