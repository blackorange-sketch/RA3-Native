#include <functional>
#include <unistd.h>
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
	fprintf(stderr, "%s\n", buf);
	if (logFp) { fprintf(logFp, "%s\n", buf); fflush(logFp); }
	if (logHook) logHook(buf);
}

#ifdef RA3_TRACE
}
thread_local uint32_t ra3_ring[256];
thread_local uint32_t ra3_ring_pos;
namespace rt {
static struct { uint32_t* ring; uint32_t* pos; uint32_t tid; } rings[32];
static std::atomic<int> ringCount{0};
void traceRegister(uint32_t tid)
{
	int i = ringCount++;
	if (i < 32) rings[i] = {ra3_ring, &ra3_ring_pos, tid};
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
			for (uint32_t k = 0; k < 256; k++) fprintf(f, " %08x", rings[i].ring[(p - 256 + k) & 255]);
			fprintf(f, "\n");
			fflush(f);
		}
	}
}
#else
void traceRegister(uint32_t) {}
void traceDump() {}
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
	ra3_ring[ra3_ring_pos++ & 255] = 0xc0000000u ^ addr;   // marker: indirect call target
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

void ra3_hook_after(CPU* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t ecx)
{
	rt::log("hook %08x(ecx=%08x, %08x, %08x, %08x) -> eax=%08x", fn, ecx, a0, a1, a2, c->r[EAX]);
	auto tryStr = [](uint32_t p) -> std::string {
		if (p < 0x400000 || (p >= 0x60000000 && p < 0x71000000) || p >= 0x7f000000) return "";
		const unsigned char* q = (const unsigned char*)guest::host(p);
		std::string r;
		for (int i = 0; i < 100; i++) { if (!q[i]) return r.size() >= 2 ? r : ""; if (q[i] < 32 || q[i] > 126) return ""; r += (char)q[i]; }
		return r;
	};
	std::string s0 = tryStr(a0), s1 = tryStr(a1), s2 = tryStr(a2);
	if (!s0.empty() || !s1.empty() || !s2.empty()) rt::log("   strs: a0='%s' a1='%s' a2='%s'", s0.c_str(), s1.c_str(), s2.c_str());
}
