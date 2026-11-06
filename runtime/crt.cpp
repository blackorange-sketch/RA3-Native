#include <atomic>
// MSVCR80 replacement: the C runtime the game imports.
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <ctime>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

#include "rt.h"

#define DLL "MSVCR80.dll"
using namespace rt;

namespace
{
uint32_t iobBase;
std::mutex fmu;
std::map<uint32_t, FILE*> files;   // guest FILE* -> host FILE*
uint32_t nextFileId = 3;

FILE* hostFile(uint32_t g)
{
	if (g == iobBase) return stdin;
	if (g == iobBase + 32) return stdout;
	if (g == iobBase + 64) return stderr;
	std::lock_guard<std::mutex> l(fmu);
	auto it = files.find(g);
	return it == files.end() ? nullptr : it->second;
}

uint32_t iobInit() { iobBase = gmalloc(96); memset(guest::host(iobBase), 0, 96); return iobBase; }

std::string cmdline() { return "\"C:\\RA3\\Data\\ra3_1.12.game\" -config RA3_russian_1.12.SkuDef -win"; }

uint32_t acmdlnInit() { return newStr(cmdline()); }

int errnoVal;
uint32_t errnoAddr()
{
	static uint32_t a = gmalloc(4);
	return a;
}

uint32_t randSeed = 1;

uint32_t crtFcwFromGuest(uint16_t fcw)
{
	uint32_t r = 0;
	r |= (fcw & 0x3f);                           // exception masks -> _EM_* occupy the same low bits
	uint32_t pc = (fcw >> 8) & 3;
	r |= pc == 0 ? 0x20000 : pc == 2 ? 0x10000 : 0;
	r |= ((fcw >> 10) & 3) << 8;
	return r;
}
void crtFcwApply(CPU* c, uint32_t val, uint32_t mask)
{
	uint16_t fcw = c->fcw;
	if (mask & 0x30000) { uint32_t pc = val & 0x30000; fcw = (fcw & ~0x300) | (pc == 0x20000 ? 0 : pc == 0x10000 ? 0x200 : 0x300); }
	if (mask & 0x300) fcw = (fcw & ~0xc00) | (((val >> 8) & 3) << 10);
	if (mask & 0x3f) fcw = (fcw & ~0x3f) | (val & 0x3f);
	c->fcw = fcw;
}

std::string defaultStrtokState;
}

namespace
{
const struct DataRegs
{
	DataRegs()
	{
		imports::dataAlloc = [](uint32_t n) { return gmalloc(n); };
		imports::registerData(DLL, "_acmdln", 4, acmdlnInit);
		imports::registerData(DLL, "_adjust_fdiv", 4, nullptr);
	}
} dataRegs;
}

// ---- memory
IMP_C(DLL, malloc) { SET(gmalloc(ARG(0))); }
IMP_C(DLL, calloc) { uint32_t n = ARG(0) * ARG(1); uint32_t p = gmalloc(n); memset(guest::host(p), 0, n); SET(p); }
IMP_C(DLL, realloc) { SET(grealloc(ARG(0), ARG(1))); }
IMP_C(DLL, free) { gfree(ARG(0)); }
IMP_C(DLL, _aligned_malloc) { SET(gmalloc(ARG(0), ARG(1))); }
IMP_C(DLL, _aligned_free) { gfree(ARG(0)); }
IMP_C(DLL, _aligned_realloc) { uint32_t n = gmalloc(ARG(1), ARG(2)); if (ARG(0)) { memcpy(guest::host(n), guest::host(ARG(0)), std::min(gsize(ARG(0)), ARG(1))); gfree(ARG(0)); } SET(n); }
IMP_C(DLL, memcpy) { memcpy(guest::host(ARG(0)), guest::host(ARG(1)), ARG(2)); SET(ARG(0)); }
IMP_C(DLL, memmove) { memmove(guest::host(ARG(0)), guest::host(ARG(1)), ARG(2)); SET(ARG(0)); }
IMP_C(DLL, memset) { memset(guest::host(ARG(0)), (int)ARG(1), ARG(2)); SET(ARG(0)); }
IMP_C(DLL, memchr) { void* p = memchr(guest::host(ARG(0)), (int)ARG(1), ARG(2)); SET(p ? (uint8_t*)p - guest::base : 0); }

