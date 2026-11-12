#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <functional>
#include <unistd.h>
#include <sys/syscall.h>
#include <atomic>
#include <algorithm>
#include <dirent.h>
#include <map>
#include <mutex>
#include <sys/stat.h>

#include "rt.h"

struct LiftedEntry { uint32_t addr; LiftedFn fn; };
extern const LiftedEntry lifted_table[];
extern const unsigned lifted_count;

namespace rt
{
Config config;
uint32_t imageBase, imageEntry;

static FILE* logFp;
static std::mutex logMu;

void (*logHook)(const char*) = nullptr;
void log(const char* f, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof buf, f, ap);
	va_end(ap);
	std::lock_guard<std::mutex> l(logMu);
	if (!logFp && !config.logFile.empty()) logFp = fopen(config.logFile.c_str(), "a");
	static const auto t0 = std::chrono::steady_clock::now();
	char ts[24] = "";
	if (strncmp(buf, "build ", 6) != 0) snprintf(ts, sizeof ts, "[%7.3f] ", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	fprintf(stderr, "%s%s\n", ts, buf);
	if (logFp) { fprintf(logFp, "%s%s\n", ts, buf); fflush(logFp); }
	if (logHook) logHook(buf);
}

#ifdef RA3_TRACE
}
thread_local uint32_t ra3_ring[256];
thread_local uint32_t ra3_ring_ra[256];
thread_local uint32_t ra3_ring_pos;
namespace rt {
static struct { uint32_t* ring; uint32_t* pos; uint32_t tid; uint32_t* ra; int ostid; } rings[32];
static std::atomic<int> ringCount{0};
static void profLoop();
void traceRegister(uint32_t tid)
{
	// fill the slot first, publish the count afterwards (the profiler thread reads rings[] concurrently)
	static std::mutex regMu;
	{
		std::lock_guard<std::mutex> l(regMu);
		int i = ringCount.load();
		if (i < 32) { rings[i] = {ra3_ring, &ra3_ring_pos, tid, ra3_ring_ra, (int)syscall(SYS_gettid)}; ringCount.store(i + 1); }
	}
	static std::once_flag once; std::call_once(once, [] { std::thread(profLoop).detach(); });
}
// poor man's profiler: every 2 ms record the most recently entered function of each thread
static std::mutex profMu;
static std::map<std::pair<uint32_t, uint32_t>, uint32_t> profHist;
void profDump();
static void profLoop()
{
	for (int iter = 1;; iter++)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		if (iter % 2500 == 0) profDump();   // every ~5 s, also while the game is still loading
		std::lock_guard<std::mutex> l(profMu);
		for (int i = 0; i < ringCount && i < 32; i++)
		{
			uint32_t v = rings[i].ring[(*rings[i].pos - 1) & 255];
			profHist[{rings[i].tid, v}]++;
		}
	}
}
void profDump()
{
	std::lock_guard<std::mutex> l(profMu);
	std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> byThread;
	for (auto& kv : profHist) byThread[kv.first.first].push_back({kv.second, kv.first.second});
	for (auto& t : byThread)
	{
		std::sort(t.second.rbegin(), t.second.rend());
		uint32_t total = 0; for (auto& e : t.second) total += e.first;
		std::string line;
		for (size_t k = 0; k < t.second.size() && k < 6; k++)
		{
			uint32_t v = t.second[k].second; char b[96];
			imports::Entry* e = (v >> 28) == 0xb ? imports::lookup(v ^ 0xc0000000u) : nullptr;
			if (e) snprintf(b, sizeof b, " %s:%u", e->name.c_str(), t.second[k].first); else snprintf(b, sizeof b, " %08x:%u", v, t.second[k].first);
			line += b;
		}
		long long cpuMs = -1;
		for (int i = 0; i < ringCount && i < 32; i++) if (rings[i].tid == t.first)
		{
			char path[64]; snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", rings[i].ostid);
			if (FILE* f = fopen(path, "r")) { long long ns = 0; if (fscanf(f, "%lld", &ns) == 1) cpuMs = ns / 1000000; fclose(f); }
		}
		log("PROF thread %u samples %u cpu_ms_total %lld top:%s", t.first, total, cpuMs, line.c_str());
	}
	profHist.clear();
}
void traceDump()
{
	for (int i = 0; i < ringCount && i < 32; i++)
	{
		for (FILE* f : {stderr, logFp})
		{
			if (!f) continue;
			fprintf(f, "-- thread %u%s, last functions (oldest first):", rings[i].tid, rings[i].ring == ra3_ring ? " (CURRENT)" : "");
			uint32_t p = *rings[i].pos;
			for (uint32_t k = 0; k < 256; k++)
			{
				uint32_t v = rings[i].ring[(p - 256 + k) & 255];
				imports::Entry* e = (v >> 28) == 0xb ? imports::lookup(v ^ 0xc0000000u) : nullptr;
				uint32_t ra = rings[i].ra[(p - 256 + k) & 255];
				if (e) fprintf(f, " %s!%s", e->dll.c_str(), e->name.c_str());
				else if (ra) fprintf(f, " %08x<%08x", v, ra);
				else fprintf(f, " %08x", v);
			}
			fprintf(f, "\n");
			fflush(f);
		}
	}
}
void traceDumpCurrent(int n)
{
	for (FILE* f : {stderr, logFp})
	{
		if (!f) continue;
		fprintf(f, "   writer ring (last %d, oldest first):", n);
		uint32_t p = ra3_ring_pos;
		for (int k = n; k > 0; k--)
		{
			uint32_t v = ra3_ring[(p - k) & 255], ra = ra3_ring_ra[(p - k) & 255];
			imports::Entry* e = (v >> 28) == 0xb ? imports::lookup(v ^ 0xc0000000u) : nullptr;
			if (e) fprintf(f, " %s!%s", e->dll.c_str(), e->name.c_str());
			else if (ra) fprintf(f, " %08x<%08x", v, ra);
			else fprintf(f, " %08x", v);
		}
		fprintf(f, "\n");
		fflush(f);
	}
}
std::atomic<uint32_t> gLowerCallers[64], gLowerKeys[64];
void callStats()
{
	std::vector<std::pair<uint64_t, std::string>> v;
	for (auto& e : imports::all()) if (e.calls && (e.dll.find("d3d") != std::string::npos || e.name.find("::") != std::string::npos || e.name.find("D3DX") != std::string::npos)) v.push_back({e.calls, e.name});
	std::sort(v.rbegin(), v.rend());
	std::string line;
	for (size_t i = 0; i < v.size() && i < 40; i++) line += " " + v[i].second + "=" + std::to_string(v[i].first);
	log("calls:%s", line.c_str());
	std::vector<std::pair<uint64_t, std::string>> w;
	for (auto& e : imports::all()) if (e.calls && !(e.dll.find("d3d") != std::string::npos || e.name.find("::") != std::string::npos || e.name.find("D3DX") != std::string::npos)) w.push_back({e.calls, e.dll + "!" + e.name});
	std::sort(w.rbegin(), w.rend());
	std::string l2;
	for (size_t i = 0; i < w.size() && i < 60; i++) l2 += " " + w[i].second + "=" + std::to_string(w[i].first);
	log("calls-other:%s", l2.c_str());
	{ std::string t; for (int i = 0; i < 64; i++) if (gLowerCallers[i] > 100000) { char b[32]; snprintf(b, sizeof b, " %x=", (unsigned)gLowerKeys[i].load()); t += b + std::to_string(gLowerCallers[i].load()); } log("tolower callers:%s", t.c_str()); }
}
void traceProfile(int ms)
{
	std::map<std::pair<uint32_t, uint32_t>, int> hist;
	int n = ringCount < 32 ? (int)ringCount : 32;
	for (int t = 0; t < ms; t++)
	{
		for (int i = 0; i < n; i++)
		{
			uint32_t v = rings[i].ring[(*rings[i].pos - 1) & 255];
			hist[{rings[i].tid, v}]++;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	for (int i = 0; i < n; i++)
	{
		std::vector<std::pair<int, uint32_t>> v;
		for (auto& kv : hist) if (kv.first.first == rings[i].tid) v.push_back({kv.second, kv.first.second});
		std::sort(v.rbegin(), v.rend());
		std::string line;
		for (size_t k = 0; k < v.size() && k < 6; k++)
		{
			char b[96];
			imports::Entry* e = (v[k].second >> 28) == 0xb ? imports::lookup(v[k].second ^ 0xc0000000u) : nullptr;
			if (e) snprintf(b, sizeof b, " %s:%d", e->name.c_str(), v[k].first);
			else snprintf(b, sizeof b, " %08x:%d", v[k].second, v[k].first);
			line += b;
		}
		log("profile t%u (%d samples):%s", rings[i].tid, ms, line.c_str());
	}
}
#else
void traceRegister(uint32_t) {}
void traceDump() {}
void profDump() {}
void traceDumpCurrent(int) {}
void traceProfile(int) {}
void callStats() {}
#endif

void fatal(const char* f, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof buf, f, ap);
	va_end(ap);
	log("FATAL: %s", buf);
	traceDump();
	if (logFp) fflush(logFp);
	fflush(stderr); _exit(70);
}

// ---- strings
std::string gstr(uint32_t a)
{
	if (!a) return "";
	return std::string(reinterpret_cast<const char*>(guest::host(a)));
}
std::string gstrn(uint32_t a, uint32_t n)
{
	const char* p = reinterpret_cast<const char*>(guest::host(a));
	return std::string(p, strnlen(p, n));
}
std::u16string gwstr(uint32_t a)
{
	std::u16string s;
	if (!a) return s;
	for (;; a += 2) { char16_t ch = guest::load<char16_t>(a); if (!ch) break; s += ch; }
	return s;
}
std::string utf16to8(const std::u16string& s)
{
	std::string o;
	for (size_t i = 0; i < s.size(); i++)
	{
		uint32_t cp = s[i];
		if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < s.size()) { cp = 0x10000 + ((cp - 0xd800) << 10) + (s[++i] - 0xdc00); }
		if (cp < 0x80) o += (char)cp;
		else if (cp < 0x800) { o += (char)(0xc0 | cp >> 6); o += (char)(0x80 | (cp & 0x3f)); }
		else if (cp < 0x10000) { o += (char)(0xe0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 0x3f)); o += (char)(0x80 | (cp & 0x3f)); }
		else { o += (char)(0xf0 | cp >> 18); o += (char)(0x80 | ((cp >> 12) & 0x3f)); o += (char)(0x80 | ((cp >> 6) & 0x3f)); o += (char)(0x80 | (cp & 0x3f)); }
	}
	return o;
}
std::u16string utf8to16(const std::string& s)
{
	std::u16string o;
	for (size_t i = 0; i < s.size();)
	{
		unsigned char ch = s[i];
		uint32_t cp; int n;
		if (ch < 0x80) { cp = ch; n = 1; }
		else if ((ch >> 5) == 6) { cp = ch & 0x1f; n = 2; }
		else if ((ch >> 4) == 14) { cp = ch & 0x0f; n = 3; }
		else if ((ch >> 3) == 30) { cp = ch & 7; n = 4; }
		else { cp = ch; n = 1; }   // stray byte: pass through (Latin-1)
		for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (s[i + k] & 0x3f);
		i += n;
		if (cp >= 0x10000) { cp -= 0x10000; o += (char16_t)(0xd800 + (cp >> 10)); o += (char16_t)(0xdc00 + (cp & 0x3ff)); }
		else o += (char16_t)cp;
	}
	return o;
}
uint32_t putStr(uint32_t dst, const std::string& s)
{
	memcpy(guest::host(dst), s.c_str(), s.size() + 1);
	return (uint32_t)s.size();
}
uint32_t putWStr(uint32_t dst, const std::u16string& s)
{
	memcpy(guest::host(dst), s.c_str(), (s.size() + 1) * 2);
	return (uint32_t)s.size();
}
uint32_t newStr(const std::string& s)
{
	uint32_t a = gmalloc((uint32_t)s.size() + 1);
	putStr(a, s);
	return a;
}

