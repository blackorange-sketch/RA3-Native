// Stand-in steam_api.dll for the Steam build (ra3_1.13.game), which delay-loads it. There is no Steam on the
// phone, but the game insists on it: the stand-in reports Steam as running and initialised, and every
// interface it asks for is a fake object. Its methods pop exactly the argument bytes the real ones do (tables
// generated from the Steamworks SDK by tools/steam_gen.py, steam_ifaces.inc) and answer "nothing": 0, false, empty
// strings - except a few ownership checks, which answer yes (the game is a bought copy) - and the app id.
// The first calls of each method are logged ("steam:") to see what the game wants.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "fpu.h"
#include "rt.h"
#include "steam.h"

using namespace rt;

namespace
{
struct SteamMethod { const char* name; int pop; char ret; };   // ret: V I B L F S (char*) H (hidden class pointer)
struct SteamIface { const char* version; const char* cls; const SteamMethod* m; int n; };
#if __has_include("steam_ifaces.inc")
#include "steam_ifaces.inc"
#else
static const SteamIface kSteamIfaces[] = {{"", "", nullptr, 0}};
#endif
constexpr int kIfaces = sizeof kSteamIfaces / sizeof kSteamIfaces[0];
constexpr uint32_t kAppId = 17480;   // Command & Conquer Red Alert 3 on Steam
constexpr uint64_t kSteamId = 0x0110000100000001ull;   // individual account in the public universe
// the Steam folder is C:\Steam (misc.cpp registry); the library's copy of the game is the game folder
const char* kInstallDir = "C:\\Steam\\steamapps\\common\\Command and Conquer Red Alert 3";

std::mutex mu;
std::map<std::string, uint32_t> objects;   // interface version -> fake object
std::vector<std::pair<const SteamIface*, int>> methodOf;   // user value of a stub -> interface, method

uint32_t emptyStr() { static uint32_t s = newStr(""); return s; }

void method(CPU* c)
{
	uint32_t u = imports::current ? imports::current->user : 0;
	const SteamIface* f = nullptr; int k = -1;
	{
		std::lock_guard<std::mutex> l(mu);
		if (u < methodOf.size()) { f = methodOf[u].first; k = methodOf[u].second; }
	}
	const SteamMethod* m = f && f->m && k >= 0 && k < f->n ? &f->m[k] : nullptr;
	std::string name = m ? m->name : "?";
	static std::map<std::string, int> seen;
	static std::atomic<int> total{0};
	{
		std::lock_guard<std::mutex> l(mu);
		if (seen[name]++ < 3 && total++ < 300) log("steam: %s::%s (method %d)", f ? f->cls : "?", name.c_str(), k);
	}
	char r = m ? m->ret : 'I';
	switch (r)
	{
	case 'H':
	{
		// CSteamID through the hidden pointer: a valid individual account id for the user (the game wants one)
		uint64_t v = name == "GetSteamID" ? kSteamId : 0;
		guest::store<uint32_t>(ARG(0), (uint32_t)v); guest::store<uint32_t>(ARG(0) + 4, (uint32_t)(v >> 32));
		SET(ARG(0));
		return;
	}
	case 'L': SET(0); c->r[EDX] = 0; return;
	case 'F': RET_F(0.0); return;
	case 'S':
		if (name == "GetCurrentGameLanguage" || name == "GetSteamUILanguage") { static uint32_t s = newStr(config.language); SET(s); return; }
		if (name == "GetPersonaName") { static uint32_t s = newStr("Commander"); SET(s); return; }
		SET(emptyStr()); return;
	case 'B':
		SET(name.rfind("BIsSubscribed", 0) == 0 || name == "BIsAppInstalled" || name == "BIsDlcInstalled" || name == "BLoggedOn" ? 1 : 0);
		return;
	default:
		if (name == "GetAppInstallDir" && ARG(1) && ARG(2))
		{
			// (appid, char* folder, size): the game folder as seen through the Steam library (rt.cpp hostPath maps it)
			uint32_t n = std::min<uint32_t>((uint32_t)strlen(kInstallDir), ARG(2) - 1);
			memcpy(guest::host(ARG(1)), kInstallDir, n);
			guest::store<uint8_t>(ARG(1) + n, 0);
			SET(n);
			return;
		}
		SET(name == "GetAppID" ? kAppId : 0);
		return;
	}
}

// fake object for an interface version: {vtable}; methods by the generated table (unknown: 256 methods popping 0)
uint32_t object(const std::string& version)
{
	std::lock_guard<std::mutex> l(mu);
	auto it = objects.find(version);
	if (it != objects.end()) return it->second;
	const SteamIface* f = nullptr;
	for (int i = 0; i < kIfaces; i++) if (kSteamIfaces[i].m && version == kSteamIfaces[i].version) f = &kSteamIfaces[i];
	if (!f)
	{
		// same interface, other version (e.g. SteamUtils009 asked, SteamUtils010 known): methods are mostly appended
		std::string stem = version;
		while (!stem.empty() && isdigit((unsigned char)stem.back())) stem.pop_back();
		for (int i = 0; i < kIfaces; i++)
			if (kSteamIfaces[i].m && std::string(kSteamIfaces[i].version).compare(0, stem.size(), stem) == 0) f = &kSteamIfaces[i];
		log("steam: interface %s %s", version.c_str(), f ? (std::string("not in the table, using ") + f->version).c_str() : "unknown (methods pop nothing)");
	}
	int n = f ? f->n : 256;
	uint32_t vt = gmalloc(4 * n), obj = gmalloc(16);
	for (int k = 0; k < n; k++)
	{
		uint32_t user = (uint32_t)methodOf.size();
		methodOf.push_back({f, k});
		std::string nm = std::string(f ? f->cls : version.c_str()) + "::" + (f ? f->m[k].name : std::to_string(k));
		guest::store<uint32_t>(vt + 4 * k, imports::addDynamic("steam_api.dll", nm, method, f ? f->m[k].pop : 0, user));
	}
	guest::store<uint32_t>(obj, vt);
	objects[version] = obj;
	return obj;
}

// ---- flat exports (cdecl)
void exNothing(CPU* c)
{
	static std::atomic<int> nl{0};
	if (imports::current && imports::current->name == "SteamAPI_RunCallbacks") { SET(0); return; }
	if (nl++ < 40) log("steam_api: %s() -> 0", imports::current ? imports::current->name.c_str() : "?");
	SET(0);
}
void exGetHSteamUser(CPU* c) { SET(1); }
// the game refuses to start unless Steam is running ("Steam must be running to play this game")
void exOne(CPU* c) { SET(1); }
void exGetHSteamPipe(CPU* c) { SET(1); }
// SteamInternal_FindOrCreateUserInterface(HSteamUser, const char* version) / ..EngineInterface(const char* version)
void exFindUser(CPU* c) { std::string v = ARG(1) ? gstr(ARG(1)) : ""; uint32_t o = object(v); log("steam: interface %s -> %08x", v.c_str(), o); SET(o); }
void exFindEngine(CPU* c) { std::string v = ARG(0) ? gstr(ARG(0)) : ""; uint32_t o = object(v); log("steam: interface %s -> %08x", v.c_str(), o); SET(o); }
// SteamInternal_ContextInit(CSteamAPIContextInitData* d): d = {void (*fill)(void* ctx), counter, context...}: the
// accessors read their interface from the context, filled once through the game's own fill function
void exContextInit(CPU* c)
{
	uint32_t d = ARG(0);
	if (!d) { SET(0); return; }
	if (guest::load<uint32_t>(d + 4) != 1)
	{
		guest::store<uint32_t>(d + 4, 1);
		if (uint32_t fill = guest::load<uint32_t>(d))
		{
			uint32_t sv[8]; memcpy(sv, c->r, sizeof sv);
			rt::callGuest(c, fill, {d + 8});
			memcpy(c->r, sv, sizeof sv);
		}
	}
	SET(d + 8);
}
// SteamInternal_CreateInterface(const char* version)
void exCreateInterface(CPU* c) { std::string v = ARG(0) ? gstr(ARG(0)) : ""; SET(object(v)); }
}