// ---- ctype
#define CTYPE(name, expr) IMP_C(DLL, name) { int ch = (int)ARG(0); SET((expr) ? 1 : 0); }
CTYPE(isalpha, ch >= 0 && ch < 256 && isalpha(ch)) CTYPE(isdigit, ch >= 0 && ch < 256 && isdigit(ch)) CTYPE(isalnum, ch >= 0 && ch < 256 && isalnum(ch))
CTYPE(isspace, ch >= 0 && ch < 256 && isspace(ch)) CTYPE(isupper, ch >= 0 && ch < 256 && isupper(ch)) CTYPE(islower, ch >= 0 && ch < 256 && islower(ch))
CTYPE(ispunct, ch >= 0 && ch < 256 && ispunct(ch)) CTYPE(isprint, ch >= 0 && ch < 256 && isprint(ch)) CTYPE(iscntrl, ch >= 0 && ch < 256 && iscntrl(ch))
CTYPE(isxdigit, ch >= 0 && ch < 256 && isxdigit(ch)) CTYPE(iswspace, ch == ' ' || (ch >= 9 && ch <= 13)) CTYPE(iswalpha, iswalpha((wint_t)ch))
CTYPE(iswalnum, iswalnum((wint_t)ch)) CTYPE(_ismbblead, false)
IMP_C(DLL, toupper) { int ch = (int)ARG(0); SET(ch >= 0 && ch < 256 ? toupper(ch) : ch); }
IMP_C(DLL, tolower) { { uint32_t ra = guest::load<uint32_t>(c->r[ESP]); uint32_t h = (ra >> 2) & 63; gLowerKeys[h] = ra; gLowerCallers[h]++; } int ch = (int)ARG(0); SET(ch >= 0 && ch < 256 ? tolower(ch) : ch); }
IMP_C(DLL, towlower) { SET(towlower((wint_t)ARG(0))); }

