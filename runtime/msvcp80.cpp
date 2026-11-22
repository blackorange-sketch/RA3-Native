// Imports only the Steam build (ra3_1.13.game) uses: MSVCP80 std::string, MSVCR80 std::exception and a few file
// functions, SHLWAPI PathFileExistsA, WININET (no internet: every request fails).
// std::string of VC8 (release): 28 bytes - allocator (4, empty), _Bx (16: the characters while capacity < 16, else
// the heap pointer), _Mysize, _Myres (capacity).
#include <algorithm>
#include <cstring>
#include <string>
#include <unistd.h>

#include "rt.h"

using namespace rt;

namespace
{
constexpr uint32_t BX = 4, SIZE = 20, RES = 24;

uint32_t sData(uint32_t s) { return guest::load<uint32_t>(s + RES) >= 16 ? guest::load<uint32_t>(s + BX) : s + BX; }
uint32_t sSize(uint32_t s) { return guest::load<uint32_t>(s + SIZE); }
std::string sGet(uint32_t s) { return gstrn(sData(s), sSize(s)); }
void sInit(uint32_t s)
{
	guest::store<uint32_t>(s, 0);
	guest::store<uint32_t>(s + SIZE, 0);
	guest::store<uint32_t>(s + RES, 15);
	guest::store<uint8_t>(s + BX, 0);
}
void sFree(uint32_t s)
{
	if (guest::load<uint32_t>(s + RES) >= 16) gfree(guest::load<uint32_t>(s + BX));
	sInit(s);
}
// capacity for at least n characters (keeps the contents)
void sReserve(uint32_t s, uint32_t n)
{
	uint32_t res = guest::load<uint32_t>(s + RES);
	if (n <= res) return;
	uint32_t cap = std::max(n | 15u, res + res / 2);
	uint32_t buf = gmalloc(cap + 1), size = sSize(s);
	memcpy(guest::host(buf), guest::host(sData(s)), size + 1);
	if (res >= 16) gfree(guest::load<uint32_t>(s + BX));
	guest::store<uint32_t>(s + BX, buf);
	guest::store<uint32_t>(s + RES, cap);
}
void sAssign(uint32_t s, const std::string& v)   // v is a host copy: the source may be inside s
{
	sReserve(s, (uint32_t)v.size());
	uint32_t d = sData(s);
	memcpy(guest::host(d), v.data(), v.size());
	guest::store<uint8_t>(d + (uint32_t)v.size(), 0);
	guest::store<uint32_t>(s + SIZE, (uint32_t)v.size());
}
void sNew(uint32_t s, const std::string& v) { sInit(s); sAssign(s, v); }

// Windows-style full path; the game's current directory is C:\RA3 (GetCurrentDirectoryA)
std::string fullPath(std::string p)
{
	for (auto& ch : p) if (ch == '/') ch = '\\';
	if (p.size() >= 2 && p[1] == ':') return p;
	if (!p.empty() && p[0] == '\\') return "C:" + p;
	while (p.compare(0, 2, ".\\") == 0) p = p.substr(2);
	return "C:\\RA3\\" + p;
}
}

#define P "MSVCP80.dll"
#define STR "?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@"

IMP_ID(P, "??0" STR "QAE@XZ", str_ctor, 0) { sInit(c->r[ECX]); SET(c->r[ECX]); }
IMP_ID(P, "??0" STR "QAE@PBD@Z", str_ctor_cstr, 4) { sNew(c->r[ECX], ARG(0) ? gstr(ARG(0)) : std::string()); SET(c->r[ECX]); }
IMP_ID(P, "??0" STR "QAE@ABV01@@Z", str_ctor_copy, 4) { sNew(c->r[ECX], sGet(ARG(0))); SET(c->r[ECX]); }
IMP_ID(P, "??1" STR "QAE@XZ", str_dtor, 0) { sFree(c->r[ECX]); }
IMP_ID(P, "??4" STR "QAEAAV01@ABV01@@Z", str_assign, 4) { if (ARG(0) != c->r[ECX]) sAssign(c->r[ECX], sGet(ARG(0))); SET(c->r[ECX]); }
IMP_ID(P, "??4" STR "QAEAAV01@PBD@Z", str_assign_cstr, 4) { sAssign(c->r[ECX], ARG(0) ? gstr(ARG(0)) : std::string()); SET(c->r[ECX]); }
IMP_ID(P, "??Y" STR "QAEAAV01@PBD@Z", str_append_cstr, 4) { sAssign(c->r[ECX], sGet(c->r[ECX]) + (ARG(0) ? gstr(ARG(0)) : std::string())); SET(c->r[ECX]); }
IMP_ID(P, "?reserve@" STR "QAEXI@Z", str_reserve, 4) { sReserve(c->r[ECX], ARG(0)); }
// substr(off, count) const: the result is returned through the hidden pointer (first argument)
IMP_ID(P, "?substr@" STR "QBE?AV12@II@Z", str_substr, 12)
{
	std::string v = sGet(c->r[ECX]);
	uint32_t off = std::min<uint32_t>(ARG(1), (uint32_t)v.size());
	sNew(ARG(0), v.substr(off, ARG(2)));
	SET(ARG(0));
}
// operator+(const string&, const string&) and operator+(const string&, const char*): cdecl, hidden result pointer
IMP_ID(P, "??$?HDU?$char_traits@D@std@@V?$allocator@D@1@@std@@YA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@ABV10@0@Z", str_plus, 0)
{
	sNew(ARG(0), sGet(ARG(1)) + sGet(ARG(2)));
	SET(ARG(0));
}
IMP_ID(P, "??$?HDU?$char_traits@D@std@@V?$allocator@D@1@@std@@YA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@ABV10@PBD@Z", str_plus_cstr, 0)
{
	sNew(ARG(0), sGet(ARG(1)) + (ARG(2) ? gstr(ARG(2)) : std::string()));
	SET(ARG(0));
}