namespace steam
{
constexpr uint32_t kModule = 0x6f000000;
uint32_t loadLibrary(const std::string& name)
{
	std::string l = name;
	for (auto& ch : l) ch = (char)tolower((unsigned char)ch);
	size_t s = l.find_last_of("\\/");
	if (s != std::string::npos) l = l.substr(s + 1);
	return l == "steam_api.dll" ? kModule : 0;
}
uint32_t procAddress(uint32_t module, const std::string& n)
{
	if (module != kModule) return 0;
	// the game's callback thread looks SteamAPI_RunCallbacks up again every 100 ms: one stub per export
	static std::mutex pmu;
	static std::map<std::string, uint32_t> known;
	std::lock_guard<std::mutex> pl(pmu);
	auto it = known.find(n);
	if (it != known.end()) return it->second;
	imports::HostFn fn = exNothing;
	if (n == "SteamInternal_ContextInit") fn = exContextInit;
	else if (n == "SteamInternal_FindOrCreateUserInterface") fn = exFindUser;
	else if (n == "SteamInternal_FindOrCreateGameServerInterface") fn = exFindUser;
	else if (n == "SteamInternal_CreateInterface") fn = exCreateInterface;
	else if (n == "SteamAPI_GetHSteamUser") fn = exGetHSteamUser;
	else if (n == "SteamAPI_IsSteamRunning") fn = exOne;
	// SteamInternal_SteamAPI_Init returns ESteamAPIInitResult (0 = OK), SteamAPI_Init a bool (1 = OK)
	else if (n == "SteamAPI_Init" || n == "SteamAPI_InitSafe") fn = exOne;
	else if (n == "SteamAPI_GetHSteamPipe") fn = exGetHSteamPipe;
	else if (n.find("EngineInterface") != std::string::npos) fn = exFindEngine;
	log("steam_api: %s (%s)", n.c_str(), fn == exNothing ? "answers 0" : "handled");
	return known[n] = imports::addDynamic("steam_api.dll", n, fn, 0, 0);
}
}