// ---- strings
IMP_C(DLL, strncpy) { char* d = (char*)guest::host(ARG(0)); const char* s = (const char*)guest::host(ARG(1)); strncpy(d, s, ARG(2)); SET(ARG(0)); }
IMP_C(DLL, strncmp) { SET((uint32_t)strncmp((char*)guest::host(ARG(0)), (char*)guest::host(ARG(1)), ARG(2))); }
IMP_C(DLL, _stricmp) { SET((uint32_t)strcasecmp((char*)guest::host(ARG(0)), (char*)guest::host(ARG(1)))); }
IMP_C(DLL, _strnicmp) { SET((uint32_t)strncasecmp((char*)guest::host(ARG(0)), (char*)guest::host(ARG(1)), ARG(2))); }
IMP_C(DLL, strcoll) { SET((uint32_t)strcmp((char*)guest::host(ARG(0)), (char*)guest::host(ARG(1)))); }
IMP_C(DLL, strchr) { char* p = strchr((char*)guest::host(ARG(0)), (int)ARG(1)); SET(p ? (uint8_t*)p - guest::base : 0); }
IMP_C(DLL, strrchr) { char* p = strrchr((char*)guest::host(ARG(0)), (int)ARG(1)); SET(p ? (uint8_t*)p - guest::base : 0); }
IMP_C(DLL, strstr) { char* p = strstr((char*)guest::host(ARG(0)), (char*)guest::host(ARG(1))); SET(p ? (uint8_t*)p - guest::base : 0); }
IMP_C(DLL, strspn) { SET(strspn((char*)guest::host(ARG(0)), (char*)guest::host(ARG(1)))); }
IMP_C(DLL, strcspn) { SET(strcspn((char*)guest::host(ARG(0)), (char*)guest::host(ARG(1)))); }
IMP_C(DLL, strpbrk) { char* p = strpbrk((char*)guest::host(ARG(0)), (char*)guest::host(ARG(1))); SET(p ? (uint8_t*)p - guest::base : 0); }
IMP_C(DLL, strncat) { strncat((char*)guest::host(ARG(0)), (char*)guest::host(ARG(1)), ARG(2)); SET(ARG(0)); }
IMP_C(DLL, _strlwr) { for (char* p = (char*)guest::host(ARG(0)); *p; p++) *p = (char)tolower((unsigned char)*p); SET(ARG(0)); }
IMP_C(DLL, strcpy_s)
{
	std::string s = gstr(ARG(2));
	if (!ARG(0) || s.size() + 1 > ARG(1)) { if (ARG(0) && ARG(1)) guest::store<uint8_t>(ARG(0), 0); SET(34); return; }
	putStr(ARG(0), s);
	SET(0);
}
IMP_C(DLL, strncpy_s)
{
	uint32_t count = ARG(3);
	std::string s = gstr(ARG(2));
	if (count != 0xffffffffu && s.size() > count) s.resize(count);
	if (s.size() + 1 > ARG(1)) { guest::store<uint8_t>(ARG(0), 0); SET(34); return; }
	putStr(ARG(0), s);
	SET(0);
}
IMP_C(DLL, strcat_s)
{
	std::string d = gstr(ARG(0)), s = gstr(ARG(2));
	if (d.size() + s.size() + 1 > ARG(1)) { guest::store<uint8_t>(ARG(0), 0); SET(34); return; }
	putStr(ARG(0) + (uint32_t)d.size(), s);
	SET(0);
}
IMP_C(DLL, strncat_s)
{
	std::string d = gstr(ARG(0)), s = gstr(ARG(2));
	if (ARG(3) != 0xffffffffu && s.size() > ARG(3)) s.resize(ARG(3));
	if (d.size() + s.size() + 1 > ARG(1)) { guest::store<uint8_t>(ARG(0), 0); SET(34); return; }
	putStr(ARG(0) + (uint32_t)d.size(), s);
	SET(0);
}
IMP_C(DLL, strtok)
{
	static thread_local char* save = nullptr;
	char* r = strtok_r(ARG(0) ? (char*)guest::host(ARG(0)) : nullptr, (char*)guest::host(ARG(1)), &save);
	SET(r ? (uint8_t*)r - guest::base : 0);
}
IMP_C(DLL, strtok_s)
{
	uint32_t ctx = ARG(2);
	char* save = (char*)(guest::load<uint32_t>(ctx) ? guest::host(guest::load<uint32_t>(ctx)) : nullptr);
	char* r = strtok_r(ARG(0) ? (char*)guest::host(ARG(0)) : save, (char*)guest::host(ARG(1)), &save);
	guest::store<uint32_t>(ctx, save ? (uint32_t)((uint8_t*)save - guest::base) : 0);
	SET(r ? (uint8_t*)r - guest::base : 0);
}
// wide strings (16-bit wchar_t)
IMP_C(DLL, wcsstr) { std::u16string h = gwstr(ARG(0)), n = gwstr(ARG(1)); size_t p = h.find(n); SET(p == std::u16string::npos ? 0 : ARG(0) + 2 * p); }
IMP_C(DLL, wcschr) { std::u16string h = gwstr(ARG(0)); size_t p = h.find((char16_t)ARG(1)); SET(p == std::u16string::npos ? 0 : ARG(0) + 2 * p); }
IMP_C(DLL, wcsrchr) { std::u16string h = gwstr(ARG(0)); size_t p = h.rfind((char16_t)ARG(1)); SET(p == std::u16string::npos ? 0 : ARG(0) + 2 * p); }
IMP_C(DLL, wcsncpy)
{
	uint32_t d = ARG(0), s = ARG(1), n = ARG(2);
	bool end = false;
	for (uint32_t i = 0; i < n; i++)
	{
		char16_t ch = end ? 0 : guest::load<char16_t>(s + 2 * i);
		if (!ch) end = true;
		guest::store<char16_t>(d + 2 * i, ch);
	}
	SET(d);
}
IMP_C(DLL, _wtoi) { SET((uint32_t)atoi(utf16to8(gwstr(ARG(0))).c_str())); }
IMP_C(DLL, _itow) { std::string s; char b[40]; int r = (int)ARG(2); int v = (int)ARG(0); if (r == 10) snprintf(b, sizeof b, "%d", v); else if (r == 16) snprintf(b, sizeof b, "%x", (unsigned)v); else snprintf(b, sizeof b, "%d", v); putWStr(ARG(1), utf8to16(b)); SET(ARG(1)); }