// ---- MSVCR80: std::exception {vftable, const char* what, int free}
#define R "MSVCR80.dll"
namespace
{
uint32_t exVtable();
void exInit(uint32_t e, uint32_t what, bool own)
{
	guest::store<uint32_t>(e, exVtable());
	guest::store<uint32_t>(e + 4, what && own ? newStr(gstr(what)) : what);
	guest::store<uint32_t>(e + 8, what && own ? 1 : 0);
}
void exDestroy(uint32_t e)
{
	if (guest::load<uint32_t>(e + 8)) gfree(guest::load<uint32_t>(e + 4));
	guest::store<uint32_t>(e + 4, 0);
	guest::store<uint32_t>(e + 8, 0);
}
void exWhat(CPU* c)
{
	static uint32_t unknown = newStr("Unknown exception");
	uint32_t w = guest::load<uint32_t>(c->r[ECX] + 4);
	SET(w ? w : unknown);
}
void exDeletingDtor(CPU* c)   // virtual: (flags) - bit 0 frees the object
{
	exDestroy(c->r[ECX]);
	if (ARG(0) & 1) gfree(c->r[ECX]);
	SET(c->r[ECX]);
}
uint32_t exVtable()
{
	static uint32_t vt = [] {
		uint32_t v = gmalloc(8);
		guest::store<uint32_t>(v, imports::addDynamic(R, "std::exception::`deleting destructor'", exDeletingDtor, 4, 0));
		guest::store<uint32_t>(v + 4, imports::addDynamic(R, "std::exception::what", exWhat, 0, 0));
		return v;
	}();
	return vt;
}
}
IMP_ID(R, "??0exception@std@@QAE@XZ", ex_ctor, 0) { exInit(c->r[ECX], 0, false); SET(c->r[ECX]); }
IMP_ID(R, "??0exception@std@@QAE@ABQBD@Z", ex_ctor_str, 4) { exInit(c->r[ECX], guest::load<uint32_t>(ARG(0)), true); SET(c->r[ECX]); }
IMP_ID(R, "??0exception@std@@QAE@ABV01@@Z", ex_ctor_copy, 4)
{
	uint32_t src = ARG(0);
	exInit(c->r[ECX], guest::load<uint32_t>(src + 4), guest::load<uint32_t>(src + 8) != 0);
	SET(c->r[ECX]);
}
IMP_ID(R, "??1exception@std@@UAE@XZ", ex_dtor, 0) { exDestroy(c->r[ECX]); }
IMP_ID(R, "?what@exception@std@@UBEPBDXZ", ex_what, 0) { exWhat(c); }

IMP_ID(R, "_fullpath", fullpath, 0)
{
	std::string f = fullPath(ARG(1) ? gstr(ARG(1)) : std::string());
	uint32_t buf = ARG(0);
	if (!buf) { SET(newStr(f)); return; }
	if (f.size() + 1 > ARG(2)) { SET(0); return; }
	putStr(buf, f);
	SET(buf);
}
IMP_ID(R, "_getcwd", getcwd_, 0)
{
	std::string d = "C:\\RA3";
	uint32_t buf = ARG(0);
	if (!buf) { SET(newStr(d)); return; }
	if ((int)ARG(1) < (int)d.size() + 1) { SET(0); return; }
	putStr(buf, d);
	SET(buf);
}
IMP_ID(R, "memmove_s", memmove_s_, 0)
{
	if (ARG(3) > ARG(1)) { SET(34); return; }   // ERANGE
	if (ARG(3)) memmove(guest::host(ARG(0)), guest::host(ARG(2)), ARG(3));
	SET(0);
}

// ---- KERNEL32 / SHLWAPI
IMP_ID("KERNEL32.dll", "GetFullPathNameA", GetFullPathNameA, 16)
{
	std::string f = fullPath(gstr(ARG(0)));
	if (ARG(1) < f.size() + 1) { SET(f.size() + 1); return; }
	putStr(ARG(2), f);
	if (ARG(3))
	{
		size_t k = f.find_last_of('\\');
		guest::store<uint32_t>(ARG(3), ARG(2) + (uint32_t)(k == std::string::npos ? 0 : k + 1));
	}
	SET(f.size());
}
IMP_ID("SHLWAPI.dll", "PathFileExistsA", PathFileExistsA, 4)
{
	std::string hp = hostPath(gstr(ARG(0)));
	SET(access(hp.c_str(), F_OK) == 0);
}

// ---- WININET: no internet access for the game (online services are gone); every call fails
#define W "WININET.dll"
IMP_ID(W, "InternetOpenA", InternetOpenA, 20) { SET(0); }
IMP_ID(W, "InternetConnectA", InternetConnectA, 32) { SET(0); }
IMP_ID(W, "HttpOpenRequestA", HttpOpenRequestA, 32) { SET(0); }
IMP_ID(W, "HttpAddRequestHeadersA", HttpAddRequestHeadersA, 16) { SET(0); }
IMP_ID(W, "HttpSendRequestA", HttpSendRequestA, 20) { SET(0); }
IMP_ID(W, "InternetQueryDataAvailable", InternetQueryDataAvailable, 16) { SET(0); }
IMP_ID(W, "InternetReadFile", InternetReadFile, 16) { if (ARG(3)) guest::store<uint32_t>(ARG(3), 0); SET(0); }
IMP_ID(W, "InternetCloseHandle", InternetCloseHandle, 4) { SET(1); }
