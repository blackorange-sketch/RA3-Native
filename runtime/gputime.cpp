// GPU time per render pass (/sdcard/ra3_prof): every change of render target starts a GL_EXT_disjoint_timer_query
// interval, labelled with the target (the back buffer, a render target by size and format, the final present).
// Results are collected a few frames later without waiting and logged every 30 s as GPU milliseconds per frame,
// so it is visible which passes (shadow map, reflections, the scene, post-processing) the GPU spends its time on.
// Runs on the render thread with the raw driver functions (no glrec.h here).
#include "gputime.h"
#include "rt.h"
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using rt::log;

namespace gputime
{
static bool gOn = false, gInit = false, gOk = false;
static PFNGLGETQUERYOBJECTUI64VEXTPROC pGet64 = nullptr;

struct Pending { GLuint q; uint64_t key; uint32_t frame; uint32_t info, id; };
static std::vector<GLuint> gFree;
static std::vector<Pending> gPend;
static GLuint gCur = 0;
static uint64_t gCurKey = 0;
static uint32_t gCurId = 0, gCurInfo = 0;   // the target object; draws and clears of passes merged into this interval
static uint32_t gFrame = 0, gFrames = 0;
struct Acc { uint64_t ns = 0; uint32_t n = 0; };
static std::unordered_map<uint64_t, Acc> gAcc;
static uint64_t gTotalNs = 0;
static uint32_t gDropped = 0;
// one frame in every 900, pass by pass in order: target, GPU time, draws, clears
struct SeqE { uint64_t key, ns; uint32_t info, id; };
static std::vector<SeqE> gSeq;
static uint32_t gSeqFrame = ~0u;

bool on()
{
	static const bool v = access("/sdcard/ra3_prof", F_OK) == 0;
	return v;
}

uint64_t key(int kind, uint32_t w, uint32_t h, uint32_t fmt)
{
	return ((uint64_t)kind << 56) | ((uint64_t)(fmt & 0xffff) << 40) | ((uint64_t)(w & 0xfffff) << 20) | (h & 0xfffff);
}

static std::string label(uint64_t k)
{
	int kind = (int)(k >> 56);
	uint32_t fmt = (uint32_t)(k >> 40) & 0xffff, w = (uint32_t)(k >> 20) & 0xfffff, h = (uint32_t)k & 0xfffff;
	char b[64];
	if (kind == 0) snprintf(b, sizeof b, "back %ux%u", w, h);
	else if (kind == 1) snprintf(b, sizeof b, "rt %ux%u f%u", w, h, fmt);
	else if (kind == 2) snprintf(b, sizeof b, "present %ux%u", w, h);
	else snprintf(b, sizeof b, "other");
	return b;
}

static bool init()
{
	if (gInit) return gOk;
	gInit = true;
	const char* ext = (const char*)glGetString(GL_EXTENSIONS);
	gOk = ext && strstr(ext, "GL_EXT_disjoint_timer_query");
	if (gOk) pGet64 = (PFNGLGETQUERYOBJECTUI64VEXTPROC)eglGetProcAddress("glGetQueryObjectui64vEXT");
	log("gputime: %s", gOk ? "GPU time per render pass on (GL_EXT_disjoint_timer_query)" : "GL_EXT_disjoint_timer_query missing, no GPU times");
	if (gOk) { GLint d = 0; glGetIntegerv(GL_GPU_DISJOINT_EXT, &d); }   // clears the flag
	return gOk;
}

static uint32_t addInfo(uint32_t a, uint32_t b) { return std::min((a & 0xffffff) + (b & 0xffffff), 0xffffffu) | ((a | b) & 0xff000000u); }
static void endCur(uint32_t info)
{
	if (!gCur) return;
	glEndQuery(GL_TIME_ELAPSED_EXT);
	gPend.push_back({gCur, gCurKey, gFrame, addInfo(gCurInfo, info), gCurId});
	gCur = 0; gCurInfo = 0;
}

static void logSeq()
{
	std::string s;
	char b[160];
	uint64_t tot = 0;
	for (auto& e : gSeq)
	{
		uint32_t d = e.info & 0xffffff, c = e.info >> 24;
		std::string cl;
		if (c & 8) { cl = " clear"; if (c & 1) cl += "C"; if (c & 2) cl += "Z"; if (c & 4) cl += "S"; }
		if (c & 0x70) { cl += " later-clear"; if (c & 0x10) cl += "C"; if (c & 0x20) cl += "Z"; if (c & 0x40) cl += "S"; }
		snprintf(b, sizeof b, "\n    %-22s %08x %6.2f ms %5u draws%s", label(e.key).c_str(), e.id, e.ns / 1e6, d, cl.c_str());
		s += b; tot += e.ns;
	}
	log("gputime: passes of frame %u (%zu passes, %.2f ms):%s", gSeqFrame, gSeq.size(), tot / 1e6, s.c_str());
	gSeq.clear();
}

void mark(uint64_t k, uint32_t prevInfo, uint32_t id)
{
	if (!gOn && !(gOn = on())) return;
	if (!init()) return;
	if (gCur && k == gCurKey && id == gCurId) { gCurInfo = addInfo(gCurInfo, prevInfo); return; }
	endCur(prevInfo);
	if (gPend.size() > 4096) return;   // results not coming back: stop adding
	GLuint q;
	if (!gFree.empty()) { q = gFree.back(); gFree.pop_back(); }
	else glGenQueries(1, &q);
	glBeginQuery(GL_TIME_ELAPSED_EXT, q);
	gCur = q; gCurKey = k; gCurId = id;
}

void frameEnd(uint32_t lastInfo)
{
	if (!gOn || !gOk) return;
	endCur(lastInfo);
	gFrame++;
	if (gFrame % 900 == 450) gSeqFrame = gFrame;
	GLint disjoint = 0;
	glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
	// results come back in order: take the finished ones from the front
	size_t i = 0;
	for (; i < gPend.size(); i++)
	{
		GLuint avail = 0;
		glGetQueryObjectuiv(gPend[i].q, GL_QUERY_RESULT_AVAILABLE, &avail);
		if (!avail) break;
		uint64_t ns = 0;
		if (pGet64) pGet64(gPend[i].q, GL_QUERY_RESULT, &ns);
		else { GLuint v = 0; glGetQueryObjectuiv(gPend[i].q, GL_QUERY_RESULT, &v); ns = v; }
		if (disjoint) gDropped++;
		else { auto& a = gAcc[gPend[i].key]; a.ns += ns; a.n++; gTotalNs += ns; }
		if (!gSeq.empty() && gPend[i].frame != gSeqFrame) logSeq();
		if (gPend[i].frame == gSeqFrame) gSeq.push_back({gPend[i].key, ns, gPend[i].info, gPend[i].id});
		gFree.push_back(gPend[i].q);
	}
	if (i) gPend.erase(gPend.begin(), gPend.begin() + i);
	if (!disjoint) gFrames++;
	if (gFrame % 900 == 0 && gFrames)
	{
		std::vector<std::pair<uint64_t, Acc>> v(gAcc.begin(), gAcc.end());
		std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.ns > b.second.ns; });
		std::string s;
		char b[128];
		for (size_t j = 0; j < v.size() && j < 14; j++)
		{
			snprintf(b, sizeof b, " | %s %.2f ms (%.1fx)", label(v[j].first).c_str(), v[j].second.ns / 1e6 / gFrames, (double)v[j].second.n / gFrames);
			s += b;
		}
		log("gputime: %.2f ms GPU per frame over %u frames%s%s", gTotalNs / 1e6 / gFrames, gFrames, gDropped ? " (some intervals dropped: GPU clock disjoint)" : "", s.c_str());
		gAcc.clear(); gTotalNs = 0; gFrames = 0; gDropped = 0;
	}
}
}