// ---- number <-> string
IMP_C(DLL, atoi) { SET((uint32_t)atoi(gstr(ARG(0)).c_str())); }
IMP_C(DLL, atol) { SET((uint32_t)atol(gstr(ARG(0)).c_str())); }
IMP_C(DLL, atof) { RET_F(atof(gstr(ARG(0)).c_str())); }
IMP_C(DLL, strtod)
{
	const char* s = (const char*)guest::host(ARG(0));
	char* end;
	double v = strtod(s, &end);
	if (ARG(1)) guest::store<uint32_t>(ARG(1), ARG(0) + (uint32_t)(end - s));
	RET_F(v);
}
IMP_C(DLL, strtol)
{
	const char* s = (const char*)guest::host(ARG(0));
	char* end;
	long v = strtol(s, &end, (int)ARG(2));
	if (ARG(1)) guest::store<uint32_t>(ARG(1), ARG(0) + (uint32_t)(end - s));
	SET((uint32_t)v);
}
IMP_C(DLL, strtoul)
{
	const char* s = (const char*)guest::host(ARG(0));
	char* end;
	unsigned long v = strtoul(s, &end, (int)ARG(2));
	if (ARG(1)) guest::store<uint32_t>(ARG(1), ARG(0) + (uint32_t)(end - s));
	SET((uint32_t)v);
}
static void ret64(CPU* c, uint64_t v) { c->r[EAX] = (uint32_t)v; c->r[EDX] = (uint32_t)(v >> 32); }
IMP_C(DLL, _atoi64) { ret64(c, (uint64_t)atoll(gstr(ARG(0)).c_str())); }
IMP_C(DLL, _strtoui64)
{
	const char* s = (const char*)guest::host(ARG(0));
	char* end;
	unsigned long long v = strtoull(s, &end, (int)ARG(2));
	if (ARG(1)) guest::store<uint32_t>(ARG(1), ARG(0) + (uint32_t)(end - s));
	ret64(c, v);
}
static std::string toRadix(unsigned long long v, int radix, bool neg)
{
	if (v == 0) return "0";
	std::string s;
	while (v) { s += "0123456789abcdefghijklmnopqrstuvwxyz"[v % radix]; v /= radix; }
	if (neg) s += '-';
	std::reverse(s.begin(), s.end());
	return s;
}
IMP_C(DLL, _itoa) { int v = (int)ARG(0), r = (int)ARG(2); putStr(ARG(1), r == 10 && v < 0 ? toRadix((unsigned long long)(-(long long)v), 10, true) : toRadix(r == 10 ? (unsigned)v : (unsigned)v, r, false)); SET(ARG(1)); }
IMP_C(DLL, _ultoa) { putStr(ARG(1), toRadix(ARG(0), (int)ARG(2), false)); SET(ARG(1)); }
IMP_C(DLL, _i64toa) { int64_t v = (int64_t)(ARG(0) | (uint64_t)ARG(1) << 32); int r = (int)ARG(3); putStr(ARG(2), r == 10 && v < 0 ? toRadix((unsigned long long)(-v), 10, true) : toRadix((unsigned long long)v, r, false)); SET(ARG(2)); }
IMP_C(DLL, _ui64toa) { uint64_t v = ARG(0) | (uint64_t)ARG(1) << 32; putStr(ARG(2), toRadix(v, (int)ARG(3), false)); SET(ARG(2)); }

// ---- formatted output
static void emit(FILE* f, const std::string& s) { fwrite(s.data(), 1, s.size(), f); }
IMP_C(DLL, printf) { ArgReader ap{c->r[ESP] + 8}; emit(stdout, formatA(gstr(ARG(0)), ap)); SET(0); }
IMP_C(DLL, fprintf) { ArgReader ap{c->r[ESP] + 12}; FILE* f = hostFile(ARG(0)); std::string s = formatA(gstr(ARG(1)), ap); if (f) emit(f, s); SET(s.size()); }
IMP_C(DLL, sprintf) { ArgReader ap{c->r[ESP] + 12}; SET(putStr(ARG(0), formatA(gstr(ARG(1)), ap))); }
IMP_C(DLL, vsprintf) { ArgReader ap{ARG(2)}; SET(putStr(ARG(0), formatA(gstr(ARG(1)), ap))); }
static void snprintfImpl(CPU* c, uint32_t dst, uint32_t n, const std::string& s)
{
	if (s.size() >= n) { memcpy(guest::host(dst), s.data(), n); SET(n == 0 ? (uint32_t)-1 : (uint32_t)-1); if (n) guest::store<uint8_t>(dst + n - 1, 0); return; }   // MSVC returns -1 on truncation
	putStr(dst, s);
	SET(s.size());
}
IMP_C(DLL, _snprintf) { ArgReader ap{c->r[ESP] + 16}; snprintfImpl(c, ARG(0), ARG(1), formatA(gstr(ARG(2)), ap)); }
IMP_C(DLL, _vsnprintf) { ArgReader ap{ARG(3)}; snprintfImpl(c, ARG(0), ARG(1), formatA(gstr(ARG(2)), ap)); }
IMP_C(DLL, vsprintf_s) { ArgReader ap{ARG(3)}; std::string s = formatA(gstr(ARG(2)), ap); if (s.size() + 1 > ARG(1)) { guest::store<uint8_t>(ARG(0), 0); SET((uint32_t)-1); return; } SET(putStr(ARG(0), s)); }
static void wsnprintf(CPU* c, uint32_t dst, uint32_t n, const std::u16string& s)
{
	if (s.size() >= n) { memcpy(guest::host(dst), s.data(), n * 2); if (n) guest::store<char16_t>(dst + 2 * (n - 1), 0); SET((uint32_t)-1); return; }
	putWStr(dst, s);
	SET(s.size());
}
IMP_C(DLL, _snwprintf) { ArgReader ap{c->r[ESP] + 16}; wsnprintf(c, ARG(0), ARG(1), formatW(gwstr(ARG(2)), ap)); }
IMP_C(DLL, _vsnwprintf) { ArgReader ap{ARG(3)}; wsnprintf(c, ARG(0), ARG(1), formatW(gwstr(ARG(2)), ap)); }
IMP_C(DLL, sscanf) { ArgReader ap{c->r[ESP] + 12}; SET((uint32_t)scanA(gstr(ARG(0)), gstr(ARG(1)), ap)); }
IMP_C(DLL, sscanf_s) { ArgReader ap{c->r[ESP] + 12}; SET((uint32_t)scanA(gstr(ARG(0)), gstr(ARG(1)), ap, true)); }
IMP_C(DLL, fputs) { FILE* f = hostFile(ARG(1)); if (f) fputs(gstr(ARG(0)).c_str(), f); SET(0); }
IMP_C(DLL, wsprintfA_unused) { SET(0); }

