// Direct3D 9 device: state tracking and draw calls. Drawing itself is done by the GL renderer (d3d9_gl.cpp).
#include <atomic>
#include <chrono>
#include <thread>

#include "d3d9_int.h"
#include "gfx.h"

using namespace rt;

namespace d3d
{
DevState dev;
Obj* gBackBuffer = nullptr;

DevState::DevState()
{
	memset(rs, 0, sizeof rs);
	memset(samp, 0, sizeof samp);
	memset(vsF, 0, sizeof vsF); memset(psF, 0, sizeof psF);
	memset(vsI, 0, sizeof vsI); memset(psI, 0, sizeof psI);
	memset(vsB, 0, sizeof vsB); memset(psB, 0, sizeof psB);
	memset(scissor, 0, sizeof scissor);
	rs[7] = 1; rs[14] = 1; rs[23] = 4; rs[22] = 3; rs[19] = 2; rs[20] = 1; rs[171] = 1; rs[168] = 15; rs[8] = 3; rs[9] = 2;
	rs[25] = 8; rs[56] = 8; rs[58] = 0xffffffff; rs[59] = 0xffffffff; rs[53] = rs[54] = rs[55] = 1; rs[207] = 2; rs[208] = 1; rs[209] = 1;
	rs[189] = 8; rs[186] = rs[187] = rs[188] = 1; rs[193] = 0xffffffff; rs[190] = rs[191] = rs[192] = 15;
	for (auto& s : samp) { s[1] = s[2] = s[3] = 1; s[5] = s[6] = 1; s[7] = 0; s[10] = 1; }
}

static const char* const kRsName(uint32_t) { return ""; }
static uint32_t stateIdx(uint32_t v) { return v & 255; }

H("IDirect3DDevice9::SetRenderState") { dev.rs[stateIdx(ARG(1))] = ARG(2); SET(0); } END;
H("IDirect3DDevice9::GetRenderState") { if (ARG(2)) guest::store<uint32_t>(ARG(2), dev.rs[stateIdx(ARG(1))]); SET(0); } END;
H("IDirect3DDevice9::SetSamplerState")
{
	uint32_t s = ARG(1), t = ARG(2);
	if (s < 16 && t < 16) dev.samp[s][t] = ARG(3);
	SET(0);
} END;
H("IDirect3DDevice9::GetSamplerState") { uint32_t s = ARG(1), t = ARG(2); if (ARG(3)) guest::store<uint32_t>(ARG(3), s < 16 && t < 16 ? dev.samp[s][t] : 0); SET(0); } END;
H("IDirect3DDevice9::SetTexture")
{
	uint32_t s = ARG(1);
	if (s < 16) dev.tex[s] = ARG(2) ? objectAt(ARG(2)) : nullptr;
	{
		static int nl = 0;
		if (ARG(2) && nl++ < 60) log("d3d9: SetTexture stage %u tex %x -> %p", s, ARG(2), (void*)dev.tex[s < 16 ? s : 0]);
	}
	SET(0);
} END;
H("IDirect3DDevice9::GetTexture")
{
	uint32_t s = ARG(1);
	Obj* t = s < 16 ? dev.tex[s] : nullptr;
	if (t) t->ref++;
	guest::store<uint32_t>(ARG(2), t ? t->addr : 0);
	SET(0);
} END;
H("IDirect3DDevice9::GetTextureStageState") { if (ARG(3)) guest::store<uint32_t>(ARG(3), 0); SET(0); } END;
H("IDirect3DDevice9::SetViewport")
{
	uint32_t p = ARG(1);
	for (int i = 0; i < 4; i++) dev.vp[i] = guest::load<uint32_t>(p + 4 * i);
	dev.vpZ[0] = guest::load<float>(p + 16); dev.vpZ[1] = guest::load<float>(p + 20);
	SET(0);
} END;
H("IDirect3DDevice9::GetViewport")
{
	uint32_t p = ARG(1);
	bool set = dev.vp[2] && dev.vp[3];
	guest::store<uint32_t>(p, set ? dev.vp[0] : 0); guest::store<uint32_t>(p + 4, set ? dev.vp[1] : 0);
	guest::store<uint32_t>(p + 8, set ? dev.vp[2] : backW); guest::store<uint32_t>(p + 12, set ? dev.vp[3] : backH);
	guest::store<float>(p + 16, dev.vpZ[0]); guest::store<float>(p + 20, dev.vpZ[1]);
	SET(0);
} END;
H("IDirect3DDevice9::SetScissorRect") { uint32_t p = ARG(1); for (int i = 0; i < 4; i++) dev.scissor[i] = guest::load<int32_t>(p + 4 * i); SET(0); } END;
H("IDirect3DDevice9::GetScissorRect") { uint32_t p = ARG(1); for (int i = 0; i < 4; i++) guest::store<int32_t>(p + 4 * i, dev.scissor[i]); SET(0); } END;

H("IDirect3DDevice9::SetStreamSource")
{
	uint32_t s = ARG(1);
	if (s < 16) { dev.streams[s].vb = ARG(2) ? objectAt(ARG(2)) : nullptr; dev.streams[s].offset = ARG(3); dev.streams[s].stride = ARG(4); }
	SET(0);
} END;
H("IDirect3DDevice9::GetStreamSource")
{
	uint32_t s = ARG(1);
	Stream st = s < 16 ? dev.streams[s] : Stream();
	if (st.vb) st.vb->ref++;
	if (ARG(2)) guest::store<uint32_t>(ARG(2), st.vb ? st.vb->addr : 0);
	if (ARG(3)) guest::store<uint32_t>(ARG(3), st.offset);
	if (ARG(4)) guest::store<uint32_t>(ARG(4), st.stride);
	SET(0);
} END;
H("IDirect3DDevice9::SetStreamSourceFreq") { if (ARG(1) < 16) dev.streams[ARG(1)].freq = ARG(2); SET(0); } END;
H("IDirect3DDevice9::GetStreamSourceFreq") { if (ARG(2)) guest::store<uint32_t>(ARG(2), ARG(1) < 16 ? dev.streams[ARG(1)].freq : 1); SET(0); } END;
H("IDirect3DDevice9::SetIndices") { dev.ib = ARG(1) ? objectAt(ARG(1)) : nullptr; SET(0); } END;
H("IDirect3DDevice9::GetIndices") { if (dev.ib) dev.ib->ref++; guest::store<uint32_t>(ARG(1), dev.ib ? dev.ib->addr : 0); SET(0); } END;
H("IDirect3DDevice9::SetVertexDeclaration") { dev.vdecl = ARG(1) ? objectAt(ARG(1)) : nullptr; if (dev.vdecl) dev.fvf = 0; SET(0); } END;
H("IDirect3DDevice9::GetVertexDeclaration") { if (dev.vdecl) dev.vdecl->ref++; guest::store<uint32_t>(ARG(1), dev.vdecl ? dev.vdecl->addr : 0); SET(0); } END;
H("IDirect3DDevice9::SetFVF") { dev.fvf = ARG(1); if (dev.fvf) dev.vdecl = nullptr; SET(0); } END;
H("IDirect3DDevice9::GetFVF") { guest::store<uint32_t>(ARG(1), dev.fvf); SET(0); } END;

H("IDirect3DDevice9::SetVertexShader") { Obj* s = ARG(1) ? objectAt(ARG(1)) : nullptr; dev.vs = s ? s->sh : nullptr; SET(0); } END;
H("IDirect3DDevice9::SetPixelShader") { Obj* s = ARG(1) ? objectAt(ARG(1)) : nullptr; dev.ps = s ? s->sh : nullptr; SET(0); } END;
H("IDirect3DDevice9::GetVertexShader") { guest::store<uint32_t>(ARG(1), 0); SET(0); } END;
H("IDirect3DDevice9::GetPixelShader") { guest::store<uint32_t>(ARG(1), 0); SET(0); } END;

static void setConstF(float* regs, uint32_t maxReg, uint32_t start, uint32_t src, uint32_t count)
{
	for (uint32_t i = 0; i < count && start + i < maxReg; i++)
		for (int k = 0; k < 4; k++) regs[(start + i) * 4 + k] = guest::load<float>(src + 16 * i + 4 * k);
}
H("IDirect3DDevice9::SetVertexShaderConstantF") { setConstF(dev.vsF, 256, ARG(1), ARG(2), ARG(3)); SET(0); } END;
H("IDirect3DDevice9::SetPixelShaderConstantF") { setConstF(dev.psF, 224, ARG(1), ARG(2), ARG(3)); SET(0); } END;
H("IDirect3DDevice9::SetVertexShaderConstantI") { for (uint32_t i = 0; i < ARG(3) && ARG(1) + i < 16; i++) for (int k = 0; k < 4; k++) dev.vsI[(ARG(1) + i) * 4 + k] = guest::load<int32_t>(ARG(2) + 16 * i + 4 * k); SET(0); } END;
H("IDirect3DDevice9::SetPixelShaderConstantI") { for (uint32_t i = 0; i < ARG(3) && ARG(1) + i < 16; i++) for (int k = 0; k < 4; k++) dev.psI[(ARG(1) + i) * 4 + k] = guest::load<int32_t>(ARG(2) + 16 * i + 4 * k); SET(0); } END;
H("IDirect3DDevice9::SetVertexShaderConstantB") { for (uint32_t i = 0; i < ARG(3) && ARG(1) + i < 16; i++) dev.vsB[ARG(1) + i] = guest::load<int32_t>(ARG(2) + 4 * i); SET(0); } END;
H("IDirect3DDevice9::SetPixelShaderConstantB") { for (uint32_t i = 0; i < ARG(3) && ARG(1) + i < 16; i++) dev.psB[ARG(1) + i] = guest::load<int32_t>(ARG(2) + 4 * i); SET(0); } END;
H("IDirect3DDevice9::GetVertexShaderConstantF") { for (uint32_t i = 0; i < ARG(3) && ARG(1) + i < 256; i++) for (int k = 0; k < 4; k++) guest::store<float>(ARG(2) + 16 * i + 4 * k, dev.vsF[(ARG(1) + i) * 4 + k]); SET(0); } END;
H("IDirect3DDevice9::GetPixelShaderConstantF") { for (uint32_t i = 0; i < ARG(3) && ARG(1) + i < 224; i++) for (int k = 0; k < 4; k++) guest::store<float>(ARG(2) + 16 * i + 4 * k, dev.psF[(ARG(1) + i) * 4 + k]); SET(0); } END;

// ---- render targets
H("IDirect3DDevice9::SetRenderTarget")
{
	if (ARG(1) == 0)
	{
		Obj* s = ARG(2) ? objectAt(ARG(2)) : nullptr;
		dev.rt0 = (s && s != gBackBuffer) ? s : nullptr;
	}
	SET(0);
} END;
H("IDirect3DDevice9::GetRenderTarget")
{
	Obj* s = dev.rt0;
	if (!s)
	{
		if (!gBackBuffer) gBackBuffer = makeSurface(backW, backH, 22, 1, 0);
		s = gBackBuffer;
	}
	s->ref++;
	writeOut(ARG(2), s);
	SET(0);
} END;
H("IDirect3DDevice9::SetDepthStencilSurface") { dev.ds = ARG(1) ? objectAt(ARG(1)) : nullptr; SET(0); } END;
H("IDirect3DDevice9::GetDepthStencilSurface")
{
	static Obj* defaultDs = nullptr;
	Obj* s = dev.ds;
	if (!s) { if (!defaultDs) defaultDs = makeSurface(backW, backH, 75, 2, 0); s = defaultDs; }
	s->ref++;
	writeOut(ARG(1), s);
	SET(0);
} END;
H("IDirect3DDevice9::GetBackBuffer")
{
	if (!gBackBuffer) gBackBuffer = makeSurface(backW, backH, 22, 1, 0);
	gBackBuffer->ref++;
	writeOut(ARG(4), gBackBuffer);
	SET(0);
} END;

// ---- frame
H("IDirect3DDevice9::Clear")
{
	uint32_t zb = ARG(5);
	float zf;
	memcpy(&zf, &zb, 4);
	glr::clear(ARG(1), ARG(2), ARG(3), ARG(4), zf, ARG(6));
	SET(0);
} END;
H("IDirect3DDevice9::Present")
{
	static std::atomic<uint32_t> frames{0};
	uint32_t n = ++frames;
	if (n == 1 || n % 300 == 0) log("d3d9: Present #%u", n);
	glr::present();
	SET(0);
} END;
H("IDirect3DSwapChain9::Present") { glr::present(); SET(0); } END;

// ---- drawing
static std::atomic<uint32_t> drawCount{0};
static void doDraw(const DrawCall& dc)
{
	uint32_t n = ++drawCount;
	if (n <= 12) log("d3d9: draw #%u prim %u count %u indexed %d vs %p ps %p vdecl %p fvf %x", n, dc.prim, dc.primCount, (int)dc.indexed, (void*)dev.vs, (void*)dev.ps, (void*)dev.vdecl, dev.fvf);
	glr::draw(dc);
}
H("IDirect3DDevice9::DrawPrimitive")
{
	DrawCall dc;
	dc.prim = ARG(1); dc.baseVertex = (int32_t)ARG(2); dc.primCount = ARG(3);
	doDraw(dc);
	SET(0);
} END;
H("IDirect3DDevice9::DrawIndexedPrimitive")
{
	DrawCall dc;
	dc.prim = ARG(1); dc.indexed = true; dc.baseVertex = (int32_t)ARG(2); dc.minIndex = ARG(3); dc.numVertices = ARG(4); dc.startIndex = ARG(5); dc.primCount = ARG(6);
	doDraw(dc);
	SET(0);
} END;
H("IDirect3DDevice9::DrawPrimitiveUP")
{
	DrawCall dc;
	dc.prim = ARG(1); dc.primCount = ARG(2); dc.upVerts = guest::host(ARG(3)); dc.upStride = ARG(4);
	doDraw(dc);
	SET(0);
} END;
H("IDirect3DDevice9::DrawIndexedPrimitiveUP")
{
	DrawCall dc;
	dc.prim = ARG(1); dc.indexed = true; dc.minIndex = ARG(2); dc.numVertices = ARG(3); dc.primCount = ARG(4);
	dc.upIdx = guest::host(ARG(5)); dc.upIdxFmt = ARG(6); dc.upVerts = guest::host(ARG(7)); dc.upStride = ARG(8);
	doDraw(dc);
	SET(0);
} END;

// ---- copies
H("IDirect3DDevice9::StretchRect")
{
	Obj* src = objectAt(ARG(1));
	Obj* dst = objectAt(ARG(3));
	int32_t sr[4], dr[4];
	bool hs = ARG(2) != 0, hd = ARG(4) != 0;
	if (hs) for (int i = 0; i < 4; i++) sr[i] = guest::load<int32_t>(ARG(2) + 4 * i);
	if (hd) for (int i = 0; i < 4; i++) dr[i] = guest::load<int32_t>(ARG(4) + 4 * i);
	if (src && dst) glr::stretchRect(src, hs ? sr : nullptr, dst, hd ? dr : nullptr, ARG(5));
	SET(0);
} END;
H("IDirect3DDevice9::ColorFill") { SET(0); } END;
H("IDirect3DDevice9::UpdateTexture")
{
	Obj* s = objectAt(ARG(1));
	Obj* d = objectAt(ARG(2));
	if (s && d && s->data && d->data)
	{
		// copy the matching mip levels (the destination may have fewer)
		uint32_t skip = s->levels > d->levels ? s->levels - d->levels : 0;
		for (uint32_t f = 0; f < d->faces && f < s->faces; f++)
			for (uint32_t l = 0; l < d->levels; l++)
			{
				uint32_t si = f * s->levels + l + skip, di = f * d->levels + l;
				if (si >= s->levelOff.size() || di >= d->levelOff.size()) continue;
				uint32_t sz = levelBytes(d->fmt, std::max(1u, d->w >> l), std::max(1u, d->h >> l)) * std::max(1u, d->d >> l);
				memcpy(guest::host(d->data + d->levelOff[di]), guest::host(s->data + s->levelOff[si]), sz);
			}
		d->ver++;
	}
	SET(0);
} END;
H("IDirect3DDevice9::UpdateSurface")
{
	Obj* s = objectAt(ARG(1));
	Obj* d = objectAt(ARG(3));
	if (s && d && s->data && d->data && s->w == d->w && s->h == d->h && s->fmt == d->fmt && !ARG(2) && !ARG(4))
	{
		memcpy(guest::host(d->data), guest::host(s->data), std::min(s->size, d->size));
		d->ver++;
		if (d->parent) if (Obj* t = objectAt(d->parent)) t->ver++;
	}
	SET(0);
} END;
H("IDirect3DDevice9::GetRenderTargetData") { SET(0); } END;
}   // namespace d3d
