// Direct3D 9 layer interface (d3d9*.cpp).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "d3d9_sh.h"
#include "rt.h"

namespace d3d
{
enum Iface { IDirect3D9, IDevice, ITexture, ICube, IVolume, ISurface, IVB, IIB, IVDecl, IVS, IPS, IQuery, ISB, ISwap, IBuffer, IEffect, INumIfaces };

struct FxInfo;
struct ShaderObj
{
	std::vector<uint8_t> code;
	ShaderInfo info;
	uint64_t hash = 0;
	bool ok = false;
	uint32_t glVs = 0, glPs = 0;   // compiled GL shader objects (renderer-owned)
};
ShaderObj* getShader(const uint8_t* code, size_t n);   // cached by content, translated on first use

struct Obj
{
	uint32_t addr = 0;     // guest address of the COM object
	Iface iface = IDevice;
	int ref = 1;
	uint32_t w = 0, h = 0, d = 1, levels = 1, fmt = 0, usage = 0, pool = 0, size = 0, fvf = 0, type = 0;
	uint32_t data = 0;                // guest storage (all mip levels / faces / buffer bytes)
	std::vector<uint32_t> levelOff;   // byte offset of each level (per face, faces follow each other)
	uint32_t faces = 1;
	uint32_t parent = 0;              // surface -> owning texture object address
	uint32_t level = 0, face = 0;
	uint32_t pitch = 0;               // surface pitch
	bool ownsData = true;
	std::vector<uint8_t> blob;        // declarations, effect source...
	std::shared_ptr<FxInfo> fx;       // parsed effect (IEffect)
	ShaderObj* sh = nullptr;          // IVS / IPS
	// renderer state (GL names are owned by d3d9_gl.cpp)
	uint32_t glTex = 0, glBuf = 0, glFbo = 0, glDepth = 0, glFmt = 0;
	uint32_t ver = 1, glVer = 0;      // content version / uploaded version
	bool rtTex = false;               // contents live on the GPU (render target)
};

using Handler = void (*)(CPU*, Obj*);
struct Reg { Reg(const char* name, Handler h); };
Obj* objectAt(uint32_t addr);
Obj* newObj(Iface i);
void allocLevels(Obj* o, uint32_t faces);

// ---- format helpers (d3d9.cpp)
bool isBlock(uint32_t f);
uint32_t blockBytes(uint32_t f);
uint32_t bpp(uint32_t f);
uint32_t levelPitch(uint32_t f, uint32_t w);
uint32_t levelBytes(uint32_t f, uint32_t w, uint32_t h);
Obj* makeSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool);
void writeOut(uint32_t pp, Obj* o);

// ---- device state
struct Stream { Obj* vb = nullptr; uint32_t offset = 0, stride = 0, freq = 1; };
struct DevState
{
	uint32_t rs[256];
	uint32_t samp[16][16];       // [stage][D3DSAMP_*]
	Obj* tex[16] = {};
	Stream streams[16];
	Obj* ib = nullptr;
	Obj* vdecl = nullptr;
	uint32_t fvf = 0;
	ShaderObj* vs = nullptr;
	ShaderObj* ps = nullptr;
	float vsF[256 * 4], psF[224 * 4];
	int32_t vsI[16 * 4], psI[16 * 4];
	int32_t vsB[16], psB[16];
	Obj* rt0 = nullptr;          // null: back buffer
	Obj* ds = nullptr;
	uint32_t vp[4] = {0, 0, 0, 0};
	float vpZ[2] = {0.f, 1.f};
	int32_t scissor[4] = {0, 0, 0, 0};
	DevState();
};
extern DevState dev;
extern Obj* gBackBuffer;
extern uint32_t backW, backH;

// ---- effect application (d3d9_fx.cpp)


// ---- GL renderer (d3d9_gl.cpp). All functions are no-ops when no GL context is available.
struct DrawCall
{
	uint32_t prim = 4, primCount = 0;
	bool indexed = false;
	int32_t baseVertex = 0;
	uint32_t minIndex = 0, numVertices = 0, startIndex = 0;
	const void* upVerts = nullptr;   // DrawPrimitiveUP
	uint32_t upStride = 0;
	const void* upIdx = nullptr;
	uint32_t upIdxFmt = 101;
};
namespace glr
{
bool init();
bool ready();
void clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
void draw(const DrawCall& dc);
void present();
void stretchRect(Obj* src, const int32_t* srcRect, Obj* dst, const int32_t* dstRect, uint32_t filter);
void destroy(Obj* o);
}
}