// ---- stdio files
IMP_C(DLL, __iob_func) { SET(iobBase ? iobBase : iobInit()); }
static uint32_t openHost(const std::string& winPath, const std::string& mode)
{
	std::string m;
	for (char ch : mode) if (ch != 'b' && ch != 't') m += ch;
	if (mode.find('b') != std::string::npos || true) m += 'b';
	std::string hp = hostPath(winPath);
	if (m[0] != 'r') { size_t k = hp.rfind('/'); if (k != std::string::npos) { std::string d = hp.substr(0, k); for (size_t i = 1; i < d.size(); i++) if (d[i] == '/') mkdir(d.substr(0, i).c_str(), 0777); mkdir(d.c_str(), 0777); } }
	FILE* f = fopen(hp.c_str(), m.c_str());
	if (!f) return 0;
	uint32_t g = gmalloc(32);
	memset(guest::host(g), 0, 32);
	std::lock_guard<std::mutex> l(fmu);
	files[g] = f;
	guest::store<uint32_t>(g + 16, nextFileId++);
	return g;
}
IMP_C(DLL, fopen) { SET(openHost(gstr(ARG(0)), gstr(ARG(1)))); }
IMP_C(DLL, _wfopen) { SET(openHost(utf16to8(gwstr(ARG(0))), utf16to8(gwstr(ARG(1))))); }
IMP_C(DLL, freopen) { SET(0); }
IMP_C(DLL, fclose)
{
	FILE* f = hostFile(ARG(0));
	if (f && f != stdin && f != stdout && f != stderr) { fclose(f); std::lock_guard<std::mutex> l(fmu); files.erase(ARG(0)); gfree(ARG(0)); }
	SET(0);
}
IMP_C(DLL, fread) { FILE* f = hostFile(ARG(3)); SET(f ? fread(guest::host(ARG(0)), ARG(1), ARG(2), f) : 0); }
IMP_C(DLL, fwrite) { FILE* f = hostFile(ARG(3)); SET(f ? fwrite(guest::host(ARG(0)), ARG(1), ARG(2), f) : 0); }
IMP_C(DLL, fseek) { FILE* f = hostFile(ARG(0)); SET(f ? (uint32_t)fseek(f, (int32_t)ARG(1), (int)ARG(2)) : (uint32_t)-1); }
IMP_C(DLL, ftell) { FILE* f = hostFile(ARG(0)); SET(f ? (uint32_t)ftell(f) : (uint32_t)-1); }
IMP_C(DLL, rewind) { FILE* f = hostFile(ARG(0)); if (f) rewind(f); }
IMP_C(DLL, fflush) { FILE* f = ARG(0) ? hostFile(ARG(0)) : nullptr; if (f) fflush(f); SET(0); }
IMP_C(DLL, feof) { FILE* f = hostFile(ARG(0)); SET(f ? feof(f) : 1); }
IMP_C(DLL, ferror) { FILE* f = hostFile(ARG(0)); SET(f ? ferror(f) : 1); }
IMP_C(DLL, fgetc) { FILE* f = hostFile(ARG(0)); SET(f ? (uint32_t)fgetc(f) : (uint32_t)-1); }
IMP_C(DLL, ungetc) { FILE* f = hostFile(ARG(1)); SET(f ? (uint32_t)ungetc((int)ARG(0), f) : (uint32_t)-1); }
IMP_C(DLL, fgets)
{
	FILE* f = hostFile(ARG(2));
	char* r = f ? fgets((char*)guest::host(ARG(0)), (int)ARG(1), f) : nullptr;
	SET(r ? ARG(0) : 0);
}
IMP_C(DLL, _fileno) { SET(guest::load<uint32_t>(ARG(0) + 16)); }
IMP_C(DLL, _fstat64i32)
{
	// fd is the fake id stored in the guest FILE; struct _stat64i32 is 48 bytes
	FILE* f = nullptr;
	{
		std::lock_guard<std::mutex> l(fmu);
		for (auto& [g, hf] : files) if (guest::load<uint32_t>(g + 16) == ARG(0)) { f = hf; break; }
	}
	struct stat st{};
	if (!f || fstat(fileno(f), &st) != 0) { SET((uint32_t)-1); return; }
	uint32_t o = ARG(1);
	memset(guest::host(o), 0, 48);
	guest::store<uint16_t>(o + 6, (uint16_t)(S_ISDIR(st.st_mode) ? 0x4000 : 0x8000));
	guest::store<uint16_t>(o + 8, 1);
	guest::store<uint32_t>(o + 20, (uint32_t)st.st_size);
	guest::store<uint64_t>(o + 24, (uint64_t)st.st_atime);
	guest::store<uint64_t>(o + 32, (uint64_t)st.st_mtime);
	guest::store<uint64_t>(o + 40, (uint64_t)st.st_ctime);
	SET(0);
}
IMP_C(DLL, mbstowcs)
{
	std::u16string w = utf8to16(gstr(ARG(1)));
	if (!ARG(0)) { SET((uint32_t)w.size()); return; }
	uint32_t n = std::min<uint32_t>((uint32_t)w.size(), ARG(2));
	for (uint32_t i = 0; i < n; i++) guest::store<uint16_t>(ARG(0) + 2 * i, w[i]);
	if (n < ARG(2)) guest::store<uint16_t>(ARG(0) + 2 * n, 0);
	SET(n);
}
// fscanf: scans one line per call (enough for the line-oriented config files the game reads)
IMP_C(DLL, fscanf)
{
	FILE* f = hostFile(ARG(0));
	char line[2048];
	if (!f || !fgets(line, sizeof line, f)) { SET((uint32_t)-1); return; }
	ArgReader ap{c->r[ESP] + 12};
	SET((uint32_t)scanA(line, gstr(ARG(1)), ap));
}
IMP_C(DLL, remove) { SET((uint32_t)remove(hostPath(gstr(ARG(0))).c_str())); }
IMP_C(DLL, _unlink) { SET((uint32_t)unlink(hostPath(gstr(ARG(0))).c_str())); }
IMP_C(DLL, rename) { SET((uint32_t)rename(hostPath(gstr(ARG(0))).c_str(), hostPath(gstr(ARG(1))).c_str())); }
IMP_C(DLL, tmpnam) { static uint32_t buf = gmalloc(64); static int n; putStr(buf, "tmp" + std::to_string(++n)); SET(ARG(0) ? (putStr(ARG(0), gstr(buf)), ARG(0)) : buf); }

