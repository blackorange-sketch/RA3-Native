// Direct3D 9 layer interface (d3d9.cpp).
#pragma once
#include <memory>
#include <vector>

#include "rt.h"

namespace d3d
{
enum Iface { IDirect3D9, IDevice, ITexture, ICube, IVolume, ISurface, IVB, IIB, IVDecl, IVS, IPS, IQuery, ISB, ISwap, IBuffer, IEffect, INumIfaces };

struct FxInfo;
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
	std::vector<uint8_t> blob;        // shader byte code, declarations, effect source...
	std::shared_ptr<FxInfo> fx;       // parsed effect (IEffect)
};

using Handler = void (*)(CPU*, Obj*);
Obj* objectAt(uint32_t addr);
Obj* newObj(Iface i);
void allocLevels(Obj* o, uint32_t faces);
}
