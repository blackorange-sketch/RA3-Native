// DSOUND.dll (ordinal imports): a software DirectSound. The game's buffers live in guest memory, a mixer thread feeds an AAudio stream.
// Implemented: IDirectSound / IDirectSoundBuffer(8) with Lock/Unlock/Play/Stop/position/volume/pan/frequency; notifications are not (yet).
#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

#include "rt.h"

#define D "DSOUND.dll"
using namespace rt;

namespace
{
const uint32_t DS_OK = 0, DSERR_NOINTERFACE = 0x80004002, DSERR_INVALIDPARAM = 0x80070057, DSERR_UNSUPPORTED = 0x80004001, DSERR_OUTOFMEMORY = 0x8007000e;
enum Kind { KDirectSound, KBuffer };
struct Method { const char* name; int nargs; };
const std::vector<Method> kUnk = {{"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}};
std::vector<Method> cat(std::vector<Method> a, const std::vector<Method>& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
const std::vector<Method> kDS = cat(kUnk, {{"CreateSoundBuffer", 3}, {"GetCaps", 1}, {"DuplicateSoundBuffer", 2}, {"SetCooperativeLevel", 2}, {"Compact", 0}, {"GetSpeakerConfig", 1}, {"SetSpeakerConfig", 1}, {"Initialize", 1}});
const std::vector<Method> kBuf = cat(kUnk, {{"GetCaps", 1}, {"GetCurrentPosition", 2}, {"GetFormat", 3}, {"GetVolume", 1}, {"GetPan", 1}, {"GetFrequency", 1}, {"GetStatus", 1}, {"Initialize", 2},
	{"Lock", 6}, {"Play", 3}, {"SetCurrentPosition", 1}, {"SetFormat", 1}, {"SetVolume", 1}, {"SetPan", 1}, {"SetFrequency", 1}, {"Stop", 0}, {"Unlock", 4}, {"Restore", 0},
	{"SetFX", 3}, {"AcquireResources", 3}, {"GetObjectInPath", 4}});

struct Obj
{
	Kind kind;
	uint32_t addr = 0, ref = 1;
	// buffer
	uint32_t flags = 0, size = 0, data = 0;
	uint8_t fmt[18] = {};   // WAVEFORMATEX
	int channels = 2, bits = 16, rate = 44100;
	bool primary = false;
	// playback (guarded by mixMu)
	bool playing = false, looping = false;
	double pos = 0;   // in frames
	int volume = 0, pan = 0;   // millibels
	uint32_t freq = 0;   // 0 = native
};

std::mutex mu, mixMu;
std::map<uint32_t, Obj*> objs;
std::vector<Obj*> playingList;
uint32_t vtables[2] = {};
std::vector<std::pair<Kind, std::string>> methodNames;
int outRate = 48000;
#ifdef __ANDROID__
AAudioStream* stream = nullptr;
#endif

Obj* find(uint32_t a) { std::lock_guard<std::mutex> l(mu); auto it = objs.find(a); return it == objs.end() ? nullptr : it->second; }

float gainOf(int mb) { return mb <= -10000 ? 0.f : powf(10.f, mb / 2000.f); }

#ifdef __ANDROID__
aaudio_data_callback_result_t audioCb(AAudioStream*, void*, void* audio, int32_t frames)
{
	int16_t* out = (int16_t*)audio;
	static std::vector<float> acc;
	acc.assign((size_t)frames * 2, 0.f);
	{
		std::lock_guard<std::mutex> l(mixMu);
		for (size_t bi = 0; bi < playingList.size();)
		{
			Obj* b = playingList[bi];
			if (!b->playing || !b->data || b->size == 0) { playingList.erase(playingList.begin() + bi); continue; }
			const uint8_t* src = (const uint8_t*)guest::host(b->data);
			int bytesPer = b->bits / 8 * b->channels;
			uint32_t totalFrames = b->size / bytesPer;
			double step = (double)(b->freq ? b->freq : b->rate) / outRate;
			float g = gainOf(b->volume), pl = 1.f, pr = 1.f;
			if (b->pan > 0) pl = gainOf(-b->pan); else if (b->pan < 0) pr = gainOf(b->pan);
			for (int i = 0; i < frames; i++)
			{
				if (b->pos >= totalFrames)
				{
					if (b->looping) b->pos -= totalFrames; else { b->playing = false; break; }
				}
				uint32_t f = (uint32_t)b->pos;
				float l, r;
				if (b->bits == 16)
				{
					const int16_t* s = (const int16_t*)(src + (size_t)f * bytesPer);
					l = s[0] / 32768.f; r = b->channels == 2 ? s[1] / 32768.f : l;
				}
				else
				{
					const uint8_t* s = src + (size_t)f * bytesPer;
					l = (s[0] - 128) / 128.f; r = b->channels == 2 ? (s[1] - 128) / 128.f : l;
				}
				acc[2 * i] += l * g * pl;
				acc[2 * i + 1] += r * g * pr;
				b->pos += step;
			}
			bi++;
		}
	}
	for (int i = 0; i < frames * 2; i++) out[i] = (int16_t)std::max(-32768.f, std::min(32767.f, acc[i] * 32767.f));
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void startAudio()
{
	static bool tried = false;
	if (tried) return;
	tried = true;
	AAudioStreamBuilder* b = nullptr;
	if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) { log("dsound: AAudio builder failed"); return; }
	AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
	AAudioStreamBuilder_setChannelCount(b, 2);
	AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
	AAudioStreamBuilder_setDataCallback(b, audioCb, nullptr);
	aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream);
	AAudioStreamBuilder_delete(b);
	if (r != AAUDIO_OK) { log("dsound: AAudio open failed: %s", AAudio_convertResultToText(r)); stream = nullptr; return; }
	outRate = AAudioStream_getSampleRate(stream);
	r = AAudioStream_requestStart(stream);
	log("dsound: AAudio stream %d Hz, start %s", outRate, r == AAUDIO_OK ? "ok" : AAudio_convertResultToText(r));
}

#else
void startAudio() {}
#endif

void dispatch(CPU* c);

uint32_t vtableFor(Kind k)
{
	if (vtables[k]) return vtables[k];
	const std::vector<Method>& ms = k == KDirectSound ? kDS : kBuf;
	uint32_t vt = gmalloc(4 * (uint32_t)ms.size());
	for (size_t i = 0; i < ms.size(); i++)
	{
		methodNames.push_back({k, ms[i].name});
		uint32_t stub = imports::addDynamic("dsound.dll", std::string(k == KDirectSound ? "IDirectSound::" : "IDirectSoundBuffer::") + ms[i].name, dispatch, 4 * (ms[i].nargs + 1), (uint32_t)methodNames.size() - 1);
		guest::store<uint32_t>(vt + 4 * (uint32_t)i, stub);
	}
	return vtables[k] = vt;
}

Obj* make(Kind k)
{
	auto* o = new Obj;
	o->kind = k;
	o->addr = gmalloc(8);
	guest::store<uint32_t>(o->addr, vtableFor(k));
	std::lock_guard<std::mutex> l(mu);
	objs[o->addr] = o;
	return o;
}

void parseFormat(Obj* b, uint32_t pwfx)
{
	if (!pwfx) return;
	memcpy(b->fmt, guest::host(pwfx), 16);
	b->channels = guest::load<uint16_t>(pwfx + 2);
	b->rate = guest::load<uint32_t>(pwfx + 4);
	b->bits = guest::load<uint16_t>(pwfx + 14);
	if (b->channels < 1) b->channels = 1;
	if (b->bits != 8) b->bits = 16;
}

void dispatch(CPU* c)
{
	auto& mn = methodNames[imports::current->user];
	const std::string& m = mn.second;
	Obj* o = find(ARG(0));
	static int logged = 0;
	if (imports::current->calls == 1) log("dsound: first call %s", imports::current->name.c_str());
	if (!o) { SET(DSERR_INVALIDPARAM); return; }
	if (m == "QueryInterface")
	{
		uint32_t iid = ARG(1);
		uint8_t g[16]; memcpy(g, guest::host(iid), 16);
		char b[40]; for (int i = 0; i < 16; i++) snprintf(b + 2 * i, 3, "%02x", g[i]);
		uint32_t d1 = guest::load<uint32_t>(iid);
		// IDirectSound 279afa83 / IDirectSound8 c50a7e93, IDirectSoundBuffer 279afa85 / IDirectSoundBuffer8 6825a449, IUnknown 00000000
		bool ok = d1 == 0 || (o->kind == KDirectSound && (d1 == 0x279afa83 || d1 == 0xc50a7e93)) || (o->kind == KBuffer && (d1 == 0x279afa85 || d1 == 0x6825a449));
		if (logged++ < 40) log("dsound: QueryInterface %s on %s -> %s", b, o->kind == KDirectSound ? "DS" : "buffer", ok ? "self" : "E_NOINTERFACE");
		if (ok) { o->ref++; guest::store<uint32_t>(ARG(2), o->addr); SET(DS_OK); }
		else { guest::store<uint32_t>(ARG(2), 0); SET(DSERR_NOINTERFACE); }
		return;
	}
	if (m == "AddRef") { SET(++o->ref); return; }
	if (m == "Release")
	{
		uint32_t r = --o->ref;
		if (r == 0 && o->kind == KBuffer)
		{
			{ std::lock_guard<std::mutex> l(mixMu); o->playing = false; playingList.erase(std::remove(playingList.begin(), playingList.end(), o), playingList.end()); }
			{ std::lock_guard<std::mutex> l(mu); objs.erase(o->addr); }
			if (o->data) gfree(o->data);
			gfree(o->addr);
			delete o;
		}
		SET(r);
		return;
	}
	if (o->kind == KDirectSound)
	{
		if (m == "CreateSoundBuffer")
		{
			uint32_t desc = ARG(1), out = ARG(2);
			if (!desc || !out) { SET(DSERR_INVALIDPARAM); return; }
			Obj* b = make(KBuffer);
			b->flags = guest::load<uint32_t>(desc + 4);
			b->size = guest::load<uint32_t>(desc + 8);
			b->primary = (b->flags & 1) != 0;
			parseFormat(b, guest::load<uint32_t>(desc + 16));
			if (!b->primary)
			{
				if (!b->size || b->size > (64u << 20)) { SET(DSERR_INVALIDPARAM); return; }
				b->data = gmalloc(b->size);
				memset(guest::host(b->data), b->bits == 8 ? 0x80 : 0, b->size);
			}
			static int n = 0;
			if (n++ < 60) log("dsound: CreateSoundBuffer flags %x size %u %d ch %d bit %d Hz%s", b->flags, b->size, b->channels, b->bits, b->rate, b->primary ? " (primary)" : "");
			guest::store<uint32_t>(out, b->addr);
			startAudio();
			SET(DS_OK);
			return;
		}
		if (m == "DuplicateSoundBuffer")
		{
			Obj* s = find(ARG(1));
			if (!s || !ARG(2)) { SET(DSERR_INVALIDPARAM); return; }
			Obj* b = make(KBuffer);
			b->flags = s->flags; b->size = s->size; b->primary = s->primary; memcpy(b->fmt, s->fmt, 18); b->channels = s->channels; b->bits = s->bits; b->rate = s->rate;
			if (s->data) { b->data = gmalloc(b->size); memcpy(guest::host(b->data), guest::host(s->data), b->size); }
			guest::store<uint32_t>(ARG(2), b->addr);
			SET(DS_OK);
			return;
		}
		if (m == "GetCaps")
		{
			uint32_t p = ARG(1);
			if (!p) { SET(DSERR_INVALIDPARAM); return; }
			uint32_t sz = guest::load<uint32_t>(p);
			memset(guest::host(p), 0, std::min(sz, 96u));
			guest::store<uint32_t>(p, sz);
			guest::store<uint32_t>(p + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x80);   // PRIMARYMONO/STEREO/8BIT/16BIT, CONTINUOUSRATE, ...
			guest::store<uint32_t>(p + 8, 8000); guest::store<uint32_t>(p + 12, 192000);
			SET(DS_OK);
			return;
		}
		if (m == "GetSpeakerConfig") { if (ARG(1)) guest::store<uint32_t>(ARG(1), 4 /*STEREO*/); SET(DS_OK); return; }
		SET(DS_OK);   // SetCooperativeLevel, Compact, SetSpeakerConfig, Initialize
		return;
	}
	// buffer
	std::lock_guard<std::mutex> l(mixMu);
	uint32_t bpf = std::max(1, o->bits / 8 * o->channels);
	if (m == "GetCurrentPosition")
	{
		uint32_t play = o->primary ? 0 : (uint32_t)(o->pos * bpf) % std::max(1u, o->size);
		uint32_t lead = std::min(o->size ? o->size : 0u, (uint32_t)(o->rate / 25) * bpf);   // 40 ms
		if (ARG(1)) guest::store<uint32_t>(ARG(1), play);
		if (ARG(2)) guest::store<uint32_t>(ARG(2), o->size ? (play + lead) % o->size : 0);
		SET(DS_OK);
	}
	else if (m == "GetStatus") { if (ARG(1)) guest::store<uint32_t>(ARG(1), (o->playing ? 1u : 0u) | (o->playing && o->looping ? 4u : 0u)); SET(DS_OK); }
	else if (m == "GetVolume") { if (ARG(1)) guest::store<int32_t>(ARG(1), o->volume); SET(DS_OK); }
	else if (m == "GetPan") { if (ARG(1)) guest::store<int32_t>(ARG(1), o->pan); SET(DS_OK); }
	else if (m == "GetFrequency") { if (ARG(1)) guest::store<uint32_t>(ARG(1), o->freq ? o->freq : o->rate); SET(DS_OK); }
	else if (m == "GetFormat")
	{
		uint32_t cap = ARG(2);
		if (ARG(1) && cap >= 18) { memcpy(guest::host(ARG(1)), o->fmt, 18); guest::store<uint16_t>(ARG(1) + 16, 0); }
		if (ARG(3)) guest::store<uint32_t>(ARG(3), 18);
		SET(DS_OK);
	}
	else if (m == "GetCaps")
	{
		uint32_t p = ARG(1);
		if (p) { memset(guest::host(p), 0, 20); guest::store<uint32_t>(p, 20); guest::store<uint32_t>(p + 4, o->flags); guest::store<uint32_t>(p + 8, o->size); }
		SET(DS_OK);
	}
	else if (m == "Lock")
	{
		uint32_t off = ARG(1), bytes = ARG(2), p1 = ARG(3), l1 = ARG(4), p2 = ARG(5), l2 = ARG(6), fl = ARG(7);
		if (o->primary || !o->data) { SET(DSERR_INVALIDPARAM); return; }
		if (fl & 2) { off = 0; bytes = o->size; }
		if (off >= o->size || bytes > o->size) { SET(DSERR_INVALIDPARAM); return; }
		uint32_t first = std::min(bytes, o->size - off);
		if (p1) guest::store<uint32_t>(p1, o->data + off);
		if (l1) guest::store<uint32_t>(l1, first);
		if (p2) guest::store<uint32_t>(p2, bytes > first ? o->data : 0);
		if (l2) guest::store<uint32_t>(l2, bytes - first);
		SET(DS_OK);
	}
	else if (m == "Unlock") SET(DS_OK);
	else if (m == "Play")
	{
		uint32_t fl = ARG(3);
		if (!o->primary)
		{
			o->looping = (fl & 1) != 0;
			if (!o->playing) { o->playing = true; playingList.push_back(o); }
		}
		SET(DS_OK);
	}
	else if (m == "Stop") { o->playing = false; SET(DS_OK); }
	else if (m == "SetCurrentPosition") { o->pos = (double)(ARG(1) / bpf); SET(DS_OK); }
	else if (m == "SetVolume") { o->volume = (int32_t)ARG(1); SET(DS_OK); }
	else if (m == "SetPan") { o->pan = (int32_t)ARG(1); SET(DS_OK); }
	else if (m == "SetFrequency") { o->freq = ARG(1) == 0 ? 0 : ARG(1); if (o->freq == (uint32_t)o->rate) o->freq = 0; SET(DS_OK); }
	else if (m == "SetFormat") { parseFormat(o, ARG(1)); SET(DS_OK); }
	else if (m == "Restore" || m == "Initialize") SET(DS_OK);
	else SET(DSERR_UNSUPPORTED);   // SetFX, AcquireResources, GetObjectInPath
}

const uint8_t kDevGuid[16] = {0x41, 0x6e, 0x64, 0x72, 0x6f, 0x69, 0x64, 0x41, 0x75, 0x64, 0x69, 0x6f, 0x30, 0x30, 0x30, 0x31};
}

IMP_ID(D, "#1", DirectSoundCreate, 12)
{
	static int n = 0;
	if (n++ < 10) log("dsound: DirectSoundCreate from %x", guest::load<uint32_t>(c->r[ESP]));
	if (!ARG(1)) { SET(DSERR_INVALIDPARAM); return; }
	Obj* o = make(KDirectSound);
	guest::store<uint32_t>(ARG(1), o->addr);
	SET(DS_OK);
}

// DirectSoundEnumerateW(callback, context): the primary device (NULL guid) and one real device
IMP_ID(D, "#3", DirectSoundEnumerateW, 8)
{
	uint32_t cb = ARG(0), ctx = ARG(1);
	static uint32_t guidMem = 0, descP = 0, descD = 0, mod = 0;
	if (!guidMem)
	{
		guidMem = gmalloc(16); memcpy(guest::host(guidMem), kDevGuid, 16);
		auto wstr = [](const char* s) { uint32_t p = gmalloc(2 * (uint32_t)strlen(s) + 2); for (size_t i = 0; i <= strlen(s); i++) guest::store<uint16_t>(p + 2 * (uint32_t)i, (uint16_t)s[i]); return p; };
		descP = wstr("Primary Sound Driver"); descD = wstr("Android Audio"); mod = wstr("");
	}
	static int n = 0;
	if (n++ < 5) log("dsound: DirectSoundEnumerateW from %x callback %x", guest::load<uint32_t>(c->r[ESP]), cb);
	if (cb)
	{
		uint32_t r = callGuestRet(c, cb, {0, descP, mod, ctx}, 16);
		if (r) callGuestRet(c, cb, {guidMem, descD, mod, ctx}, 16);
	}
	SET(DS_OK);
}
IMP_ID(D, "#6", DirectSoundCaptureCreate, 12) { if (ARG(1)) guest::store<uint32_t>(ARG(1), 0); SET(0x88780078); }
IMP_ID(D, "#8", DirectSoundCaptureEnumerateW, 8) { SET(0); }
IMP_ID(D, "#9", GetDeviceID, 8)
{
	// DSDEVID_DefaultPlayback / DefaultVoicePlayback -> the one device we expose
	if (ARG(1)) memcpy(guest::host(ARG(1)), kDevGuid, 16);
	SET(0);
}

// XINPUT1_3.dll: the only import is ordinal 2 = XInputGetState(user, state*). No gamepads: ERROR_DEVICE_NOT_CONNECTED.
IMP_ID("XINPUT1_3.dll", "#2", XInputGetState, 8) { SET(1167); }