// ---- time
static uint32_t tmBuf()
{
	static thread_local uint32_t a = gmalloc(64);
	return a;
}
static void putTm(uint32_t a, const struct tm& t)
{
	int v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
	for (int i = 0; i < 9; i++) guest::store<int32_t>(a + 4 * i, v[i]);
}
static struct tm getTm(uint32_t a)
{
	struct tm t{};
	int* v[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
	for (int i = 0; i < 9; i++) *v[i] = guest::load<int32_t>(a + 4 * i);
	return t;
}
IMP_C(DLL, _time64) { time_t t = time(nullptr); if (ARG(0)) guest::store<int64_t>(ARG(0), t); ret64(c, (uint64_t)t); }
IMP_C(DLL, _localtime64) { time_t t = (time_t)guest::load<int64_t>(ARG(0)); struct tm tm; localtime_r(&t, &tm); putTm(tmBuf(), tm); SET(tmBuf()); }
IMP_C(DLL, _gmtime64) { time_t t = (time_t)guest::load<int64_t>(ARG(0)); struct tm tm; gmtime_r(&t, &tm); putTm(tmBuf(), tm); SET(tmBuf()); }
IMP_C(DLL, _mktime64) { struct tm tm = getTm(ARG(0)); time_t t = mktime(&tm); putTm(ARG(0), tm); ret64(c, (uint64_t)t); }
IMP_C(DLL, _mkgmtime64) { struct tm tm = getTm(ARG(0)); time_t t = timegm(&tm); ret64(c, (uint64_t)t); }
IMP_C(DLL, _difftime64) { RET_F((double)(int64_t)(ARG(0) | (uint64_t)ARG(1) << 32) - (double)(int64_t)(ARG(2) | (uint64_t)ARG(3) << 32)); }
IMP_C(DLL, clock) { SET((uint32_t)(clock() / (CLOCKS_PER_SEC / 1000))); }
IMP_C(DLL, strftime) { struct tm tm = getTm(ARG(3)); char buf[512]; size_t n = strftime(buf, std::min<size_t>(sizeof buf, ARG(1)), gstr(ARG(2)).c_str(), &tm); memcpy(guest::host(ARG(0)), buf, n); guest::store<uint8_t>(ARG(0) + (uint32_t)n, 0); SET(n); }
IMP_C(DLL, asctime) { struct tm tm = getTm(ARG(0)); char buf[64]; asctime_r(&tm, buf); static uint32_t a = gmalloc(64); putStr(a, buf); SET(a); }

// ---- math (doubles come on the stack, results in st0)
IMP_C(DLL, floor) { RET_F(floor(guest::load<double>(c->r[ESP] + 4))); }
IMP_C(DLL, ceil) { RET_F(ceil(guest::load<double>(c->r[ESP] + 4))); }
IMP_C(DLL, ldexp) { RET_F(ldexp(guest::load<double>(c->r[ESP] + 4), (int)ARG(2))); }
IMP_C(DLL, frexp) { int e; double m = frexp(guest::load<double>(c->r[ESP] + 4), &e); guest::store<int32_t>(ARG(2), e); RET_F(m); }
IMP_C(DLL, _isnan) { SET(std::isnan(guest::load<double>(c->r[ESP] + 4)) ? 1 : 0); }
IMP_C(DLL, _finite) { SET(std::isfinite(guest::load<double>(c->r[ESP] + 4)) ? 1 : 0); }
IMP_C(DLL, _CIpow) { double y = fpop(c), x = fpop(c); RET_F(pow(x, y)); }
IMP_C(DLL, _CIfmod) { double y = fpop(c), x = fpop(c); RET_F(fmod(x, y)); }
IMP_C(DLL, _CIasin) { double x = fpop(c); RET_F(asin(x)); }
IMP_C(DLL, _CIacos) { double x = fpop(c); RET_F(acos(x)); }
IMP_C(DLL, rand) { randSeed = randSeed * 214013u + 2531011u; SET((randSeed >> 16) & 0x7fff); }
IMP_C(DLL, srand) { randSeed = ARG(0); }

// qsort calls back into guest code
static void msort(CPU* c, uint32_t base, uint32_t tmp, uint32_t n, uint32_t sz, uint32_t cmp)
{
	if (n < 2) return;
	uint32_t h = n / 2;
	msort(c, base, tmp, h, sz, cmp);
	msort(c, base + h * sz, tmp, n - h, sz, cmp);
	uint32_t i = 0, j = h, k = 0;
	while (i < h && j < n)
	{
		uint32_t r = callGuestRet(c, cmp, {base + i * sz, base + j * sz});
		if ((int32_t)r <= 0) { memcpy(guest::host(tmp + k++ * sz), guest::host(base + i++ * sz), sz); }
		else { memcpy(guest::host(tmp + k++ * sz), guest::host(base + j++ * sz), sz); }
	}
	while (i < h) memcpy(guest::host(tmp + k++ * sz), guest::host(base + i++ * sz), sz);
	while (j < n) memcpy(guest::host(tmp + k++ * sz), guest::host(base + j++ * sz), sz);
	memcpy(guest::host(base), guest::host(tmp), (size_t)n * sz);
}
IMP_C(DLL, qsort)
{
	uint32_t n = ARG(1), sz = ARG(2);
	uint32_t tmp = gmalloc(n * sz + 16);
	msort(c, ARG(0), tmp, n, sz, ARG(3));
	gfree(tmp);
}

// ---- process / startup
IMP_C(DLL, _controlfp) { crtFcwApply(c, ARG(0), ARG(1)); SET(crtFcwFromGuest(c->fcw)); }
IMP_C(DLL, _control87) { crtFcwApply(c, ARG(0), ARG(1)); SET(crtFcwFromGuest(c->fcw)); }
IMP_C(DLL, _controlfp_s) { crtFcwApply(c, ARG(1), ARG(2)); if (ARG(0)) guest::store<uint32_t>(ARG(0), crtFcwFromGuest(c->fcw)); SET(0); }
IMP_C(DLL, _statusfp) { SET(0); }
IMP_C(DLL, _fpreset) { c->fcw = 0x37f; c->fcc = 0; }
IMP_C(DLL, _set_SSE2_enable) { SET(0); }
IMP_C(DLL, __set_app_type) {}
IMP_C(DLL, __p__fmode) { static uint32_t a = gmalloc(4); SET(a); }
IMP_C(DLL, __p__commode) { static uint32_t a = gmalloc(4); SET(a); }
IMP_C(DLL, __setusermatherr) {}
IMP_C(DLL, _configthreadlocale) { SET(0); }
IMP_C(DLL, _encode_pointer) { SET(ARG(0)); }
IMP_C(DLL, _decode_pointer) { SET(ARG(0)); }
IMP_C(DLL, _crt_debugger_hook) {}
IMP_C(DLL, _adjust_fdiv_fn) {}
IMP_C(DLL, _invoke_watson) { fatal("_invoke_watson called (CRT assertion)"); }
IMP_C(DLL, _XcptFilter) { SET(0); }
IMP_C(DLL, _errno) { SET(errnoAddr()); }
IMP_C(DLL, getenv) { SET(0); }
IMP_C(DLL, system) { SET((uint32_t)-1); }
IMP_C(DLL, setlocale) { static uint32_t a = newStr("C"); SET(a); }
IMP_C(DLL, _lock) {}
IMP_C(DLL, _unlock) {}
IMP_C(DLL, _onexit) { SET(ARG(0)); }
IMP_C(DLL, __dllonexit) { SET(ARG(0)); }
IMP_C(DLL, _cexit) {}
IMP_C(DLL, exit) { log("exit(%d)", (int)ARG(0)); exit((int)ARG(0)); }
IMP_C(DLL, _exit) { log("_exit(%d)", (int)ARG(0)); exit((int)ARG(0)); }
IMP_C(DLL, _amsg_exit) { fatal("_amsg_exit(%u)", ARG(0)); }
IMP_C(DLL, _initterm)
{
	for (uint32_t p = ARG(0); p < ARG(1); p += 4)
		if (uint32_t fn = guest::load<uint32_t>(p)) callGuest(c, fn, {});
}
IMP_C(DLL, _initterm_e)
{
	for (uint32_t p = ARG(0); p < ARG(1); p += 4)
		if (uint32_t fn = guest::load<uint32_t>(p))
			if (uint32_t r = callGuestRet(c, fn, {})) { SET(r); return; }
	SET(0);
}
IMP_C(DLL, __getmainargs)
{
	// argv: [0] program, then the rest of cmdline() split on spaces (quotes kept simple)
	std::string line = cmdline();
	std::vector<std::string> args;
	for (size_t i = 0; i < line.size();)
	{
		while (i < line.size() && line[i] == ' ') i++;
		if (i >= line.size()) break;
		std::string a;
		if (line[i] == '"') { i++; while (i < line.size() && line[i] != '"') a += line[i++]; i++; }
		else while (i < line.size() && line[i] != ' ') a += line[i++];
		args.push_back(a);
	}
	uint32_t argv = gmalloc(4 * (uint32_t)args.size() + 8);
	for (size_t i = 0; i < args.size(); i++) guest::store<uint32_t>(argv + 4 * (uint32_t)i, newStr(args[i]));
	guest::store<uint32_t>(argv + 4 * (uint32_t)args.size(), 0);
	uint32_t env = gmalloc(8);
	guest::store<uint32_t>(env, 0);
	guest::store<int32_t>(ARG(0), (int32_t)args.size());
	guest::store<uint32_t>(ARG(1), argv);
	guest::store<uint32_t>(ARG(2), env);
	SET(0);
}
// exception-related: only reached when the game actually throws or faults
IMP_C(DLL, _CxxThrowException) { fatal("C++ exception thrown by the game (object %08x, type info %08x) - not supported yet", ARG(0), ARG(1)); }
IMP_C(DLL, __CxxFrameHandler3) { fatal("__CxxFrameHandler3 invoked - C++ exception unwinding is not supported yet"); }
IMP_C(DLL, _except_handler3) { fatal("_except_handler3 invoked (SEH)"); }
IMP_C(DLL, _except_handler4_common) { fatal("_except_handler4_common invoked (SEH)"); }
IMP_C(DLL, longjmp) { fatal("longjmp is not supported yet"); }
IMP_C(DLL, _setjmp3) { SET(0); }   // first return only; longjmp is fatal (used for error paths)
IMP_ID(DLL, "?terminate@@YAXXZ", terminate_fn, 0) { fatal("std::terminate called"); }
IMP_ID(DLL, "?_type_info_dtor_internal_method@type_info@@QAEXXZ", type_info_dtor, 0) {}