// ---- guest calls
void callGuest(CPU* c, uint32_t fn, std::initializer_list<uint32_t> args, int popBytes)
{
	uint32_t saved = c->r[ESP];
	uint32_t esp = saved;
	std::vector<uint32_t> v(args);
	for (size_t i = v.size(); i-- > 0;) { esp -= 4; guest::store<uint32_t>(esp, v[i]); }
	esp -= 4;
	guest::store<uint32_t>(esp, 0xfffffff4);   // return address, never executed: lifted code returns by C return
	c->r[ESP] = esp;
	::call_target(c, fn);
	c->r[ESP] = saved;   // cdecl and stdcall both end up with the arguments gone
	(void)popBytes;
}
uint32_t callGuestRet(CPU* c, uint32_t fn, std::initializer_list<uint32_t> args, int popBytes)
{
	callGuest(c, fn, args, popBytes);
	return c->r[EAX];
}

// ---- handles
namespace
{
std::mutex hmu;
std::map<uint32_t, std::pair<void*, int>> handles;
uint32_t nextHandle = 0x100;
}
uint32_t newHandle(void* obj, int kind)
{
	std::lock_guard<std::mutex> l(hmu);
	uint32_t h = nextHandle;
	nextHandle += 4;
	handles[h] = {obj, kind};
	return h;
}
void* getHandle(uint32_t h, int kind)
{
	std::lock_guard<std::mutex> l(hmu);
	auto it = handles.find(h);
	return it != handles.end() && (kind == 0 || it->second.second == kind) ? it->second.first : nullptr;
}
void closeHandle(uint32_t h)
{
	std::lock_guard<std::mutex> l(hmu);
	handles.erase(h);
}

