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

void log(const char* f, ...)
{
	std::lock_guard<std::mutex> l(logMu);
	if (!logFp && !config.logFile.empty()) logFp = fopen(config.logFile.c_str(), "a");
	va_list ap;
	va_start(ap, f);
	vfprintf(stderr, f, ap);
	fputc('\n', stderr);
	va_end(ap);
	if (logFp)
	{
		va_start(ap, f);
		vfprintf(logFp, f, ap);
		fputc('\n', logFp);
		fflush(logFp);
		va_end(ap);
	}
}

void fatal(const char* f, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof buf, f, ap);
	va_end(ap);
	log("FATAL: %s", buf);
	if (logFp) fflush(logFp);
	exit(70);
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
	if (p.size() >= 2 && p[1] == ':') p = p.substr(2);
	while (!p.empty() && p[0] == '/') p.erase(0, 1);
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
	if (auto* e = imports::lookup(addr))
	{
		if (!e->fn) fatal("unimplemented import %s!%s (called from guest %08x)", e->dll.c_str(), e->name.c_str(), guest::load<uint32_t>(c->r[ESP]));
		e->calls++;
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
	fatal("call to unknown guest address %08x (return address %08x)", addr, guest::load<uint32_t>(c->r[ESP]));
}

void unsupported(CPU* c, uint32_t va, const char* what)
{
	rt::fatal("unsupported instruction at %08x: %s", va, what);
}
