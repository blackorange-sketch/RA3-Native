// Direct3D 9 / D3DX9 layer. Guest COM objects are small guest-memory blocks {vtable, refcount, id}; every
// method is a dynamically created import stub that lands in comDispatch(). This first version is a "null"
// device: resources have real storage (Lock/Unlock work) but nothing is drawn yet.
#include "d3d9.h"

#include <sys/stat.h>

#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace rt;

namespace d3d
{
namespace
{
struct Method { const char* name; int nargs; };
struct IfaceDef { const char* name; std::vector<Method> methods; uint32_t vtable = 0; };

std::vector<Method> unknown() { return {{"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}}; }
std::vector<Method> cat(std::vector<Method> a, const std::vector<Method>& b) { a.insert(a.end(), b.begin(), b.end()); return a; }
std::vector<Method> resourceM() { return cat(unknown(), {{"GetDevice", 1}, {"SetPrivateData", 4}, {"GetPrivateData", 3}, {"FreePrivateData", 1}, {"SetPriority", 1}, {"GetPriority", 0}, {"PreLoad", 0}, {"GetType", 0}}); }
std::vector<Method> baseTexM() { return cat(resourceM(), {{"SetLOD", 1}, {"GetLOD", 0}, {"GetLevelCount", 0}, {"SetAutoGenFilterType", 1}, {"GetAutoGenFilterType", 0}, {"GenerateMipSubLevels", 0}}); }

IfaceDef defs[INumIfaces];
std::map<std::string, Handler>& handlers() { static std::map<std::string, Handler> h; return h; }
struct MethodInfo { Handler h; };
std::vector<MethodInfo> methodInfos;
std::mutex objMu;
std::unordered_map<uint32_t, Obj*> objs;
bool defsReady = false;

struct Reg { Reg(const char* name, Handler h) { handlers()[name] = h; } };
#define H(name) static Reg CAT(reg_, __LINE__)(name, [](CPU* c, Obj* o) -> void
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define END )

void initDefs()
{
	if (defsReady) return;
	defsReady = true;
	defs[IDirect3D9] = {"IDirect3D9", cat(unknown(), {{"RegisterSoftwareDevice", 1}, {"GetAdapterCount", 0}, {"GetAdapterIdentifier", 3}, {"GetAdapterModeCount", 2}, {"EnumAdapterModes", 4}, {"GetAdapterDisplayMode", 2}, {"CheckDeviceType", 5}, {"CheckDeviceFormat", 6}, {"CheckDeviceMultiSampleType", 6}, {"CheckDepthStencilMatch", 5}, {"CheckDeviceFormatConversion", 4}, {"GetDeviceCaps", 3}, {"GetAdapterMonitor", 1}, {"CreateDevice", 6}})};
	defs[IDevice] = {"IDirect3DDevice9", cat(unknown(), {
		{"TestCooperativeLevel", 0}, {"GetAvailableTextureMem", 0}, {"EvictManagedResources", 0}, {"GetDirect3D", 1}, {"GetDeviceCaps", 1}, {"GetDisplayMode", 2}, {"GetCreationParameters", 1},
		{"SetCursorProperties", 3}, {"SetCursorPosition", 3}, {"ShowCursor", 1}, {"CreateAdditionalSwapChain", 2}, {"GetSwapChain", 2}, {"GetNumberOfSwapChains", 0}, {"Reset", 1}, {"Present", 4},
		{"GetBackBuffer", 4}, {"GetRasterStatus", 2}, {"SetDialogBoxMode", 1}, {"SetGammaRamp", 3}, {"GetGammaRamp", 2}, {"CreateTexture", 8}, {"CreateVolumeTexture", 9}, {"CreateCubeTexture", 7},
		{"CreateVertexBuffer", 6}, {"CreateIndexBuffer", 6}, {"CreateRenderTarget", 8}, {"CreateDepthStencilSurface", 8}, {"UpdateSurface", 4}, {"UpdateTexture", 2}, {"GetRenderTargetData", 2},
		{"GetFrontBufferData", 2}, {"StretchRect", 5}, {"ColorFill", 3}, {"CreateOffscreenPlainSurface", 6}, {"SetRenderTarget", 2}, {"GetRenderTarget", 2}, {"SetDepthStencilSurface", 1},
		{"GetDepthStencilSurface", 1}, {"BeginScene", 0}, {"EndScene", 0}, {"Clear", 6}, {"SetTransform", 2}, {"GetTransform", 2}, {"MultiplyTransform", 2}, {"SetViewport", 1}, {"GetViewport", 1},
		{"SetMaterial", 1}, {"GetMaterial", 1}, {"SetLight", 2}, {"GetLight", 2}, {"LightEnable", 2}, {"GetLightEnable", 2}, {"SetClipPlane", 2}, {"GetClipPlane", 2}, {"SetRenderState", 2},
		{"GetRenderState", 2}, {"CreateStateBlock", 2}, {"BeginStateBlock", 0}, {"EndStateBlock", 1}, {"SetClipStatus", 1}, {"GetClipStatus", 1}, {"GetTexture", 2}, {"SetTexture", 2},
		{"GetTextureStageState", 3}, {"SetTextureStageState", 3}, {"GetSamplerState", 3}, {"SetSamplerState", 3}, {"ValidateDevice", 1}, {"SetPaletteEntries", 2}, {"GetPaletteEntries", 2},
		{"SetCurrentTexturePalette", 1}, {"GetCurrentTexturePalette", 1}, {"SetScissorRect", 1}, {"GetScissorRect", 1}, {"SetSoftwareVertexProcessing", 1}, {"GetSoftwareVertexProcessing", 0},
		{"SetNPatchMode", 1}, {"GetNPatchMode", 0}, {"DrawPrimitive", 3}, {"DrawIndexedPrimitive", 6}, {"DrawPrimitiveUP", 4}, {"DrawIndexedPrimitiveUP", 8}, {"ProcessVertices", 5},
		{"CreateVertexDeclaration", 2}, {"SetVertexDeclaration", 1}, {"GetVertexDeclaration", 1}, {"SetFVF", 1}, {"GetFVF", 1}, {"CreateVertexShader", 2}, {"SetVertexShader", 1}, {"GetVertexShader", 1},
		{"SetVertexShaderConstantF", 3}, {"GetVertexShaderConstantF", 3}, {"SetVertexShaderConstantI", 3}, {"GetVertexShaderConstantI", 3}, {"SetVertexShaderConstantB", 3}, {"GetVertexShaderConstantB", 3},
		{"SetStreamSource", 4}, {"GetStreamSource", 4}, {"SetStreamSourceFreq", 2}, {"GetStreamSourceFreq", 2}, {"SetIndices", 1}, {"GetIndices", 1}, {"CreatePixelShader", 2}, {"SetPixelShader", 1},
		{"GetPixelShader", 1}, {"SetPixelShaderConstantF", 3}, {"GetPixelShaderConstantF", 3}, {"SetPixelShaderConstantI", 3}, {"GetPixelShaderConstantI", 3}, {"SetPixelShaderConstantB", 3},
		{"GetPixelShaderConstantB", 3}, {"DrawRectPatch", 3}, {"DrawTriPatch", 3}, {"DeletePatch", 1}, {"CreateQuery", 2}})};
	defs[ITexture] = {"IDirect3DTexture9", cat(baseTexM(), {{"GetLevelDesc", 2}, {"GetSurfaceLevel", 2}, {"LockRect", 4}, {"UnlockRect", 1}, {"AddDirtyRect", 1}})};
	defs[ICube] = {"IDirect3DCubeTexture9", cat(baseTexM(), {{"GetLevelDesc", 2}, {"GetCubeMapSurface", 3}, {"LockRect", 5}, {"UnlockRect", 2}, {"AddDirtyRect", 2}})};
	defs[IVolume] = {"IDirect3DVolumeTexture9", cat(baseTexM(), {{"GetLevelDesc", 2}, {"GetVolumeLevel", 2}, {"LockBox", 4}, {"UnlockBox", 1}, {"AddDirtyBox", 1}})};
	defs[ISurface] = {"IDirect3DSurface9", cat(resourceM(), {{"GetContainer", 2}, {"GetDesc", 1}, {"LockRect", 3}, {"UnlockRect", 0}, {"GetDC", 1}, {"ReleaseDC", 1}})};
	defs[IVB] = {"IDirect3DVertexBuffer9", cat(resourceM(), {{"Lock", 4}, {"Unlock", 0}, {"GetDesc", 1}})};
	defs[IIB] = {"IDirect3DIndexBuffer9", cat(resourceM(), {{"Lock", 4}, {"Unlock", 0}, {"GetDesc", 1}})};
	defs[IVDecl] = {"IDirect3DVertexDeclaration9", cat(unknown(), {{"GetDevice", 1}, {"GetDeclaration", 2}})};
	defs[IVS] = {"IDirect3DVertexShader9", cat(unknown(), {{"GetDevice", 1}, {"GetFunction", 2}})};
	defs[IPS] = {"IDirect3DPixelShader9", cat(unknown(), {{"GetDevice", 1}, {"GetFunction", 2}})};
	defs[IQuery] = {"IDirect3DQuery9", cat(unknown(), {{"GetDevice", 1}, {"GetType", 0}, {"GetDataSize", 0}, {"Issue", 1}, {"GetData", 3}})};
	defs[ISB] = {"IDirect3DStateBlock9", cat(unknown(), {{"GetDevice", 1}, {"Capture", 0}, {"Apply", 0}})};
	defs[ISwap] = {"IDirect3DSwapChain9", cat(unknown(), {{"Present", 5}, {"GetFrontBufferData", 1}, {"GetBackBuffer", 3}, {"GetRasterStatus", 1}, {"GetDisplayMode", 1}, {"GetDevice", 1}, {"GetPresentParameters", 1}})};
	defs[IBuffer] = {"ID3DXBuffer", cat(unknown(), {{"GetBufferPointer", 0}, {"GetBufferSize", 0}})};
	defs[IEffect] = {"ID3DXEffect", cat(unknown(), {
		{"GetDesc", 1}, {"GetParameterDesc", 2}, {"GetTechniqueDesc", 2}, {"GetPassDesc", 2}, {"GetFunctionDesc", 2}, {"GetParameter", 2}, {"GetParameterByName", 2}, {"GetParameterBySemantic", 2},
		{"GetParameterElement", 2}, {"GetTechnique", 1}, {"GetTechniqueByName", 1}, {"GetPass", 2}, {"GetPassByName", 2}, {"GetFunction", 1}, {"GetFunctionByName", 1}, {"GetAnnotation", 2},
		{"GetAnnotationByName", 2}, {"SetValue", 3}, {"GetValue", 3}, {"SetBool", 2}, {"GetBool", 2}, {"SetBoolArray", 3}, {"GetBoolArray", 3}, {"SetInt", 2}, {"GetInt", 2}, {"SetIntArray", 3},
		{"GetIntArray", 3}, {"SetFloat", 2}, {"GetFloat", 2}, {"SetFloatArray", 3}, {"GetFloatArray", 3}, {"SetVector", 2}, {"GetVector", 2}, {"SetVectorArray", 3}, {"GetVectorArray", 3},
		{"SetMatrix", 2}, {"GetMatrix", 2}, {"SetMatrixArray", 3}, {"GetMatrixArray", 3}, {"SetMatrixPointerArray", 3}, {"GetMatrixPointerArray", 3}, {"SetMatrixTranspose", 2},
		{"GetMatrixTranspose", 2}, {"SetMatrixTransposeArray", 3}, {"GetMatrixTransposeArray", 3}, {"SetMatrixTransposePointerArray", 3}, {"GetMatrixTransposePointerArray", 3},
		{"SetString", 2}, {"GetString", 2}, {"SetTexture", 2}, {"GetTexture", 2}, {"GetPixelShader", 2}, {"GetVertexShader", 2}, {"SetArrayRange", 3},
		{"GetPool", 1}, {"SetTechnique", 1}, {"GetCurrentTechnique", 0}, {"ValidateTechnique", 1}, {"FindNextValidTechnique", 2}, {"IsParameterUsed", 2}, {"Begin", 2}, {"BeginPass", 1},
		{"CommitChanges", 0}, {"EndPass", 0}, {"End", 0}, {"GetDevice", 1}, {"OnLostDevice", 0}, {"OnResetDevice", 0}, {"SetStateManager", 1}, {"GetStateManager", 1},
		{"BeginParameterBlock", 0}, {"EndParameterBlock", 0}, {"ApplyParameterBlock", 1}, {"DeleteParameterBlock", 1}, {"CloneEffect", 2}, {"SetRawValue", 4}})};
}

void comDispatch(CPU* c)
{
	Handler h = methodInfos[imports::current->user].h;
	if (!h) { SET(0); return; }
	h(c, objectAt(ARG(0)));
}

void buildVtable(IfaceDef& d)
{
	if (d.vtable) return;
	uint32_t vt = gmalloc(4 * (uint32_t)d.methods.size());
	for (size_t i = 0; i < d.methods.size(); i++)
	{
		std::string full = std::string(d.name) + "::" + d.methods[i].name;
		auto it = handlers().find(full);
		Handler h = it != handlers().end() ? it->second : nullptr;
		if (!h)
		{
			std::string m = d.methods[i].name;
			if (m == "AddRef" || m == "Release" || m == "QueryInterface" || m == "GetDevice")
			{
				auto g = handlers().find("Common::" + m);
				if (g != handlers().end()) h = g->second;
			}
		}
		methodInfos.push_back({h});
		uint32_t stub = imports::addDynamic("d3d9.dll", full, comDispatch, 4 * (d.methods[i].nargs + 1), (uint32_t)methodInfos.size() - 1);
		guest::store<uint32_t>(vt + 4 * (uint32_t)i, stub);
	}
	d.vtable = vt;
}

Obj* theDevice = nullptr;
Obj* theD3D = nullptr;
uint32_t backW = 1280, backH = 720;
}

Obj* objectAt(uint32_t addr)
{
	std::lock_guard<std::mutex> l(objMu);
	auto it = objs.find(addr);
	return it == objs.end() ? nullptr : it->second;
}

Obj* newObj(Iface i)
{
	initDefs();
	buildVtable(defs[i]);
	auto* o = new Obj;
	o->iface = i;
	o->addr = gmalloc(16);
	guest::store<uint32_t>(o->addr, defs[i].vtable);
	guest::store<uint32_t>(o->addr + 4, 1);
	guest::store<uint32_t>(o->addr + 8, i);
	std::lock_guard<std::mutex> l(objMu);
	objs[o->addr] = o;
	return o;
}

// ---- formats
static bool isBlock(uint32_t f) { return f == 0x31545844 || f == 0x32545844 || f == 0x33545844 || f == 0x34545844 || f == 0x35545844; }
static uint32_t blockBytes(uint32_t f) { return f == 0x31545844 ? 8 : 16; }
static uint32_t bpp(uint32_t f)
{
	switch (f)
	{
	case 20: return 3;
	case 21: case 22: case 31: case 32: case 35: case 112: case 114: case 71: case 75: case 77: case 102: case 63: case 17: return 4;
	case 23: case 24: case 25: case 26: case 51: case 80: case 81: case 82: case 101: case 111: return 2;
	case 28: case 50: case 41: case 30: case 29: return 1;
	case 113: case 36: case 34: return 8;
	case 116: return 16;
	default: return 4;
	}
}
static uint32_t levelPitch(uint32_t f, uint32_t w) { return isBlock(f) ? std::max(1u, (w + 3) / 4) * blockBytes(f) : w * bpp(f); }
static uint32_t levelBytes(uint32_t f, uint32_t w, uint32_t h) { return isBlock(f) ? std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * blockBytes(f) : levelPitch(f, w) * h; }

void allocLevels(Obj* o, uint32_t faces)
{
	o->faces = faces;
	uint32_t total = 0;
	o->levelOff.clear();
	for (uint32_t f = 0; f < faces; f++)
		for (uint32_t l = 0; l < o->levels; l++)
		{
			o->levelOff.push_back(total);
			total += levelBytes(o->fmt, std::max(1u, o->w >> l), std::max(1u, o->h >> l)) * std::max(1u, o->d >> l);
		}
	o->size = total;
	o->data = gmalloc(total ? total : 16);
	memset(guest::host(o->data), 0, total);
}

static void writeOut(uint32_t pp, Obj* o) { if (pp) guest::store<uint32_t>(pp, o->addr); }

static Obj* makeSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool)
{
	Obj* s = newObj(ISurface);
	s->w = w; s->h = h; s->fmt = fmt; s->usage = usage; s->pool = pool; s->levels = 1;
	s->pitch = levelPitch(fmt, w);
	s->size = levelBytes(fmt, w, h);
	s->data = gmalloc(s->size ? s->size : 16);
	memset(guest::host(s->data), 0, s->size);
	return s;
}

static Obj* surfaceOfLevel(Obj* tex, uint32_t level, uint32_t face)
{
	Obj* s = newObj(ISurface);
	uint32_t idx = face * tex->levels + level;
	s->w = std::max(1u, tex->w >> level); s->h = std::max(1u, tex->h >> level); s->fmt = tex->fmt; s->usage = tex->usage; s->pool = tex->pool;
	s->pitch = levelPitch(tex->fmt, s->w);
	s->data = tex->data + tex->levelOff[idx];
	s->size = levelBytes(tex->fmt, s->w, s->h);
	s->ownsData = false;
	s->parent = tex->addr;
	s->level = level; s->face = face;
	guest::store<uint32_t>(tex->addr + 4, guest::load<uint32_t>(tex->addr + 4));   // keep texture alive while surfaces exist
	return s;
}

static void fillDesc(uint32_t pDesc, Obj* s)   // D3DSURFACE_DESC
{
	guest::store<uint32_t>(pDesc + 0, s->fmt);
	guest::store<uint32_t>(pDesc + 4, 1);               // D3DRTYPE_SURFACE
	guest::store<uint32_t>(pDesc + 8, s->usage);
	guest::store<uint32_t>(pDesc + 12, s->pool);
	guest::store<uint32_t>(pDesc + 16, 0);              // MultiSampleType
	guest::store<uint32_t>(pDesc + 20, 0);
	guest::store<uint32_t>(pDesc + 24, s->w);
	guest::store<uint32_t>(pDesc + 28, s->h);
}

// ---- IUnknown-ish
H("Common::AddRef") { o->ref++; SET(o->ref); } END;
H("Common::Release")
{
	if (!o) { SET(0); return; }
	int r = --o->ref;
	if (r <= 0 && o->ownsData && o->data && o->iface != IDevice) { /* storage kept: the game may still hold pointers from Lock() */ }
	SET(r < 0 ? 0 : r);
} END;
H("Common::QueryInterface") { if (ARG(2)) guest::store<uint32_t>(ARG(2), 0); SET(0x80004002); } END;
H("Common::GetDevice") { if (ARG(1) && theDevice) { guest::store<uint32_t>(ARG(1), theDevice->addr); theDevice->ref++; } SET(0); } END;

// ---- IDirect3D9
H("IDirect3D9::GetAdapterCount") { SET(1); } END;
H("IDirect3D9::GetAdapterIdentifier")
{
	uint32_t p = ARG(3);
	memset(guest::host(p), 0, 1104);
	putStr(p, "nvd3dum.dll");
	putStr(p + 512, "Android GPU (RA3 native D3D9 layer)");
	putStr(p + 1024, "\\\\.\\DISPLAY1");
	guest::store<uint64_t>(p + 1056, 0x0009000a00000000ull | 0x1000);
	guest::store<uint32_t>(p + 1064, 0x10DE);
	guest::store<uint32_t>(p + 1068, 0x0640);
	guest::store<uint32_t>(p + 1072, 0);
	guest::store<uint32_t>(p + 1076, 1);
	guest::store<uint32_t>(p + 1096, 1);
	SET(0);
} END;
H("IDirect3D9::GetAdapterModeCount") { SET(3); } END;
static void fillMode(uint32_t p, uint32_t idx)
{
	static const uint32_t res[3][2] = {{1024, 768}, {1280, 720}, {1920, 1080}};
	guest::store<uint32_t>(p, res[idx % 3][0]);
	guest::store<uint32_t>(p + 4, res[idx % 3][1]);
	guest::store<uint32_t>(p + 8, 60);
	guest::store<uint32_t>(p + 12, 22);   // X8R8G8B8
}
H("IDirect3D9::EnumAdapterModes") { if (ARG(3) >= 3) { SET(0x8876086c); return; } fillMode(ARG(4), ARG(3)); SET(0); } END;
H("IDirect3D9::GetAdapterDisplayMode") { fillMode(ARG(2), 1); SET(0); } END;
H("IDirect3D9::CheckDeviceType") { SET(0); } END;
H("IDirect3D9::CheckDeviceFormat") { SET(0); } END;
H("IDirect3D9::CheckDeviceMultiSampleType") { if (ARG(5) != 0 && ARG(5) != 1) { SET(0x8876086a); return; } if (ARG(7)) guest::store<uint32_t>(ARG(7), 1); SET(0); } END;
H("IDirect3D9::CheckDepthStencilMatch") { SET(0); } END;
H("IDirect3D9::CheckDeviceFormatConversion") { SET(0); } END;
static void fillCaps(uint32_t p)
{
	memset(guest::host(p), 0, 304);
	auto W = [&](uint32_t off, uint32_t v) { guest::store<uint32_t>(p + off, v); };
	auto F = [&](uint32_t off, float v) { guest::store<float>(p + off, v); };
	W(0, 1); W(8, 0x00020000 | 0x80000); W(12, 0x20000000 | 0x10000000 | 0x00000800); W(16, 0x120 | 0x80); W(20, 0x1 | 0x80000000);
	W(28, 0x000000e0 | 0x10 | 0x20000 | 0x40000 | 0x100000 | 0x400000 | 0x800000 | 0x1000 | 0x2000 | 0x8000 | 0x80000 | 0x40);
	W(32, 0xffffffff & ~0x80000000u); W(36, 0xffffffff & ~0x80000000u); W(40, 0xff); W(44, 0x3fff); W(48, 0x3fff); W(52, 0xff); W(56, 0x38001b);
	W(60, 0xffffffff & ~0x80000000u); W(64, 0x03030303 | 0x07000000 | 0x0f000000); W(68, 0x0f000303 | 0x07000000); W(72, 0x07000303 | 0x07000000); W(76, 0x1f); W(80, 0x1f); W(84, 0x1f);
	W(88, 4096); W(92, 4096); W(96, 2048); W(100, 8192); W(104, 8192); W(108, 16);
	F(112, 1e10f); F(116, -1e8f); F(120, -1e8f); F(124, 1e8f); F(128, 1e8f);
	W(136, 0xff); W(140, 0x10008); W(144, 0x03ffffff); W(148, 8); W(152, 8); W(156, 0x1ff); W(160, 8); W(164, 6); W(168, 4); W(172, 255);
	F(176, 8192.0f); W(180, 0x00ffffff); W(184, 0x00ffffff); W(188, 16); W(192, 508);
	W(196, 0xfffe0300); W(200, 256); W(204, 0xffff0300); F(208, 65504.0f); W(212, 0x3); F(216, 1.0f);
	W(224, 0); W(232, 1); W(236, 0x3ff); W(240, 4); W(244, 0x0f000000 | 0x07000000);
	W(248, 0xf); W(252, 24); W(256, 32); W(260, 4);
	W(264, 0x1f); W(268, 24); W(272, 32); W(276, 4); W(280, 512);
	W(284, 0x03030303); W(288, 65535); W(292, 65535); W(296, 32768); W(300, 32768);
}
H("IDirect3D9::GetDeviceCaps") { fillCaps(ARG(3)); SET(0); } END;
H("IDirect3D9::GetAdapterMonitor") { SET(0x10001); } END;
H("IDirect3D9::CreateDevice")
{
	uint32_t pp = ARG(5);
	uint32_t w = guest::load<uint32_t>(pp), h = guest::load<uint32_t>(pp + 4);
	if (!w || !h) { w = backW; h = backH; guest::store<uint32_t>(pp, w); guest::store<uint32_t>(pp + 4, h); }
	backW = w; backH = h;
	log("D3D9 CreateDevice: %ux%u fmt %u windowed %u depth %u", w, h, guest::load<uint32_t>(pp + 8), guest::load<uint32_t>(pp + 32), guest::load<uint32_t>(pp + 40));
	theDevice = newObj(IDevice);
	writeOut(ARG(6), theDevice);
	SET(0);
} END;

// ---- device
H("IDirect3DDevice9::TestCooperativeLevel") { SET(0); } END;
H("IDirect3DDevice9::GetAvailableTextureMem") { SET(0x20000000); } END;
H("IDirect3DDevice9::GetDirect3D") { if (theD3D) { guest::store<uint32_t>(ARG(1), theD3D->addr); theD3D->ref++; } SET(0); } END;
H("IDirect3DDevice9::GetDeviceCaps") { fillCaps(ARG(1)); SET(0); } END;
H("IDirect3DDevice9::GetDisplayMode") { fillMode(ARG(2), 1); SET(0); } END;
H("IDirect3DDevice9::GetCreationParameters") { uint32_t p = ARG(1); guest::store<uint32_t>(p, 0); guest::store<uint32_t>(p + 4, 1); guest::store<uint32_t>(p + 8, 0x30001); guest::store<uint32_t>(p + 12, 0x40); SET(0); } END;
H("IDirect3DDevice9::ShowCursor") { SET(0); } END;
H("IDirect3DDevice9::GetNumberOfSwapChains") { SET(1); } END;
H("IDirect3DDevice9::GetSwapChain") { Obj* s = newObj(ISwap); writeOut(ARG(2), s); SET(0); } END;
H("IDirect3DDevice9::Reset") { uint32_t pp = ARG(1); if (guest::load<uint32_t>(pp) && guest::load<uint32_t>(pp + 4)) { backW = guest::load<uint32_t>(pp); backH = guest::load<uint32_t>(pp + 4); } SET(0); } END;
H("IDirect3DDevice9::Present") { std::this_thread::sleep_for(std::chrono::milliseconds(16)); SET(0); } END;
H("IDirect3DDevice9::GetBackBuffer")
{
	static Obj* back = nullptr;
	if (!back) back = makeSurface(backW, backH, 22, 1, 0);
	back->ref++;
	writeOut(ARG(4), back);
	SET(0);
} END;
H("IDirect3DDevice9::CreateTexture")
{
	Obj* t = newObj(ITexture);
	t->w = ARG(1); t->h = ARG(2); t->levels = ARG(3); t->usage = ARG(4); t->fmt = ARG(5); t->pool = ARG(6);
	if (t->levels == 0) { uint32_t m = std::max(t->w, t->h); t->levels = 1; while (m >>= 1) t->levels++; }
	allocLevels(t, 1);
	writeOut(ARG(7), t);
	SET(0);
} END;
H("IDirect3DDevice9::CreateVolumeTexture")
{
	Obj* t = newObj(IVolume);
	t->w = ARG(1); t->h = ARG(2); t->d = ARG(3); t->levels = ARG(4) ? ARG(4) : 1; t->usage = ARG(5); t->fmt = ARG(6); t->pool = ARG(7);
	allocLevels(t, 1);
	writeOut(ARG(8), t);
	SET(0);
} END;
H("IDirect3DDevice9::CreateCubeTexture")
{
	Obj* t = newObj(ICube);
	t->w = t->h = ARG(1); t->levels = ARG(2); t->usage = ARG(3); t->fmt = ARG(4); t->pool = ARG(5);
	if (t->levels == 0) { uint32_t m = t->w; t->levels = 1; while (m >>= 1) t->levels++; }
	allocLevels(t, 6);
	writeOut(ARG(6), t);
	SET(0);
} END;
H("IDirect3DDevice9::CreateVertexBuffer")
{
	Obj* b = newObj(IVB);
	b->size = ARG(1); b->usage = ARG(2); b->fvf = ARG(3); b->pool = ARG(4);
	b->data = gmalloc(b->size ? b->size : 16);
	memset(guest::host(b->data), 0, b->size);
	writeOut(ARG(5), b);
	SET(0);
} END;
H("IDirect3DDevice9::CreateIndexBuffer")
{
	Obj* b = newObj(IIB);
	b->size = ARG(1); b->usage = ARG(2); b->fmt = ARG(3); b->pool = ARG(4);
	b->data = gmalloc(b->size ? b->size : 16);
	memset(guest::host(b->data), 0, b->size);
	writeOut(ARG(5), b);
	SET(0);
} END;
H("IDirect3DDevice9::CreateRenderTarget") { writeOut(ARG(7), makeSurface(ARG(1), ARG(2), ARG(3), 1, 0)); SET(0); } END;
H("IDirect3DDevice9::CreateDepthStencilSurface") { writeOut(ARG(7), makeSurface(ARG(1), ARG(2), ARG(3), 2, 0)); SET(0); } END;
H("IDirect3DDevice9::CreateOffscreenPlainSurface") { writeOut(ARG(5), makeSurface(ARG(1), ARG(2), ARG(3), 0, ARG(4))); SET(0); } END;
H("IDirect3DDevice9::GetRenderTarget") { static Obj* rt = nullptr; if (!rt) rt = makeSurface(backW, backH, 22, 1, 0); rt->ref++; writeOut(ARG(2), rt); SET(0); } END;
H("IDirect3DDevice9::GetDepthStencilSurface") { static Obj* ds = nullptr; if (!ds) ds = makeSurface(backW, backH, 75, 2, 0); ds->ref++; writeOut(ARG(1), ds); SET(0); } END;
H("IDirect3DDevice9::CreateVertexDeclaration")
{
	Obj* d = newObj(IVDecl);
	uint32_t p = ARG(1), n = 0;
	while (guest::load<uint16_t>(p + 8 * n) != 0xff) n++;   // D3DDECL_END: Stream == 0xFF
	d->blob.assign(guest::host(p), guest::host(p) + 8 * (n + 1));
	writeOut(ARG(2), d);
	SET(0);
} END;
H("IDirect3DDevice9::CreateVertexShader")
{
	Obj* s = newObj(IVS);
	uint32_t p = ARG(1), n = 0;
	while (guest::load<uint32_t>(p + 4 * n) != 0x0000ffff) n++;   // D3DSIO_END
	s->blob.assign(guest::host(p), guest::host(p) + 4 * (n + 1));
	writeOut(ARG(2), s);
	SET(0);
} END;
H("IDirect3DDevice9::CreatePixelShader")
{
	Obj* s = newObj(IPS);
	uint32_t p = ARG(1), n = 0;
	while (guest::load<uint32_t>(p + 4 * n) != 0x0000ffff) n++;
	s->blob.assign(guest::host(p), guest::host(p) + 4 * (n + 1));
	writeOut(ARG(2), s);
	SET(0);
} END;
H("IDirect3DDevice9::CreateQuery") { if (ARG(2)) { Obj* q = newObj(IQuery); q->type = ARG(1); writeOut(ARG(2), q); } SET(0); } END;
H("IDirect3DDevice9::CreateStateBlock") { writeOut(ARG(2), newObj(ISB)); SET(0); } END;
H("IDirect3DDevice9::EndStateBlock") { writeOut(ARG(1), newObj(ISB)); SET(0); } END;
H("IDirect3DDevice9::GetRenderState") { if (ARG(2)) guest::store<uint32_t>(ARG(2), 0); SET(0); } END;
H("IDirect3DDevice9::GetSamplerState") { if (ARG(3)) guest::store<uint32_t>(ARG(3), 0); SET(0); } END;
H("IDirect3DDevice9::GetTextureStageState") { if (ARG(3)) guest::store<uint32_t>(ARG(3), 0); SET(0); } END;
H("IDirect3DDevice9::ValidateDevice") { if (ARG(1)) guest::store<uint32_t>(ARG(1), 1); SET(0); } END;
H("IDirect3DDevice9::GetViewport") { uint32_t p = ARG(1); guest::store<uint32_t>(p, 0); guest::store<uint32_t>(p + 4, 0); guest::store<uint32_t>(p + 8, backW); guest::store<uint32_t>(p + 12, backH); guest::store<float>(p + 16, 0.f); guest::store<float>(p + 20, 1.f); SET(0); } END;
H("IDirect3DDevice9::SetStreamSourceFreq") { SET(0); } END;
H("IDirect3DDevice9::GetSoftwareVertexProcessing") { SET(0); } END;

// ---- resources: textures
H("IDirect3DTexture9::GetLevelCount") { SET(o->levels); } END;
H("IDirect3DTexture9::GetLevelDesc") { Obj tmp = *o; tmp.w = std::max(1u, o->w >> ARG(1)); tmp.h = std::max(1u, o->h >> ARG(1)); fillDesc(ARG(2), &tmp); SET(0); } END;
H("IDirect3DTexture9::GetSurfaceLevel") { writeOut(ARG(3), surfaceOfLevel(o, ARG(1), 0)); SET(0); } END;
H("IDirect3DTexture9::LockRect")
{
	uint32_t lvl = ARG(1), pLocked = ARG(2), pRect = ARG(3);
	uint32_t w = std::max(1u, o->w >> lvl);
	uint32_t pitch = levelPitch(o->fmt, w), base = o->data + o->levelOff[lvl];
	if (pRect)
	{
		uint32_t left = guest::load<uint32_t>(pRect), top = guest::load<uint32_t>(pRect + 4);
		base += isBlock(o->fmt) ? (top / 4) * pitch + (left / 4) * blockBytes(o->fmt) : top * pitch + left * bpp(o->fmt);
	}
	guest::store<uint32_t>(pLocked, pitch);
	guest::store<uint32_t>(pLocked + 4, base);
	SET(0);
} END;
H("IDirect3DTexture9::UnlockRect") { SET(0); } END;
H("IDirect3DCubeTexture9::GetLevelCount") { SET(o->levels); } END;
H("IDirect3DCubeTexture9::GetLevelDesc") { Obj tmp = *o; tmp.w = tmp.h = std::max(1u, o->w >> ARG(1)); fillDesc(ARG(2), &tmp); SET(0); } END;
H("IDirect3DCubeTexture9::GetCubeMapSurface") { writeOut(ARG(4), surfaceOfLevel(o, ARG(2), ARG(1))); SET(0); } END;
H("IDirect3DCubeTexture9::LockRect")
{
	uint32_t face = ARG(1), lvl = ARG(2), pLocked = ARG(3);
	guest::store<uint32_t>(pLocked, levelPitch(o->fmt, std::max(1u, o->w >> lvl)));
	guest::store<uint32_t>(pLocked + 4, o->data + o->levelOff[face * o->levels + lvl]);
	SET(0);
} END;
H("IDirect3DVolumeTexture9::GetLevelCount") { SET(o->levels); } END;
H("IDirect3DVolumeTexture9::LockBox")
{
	uint32_t lvl = ARG(1), pLocked = ARG(2);
	uint32_t w = std::max(1u, o->w >> lvl), h = std::max(1u, o->h >> lvl);
	guest::store<uint32_t>(pLocked, levelPitch(o->fmt, w));
	guest::store<uint32_t>(pLocked + 4, levelBytes(o->fmt, w, h));
	guest::store<uint32_t>(pLocked + 8, o->data + o->levelOff[lvl]);
	SET(0);
} END;

// ---- resources: surfaces and buffers
H("IDirect3DSurface9::GetDesc") { fillDesc(ARG(1), o); SET(0); } END;
H("IDirect3DSurface9::LockRect")
{
	uint32_t base = o->data;
	if (ARG(2))
	{
		uint32_t left = guest::load<uint32_t>(ARG(2)), top = guest::load<uint32_t>(ARG(2) + 4);
		base += isBlock(o->fmt) ? (top / 4) * o->pitch + (left / 4) * blockBytes(o->fmt) : top * o->pitch + left * bpp(o->fmt);
	}
	guest::store<uint32_t>(ARG(1), o->pitch);
	guest::store<uint32_t>(ARG(1) + 4, base);
	SET(0);
} END;
H("IDirect3DSurface9::UnlockRect") { SET(0); } END;
H("IDirect3DSurface9::GetContainer") { if (o->parent) { guest::store<uint32_t>(ARG(2), o->parent); if (Obj* p = objectAt(o->parent)) p->ref++; SET(0); } else SET(0x80004002); } END;
H("IDirect3DVertexBuffer9::Lock") { guest::store<uint32_t>(ARG(3), o->data + ARG(1)); SET(0); } END;
H("IDirect3DVertexBuffer9::GetDesc") { uint32_t p = ARG(1); guest::store<uint32_t>(p, 0); guest::store<uint32_t>(p + 4, 3); guest::store<uint32_t>(p + 8, o->usage); guest::store<uint32_t>(p + 12, o->pool); guest::store<uint32_t>(p + 16, o->size); guest::store<uint32_t>(p + 20, o->fvf); SET(0); } END;
H("IDirect3DIndexBuffer9::Lock") { guest::store<uint32_t>(ARG(3), o->data + ARG(1)); SET(0); } END;
H("IDirect3DIndexBuffer9::GetDesc") { uint32_t p = ARG(1); guest::store<uint32_t>(p, o->fmt); guest::store<uint32_t>(p + 4, 4); guest::store<uint32_t>(p + 8, o->usage); guest::store<uint32_t>(p + 12, o->pool); guest::store<uint32_t>(p + 16, o->size); SET(0); } END;
H("IDirect3DVertexDeclaration9::GetDeclaration")
{
	if (!ARG(1)) { guest::store<uint32_t>(ARG(2), (uint32_t)o->blob.size() / 8); SET(0); return; }
	memcpy(guest::host(ARG(1)), o->blob.data(), o->blob.size());
	SET(0);
} END;
H("IDirect3DQuery9::GetData")
{
	if (ARG(1) && ARG(2) >= 4) guest::store<uint32_t>(ARG(1), 0);   // occlusion count / event done
	SET(0);
} END;
H("IDirect3DQuery9::GetDataSize") { SET(4); } END;
H("IDirect3DSwapChain9::GetBackBuffer") { Obj* s = makeSurface(backW, backH, 22, 1, 0); writeOut(ARG(3), s); SET(0); } END;
H("ID3DXBuffer::GetBufferPointer") { SET(o->data); } END;
H("ID3DXBuffer::GetBufferSize") { SET(o->size); } END;

// ---- effects (parameters are opaque handles = guest strings)
H("ID3DXEffect::GetDesc") { uint32_t p = ARG(1); memset(guest::host(p), 0, 16); guest::store<uint32_t>(p + 8, 1); SET(0); } END;
H("ID3DXEffect::GetTechniqueDesc") { uint32_t p = ARG(2); memset(guest::host(p), 0, 12); guest::store<uint32_t>(p, newStr("T")); guest::store<uint32_t>(p + 4, 1); SET(0); } END;
H("ID3DXEffect::GetPassDesc") { uint32_t p = ARG(2); memset(guest::host(p), 0, 32); SET(0); } END;
H("ID3DXEffect::GetParameter") { SET(newStr("param")); } END;
H("ID3DXEffect::GetParameterByName") { SET(ARG(2) ? newStr(gstr(ARG(2))) : 0); } END;
H("ID3DXEffect::GetParameterBySemantic") { SET(ARG(2) ? newStr(gstr(ARG(2))) : 0); } END;
H("ID3DXEffect::GetParameterElement") { SET(newStr("elem")); } END;
H("ID3DXEffect::GetTechnique") { SET(0x7e01); } END;
H("ID3DXEffect::GetTechniqueByName") { SET(0x7e01); } END;
H("ID3DXEffect::GetPass") { SET(0x7e02); } END;
H("ID3DXEffect::GetPassByName") { SET(0x7e02); } END;
H("ID3DXEffect::GetCurrentTechnique") { SET(0x7e01); } END;
H("ID3DXEffect::Begin") { if (ARG(1)) guest::store<uint32_t>(ARG(1), 1); SET(0); } END;
H("ID3DXEffect::FindNextValidTechnique") { if (ARG(2)) guest::store<uint32_t>(ARG(2), 0x7e01); SET(0); } END;
H("ID3DXEffect::IsParameterUsed") { SET(1); } END;
H("ID3DXEffect::GetFloat") { if (ARG(2)) guest::store<float>(ARG(2), 0.f); SET(0); } END;

// ---- exports
}

using namespace d3d;

IMP_ID("d3d9.dll", "Direct3DCreate9", Direct3DCreate9, 4)
{
	d3d::theD3D = d3d::newObj(d3d::IDirect3D9);
	SET(d3d::theD3D->addr);
}

// ================= D3DX9
#define DX "d3dx9_35.dll"

static uint32_t ddsFormat(uint32_t hdr)
{
	uint32_t flags = guest::load<uint32_t>(hdr + 80), four = guest::load<uint32_t>(hdr + 84), bits = guest::load<uint32_t>(hdr + 88);
	uint32_t rm = guest::load<uint32_t>(hdr + 92), am = guest::load<uint32_t>(hdr + 104);
	if (flags & 4) return four;   // FOURCC (DXTn, or a D3DFMT number for float formats)
	if (bits == 32) return am ? 21 : 22;
	if (bits == 24) return 20;
	if (bits == 16) return rm == 0xf800 ? 23 : rm == 0x7c00 ? (am ? 25 : 24) : rm == 0x0f00 ? 26 : 51;
	if (bits == 8) return (flags & 2) ? 28 : 50;   // DDPF_ALPHA -> A8, else L8
	return 21;
}

struct Image { uint32_t w = 0, h = 0, d = 1, levels = 1, fmt = 0, type = 3, fileFmt = 4, dataOff = 0; bool dds = false, cube = false, vol = false; };
static Image parseImage(uint32_t p, uint32_t n)
{
	Image im;
	if (n >= 128 && guest::load<uint32_t>(p) == 0x20534444)   // "DDS "
	{
		im.dds = true;
		im.h = guest::load<uint32_t>(p + 12); im.w = guest::load<uint32_t>(p + 16);
		im.levels = std::max(1u, guest::load<uint32_t>(p + 28));
		uint32_t caps2 = guest::load<uint32_t>(p + 112), flags = guest::load<uint32_t>(p + 8);
		im.cube = (caps2 & 0x200) != 0;
		if (flags & 0x800000) { im.vol = true; im.d = guest::load<uint32_t>(p + 24); im.type = 4; }
		if (im.cube) im.type = 5;
		im.fmt = ddsFormat(p + 4);
		im.dataOff = 128;
	}
	else
	{
		uint32_t m = n >= 4 ? guest::load<uint32_t>(p) : 0;
		log("D3DX: non-DDS image (magic %08x, %u bytes) - substituting a placeholder", m, n);
		im.w = im.h = 4; im.fmt = 21; im.fileFmt = (m & 0xffff) == 0x4d42 ? 0 : m == 0x474e5089 ? 3 : (m & 0xffff) == 0xd8ff ? 1 : 2;
	}
	return im;
}
static void fillInfo(uint32_t p, const Image& im)
{
	if (!p) return;
	uint32_t v[7] = {im.w, im.h, im.d, im.levels, im.fmt, im.type, im.fileFmt};
	for (int i = 0; i < 7; i++) guest::store<uint32_t>(p + 4 * i, v[i]);
}
static void loadPayload(d3d::Obj* t, uint32_t src, uint32_t n, const Image& im)
{
	if (!im.dds) { for (uint32_t i = 0; i < t->size / 4; i++) guest::store<uint32_t>(t->data + 4 * i, 0xffff00ffu); return; }   // magenta placeholder
	uint32_t off = im.dataOff, avail = n - off;
	for (uint32_t f = 0; f < t->faces; f++)
		for (uint32_t l = 0; l < t->levels; l++)
		{
			uint32_t sz = levelBytes(t->fmt, std::max(1u, t->w >> l), std::max(1u, t->h >> l)) * std::max(1u, t->d >> l);
			if (off - im.dataOff + sz > avail) return;
			memcpy(guest::host(t->data + t->levelOff[f * t->levels + l]), guest::host(src + off), sz);
			off += sz;
		}
}
static void dxTexture(CPU* c, d3d::Iface kind, uint32_t srcIdx, uint32_t outIdx)
{
	uint32_t src = ARG(srcIdx), n = ARG(srcIdx + 1);
	Image im = parseImage(src, n);
	d3d::Obj* t = d3d::newObj(kind);
	t->w = im.w; t->h = im.h; t->d = im.d; t->fmt = im.fmt; t->levels = im.levels; t->pool = 1;
	d3d::allocLevels(t, kind == d3d::ICube ? 6 : 1);
	loadPayload(t, src, n, im);
	guest::store<uint32_t>(ARG(outIdx), t->addr);
	SET(0);
}
IMP_ID(DX, "D3DXCreateTextureFromFileInMemoryEx", D3DXCreateTextureFromFileInMemoryEx, 60) { dxTexture(c, d3d::ITexture, 1, 14); fillInfo(ARG(12), parseImage(ARG(1), ARG(2))); }
IMP_ID(DX, "D3DXCreateCubeTextureFromFileInMemoryEx", D3DXCreateCubeTextureFromFileInMemoryEx, 56) { dxTexture(c, d3d::ICube, 1, 13); fillInfo(ARG(11), parseImage(ARG(1), ARG(2))); }
IMP_ID(DX, "D3DXCreateVolumeTextureFromFileInMemoryEx", D3DXCreateVolumeTextureFromFileInMemoryEx, 64) { dxTexture(c, d3d::IVolume, 1, 15); fillInfo(ARG(13), parseImage(ARG(1), ARG(2))); }
IMP_ID(DX, "D3DXGetImageInfoFromFileInMemory", D3DXGetImageInfoFromFileInMemory, 12) { fillInfo(ARG(2), parseImage(ARG(0), ARG(1))); SET(0); }
IMP_ID(DX, "D3DXFilterTexture", D3DXFilterTexture, 16) { SET(0); }
IMP_ID(DX, "D3DXLoadSurfaceFromSurface", D3DXLoadSurfaceFromSurface, 32) { SET(0); }
IMP_ID(DX, "D3DXLoadSurfaceFromMemory", D3DXLoadSurfaceFromMemory, 40) { SET(0); }
IMP_ID(DX, "D3DXVec3CatmullRom", D3DXVec3CatmullRom, 24)
{
	float v[4][3];
	for (int k = 0; k < 4; k++) for (int i = 0; i < 3; i++) v[k][i] = guest::load<float>(ARG(1 + k) + 4 * i);
	float s = guest::load<float>(c->r[ESP] + 4 + 20), s2 = s * s, s3 = s2 * s;
	for (int i = 0; i < 3; i++)
		guest::store<float>(ARG(0) + 4 * i, 0.5f * (2.f * v[1][i] + (-v[0][i] + v[2][i]) * s + (2.f * v[0][i] - 5.f * v[1][i] + 4.f * v[2][i] - v[3][i]) * s2 + (-v[0][i] + 3.f * v[1][i] - 3.f * v[2][i] + v[3][i]) * s3));
	SET(ARG(0));
}
IMP_ID(DX, "D3DXCreateEffectEx", D3DXCreateEffectEx, 40)
{
	static int count;
	uint32_t src = ARG(1), n = ARG(2);
	std::string path = config.dataRoot + "/effects";
	mkdir(path.c_str(), 0777);
	path += "/effect" + std::to_string(count++) + ".bin";
	if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(guest::host(src), 1, n, f); fclose(f); }
	log("D3DXCreateEffectEx: %u bytes (first dword %08x) saved to %s", n, n >= 4 ? guest::load<uint32_t>(src) : 0, path.c_str());
	d3d::Obj* fx = d3d::newObj(d3d::IEffect);
	guest::store<uint32_t>(ARG(8), fx->addr);
	if (ARG(9)) guest::store<uint32_t>(ARG(9), 0);
	SET(0);
}