// ---- paths: Windows path -> host path, case-insensitive
namespace
{
std::string lowerStr(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); }); return s; }

std::string resolveCI(const std::string& root, const std::string& rel)
{
	std::string cur = root;
	size_t i = 0;
	while (i < rel.size())
	{
		size_t j = rel.find('/', i);
		if (j == std::string::npos) j = rel.size();
		std::string part = rel.substr(i, j - i);
		i = j + 1;
		if (part.empty() || part == ".") continue;
		if (part == "..") { size_t k = cur.rfind('/'); if (k != std::string::npos && k > root.size() - 1) cur = cur.substr(0, k); continue; }
		std::string direct = cur + "/" + part;
		struct stat st;
		if (stat(direct.c_str(), &st) == 0) { cur = direct; continue; }
		DIR* d = opendir(cur.c_str());
		bool found = false;
		if (d)
		{
			std::string want = lowerStr(part);
			while (auto e = readdir(d))
				if (lowerStr(e->d_name) == want) { cur += "/" + std::string(e->d_name); found = true; break; }
			closedir(d);
		}
		if (!found) cur = direct;   // does not exist (yet): keep the requested spelling
	}
	return cur;
}
}

std::string hostPath(const std::string& win)
{
	std::string p = win;
	std::replace(p.begin(), p.end(), '\\', '/');
	bool absolute = (p.size() >= 2 && p[1] == ':') || (!p.empty() && p[0] == '/');
	if (p.size() >= 2 && p[1] == ':') p = p.substr(2);
	while (!p.empty() && p[0] == '/') p.erase(0, 1);
	while (p.rfind("./", 0) == 0) p.erase(0, 2);
	if (!absolute) p = "ra3/" + p;   // relative paths resolve against the game's working directory C:\RA3
	// everything under RA3/ is the game folder; the rest is our writable area
	std::string lp = lowerStr(p);
	if (lp == "ra3" || lp.rfind("ra3/", 0) == 0) return resolveCI(config.gameRoot, p.size() > 4 ? p.substr(4) : "");
	return resolveCI(config.dataRoot, p);
}
}

