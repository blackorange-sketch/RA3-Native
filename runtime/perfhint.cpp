// Android Dynamic Performance Framework: tell the scheduler the frame budget and how long each frame's work took,
// so the game and render threads get steady clocks instead of the governor ramping up and down (Android 13+).
// The NDK functions are looked up at run time: the app also runs on older Android versions.
#ifdef __ANDROID__
#include <dlfcn.h>
#include <unistd.h>
#include <cstdint>
#include "rt.h"

namespace
{
typedef void* (*GetManager)();
typedef void* (*CreateSession)(void*, const int32_t*, size_t, int64_t);
typedef int (*ReportActual)(void*, int64_t);
GetManager pGet; CreateSession pCreate; ReportActual pReport;
void* session = nullptr;
bool tried = false;
const int64_t kTargetNs = 33333333;   // the game's 30 fps frame
}

namespace rt
{
void perfHintFrame(uint64_t workNs, int renderTid)
{
	if (!tried)
	{
		if (!renderTid) return;   // wait until the render thread exists, both threads join the session
		tried = true;
		void* lib = dlopen("libandroid.so", RTLD_NOW);
		if (lib)
		{
			pGet = (GetManager)dlsym(lib, "APerformanceHint_getManager");
			pCreate = (CreateSession)dlsym(lib, "APerformanceHint_createSession");
			pReport = (ReportActual)dlsym(lib, "APerformanceHint_reportActualWorkDuration");
		}
		if (pGet && pCreate && pReport)
		{
			if (void* m = pGet())
			{
				int32_t tids[2] = {(int32_t)gettid(), (int32_t)renderTid};
				session = pCreate(m, tids, renderTid > 0 ? 2 : 1, kTargetNs);
			}
		}
		log("perf hint: %s", session ? "session created (game + render thread, 33.3 ms target)" : "not available on this device");
	}
	if (session && workNs > 0) pReport(session, (int64_t)workNs);
}
}
#else
#include <cstdint>
namespace rt { void perfHintFrame(uint64_t, int) {} }
#endif
