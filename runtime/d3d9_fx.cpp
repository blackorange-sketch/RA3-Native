#include <chrono>
namespace rt { extern bool gFps60; }
// ID3DXEffect: parameter storage and handles, technique/pass application onto the device state.
#include <unistd.h>
#include <set>
#include <atomic>
#include <mutex>
#include <tuple>

#include "d3d9_fxrt.h"
#include "d3d9_int.h"
#include "d3dx_fx.h"
#include "gfx.h"

using namespace rt;

namespace d3d
{
namespace
{
struct HRec { FxInfo* fx; int kind; uint32_t a, b, c; };   // kind: 1 param, 2 tech, 3 pass, 4/5/6 annotation of param/tech/pass
// append-only, stable addresses, lock-free reads: the effect setters resolve a handle on every call
template <class T> struct StableVec
{
	static constexpr uint32_t kB = 1024, kN = 4096;
	std::atomic<T*> blk[kN] = {};
	std::atomic<uint32_t> n{0};
	uint32_t push(const T& v)   // writers serialised by hMu
	{
		uint32_t i = n.load(std::memory_order_relaxed);
		if (i >= kB * kN) fatal("fx: handle table full");
		T* b = blk[i / kB].load(std::memory_order_relaxed);
		if (!b) { b = new T[kB](); blk[i / kB].store(b, std::memory_order_relaxed); }
		b[i % kB] = v;
		n.store(i + 1, std::memory_order_release);
		return i;
	}
	uint32_t size() const { return n.load(std::memory_order_acquire); }
	const T* at(uint32_t i) const { return i < size() ? &blk[i / kB].load(std::memory_order_relaxed)[i % kB] : nullptr; }
};
StableVec<HRec> hrecs;
std::map<std::tuple<FxInfo*, int, uint32_t, uint32_t, uint32_t>, uint32_t> hmap;
std::mutex hMu;
constexpr uint32_t HBase = 0x7e000000;

uint32_t mkHandle(FxInfo* fx, int kind, uint32_t a, uint32_t b = 0, uint32_t c = 0)
{
	std::lock_guard<std::mutex> l(hMu);
	auto k = std::make_tuple(fx, kind, a, b, c);
	auto it = hmap.find(k);
	if (it != hmap.end()) return it->second;
	uint32_t h = HBase + 4 * (hrecs.push({fx, kind, a, b, c}) + 1);
	hmap[k] = h;
	return h;
}
const HRec* getRec(uint32_t h)
{
	if (h <= HBase || h > HBase + 0x01000000) return nullptr;
	return hrecs.at((h - HBase) / 4 - 1);
}
FxInfo* F(Obj* o) { return o->fx.get(); }
// Resolves a technique handle (record or name string).
const FxTech* techOf(Obj* o, uint32_t h, uint32_t* idx = nullptr)
{
	FxInfo* fx = F(o);
	if (!fx) return nullptr;
	if (const HRec* r = getRec(h)) { if (r->kind == 2 && r->fx == fx && r->a < fx->techs.size()) { if (idx) *idx = r->a; return &fx->techs[r->a]; } return nullptr; }
	if (h) { std::string n = gstr(h); for (size_t i = 0; i < fx->techs.size(); i++) if (fx->techs[i].name == n) { if (idx) *idx = (uint32_t)i; return &fx->techs[i]; } }
	return nullptr;
}
const FxAnn* annOf(uint32_t h)
{
	const HRec* r = getRec(h);
	if (!r || r->kind < 4) return nullptr;
	FxInfo* fx = r->fx;
	if (r->kind == 4) return r->a < fx->params.size() && r->b < fx->params[r->a].anns.size() ? &fx->params[r->a].anns[r->b] : nullptr;
	if (r->kind == 5) return r->a < fx->techs.size() && r->b < fx->techs[r->a].anns.size() ? &fx->techs[r->a].anns[r->b] : nullptr;
	return r->a < fx->techs.size() && r->b < fx->techs[r->a].passes.size() && r->c < fx->techs[r->a].passes[r->b].anns.size() ? &fx->techs[r->a].passes[r->b].anns[r->c] : nullptr;
}
const std::vector<FxAnn>* annListOf(Obj* o, uint32_t h, int* kind, uint32_t* a, uint32_t* b)
{
	FxInfo* fx = F(o);
	if (!fx) return nullptr;
	if (const HRec* r = getRec(h))
	{
		if (r->fx != fx) return nullptr;
		*a = r->a; *b = r->b;
		if (r->kind == 1 && r->a < fx->params.size()) { *kind = 4; return &fx->params[r->a].anns; }
		if (r->kind == 2 && r->a < fx->techs.size()) { *kind = 5; return &fx->techs[r->a].anns; }
		if (r->kind == 3 && r->a < fx->techs.size() && r->b < fx->techs[r->a].passes.size()) { *kind = 6; return &fx->techs[r->a].passes[r->b].anns; }
		return nullptr;
	}
	if (h) { std::string n = gstr(h); for (size_t i = 0; i < fx->params.size(); i++) if (fx->params[i].name == n) { *kind = 4; *a = (uint32_t)i; *b = 0; return &fx->params[i].anns; } }
	return nullptr;
}


// ---- sub-parameter references (members, elements): a handle carries the parameter index, scalar offset and a type id
std::deque<FxType> gSynth;
StableVec<const FxType*> gTypes;   // index 0: none
std::map<const FxType*, uint32_t> gTypeIds;
uint32_t typeId(const FxType* t)
{
	std::lock_guard<std::mutex> l(hMu);
	if (!gTypes.size()) gTypes.push(nullptr);
	auto it = gTypeIds.find(t);
	if (it != gTypeIds.end()) return it->second;
	return gTypeIds[t] = gTypes.push(t);
}
const FxType* oneElement(const FxType& t)
{
	std::lock_guard<std::mutex> l(hMu);
	gSynth.push_back(t);
	gSynth.back().elements = 0;
	return &gSynth.back();
}
struct PRef
{
	FxParam* p = nullptr;
	uint32_t off = 0;           // scalar offset into p->val
	const FxType* ty = nullptr;
	bool valid() const { return p != nullptr; }
};
PRef refOf(Obj* o, uint32_t h)
{
	PRef r;
	FxInfo* fx = F(o);
	if (!fx) return r;
	if (const HRec* rc = getRec(h))
	{
		if (rc->kind != 1 || rc->fx != fx || rc->a >= fx->params.size()) return r;
		r.p = &fx->params[rc->a];
		r.off = rc->b;
		const FxType* const* t = rc->c ? gTypes.at(rc->c) : nullptr;
		r.ty = t ? *t : &r.p->ty;
		return r;
	}
	if (h)   // plain name
	{
		std::string n = gstr(h);
		int i = fxrt::findParam(*fx, n);
		if (i >= 0) { r.p = &fx->params[i]; r.ty = &r.p->ty; }
	}
	return r;
}

// ---- shared parameters (D3DXEffectPool): all effects see the last value written to a same-named shared
// parameter. Copied lazily: a write records its source and bumps the group version; an effect brings its copies
// up to date when one of its passes is applied.
static std::mutex gShareMu;
struct ShareGroup { std::weak_ptr<FxInfo> srcW; FxInfo* srcFx = nullptr; FxParam* src = nullptr; uint64_t ver = 0; };
static std::unordered_map<std::string, ShareGroup> gShareGroups;   // shared parameter name -> latest source
static uint64_t gShareVer = 0;
}   // anonymous namespace (reopened below)
void fxRegister(const std::shared_ptr<FxInfo>& fx)
{
	std::lock_guard<std::mutex> l(gShareMu);
	std::string sh;
	for (uint32_t i = 0; i < fx->params.size(); i++)
	{
		auto& p = fx->params[i];
		if (!(p.flags & 1)) continue;
		sh += (sh.empty() ? "" : ",") + p.name;
		p.shareGroup = &gShareGroups[p.name];
		p.shareVer = 0;
		fx->sharedIdx.push_back(i);
	}
	log("   shared params: %s", sh.c_str());
}
namespace
{
static void syncParam(FxParam& q);
PRef refOfSet(Obj* o, uint32_t h)
{
	PRef r = refOf(o, h);
	if (r.valid() && r.p->shareGroup)
	{
		// partial writes (struct members, array elements) must land on an up-to-date copy
		syncParam(*r.p);
		ShareGroup* g = (ShareGroup*)r.p->shareGroup;
		FxInfo* fx = F(o);
		if (g->srcFx != fx || g->src != r.p) { g->srcW = o->fx; g->srcFx = fx; g->src = r.p; }
		g->ver = ++gShareVer;
		r.p->shareVer = g->ver;
	}
	return r;
}
static void syncParam(FxParam& q)
{
	ShareGroup* g = (ShareGroup*)q.shareGroup;
	if (!g || q.shareVer == g->ver) return;
	q.shareVer = g->ver;
	if (!g->src || g->src == &q || g->srcW.expired()) return;
	if (q.val.size() == g->src->val.size()) q.val = g->src->val;
	q.texObj = g->src->texObj;
}
// reads of a shared parameter see the latest value written through any effect
PRef refOfGet(Obj* o, uint32_t h)
{
	PRef r = refOf(o, h);
	if (r.valid() && r.p->shareGroup) syncParam(*r.p);
	return r;
}
static void syncShared(FxInfo& fx)
{
	for (uint32_t i : fx.sharedIdx)
	{
		FxParam& q = fx.params[i];
		ShareGroup* g = (ShareGroup*)q.shareGroup;
		if (!g || q.shareVer == g->ver) continue;
		q.shareVer = g->ver;
		if (!g->src || g->src == &q || g->srcW.expired()) continue;
		if (q.val.size() == g->src->val.size()) q.val = g->src->val;
		q.texObj = g->src->texObj;
	}
}
uint32_t handleOf(Obj* o, const PRef& r)
{
	FxInfo* fx = F(o);
	uint32_t a = (uint32_t)(r.p - fx->params.data());
	uint32_t c = r.ty == &r.p->ty ? 0 : typeId(r.ty);
	return mkHandle(fx, 1, a, r.off, c);
}
// resolve "Name[2].Member[1]" relative to `base` (invalid base: global scope)
bool resolvePath(FxInfo* fx, PRef base, const std::string& path, PRef& out)
{
	size_t i = 0;
	PRef cur = base;
	bool first = !base.valid();
	while (i < path.size())
	{
		size_t e = i;
		while (e < path.size() && path[e] != '.' && path[e] != '[') e++;
		std::string id = path.substr(i, e - i);
		if (!id.empty())
		{
			if (first)
			{
				int k = fxrt::findParam(*fx, id);
				if (k < 0) return false;
				cur.p = &fx->params[k];
				cur.ty = &cur.p->ty;
				cur.off = 0;
			}
			else
			{
				if (cur.ty->cls != 5 || cur.ty->elements) return false;
				uint32_t off = 0;
				bool found = false;
				for (auto& m : cur.ty->members)
				{
					if (m.name == id) { cur.off += off; cur.ty = &m; found = true; break; }
					off += m.scalars() * std::max(1u, m.elements);
				}
				if (!found) return false;
			}
			first = false;
		}
		i = e;
		if (i < path.size() && path[i] == '[')
		{
			size_t close = path.find(']', i);
			if (close == std::string::npos || first) return false;
			uint32_t idx = (uint32_t)atoi(path.c_str() + i + 1);
			if (!cur.ty->elements || idx >= cur.ty->elements) return false;
			cur.off += idx * cur.ty->scalars();
			cur.ty = oneElement(*cur.ty);
			i = close + 1;
		}
		if (i < path.size() && path[i] == '.') i++;
	}
	if (!cur.valid()) return false;
	out = cur;
	return true;
}

// ---- value access
static uint32_t leafType(const FxType* t) { return t->cls == 5 ? 0 : t->type; }
static uint32_t toLeaf(uint32_t leaf, uint32_t srcType, uint32_t v)   // convert a scalar to the parameter's scalar type
{
	if (leaf < 1 || leaf > 3 || leaf == srcType) return v;
	float f;
	int32_t i;
	if (srcType == 3) { memcpy(&f, &v, 4); i = (int32_t)f; }
	else { i = (int32_t)v; f = srcType == 1 ? (v ? 1.f : 0.f) : (float)i; }
	if (leaf == 3) { memcpy(&v, &f, 4); return v; }
	if (leaf == 2) return (uint32_t)i;
	return srcType == 3 ? (f != 0.f) : (i != 0);
}
static void writeVals(const PRef& r, const uint32_t* src, uint32_t n, uint32_t srcType)
{
	uint32_t leaf = leafType(r.ty);
	for (uint32_t i = 0; i < n && r.off + i < r.p->val.size(); i++) r.p->val[r.off + i] = toLeaf(leaf, srcType, src[i]);
}
static void readVals(const PRef& r, uint32_t* dst, uint32_t n, uint32_t dstType)
{
	uint32_t leaf = leafType(r.ty);
	for (uint32_t i = 0; i < n; i++)
	{
		uint32_t v = r.off + i < r.p->val.size() ? r.p->val[r.off + i] : 0;
		dst[i] = (leaf >= 1 && leaf <= 3) ? toLeaf(dstType, leaf, v) : v;
	}
}
// matrices: m is a row-major 4x4 in host memory
static void putMatrix(const PRef& r, const float* m, bool transpose)
{
	const FxType* t = r.ty;
	if (t->cls != 2 && t->cls != 3)
	{
		uint32_t n = t->scalars();
		std::vector<uint32_t> tmp(n);
		memcpy(tmp.data(), m, 4 * std::min(n, 16u));
		writeVals(r, tmp.data(), n, 3);
		return;
	}
	uint32_t R = t->rows, C = t->cols, leaf = leafType(t);
	for (uint32_t row = 0; row < R; row++)
		for (uint32_t col = 0; col < C; col++)
		{
			float v = transpose ? m[col * 4 + row] : m[row * 4 + col];
			uint32_t bits;
			memcpy(&bits, &v, 4);
			uint32_t idx = r.off + (t->cls == 3 ? row * C + col : col * R + row);
			if (idx < r.p->val.size()) r.p->val[idx] = toLeaf(leaf, 3, bits);
		}
}
static void getMatrix(const PRef& r, float* m, bool transpose)
{
	for (int i = 0; i < 16; i++) m[i] = (i % 5 == 0) ? 1.f : 0.f;
	const FxType* t = r.ty;
	if (t->cls != 2 && t->cls != 3) return;
	uint32_t R = t->rows, C = t->cols, leaf = leafType(t);
	for (uint32_t row = 0; row < R; row++)
		for (uint32_t col = 0; col < C; col++)
		{
			uint32_t idx = r.off + (t->cls == 3 ? row * C + col : col * R + row);
			uint32_t v = idx < r.p->val.size() ? r.p->val[idx] : 0;
			v = toLeaf(3, leaf, v);
			float f;
			memcpy(&f, &v, 4);
			if (transpose) m[col * 4 + row] = f; else m[row * 4 + col] = f;
		}
}

H("ID3DXEffect::GetParameter")
{
	FxInfo* fx = F(o);
	if (!fx) { SET(newStr("param")); return; }
	uint32_t idx = ARG(2);
	if (!ARG(1)) { SET(idx < fx->params.size() ? mkHandle(fx, 1, idx) : 0); return; }
	PRef b = refOf(o, ARG(1));
	if (!b.valid()) { SET(0); return; }
	PRef r = b;
	if (b.ty->elements)   // element of an array
	{
		if (idx >= b.ty->elements) { SET(0); return; }
		r.off += idx * b.ty->scalars();
		r.ty = oneElement(*b.ty);
	}
	else if (b.ty->cls == 5)   // member of a struct
	{
		if (idx >= b.ty->members.size()) { SET(0); return; }
		uint32_t off = 0;
		for (uint32_t i = 0; i < idx; i++) off += b.ty->members[i].scalars() * std::max(1u, b.ty->members[i].elements);
		r.off += off;
		r.ty = &b.ty->members[idx];
	}
	else { SET(0); return; }
	SET(handleOf(o, r));
} END;
H("ID3DXEffect::GetParameterByName")
{
	FxInfo* fx = F(o);
	if (!ARG(2)) { SET(0); return; }
	std::string n = gstr(ARG(2));
	if (!fx) { SET(newStr(n)); return; }
	PRef base;
	if (ARG(1)) { base = refOf(o, ARG(1)); if (!base.valid()) { SET(0); return; } }
	PRef r;
	if (resolvePath(fx, base, n, r)) SET(handleOf(o, r)); else SET(0);
} END;
H("ID3DXEffect::GetParameterBySemantic")
{
	FxInfo* fx = F(o);
	if (!ARG(2)) { SET(0); return; }
	std::string n = gstr(ARG(2));
	if (!fx) { SET(newStr(n)); return; }
	for (size_t i = 0; i < fx->params.size(); i++) if (strcasecmp(fx->params[i].semantic.c_str(), n.c_str()) == 0) { SET(mkHandle(fx, 1, (uint32_t)i)); return; }
	SET(0);
} END;
H("ID3DXEffect::GetParameterElement")
{
	PRef b = refOf(o, ARG(1));
	if (!b.valid()) { SET(F(o) ? 0 : newStr("elem")); return; }
	uint32_t idx = ARG(2);
	if (!b.ty->elements || idx >= b.ty->elements) { SET(0); return; }
	PRef r = b;
	r.off += idx * b.ty->scalars();
	r.ty = oneElement(*b.ty);
	SET(handleOf(o, r));
} END;
H("ID3DXEffect::GetParameterDesc")
{
	uint32_t p = ARG(2);
	memset(guest::host(p), 0, 44);
	PRef pr = refOf(o, ARG(1));
	const FxAnn* an = pr.valid() ? nullptr : annOf(ARG(1));
	if (!pr.valid() && !an) { SET(0x8876086c); return; }   // D3DERR_INVALIDCALL
	std::string name, sem;
	uint32_t cls, type, rows, cols, el, members = 0, flags = 0, nanns = 0, bytes;
	if (pr.valid())
	{
		const FxType& t = *pr.ty;
		name = t.name; sem = t.semantic;
		if (pr.ty == &pr.p->ty) { name = pr.p->name; sem = pr.p->semantic; flags = pr.p->flags; nanns = (uint32_t)pr.p->anns.size(); }
		cls = t.cls; type = t.type; rows = t.rows; cols = t.cols; el = t.elements; members = (uint32_t)t.members.size();
		bytes = 4 * t.scalars() * std::max(1u, t.elements);
	}
	else
	{
		name = an->name; cls = an->cls; type = an->type; rows = an->rows; cols = an->cols; el = an->elements;
		bytes = 4 * rows * cols * std::max(1u, el);
	}
	guest::store<uint32_t>(p, newStr(name));
	guest::store<uint32_t>(p + 4, sem.empty() ? 0 : newStr(sem));
	guest::store<uint32_t>(p + 8, cls);
	guest::store<uint32_t>(p + 12, type);
	guest::store<uint32_t>(p + 16, rows);
	guest::store<uint32_t>(p + 20, cols);
	guest::store<uint32_t>(p + 24, el);
	guest::store<uint32_t>(p + 28, nanns);
	guest::store<uint32_t>(p + 32, members);
	guest::store<uint32_t>(p + 36, flags);
	guest::store<uint32_t>(p + 40, bytes);
	SET(0);
} END;

// ---- setters
// 60 fps diagnostics: how the game's shader clock 'Time' advances against wall time and presented frames, and who sets it
static void noteTime(CPU* c, const PRef& r, const void* v, const char* how)
{
	if (!verbose() || !r.valid() || r.p->name != "Time") return;   // only with ra3_verbose
	static int n = 0;
	static uint64_t lastUs = 0;
	uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (n >= 40 || us - lastUs < 1000000) return;
	lastUs = us; n++;
	float f; memcpy(&f, v, 4);
	// return addresses: the stack words that point into the game's code (EBP is not a frame pointer here)
	std::string ra;
	for (uint32_t k = 0, found = 0; k < 64 && found < 6; k++)
	{
		uint32_t w = guest::load<uint32_t>(c->r[ESP] + 4 * k);
		if (w >= 0x401000 && w < 0xa80000) { char b[12]; snprintf(b, sizeof b, " %08x", w); ra += b; found++; }
	}
	log("fxtime: %s Time=%.4f at %.3f s, frame %u, fps60 %d, callers%s", how, f, us / 1e6, glr::frameNo(), ::rt::gFps60 ? 1 : 0, ra.c_str());
}
H("ID3DXEffect::SetValue")
{
	PRef r = refOfSet(o, ARG(1));
	if (r.valid() && ARG(2))
	{
		if (r.ty->cls == 4)
		{
			if (r.p->type >= 5 && r.p->type <= 9)
			{
				r.p->texObj = guest::load<uint32_t>(ARG(2));
				static int nl = 0;
				if (nl++ < 40) log("fx: SetValue texture '%s' -> %x", r.p->name.c_str(), r.p->texObj);
			}
			SET(0);
			return;
		}
		uint32_t n = std::min(ARG(3) / 4, (uint32_t)r.p->val.size() - std::min<uint32_t>(r.off, (uint32_t)r.p->val.size()));
		memcpy(r.p->val.data() + r.off, guest::host(ARG(2)), 4ull * n);
		if (n) noteTime(c, r, guest::host(ARG(2)), "SetValue");
	}
	SET(0);
} END;
H("ID3DXEffect::SetBool") { PRef r = refOfSet(o, ARG(1)); if (r.valid()) { uint32_t v = ARG(2) ? 1 : 0; writeVals(r, &v, 1, 1); } SET(0); } END;
H("ID3DXEffect::SetInt") { PRef r = refOfSet(o, ARG(1)); if (r.valid()) { uint32_t v = ARG(2); writeVals(r, &v, 1, 2); } SET(0); } END;
H("ID3DXEffect::SetFloat")
{
	PRef r = refOfSet(o, ARG(1));
	if (r.valid())
	{
		uint32_t v = ARG(2);
		writeVals(r, &v, 1, 3);
		noteTime(c, r, &v, "SetFloat");
		static bool diag = access("/sdcard/ra3_fxdiag", F_OK) == 0;
		static int n = 0;
		if (diag && n < 20 && r.p->name == "Time") { n++; float f; memcpy(&f, &v, 4); log("FXTIME SetFloat Time=%g", f); }
	}
	SET(0);
} END;
H("ID3DXEffect::SetBoolArray") { PRef r = refOfSet(o, ARG(1)); if (r.valid() && ARG(2)) { std::vector<uint32_t> v(ARG(3)); for (uint32_t i = 0; i < ARG(3); i++) v[i] = guest::load<uint32_t>(ARG(2) + 4 * i) ? 1 : 0; writeVals(r, v.data(), ARG(3), 1); } SET(0); } END;
H("ID3DXEffect::SetIntArray") { PRef r = refOfSet(o, ARG(1)); if (r.valid() && ARG(2)) writeVals(r, (const uint32_t*)guest::host(ARG(2)), ARG(3), 2); SET(0); } END;
H("ID3DXEffect::SetFloatArray") { PRef r = refOfSet(o, ARG(1)); if (r.valid() && ARG(2)) { writeVals(r, (const uint32_t*)guest::host(ARG(2)), ARG(3), 3); noteTime(c, r, guest::host(ARG(2)), "SetFloatArray"); } SET(0); } END;
static void noteBad(const char* m, uint32_t h, const PRef& r)
{
	static int n = 0;
	if (!r.valid() && n >= 25) return;
	if (!r.valid()) { n++; const HRec* rc = getRec(h); log("fx: %s with invalid handle %x (rec %p kind %d)", m, h, (const void*)rc, rc ? (int)rc->kind : -1); }
	else if (verbose())   // first write of each param (a set lookup per call: diagnostics only)
	{
		static std::set<const void*> seen;
		static int m2 = 0;
		if (m2 < 120 && seen.insert(r.p).second) { m2++; log("fx: %s writes param '%s' type %u cls %u rows %u cols %u smpTex %u", m, r.p->name.c_str(), r.p->type, r.p->cls, r.p->rows, r.p->cols, r.p->smpTex); }
	}
	if (!r.valid()) {}
	else if ((r.p->cls == 4 || r.p->type >= 5) && n < 25) { n++; log("fx: %s on object param '%s' type %u cls %u", m, r.p->name.c_str(), r.p->type, r.p->cls); }
}
H("ID3DXEffect::SetVector")
{
	PRef r = refOfSet(o, ARG(1));
	noteBad("SetVector", ARG(1), r);
	if (r.valid() && ARG(2))
	{
		writeVals(r, (const uint32_t*)guest::host(ARG(2)), std::min(r.ty->scalars(), 4u), 3);
		noteTime(c, r, guest::host(ARG(2)), "SetVector");
		if (verbose() && r.p->name == "EyePosition")
		{
			static int n = 0; const float* v = (const float*)guest::host(ARG(2));
			if ((v[0] != 0.f || v[1] != 0.f || v[2] != 0.f) && n++ < 6) log("fx: EyePosition %g %g %g", v[0], v[1], v[2]);
		}
	}
	SET(0);
} END;
H("ID3DXEffect::SetVectorArray")
{
	PRef r = refOfSet(o, ARG(1));
	noteBad("SetVectorArray", ARG(1), r);
	if (r.valid() && ARG(2))
	{
		uint32_t per = std::min(r.ty->scalars(), 4u), stride = r.ty->scalars();
		for (uint32_t i = 0; i < ARG(3); i++)
		{
			PRef e = r;
			e.off += i * stride;
			writeVals(e, (const uint32_t*)guest::host(ARG(2) + 16 * i), per, 3);
		}
	}
	SET(0);
} END;
static void setMatrices(CPU* c, Obj* o, bool transpose, bool array, bool pointers)
{
	PRef r = refOfSet(o, ARG(1));
	noteBad("SetMatrix*", ARG(1), r);
	if (r.valid() && ARG(2))
	{
		uint32_t n = array ? ARG(3) : 1, stride = r.ty->scalars();
		for (uint32_t i = 0; i < n; i++)
		{
			PRef e = r;
			e.off += i * stride;
			const float* m = pointers ? (const float*)guest::host(guest::load<uint32_t>(ARG(2) + 4 * i)) : (const float*)guest::host(ARG(2) + 64 * i);
			putMatrix(e, m, transpose);
			if (verbose() && r.p->name == "ViewProjection")
			{
				static int total = 0, nonzero = 0, shown = 0;
				bool nz = false;
				for (int k = 0; k < 16; k++) if (m[k] != 0.f) nz = true;
				total++;
				if (nz) nonzero++;
				if (total == 1 || total == 3000 || total == 20000 || total == 40000)
				{
					std::string st;
					for (int k = 0; k < 24; k++) { char b[16]; snprintf(b, sizeof b, " %08x", guest::load<uint32_t>(c->r[ESP] + 4 * k)); st += b; }
					log("fx: ViewProjection #%d caller ret=%08x mptr=%08x stack:%s", total, guest::load<uint32_t>(c->r[ESP]), ARG(2), st.c_str());
				}
				if (verbose() && ((nz && shown++ < 8) || total % 2000 == 0))
					log("fx: ViewProjection set #%d (nonzero %d) on fx %p transpose %d [%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]", total, nonzero, (void*)F(o), (int)transpose, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
			}
		}
	}
	SET(0);
}
H("ID3DXEffect::SetMatrix") { setMatrices(c, o, false, false, false); } END;
H("ID3DXEffect::SetMatrixArray") { setMatrices(c, o, false, true, false); } END;
H("ID3DXEffect::SetMatrixPointerArray") { setMatrices(c, o, false, true, true); } END;
H("ID3DXEffect::SetMatrixTranspose") { setMatrices(c, o, true, false, false); } END;
H("ID3DXEffect::SetMatrixTransposeArray") { setMatrices(c, o, true, true, false); } END;
H("ID3DXEffect::SetMatrixTransposePointerArray") { setMatrices(c, o, true, true, true); } END;
H("ID3DXEffect::SetTexture")
{
	PRef r = refOfSet(o, ARG(1));
	if (r.valid()) r.p->texObj = ARG(2);
	{
		static int nl = 0;
		if (nl++ < 40) log("fx: SetTexture handle %x valid %d name '%s' tex %x", ARG(1), (int)r.valid(), r.valid() ? r.p->name.c_str() : "", ARG(2));
	}
	SET(0);
} END;
H("ID3DXEffect::SetString") { SET(0); } END;
H("ID3DXEffect::SetArrayRange") { SET(0); } END;

// ---- getters
H("ID3DXEffect::GetValue")
{
	PRef r = refOfGet(o, ARG(1));
	if (r.valid() && ARG(2)) { uint32_t n = std::min(ARG(3) / 4, (uint32_t)r.p->val.size() - std::min<uint32_t>(r.off, (uint32_t)r.p->val.size())); memcpy(guest::host(ARG(2)), r.p->val.data() + r.off, 4ull * n); }
	SET(0);
} END;
H("ID3DXEffect::GetBool")
{
	PRef r = refOfGet(o, ARG(1));
	if (r.valid()) { uint32_t v = 0; readVals(r, &v, 1, 1); if (ARG(2)) guest::store<uint32_t>(ARG(2), v); SET(0); return; }
	const FxAnn* an = annOf(ARG(1));
	if (ARG(2)) guest::store<uint32_t>(ARG(2), an && !an->vals.empty() && an->vals[0] ? 1 : 0);
	SET(0);
} END;
H("ID3DXEffect::GetInt")
{
	PRef r = refOfGet(o, ARG(1));
	if (r.valid()) { uint32_t v = 0; readVals(r, &v, 1, 2); if (ARG(2)) guest::store<uint32_t>(ARG(2), v); SET(0); return; }
	const FxAnn* an = annOf(ARG(1));
	if (ARG(2)) guest::store<uint32_t>(ARG(2), an && !an->vals.empty() ? (an->type == 3 ? (uint32_t)(int)*(float*)&an->vals[0] : an->vals[0]) : 0);
	SET(0);
} END;
H("ID3DXEffect::GetFloat")
{
	PRef r = refOfGet(o, ARG(1));
	if (r.valid()) { uint32_t v = 0; readVals(r, &v, 1, 3); if (ARG(2)) guest::store<uint32_t>(ARG(2), v); SET(0); return; }
	const FxAnn* an = annOf(ARG(1));
	float v = 0.f;
	if (an && !an->vals.empty()) { if (an->type == 3) memcpy(&v, &an->vals[0], 4); else v = (float)(int)an->vals[0]; }
	if (ARG(2)) guest::store<float>(ARG(2), v);
	SET(0);
} END;
H("ID3DXEffect::GetFloatArray") { PRef r = refOfGet(o, ARG(1)); if (r.valid() && ARG(2)) readVals(r, (uint32_t*)guest::host(ARG(2)), ARG(3), 3); SET(0); } END;
H("ID3DXEffect::GetIntArray") { PRef r = refOfGet(o, ARG(1)); if (r.valid() && ARG(2)) readVals(r, (uint32_t*)guest::host(ARG(2)), ARG(3), 2); SET(0); } END;
H("ID3DXEffect::GetBoolArray") { PRef r = refOfGet(o, ARG(1)); if (r.valid() && ARG(2)) readVals(r, (uint32_t*)guest::host(ARG(2)), ARG(3), 1); SET(0); } END;
H("ID3DXEffect::GetVector")
{
	PRef r = refOfGet(o, ARG(1));
	if (r.valid() && ARG(2)) { float v[4] = {0, 0, 0, 0}; readVals(r, (uint32_t*)v, std::min(r.ty->scalars(), 4u), 3); memcpy(guest::host(ARG(2)), v, 16); }
	SET(0);
} END;
H("ID3DXEffect::GetMatrix")
{
	PRef r = refOfGet(o, ARG(1));
	if (r.valid() && ARG(2)) { float m[16]; getMatrix(r, m, false); memcpy(guest::host(ARG(2)), m, 64); }
	SET(0);
} END;
H("ID3DXEffect::GetMatrixTranspose")
{
	PRef r = refOfGet(o, ARG(1));
	if (r.valid() && ARG(2)) { float m[16]; getMatrix(r, m, true); memcpy(guest::host(ARG(2)), m, 64); }
	SET(0);
} END;
H("ID3DXEffect::GetTexture")
{
	PRef r = refOfGet(o, ARG(1));
	if (ARG(2)) guest::store<uint32_t>(ARG(2), r.valid() ? r.p->texObj : 0);
	if (r.valid() && r.p->texObj) if (Obj* t = objectAt(r.p->texObj)) t->ref++;
	SET(0);
} END;

// ---- technique / pass application
struct PassCtx { FxInfo* fx = nullptr; uint32_t tech = 0, pass = 0; bool active = false; };
static PassCtx gPass;

static std::string cstrAt(const FxInfo& fx, const FxRes& r)
{
	const char* s = (const char*)fx.blob.data() + r.off;
	return std::string(s, strnlen(s, r.size));
}

static void packConst(const FxInfo& fx, const CtabConst& c, ShaderObj* sh, bool isVs, int k)
{
	if (k < 0) return;
	const FxParam& p = fx.params[k];
	if (p.ty.cls == 4 || p.val.empty() || p.skipConst) return;
	float* fregs = isVs ? dev.vsF : dev.psF;
	int32_t* iregs = isVs ? dev.vsI : dev.psI;
	int32_t* bregs = isVs ? dev.vsB : dev.psB;
	uint32_t maxF = isVs ? 256 : 224;
	// only the registers the shader's constant table assigns: the compiler drops unused rows (World float4x4 used
	// as 2 registers), and the next constant (Shroud) lives right behind them
	// /sdcard/ra3_fullpack: old behaviour (whole parameter, may spill into the following registers) for A/B checks
	static const bool fullPack = access("/sdcard/ra3_fullpack", F_OK) == 0;
	uint32_t n = fullPack ? 4096u : std::max(1u, c.regCount);
	if (c.regSet == 2)
	{
		if (c.regIndex >= maxF) return;
		fxrt::packToRegs(p.ty, p.val.data(), p.val.size(), 2, c.regIndex, (uint32_t*)fregs, std::min(maxF, c.regIndex + n));
	}
	else if (c.regSet == 1)
	{
		if (c.regIndex >= 16) return;
		fxrt::packToRegs(p.ty, p.val.data(), p.val.size(), 1, c.regIndex, (uint32_t*)iregs, std::min(16u, c.regIndex + n));
	}
	else if (c.regSet == 0)
	{
		if (c.regIndex >= 16) return;
		fxrt::packToRegs(p.ty, p.val.data(), p.val.size(), 0, c.regIndex, (uint32_t*)bregs, std::min(16u, c.regIndex + n));
	}
	(void)sh;
}

// index of the FxRes record of a pass state (-1: plain constant), cached
static int resOf(FxInfo& fx, uint32_t tech, uint32_t pass, uint32_t si)
{
	uint64_t key = (uint64_t)tech << 40 | (uint64_t)pass << 20 | si;
	auto it = fx.resIdx.find(key);
	if (it != fx.resIdx.end()) return it->second;
	int found = -1;
	for (size_t i = 0; i < fx.res.size(); i++)
		if (fx.res[i].tech == tech && fx.res[i].pass == pass && fx.res[i].state == si) { found = (int)i; break; }
	fx.resIdx[key] = found;
	return found;
}
// Shader chosen from an array by a pre-shader: the pick only changes when the pre-shader's inputs do, so the
// inputs are compared with the last ones instead of re-resolving (names, std::function, interpreter) per pass.
static bool dynPick(FxInfo& fx, uint64_t key, uint32_t tech, uint32_t pass, uint32_t si, ShaderObj*& out)
{
	FxInfo::DynSel& ds = fx.dynSel[key];
	if (ds.none) return false;
	if (!ds.pp)   // first use of this pass state: find its pre-shader once
	{
		int ri = resOf(fx, tech, pass, si);
		const FxRes* r = ri >= 0 && fx.res[ri].usage == 2 && fx.res[ri].size >= 8 ? &fx.res[ri] : nullptr;
		const fxrt::PresProg* prog0 = nullptr;
		if (r)
		{
			const uint8_t* d = fx.blob.data() + r->off;
			uint32_t nl;
			memcpy(&nl, d, 4);
			if (4ull + nl <= r->size) prog0 = &fxrt::presProg(d + 4 + nl, r->size - 4 - nl);
		}
		if (!prog0 || !prog0->ok) { ds.none = true; return false; }
		ds.pp = prog0;
		for (auto& c : prog0->ctab) ds.pidx.push_back(fxrt::findParam(fx, c.name));
	}
	const fxrt::PresProg& prog = *(const fxrt::PresProg*)ds.pp;
	static thread_local std::vector<double> in;
	static thread_local std::vector<uint32_t> regs;
	in.assign(prog.nIn, 0.0);
	for (size_t i = 0; i < prog.ctab.size(); i++)
	{
		const CtabConst& c = prog.ctab[i];
		size_t nd = 4 * std::max(1u, c.regCount);
		if (c.regIndex * 4 + nd > in.size() || ds.pidx[i] < 0) continue;
		const FxParam& pp = fx.params[ds.pidx[i]];
		regs.assign(nd, 0);
		fxrt::packToRegs(pp.ty, pp.val.data(), pp.val.size(), 2, 0, regs.data(), (uint32_t)(nd / 4));
		for (size_t k = 0; k < nd; k++) { float f; memcpy(&f, &regs[k], 4); in[c.regIndex * 4 + k] = f; }
	}
	if (ds.valid && ds.in == in) { out = (ShaderObj*)ds.sh; return true; }
	ShaderObj* sh = nullptr;
	fxrt::ShaderRef ref;
	if (fxrt::resolveShader(fx, tech, pass, si, ref))
	{
		auto ci = fx.codeCache.find(ref.code);
		if (ci != fx.codeCache.end()) sh = (ShaderObj*)ci->second;
		else { sh = getShader(ref.code, ref.size); fx.codeCache[ref.code] = sh; }
	}
	ds.in = in; ds.sh = sh; ds.valid = true;
	out = sh;
	return true;
}

static void applyShaderState(FxInfo& fx, const FxPass& ps, uint32_t tech, uint32_t pass, uint32_t si)
{
	const FxState& st = ps.sts[si];
	fxrt::StateInfo info = fxrt::stateInfo(st.op);
	if (info.cls == fxrt::SC_VS || info.cls == fxrt::SC_PS)
	{
		uint64_t key = (uint64_t)tech << 40 | (uint64_t)pass << 20 | si;
		auto it = fx.shCache.find(key);
		ShaderObj* sh = nullptr;
		if (it != fx.shCache.end()) sh = (ShaderObj*)it->second;
		else if (ShaderObj* ds; dynPick(fx, key, tech, pass, si, ds)) sh = ds;
		else
		{
			fxrt::ShaderRef ref;
			if (fxrt::resolveShader(fx, tech, pass, si, ref))
			{
				auto ci = fx.codeCache.find(ref.code);
				if (ci != fx.codeCache.end()) sh = (ShaderObj*)ci->second;
				else { sh = getShader(ref.code, ref.size); fx.codeCache[ref.code] = sh; }
			}
			// shaders picked from an array by a pre-shader depend on parameter values: resolve those every time
			bool dynamic = false;
			int ri = resOf(fx, tech, pass, si);
			if (ri >= 0 && fx.res[ri].usage == 2) dynamic = true;
			if (!dynamic) fx.shCache[key] = sh;
		}
		if (info.cls == fxrt::SC_VS) dev.vs = sh; else dev.ps = sh;
		return;
	}
	// value: constant at vo, or a parameter reference
	uint32_t value = fx.u32(fx.base + st.vo);
	if (st.res == -2) st.res = resOf(fx, tech, pass, si);   // per state, not a hash lookup per pass
	int ri = st.res;
	if (ri >= 0)
	{
		const FxRes& r = fx.res[ri];
		if (r.usage == 1)
		{
			std::string path = cstrAt(fx, r);
			PRef ref;
			if (resolvePath(&fx, PRef(), path, ref) && ref.off < ref.p->val.size())
				value = toLeaf(st.typeType, ref.ty->type, ref.p->val[ref.off]);
		}
	}
	switch (info.cls)
	{
	case fxrt::SC_RS:
		if (info.id < 256) dev.rs[info.id] = value;
		break;
	default: break;
	}
}

void fxApplyPass(Obj* o)
{
	FxInfo* fx = F(o);
	if (!fx || !gPass.active || gPass.fx != fx) return;
	if (fx->diagId < 0)
	{
		static int nextId = 0;
		fx->diagId = nextId++;
		if (fxStatOn())
		{
			std::string t;
			for (auto& tc : fx->techs) { t += tc.name; t += ' '; }
			log("FXSTAT effect #%d: %zu params, blob %zu, techniques: %s", fx->diagId, fx->params.size(), fx->blob.size(), t.c_str());
		}
	}
	gFxDiagCur = fx->diagId; gFxDiagTech = gPass.tech; gFxDiagBlob = (uint32_t)fx->blob.size();
	syncShared(*fx);
	const FxPass& ps = fx->techs[gPass.tech].passes[gPass.pass];
	for (uint32_t i = 0; i < ps.sts.size(); i++) applyShaderState(*fx, ps, gPass.tech, gPass.pass, i);
	// samplers and constants required by the selected shaders
	for (int stage = 0; stage < 2; stage++)
	{
		ShaderObj* sh = stage == 0 ? dev.vs : dev.ps;
		if (!sh || !sh->ok) continue;
		auto ci = fx->ctabIdx.find(sh);
		if (ci == fx->ctabIdx.end())
		{
			std::vector<int> v;
			for (auto& c : sh->info.ctab) v.push_back(fxrt::findParam(*fx, c.name));
			ci = fx->ctabIdx.emplace(sh, std::move(v)).first;
		}
		const std::vector<int>& pidx = ci->second;
		for (size_t cn = 0; cn < sh->info.ctab.size(); cn++)
		{
			const CtabConst& c = sh->info.ctab[cn];
			int pk = pidx[cn];
			if (c.regSet == 3)
			{
				int k = pk;
				if (k < 0 || c.regIndex >= 16) continue;
				const FxParam& sp = fx->params[k];
				{
					static int nl = 0;
					if (nl++ < 40) log("fx: sampler '%s' reg %u param %d smpTex %u texObj %x linkedTex %x", c.name.c_str(), c.regIndex, k, sp.smpTex, sp.texObj, sp.smpTex ? fx->params[sp.smpTex - 1].texObj : 0u);
				}
				if (sp.smpTex)
				{
					uint32_t t = fx->params[sp.smpTex - 1].texObj;
					if (t) dev.tex[c.regIndex] = objectAt(t);
				}
				else if (sp.texObj) dev.tex[c.regIndex] = objectAt(sp.texObj);
				for (auto& s : sp.smp) if (s.op < 16) dev.samp[c.regIndex][s.op] = s.v;
			}
			else packConst(*fx, c, sh, stage == 0, pk);
		}
		if (!sh->info.pres.empty())
		{
			// embedded pre-shader: constants computed from effect parameters (time, atlas layout, ...) that the
			// shader reads but its own constant table does not list
			// inputs are packed from the effect parameters (indices cached per shader); when they equal the last
			// ones the outputs are reused: most draws of a frame see the same time and atlas values
			FxInfo::PresCache& pc = fx->presCache[sh];
			if (pc.blob != sh->info.pres.data())   // parsed program looked up once per shader (presProg hashes the blob)
			{
				const fxrt::PresProg& prog0 = fxrt::presProg(sh->info.pres.data(), sh->info.pres.size());
				pc = FxInfo::PresCache(); pc.pp = &prog0; pc.blob = sh->info.pres.data();
				for (auto& c : prog0.ctab) pc.pidx.push_back(fxrt::findParam(*fx, c.name));
			}
			const fxrt::PresProg& prog = *(const fxrt::PresProg*)pc.pp;
			static thread_local std::vector<double> in;
			static thread_local std::vector<uint32_t> regs;
			in.assign(prog.nIn, 0.0);
			for (size_t ci2 = 0; ci2 < prog.ctab.size(); ci2++)
			{
				const CtabConst& c = prog.ctab[ci2];
				size_t nd = 4 * std::max(1u, c.regCount);
				if (c.regIndex * 4 + nd > in.size()) continue;
				int k = pc.pidx[ci2];
				if (k < 0) continue;
				const FxParam& pp = fx->params[k];
				if (pp.ty.cls == 4 || pp.val.empty()) continue;
				double* dst = &in[c.regIndex * 4];
				regs.assign(nd, 0);
				fxrt::packToRegs(pp.ty, pp.val.data(), pp.val.size(), 2, 0, regs.data(), (uint32_t)(nd / 4));
				for (size_t i = 0; i < nd; i++) { float f; memcpy(&f, &regs[i], 4); dst[i] = f; }
				// GPU particles: the shader turns Time into a frame clock with a fixed 30 (frames per second) while the
				// game stamps each particle's birth with the frame clock of the current cap. Above 30 fps the clocks
				// drift apart (particles born "in the future", effects stretched or overbright): scale Time to match.
				if (::rt::gFps60 && c.name == "Time")
				{
					if (fx->gpuParticle < 0) fx->gpuParticle = fxrt::findParam(*fx, "Physics") >= 0 && fxrt::findParam(*fx, "Update") >= 0 ? 1 : 0;
					uint32_t cap = guest::load<uint32_t>(0xcaf9d4);
					if (fx->gpuParticle == 1 && cap > 30 && cap <= 240) dst[0] *= cap / 30.0;
				}
			}
			if (!pc.valid || pc.in != in)
			{
				fxrt::PresOut out;
				pc.ok = fxrt::execPreshader(prog, in, out);
				pc.in = in; pc.f = std::move(out.f); pc.b = std::move(out.b); pc.valid = true;
			}
			bool okp = pc.ok;
			struct { const std::vector<std::pair<uint32_t, double>>& f; const std::vector<std::pair<uint32_t, double>>& b; } po{pc.f, pc.b};
			float* fregs = stage == 0 ? dev.vsF : dev.psF;
			int32_t* bregs = stage == 0 ? dev.vsB : dev.psB;
			uint32_t maxF = stage == 0 ? 256 : 224;
			if (okp)
			{
				for (auto& w : po.f) if (w.first < maxF * 4) fregs[w.first] = (float)w.second;
				for (auto& w : po.b) if (w.first / 4 < 16) bregs[w.first / 4] = w.second != 0;
			}
			static int npl = 0;
			if (npl < 6 && (!okp || verbose())) { npl++; log("fx: pre-shader %s: %zu float, %zu bool outputs", okp ? "ok" : "FAILED", po.f.size(), po.b.size()); }
			static bool presDiag = access("/sdcard/ra3_fxdiag", F_OK) == 0;
			static int npd = 0;
			if (presDiag && npd < 40 && okp)
			{
				// effects diagnostics: inputs and outputs of embedded pre-shaders (time, atlas constants)
				npd++;
				std::string in, out;
				int ti = fxrt::findParam(*fx, "Time");
				if (ti >= 0 && !fx->params[ti].val.empty()) { float tv; memcpy(&tv, &fx->params[ti].val[0], 4); char b[48]; snprintf(b, sizeof b, " Time=%g", tv); in += b; }
				for (auto& w : po.f) { char b[48]; snprintf(b, sizeof b, " c%u.%c=%g", w.first / 4, "xyzw"[w.first % 4], w.second); out += b; }
				for (auto& w : po.b) { char b[32]; snprintf(b, sizeof b, " b%u=%g", w.first / 4, w.second); out += b; }
				log("FXPRES %s vs %016llx:%s ->%s", stage == 0 ? "VS" : "PS", (unsigned long long)sh->hash, in.c_str(), out.c_str());
			}
		}
	}
}
void fxCommit(Obj* o) { fxApplyPass(o); }

H("ID3DXEffect::Begin")
{
	FxInfo* fx = F(o);
	if (ARG(1)) guest::store<uint32_t>(ARG(1), fx && fx->current < fx->techs.size() ? (uint32_t)fx->techs[fx->current].passes.size() : 1);
	if (fx) { gPass.fx = fx; gPass.tech = fx->current; gPass.pass = 0; gPass.active = false; }
	SET(0);
} END;
H("ID3DXEffect::BeginPass")
{
	FxInfo* fx = F(o);
	if (fx && fx->current < fx->techs.size() && ARG(1) < fx->techs[fx->current].passes.size())
	{
		gPass.fx = fx; gPass.tech = fx->current; gPass.pass = ARG(1); gPass.active = true;
		fxApplyPass(o);
	}
	SET(0);
} END;
H("ID3DXEffect::CommitChanges") { fxApplyPass(o); SET(0); } END;
H("ID3DXEffect::EndPass") { SET(0); } END;
H("ID3DXEffect::End") { gPass.active = false; SET(0); } END;
H("ID3DXEffect::OnLostDevice") { SET(0); } END;
H("ID3DXEffect::OnResetDevice") { SET(0); } END;
H("ID3DXEffect::GetDesc")
{
	uint32_t p = ARG(1);
	memset(guest::host(p), 0, 16);
	FxInfo* fx = F(o);
	guest::store<uint32_t>(p + 4, fx ? (uint32_t)fx->params.size() : 0);
	guest::store<uint32_t>(p + 8, fx ? (uint32_t)fx->techs.size() : 1);
	SET(0);
} END;
H("ID3DXEffect::GetTechniqueDesc")
{
	uint32_t p = ARG(2);
	memset(guest::host(p), 0, 12);
	if (const FxTech* t = techOf(o, ARG(1)))
	{
		guest::store<uint32_t>(p, newStr(t->name));
		guest::store<uint32_t>(p + 4, (uint32_t)t->passes.size());
		guest::store<uint32_t>(p + 8, (uint32_t)t->anns.size());
	}
	else { guest::store<uint32_t>(p, newStr("T")); guest::store<uint32_t>(p + 4, 1); }
	SET(0);
} END;
H("ID3DXEffect::GetPassDesc")
{
	uint32_t p = ARG(2);
	memset(guest::host(p), 0, 16);
	if (const HRec* r = getRec(ARG(1)); r && r->kind == 3 && r->fx == F(o) && r->a < r->fx->techs.size() && r->b < r->fx->techs[r->a].passes.size())
	{
		const FxPass& ps = r->fx->techs[r->a].passes[r->b];
		guest::store<uint32_t>(p, newStr(ps.name));
		guest::store<uint32_t>(p + 4, (uint32_t)ps.anns.size());
	}
	SET(0);
} END;
H("ID3DXEffect::GetTechnique")
{
	FxInfo* fx = F(o);
	if (!fx) { SET(0x7e01); return; }
	SET(ARG(1) < fx->techs.size() ? mkHandle(fx, 2, ARG(1)) : 0);
} END;
H("ID3DXEffect::GetTechniqueByName")
{
	FxInfo* fx = F(o);
	if (!fx) { SET(0x7e01); return; }
	uint32_t idx;
	SET(ARG(1) && techOf(o, ARG(1), &idx) ? mkHandle(fx, 2, idx) : 0);
} END;
H("ID3DXEffect::GetPass")
{
	FxInfo* fx = F(o);
	uint32_t ti;
	if (!fx) { SET(0x7e02); return; }
	const FxTech* t = techOf(o, ARG(1), &ti);
	SET(t && ARG(2) < t->passes.size() ? mkHandle(fx, 3, ti, ARG(2)) : 0);
} END;
H("ID3DXEffect::GetPassByName")
{
	FxInfo* fx = F(o);
	uint32_t ti;
	if (!fx) { SET(0x7e02); return; }
	const FxTech* t = techOf(o, ARG(1), &ti);
	if (t && ARG(2))
	{
		std::string n = gstr(ARG(2));
		for (size_t i = 0; i < t->passes.size(); i++) if (t->passes[i].name == n) { SET(mkHandle(fx, 3, ti, (uint32_t)i)); return; }
	}
	SET(0);
} END;
H("ID3DXEffect::GetAnnotation")
{
	int kind; uint32_t a, b;
	const std::vector<FxAnn>* l = annListOf(o, ARG(1), &kind, &a, &b);
	if (!l || ARG(2) >= l->size()) { SET(0); return; }
	SET(kind == 6 ? mkHandle(F(o), kind, a, b, ARG(2)) : mkHandle(F(o), kind, a, ARG(2)));
} END;
H("ID3DXEffect::GetAnnotationByName")
{
	int kind; uint32_t a, b;
	const std::vector<FxAnn>* l = annListOf(o, ARG(1), &kind, &a, &b);
	if (!l || !ARG(2)) { SET(0); return; }
	std::string n = gstr(ARG(2));
	{ static int cn = 0; if (cn++ < 60) { std::string all; for (auto& a : *l) all += a.name + ","; log("fx: GetAnnotationByName '%s' among [%s]", n.c_str(), all.c_str()); } }
	for (size_t i = 0; i < l->size(); i++)
		if ((*l)[i].name == n)
		{
			SET(kind == 6 ? mkHandle(F(o), kind, a, b, (uint32_t)i) : mkHandle(F(o), kind, a, (uint32_t)i));
			return;
		}
	SET(0);
} END;
H("ID3DXEffect::GetString")
{
	const FxAnn* an = annOf(ARG(1));
	std::string sv = an ? an->str : "";
	if (!an) { PRef pr = refOf(o, ARG(1)); if (pr.valid()) sv = pr.p->str; }
	{ static int n = 0; if (n++ < 80) log("fx: GetString h %x -> %s '%s'", ARG(1), an ? "ann" : "param", sv.c_str()); }
	if (ARG(2)) guest::store<uint32_t>(ARG(2), newStr(sv));
	SET(0);
} END;
H("ID3DXEffect::SetTechnique") { uint32_t idx; if (F(o) && techOf(o, ARG(1), &idx)) F(o)->current = idx; SET(0); } END;
H("ID3DXEffect::GetCurrentTechnique") { FxInfo* fx = F(o); SET(fx && fx->current < fx->techs.size() ? mkHandle(fx, 2, fx->current) : 0x7e01); } END;
H("ID3DXEffect::ValidateTechnique") { SET(0); } END;
H("ID3DXEffect::FindNextValidTechnique")
{
	FxInfo* fx = F(o);
	uint32_t idx = 0, next = 0;
	if (fx && ARG(1) && techOf(o, ARG(1), &idx)) next = idx + 1;
	if (ARG(2)) guest::store<uint32_t>(ARG(2), fx ? (next < fx->techs.size() ? mkHandle(fx, 2, next) : 0) : 0x7e01);
	SET(0);
} END;
H("ID3DXEffect::IsParameterUsed") { SET(1); } END;

}   // anonymous
}   // namespace d3d

namespace d3d
{
int gFxDiagCur = -1;
uint32_t gFxDiagTech = 0;
uint32_t gFxDiagBlob = 0;
bool fxStatOn() { static const bool on = access("/sdcard/ra3_fxstat", F_OK) == 0; return on; }
}