// ---- dispatcher: every guest call that is not a direct call to a lifted function comes through here
void call_target(CPU* c, uint32_t addr)
{
	using namespace rt;
	#ifdef RA3_TRACE
	{ uint32_t i_ = ra3_ring_pos++ & 255; ra3_ring[i_] = 0xc0000000u ^ addr; ra3_ring_ra[i_] = 0; }   // marker: indirect call target
	#endif
	if (auto* e = imports::lookup(addr))
	{
		if (!e->fn) fatal("unimplemented import %s!%s (called from guest %08x)", e->dll.c_str(), e->name.c_str(), guest::load<uint32_t>(c->r[ESP]));
		static const bool implog = getenv("RA3_IMPLOG") != nullptr;
		if (implog) log("import %s!%s ret=%08x a0=%08x a1=%08x", e->dll.c_str(), e->name.c_str(), guest::load<uint32_t>(c->r[ESP]), guest::load<uint32_t>(c->r[ESP] + 4), guest::load<uint32_t>(c->r[ESP] + 8));
		e->calls++;
		imports::current = e;
		e->fn(c);
		c->r[ESP] += 4 + e->pop;
		return;
	}
	unsigned lo = 0, hi = lifted_count;
	while (lo < hi)
	{
		unsigned mid = (lo + hi) / 2;
		if (lifted_table[mid].addr < addr) lo = mid + 1; else hi = mid;
	}
	if (lo < lifted_count && lifted_table[lo].addr == addr) { lifted_table[lo].fn(c); return; }
	{
		const char* nm[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
		for (int i = 0; i < 8; i++)
		{
			fprintf(stderr, "%s=%08x", nm[i], c->r[i]);
			uint32_t v = c->r[i];
			if (v >= 0x400000 && v < 0x70000000 && i != 4 && i != 0)
			{
				fprintf(stderr, " [");
				for (int k = 0; k < 8; k++) fprintf(stderr, " %08x", guest::load<uint32_t>(v + 4 * k));
				fprintf(stderr, " ]");
			}
			fprintf(stderr, "\n");
		}
	}
	fatal("call to unknown guest address %08x (return address %08x)", addr, guest::load<uint32_t>(c->r[ESP]));
}

void unsupported(CPU* c, uint32_t va, const char* what)
{
	rt::fatal("unsupported instruction at %08x: %s", va, what);
}

uint32_t g_eflags_hi = 0;
uint32_t ra3_stub_tolower = 0xffffffffu;

void ra3_hook_after(CPU* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t ecx)
{
	if (fn == 0x9c6870u)
	{
		static std::atomic<int> cnt{0};
		static std::atomic<int> nz{0};
		uint32_t tex = a1 ? guest::load<uint32_t>(a1 + 0x38) : 0;
		int k = cnt++;
		if (tex) nz++;
		if (k < 30 || (tex && nz < 40) || k % 20000 == 0)
			rt::log("hook SetTextureWrapper this=%08x stage=%u wrapper=%08x d3dtex=%08x (calls %d nonnull %d)", ecx, a0, a1, tex, k, nz.load());
		return;
	}
	if (fn == 0xafa2a0u || fn == 0xaf9790u)
	{
		// dump the LABELS / VALUES lists the graphics options handler built for the UI
		static std::atomic<int> cnt{0};
		if (cnt++ < 14)
		{
			uint32_t first = guest::load<uint32_t>(a0 + 0x10), last = guest::load<uint32_t>(a0 + 0x14);
			rt::log("hook gfx-list fn=%08x type=%u entries=%d", fn, a1, (int)(last - first) / 4);
			static bool tested = true;
			if (!tested && fn == 0xafa2a0u)
			{
				// experiment: ask the game's own string table for the option texts
				tested = true;
				CPU saved = *c;
				uint32_t scratch = c->r[ESP] - 0x800;
				c->r[ESP] -= 0x2000;
				static const char* keys[] = {"APT:AdvancedOption_TerrainLOD_Low", "APT:ADVANCEDOPTION_TERRAINLOD_LOW", "APT:OFF", "APT:MouseSensitivity", "APT:AdvancedOption_EffectsLOD_UltraHigh", "GUI:Low"};
				uint32_t mgr = guest::load<uint32_t>(0xcdb754);
				for (const char* k : keys)
				{
					strcpy((char*)guest::host(scratch + 0x100), k);
					guest::store<uint32_t>(scratch, 0);
					c->r[ECX] = scratch;
					rt::callGuest(c, 0x4cc910, {scratch + 0x100}, 4);
					uint32_t vt = guest::load<uint32_t>(mgr);
					c->r[ECX] = mgr;
					uint32_t res = rt::callGuestRet(c, guest::load<uint32_t>(vt + 0x3c), {scratch, 0u}, 8);
					uint32_t w = (res > 0x400000 && res < 0x7f000000) ? guest::load<uint32_t>(res) : 0;
					std::string txt;
					int len = 0;
					if (w > 0x400000 && w < 0x7f000000)
					{
						len = guest::load<uint16_t>(w - 4);
						for (int i = 0; i < len && i < 24; i++) { char b[8]; uint16_t ch = guest::load<uint16_t>(w + i * 2); if (ch >= 0x20 && ch < 0x7f) snprintf(b, sizeof b, "%c", ch); else snprintf(b, sizeof b, "\\u%04x", ch); txt += b; }
					}
					rt::log("   lookup '%s' -> res=%08x w=%08x len=%d '%s'", k, res, w, len, txt.c_str());
				}
				*c = saved;
			}
			if (fn == 0xafa2a0u && a2 > 0x400000 && a2 < 0x30000000)
			{
				// The game registers the localized text of every list item as a UI variable named after the item
				// label; that registration does not arrive here, so do it through the same game functions.
				CPU saved = *c;
				uint32_t scratch = c->r[ESP] - 0x800;
				c->r[ESP] -= 0x2000;
				std::string prefix1 = (const char*)guest::host(a2);
				uint32_t p2 = (a2 + (uint32_t)prefix1.size() + 1 + 3) & ~3u;
				std::string prefix2 = (const char*)guest::host(p2);
				uint32_t store = guest::load<uint32_t>(0xcebd54), mgr = guest::load<uint32_t>(0xcdb754);
				static int logged = 0;
				if (store && mgr && prefix2.compare(0, 4, "APT:") == 0)
				{
					int count = (int)rt::callGuestRet(c, 0x61c0d0, {a1});
					for (uint32_t i = 0; (int)i < count && i < 8; i++)
					{
						guest::store<uint32_t>(scratch + 0x40, 0);
						rt::callGuest(c, 0x61c040, {scratch + 0x40, a1, i});
						uint32_t np = guest::load<uint32_t>(scratch + 0x40);
						if (np < 0x400000 || !guest::load<uint8_t>(np)) break;
						std::string key2 = prefix2 + (const char*)guest::host(np);
						strcpy((char*)guest::host(scratch + 0x100), key2.c_str());
						guest::store<uint32_t>(scratch, 0);
						c->r[ECX] = scratch;
						rt::callGuest(c, 0x4cc910, {scratch + 0x100}, 4);
						c->r[ECX] = mgr;
						uint32_t res = rt::callGuestRet(c, guest::load<uint32_t>(guest::load<uint32_t>(mgr) + 0x3c), {scratch, 0u}, 8);
						if (res < 0x400000) continue;
						std::string var = prefix1 + std::to_string(i);
						uint32_t h = 0x811c9dc5u; for (char ch : var) h = (h * 0x1000193u) ^ (uint8_t)ch;
						guest::store<uint32_t>(scratch + 0x20, h); guest::store<uint32_t>(scratch + 0x24, 0);
						c->r[ECX] = store;
						uint32_t ok = rt::callGuestRet(c, 0x91b040, {scratch + 0x20, res, 1u}, 12) & 0xff;
						if (logged++ < 12) rt::log("   set var '%s' <- text of '%s': %u", var.c_str(), key2.c_str(), ok);
					}
				}
				*c = saved;
			}
			static int probes = 0;
			if (false && fn == 0xafa2a0u && probes++ < 2)
			{
				// experiment: is the per-item text registered in the UI variable map, and how does a "$label" resolve?
				CPU saved = *c;
				uint32_t scratch = c->r[ESP] - 0x800;
				c->r[ESP] -= 0x2000;
				auto wstr = [&](uint32_t u) {
					std::string txt; if (u < 0x400000 || u >= 0x7f000000) return std::string("<null>");
					int len = guest::load<uint16_t>(u - 4);
					for (int i = 0; i < len && i < 28; i++) { char b[8]; uint16_t ch = guest::load<uint16_t>(u + i * 2); if (ch >= 0x20 && ch < 0x7f) snprintf(b, sizeof b, "%c", ch); else snprintf(b, sizeof b, "\\u%04x", ch); txt += b; }
					return txt; };
				uint32_t store = guest::load<uint32_t>(0xcebd54);
				static const char* keys[] = {"APT:GfxTerrainDetail_0", "APT:GfxTerrainDetail_1", "APT:GfxVFXDetail_2", "$APT:GfxTerrainDetail_0", "APT:OFF"};
				for (const char* k : keys)
				{
					uint32_t h = 0x811c9dc5u; for (const char* q = k; *q; q++) h = (h * 0x1000193u) ^ (uint8_t)*q;
					guest::store<uint32_t>(scratch + 0x20, h); guest::store<uint32_t>(scratch + 0x24, 0);
					c->r[ECX] = store + 0xa4;
					rt::callGuest(c, 0x918340, {scratch + 0x30, scratch + 0x20}, 8);
					uint32_t node = guest::load<uint32_t>(scratch + 0x30);
					uint32_t nb = guest::load<uint32_t>(store + 0xa4 + 8), tab = guest::load<uint32_t>(store + 0xa4 + 4);
					uint32_t endn = guest::load<uint32_t>(tab + nb * 4);
					if (node == endn || node < 0x400000) { rt::log("   var '%s' hash=%08x: NOT in map (buckets=%u)", k, h, nb); continue; }
					rt::log("   var '%s' hash=%08x: node=%08x subs=%d val10='%s' val14='%s' flag18=%u", k, h, node,
						(int)(guest::load<uint32_t>(node + 8 + 4) - guest::load<uint32_t>(node + 8)) / 4,
						wstr(guest::load<uint32_t>(node + 8 + 0x10)).c_str(), wstr(guest::load<uint32_t>(node + 8 + 0x14)).c_str(), guest::load<uint8_t>(node + 8 + 0x18));
				}
				static const char* labels[] = {"$APT:GfxTerrainDetail_0", "$APT:OFF", "$APT:AdvancedOption_TerrainLOD_Low"};
				for (const char* k : labels)
				{
					strcpy((char*)guest::host(scratch + 0x100), k);
					guest::store<uint32_t>(scratch, 0); guest::store<uint32_t>(scratch + 8, 0);
					c->r[ECX] = scratch;
					rt::callGuest(c, 0x4cbbc0, {scratch + 0x100}, 4);
					uint32_t res = rt::callGuestRet(c, 0x917fd0, {scratch, scratch + 8, 0u});
					rt::log("   resolve '%s' (in='%s') -> '%s'", k, wstr(guest::load<uint32_t>(scratch)).c_str(), (res > 0x400000 && res < 0x7f000000) ? wstr(guest::load<uint32_t>(res)).c_str() : "<bad>");
				}
				*c = saved;
			}
			if (fn == 0xafa2a0u && a2 > 0x400000 && a2 < 0x30000000)
			{
				// the label prefix (a2) and the string-table prefix live next to each other in the component data
				std::string near;
				for (uint32_t k = 0; k < 0x70; k++) { uint8_t ch = guest::load<uint8_t>(a2 - 0x10 + k); near += (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.'; }
				rt::log("   near a2: %s", near.c_str());
			}
			for (uint32_t e = first; e < last && e < first + 64; e += 4)
			{
				uint32_t ent = guest::load<uint32_t>(e);
				if (ent < 0x400000) continue;
				uint32_t vb = guest::load<uint32_t>(ent), ve = guest::load<uint32_t>(ent + 4);
				std::string line;
				for (uint32_t v = vb; v < ve && v < vb + 0x404 * 12; v += 0x404)
				{
					uint32_t t = guest::load<uint32_t>(v);
					char b[96];
					if (t == 3) snprintf(b, sizeof b, " '%.60s'", (const char*)guest::host(v + 4));
					else snprintf(b, sizeof b, " %u:%d", t, (int)guest::load<uint32_t>(v + 4));
					line += b;
				}
				rt::log("   entry '%.32s' sel=%d n=%d:%s", (const char*)guest::host(ent + 0x20), (int)guest::load<uint32_t>(ent + 0xa0), (int)(ve - vb) / 0x404, line.c_str());
			}
		}
	}
	if (fn == 0x9b85c0u)
	{
		static std::atomic<int> cnt{0};
		int k = cnt++;
		if (k < 6 || k == 500)
		{
			rt::log("hook apply-params this=%08x masks lo=%08x hi=%08x", ecx, a0, a1);
			{
				uint32_t o = guest::load<uint32_t>(ecx + 0x74);
				uint32_t vt = o ? guest::load<uint32_t>(o) : 0;
				rt::log("   texobj [this+0x74]=%08x vtbl %08x slot4 %08x mask %08x:%08x  [this+0x60]=%08x [this+0x68]=%08x [this+0x6c]=%08x", o, vt, vt ? guest::load<uint32_t>(vt + 0x10) : 0, o ? guest::load<uint32_t>(o + 0x34) : 0, o ? guest::load<uint32_t>(o + 0x30) : 0, guest::load<uint32_t>(ecx + 0x60), guest::load<uint32_t>(ecx + 0x68), guest::load<uint32_t>(ecx + 0x6c));
			}
			uint32_t p = guest::load<uint32_t>(ecx + 0x24);
			int n = 0;
			while (p && n < 60)
			{
				uint32_t nd = p - 8;
				rt::log("   node %08x mask %08x:%08x setter %08x arg %08x", nd, guest::load<uint32_t>(nd + 0x1c), guest::load<uint32_t>(nd + 0x18), guest::load<uint32_t>(nd + 0x20), guest::load<uint32_t>(nd + 0x28));
				p = guest::load<uint32_t>(nd + 0xc);
				if (!p) break;
				n++;
			}
		}
		return;
	}
	rt::log("hook %08x(ecx=%08x, %08x, %08x, %08x) -> eax=%08x", fn, ecx, a0, a1, a2, c->r[EAX]);
	auto tryStr = [](uint32_t p) -> std::string {
		if (p < 0x400000 || (p >= 0x30000000 && p < 0x71000000) || p >= 0x7f000000) return "";
		const unsigned char* q = (const unsigned char*)guest::host(p);
		std::string r;
		for (int i = 0; i < 100; i++) { if (!q[i]) return r.size() >= 2 ? r : ""; if (q[i] < 32 || q[i] > 126) return ""; r += (char)q[i]; }
		return r;
	};
	std::string s0 = tryStr(a0), s1 = tryStr(a1), s2 = tryStr(a2);
	if (!s0.empty() || !s1.empty() || !s2.empty()) rt::log("   strs: a0='%s' a1='%s' a2='%s'", s0.c_str(), s1.c_str(), s2.c_str());
}
