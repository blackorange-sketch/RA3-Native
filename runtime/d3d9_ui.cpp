// HUD scaling, see d3d9_ui.h (moved out of the OpenGL renderer, unchanged)
#include "game_build.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unistd.h>
#include "d3d9.h"
#include "d3d9_ui.h"
#include "hud.h"
#include "input.h"
using namespace rt;
namespace d3d
{
bool rtIsBack(Obj* rt);                                    // the renderer's (d3d9_gl.cpp)
void rtTargetSize(Obj* rt, uint32_t& w, uint32_t& h);
namespace ui
{
static bool isBack(Obj* rt) { return rtIsBack(rt); }
static void targetSize(Obj* rt, uint32_t& w, uint32_t& h) { rtTargetSize(rt, w, h); }
// ---- interface scaling
// The game lays its HUD out in back-buffer pixels at a size proportional to the screen, which is small on a phone. HUD
// pieces are drawn by the interface shader (APT); every such draw that lies entirely in one corner zone of the screen
// (portrait, minimap, build menu, power bar) is scaled up around that screen corner. Touches on the enlarged panels are
// mapped back to the game's own coordinates (rt::uiUnmap).
const uint64_t kAptVs = 0xe22700c621ded1fbull;   // interface vertex shader: pixel positions, transform in c11..c14
const uint64_t kVideoVs = 0xcb5e040244f89931ull; // video.fx (commander video on the minimap): positions 0..1 of the screen
std::vector<UiRect> gUiCur, gUiPub;
std::mutex gUiMu;
float gUiScale = 1.f;
void uiUnmapImpl(int& x, int& y)
{
	std::lock_guard<std::mutex> l(gUiMu);
	if (gUiScale <= 1.001f) return;
	for (size_t i = gUiPub.size(); i-- > 0;)
	{
		const UiRect& r = gUiPub[i];
		if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) continue;
		x = (int)lroundf(r.ax + (x - r.ax) / r.sc);
		y = (int)lroundf(r.ay + (y - r.ay) / r.sc);
		return;
	}
}
// Interface scaling of one draw. Returns the scale-1 to apply with the u_ui uniform when the whole draw lies in one
// corner zone (anchor in clip space). A draw with pieces in several zones (text of the whole HUD batched into one
// draw) is rebuilt on the CPU instead: every triangle in a corner zone is moved there, the rest stays; then `split`
// holds the vertices for a non-indexed triangle list.
bool gUiDiag = false;
bool uiDiagFrame() { return gUiDiag; }   // a few frames during play: every interface draw into the log
bool gUiAptSeen = false;   // an interface draw happened this frame (diagnostics)
bool gUiHud = false;       // the HUD panels have started this frame: earlier interface draws belong to the world
float gUiColLeft = 1e9f, gUiCurColLeft = 1e9f;   // left edge of the right column (back-buffer pixels)
float gUiRightT = 0.f, gUiRightB = 1.f, gUiCurT = 0.f, gUiCurB = 1.f;   // right column: lowest top piece, highest bottom piece (fractions of the height)
std::vector<UiRect> gUiPieces;   // panel pieces scaled this frame, in the game's own (unscaled) coordinates
std::vector<UiRect> gUiPops;     // tooltip boxes scaled this frame (unscaled coordinates): their lines and icons follow
bool gUiMenu = false;      // a big window covers the battle this frame
bool gUiNested = false;     // drawing a rebuilt interface draw (no second analysis)
uint32_t gUiWorldDraws = 0;
uint32_t gAptDraws = 0, gVideoDraws = 0;
float gCamRegs[16]; uint32_t gCamRegsFrame = ~0u; bool gCamRegsNew = false;   // interface (Flash) and video draws this frame: menu vs. loading screen / cutscene   // depth-tested draws this frame: the HUD is scaled in a battle only, not in menus
float uiScaleFor(const DrawCall& dc, const DeclElem* els, uint32_t nEls, float& axN, float& ayN, float& ax, float& ay, std::vector<uint8_t>& split)
{
	if (gUiNested || gUiScale <= 1.001f || gUiWorldDraws < 50 || !dev.vs || !isBack(dev.rt0)) return 0.f;
	if (gUiDiag && gUiAptSeen)
	{
		// every back-buffer draw after the first interface draw: which shaders draw the rest (texts)
		std::string cn;
		for (auto& cc : dev.vs->info.ctab) if (cc.regSet != 3 && cn.size() < 80) { cn += " "; cn += cc.name; }
		log("ui-diag: draw vs %016llx z %u prim %u n %u up %d consts:%s", (unsigned long long)dev.vs->hash, dev.rs[7], dc.prim, dc.primCount, dc.upVerts ? 1 : 0, cn.c_str());
	}
	if (dev.vs->hash == kAptVs) gUiAptSeen = true;
	if (dev.rs[7]) return 0.f;
	// the interface shader (World matrix in c11..c14) or a shader without constants that passes clip-space positions
	// through (the commander video on the minimap); anything else is left alone
	bool apt = dev.vs->hash == kAptVs, pass = false, video = dev.vs->hash == kVideoVs;
	if (!apt && !video)
	{
		pass = true;
		for (auto& cc : dev.vs->info.ctab) if (cc.regSet != 3) { pass = false; break; }
		if (!dev.vs->info.defF.empty()) pass = false;
	}
	bool diag = uiDiagFrame();
	if (!apt && !pass && !video) { if (diag) log("ui-diag: vs %016llx prim %u n %u: not an interface shader", (unsigned long long)dev.vs->hash, dc.prim, dc.primCount); return 0.f; }
	const DeclElem* pe = nullptr;
	for (uint32_t i = 0; i < nEls; i++) if (els[i].usage == 0 && els[i].index == 0 && (els[i].type >= 1 && els[i].type <= 3)) { pe = &els[i]; break; }
	auto fail = [&](const char* why) { if (diag) log("ui-diag: vs %016llx prim %u n %u skipped: %s", (unsigned long long)dev.vs->hash, dc.prim, dc.primCount, why); return 0.f; };
	if (!pe) { if (diag) { std::string t; for (uint32_t q = 0; q < nEls; q++) { char b[32]; snprintf(b, sizeof b, " u%u.%u:t%u", els[q].usage, els[q].index, els[q].type); t += b; } log("ui-diag: no position element:%s", t.c_str()); } return 0.f; }
	const uint8_t* vb; uint32_t stride; uint64_t vbSize;
	if (dc.upVerts) { vb = (const uint8_t*)dc.upVerts; stride = dc.upStride; vbSize = ~0ull; }
	else
	{
		const Stream& st = dev.streams[pe->stream < 16 ? pe->stream : 0];
		if (!st.vb || !st.vb->data || !st.stride) return fail("no vertex data");
		vb = (const uint8_t*)guest::host(st.vb->data) + st.offset; stride = st.stride; vbSize = st.vb->size > st.offset ? st.vb->size - st.offset : 0;
	}
	uint32_t nIdx = (uint32_t)primVertices(dc.prim, dc.primCount);
	if (!nIdx || nIdx > 6000) return fail("vertex count");
	float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	if (apt) memcpy(m, dev.vsF + 4 * 11, sizeof m);
	if (video) { const float mv[16] = {2, 0, 0, -1, 0, -2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1}; memcpy(m, mv, sizeof m); }   // video.fx: positions 0..1
	if (m[12] != 0.f || m[13] != 0.f) return fail("perspective");   // w must not depend on x, y (flat interface)
	const bool pos4 = pe->type == 3;
	const size_t posBytes = pe->type == 1 ? 8 : pos4 ? 16 : 12;
	float det = m[0] * m[5] - m[1] * m[4];
	if (fabsf(det) < 1e-12f) return fail("singular matrix");
	uint32_t vx = dev.vp[0], vy = dev.vp[1], vw = dev.vp[2], vh = dev.vp[3];
	if (!vw || !vh) { vx = vy = 0; targetSize(nullptr, vw, vh); }
	const float W = (float)vw, H = (float)vh;
	const uint8_t* ip = nullptr; bool i32 = false;
	if (dc.indexed)
	{
		if (dc.upIdx) { ip = (const uint8_t*)dc.upIdx; i32 = dc.upIdxFmt == 102; }
		else if (dev.ib && dev.ib->data) { i32 = dev.ib->fmt == 102; if ((uint64_t)(dc.startIndex + nIdx) * (i32 ? 4 : 2) > dev.ib->size) return fail("index range"); ip = (const uint8_t*)guest::host(dev.ib->data) + (uint64_t)dc.startIndex * (i32 ? 4 : 2); }
		else return fail("no index buffer");
	}
	static std::vector<int64_t> vix; static std::vector<float> sxy, wv;
	vix.resize(nIdx); sxy.resize(nIdx * 2); wv.resize(nIdx);
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
	for (uint32_t i = 0; i < nIdx; i++)
	{
		int64_t v;
		if (ip) { uint32_t k; if (i32) memcpy(&k, ip + 4 * i, 4); else { uint16_t s16; memcpy(&s16, ip + 2 * i, 2); k = s16; } v = (int64_t)dc.baseVertex + k; }
		else v = (int64_t)dc.baseVertex + i;
		if (v < 0 || (uint64_t)v * stride + pe->offset + posBytes > vbSize) return fail("vertex outside the buffer");
		float p[4] = {0, 0, 0, 1}; memcpy(p, vb + (uint64_t)v * stride + pe->offset, posBytes);
		float cx = p[0] * m[0] + p[1] * m[1] + p[2] * m[2] + p[3] * m[3];
		float cy = p[0] * m[4] + p[1] * m[5] + p[2] * m[6] + p[3] * m[7];
		float cw = p[2] * m[14] + p[3] * m[15];
		if (fabsf(cw) < 1e-6f) return fail("w = 0");
		float sx = vx + (cx / cw + 1.f) * 0.5f * W, sy = vy + (1.f - cy / cw) * 0.5f * H;
		vix[i] = v; sxy[2 * i] = sx; sxy[2 * i + 1] = sy; wv[i] = cw;
		x0 = std::min(x0, sx); x1 = std::max(x1, sx); y0 = std::min(y0, sy); y1 = std::max(y1, sy);
	}
	// the right column (minimap + build menu from the top, unit panel from the bottom) has to fit the screen height:
	// its scale is limited so the two groups do not overlap (extents from the previous frame)
	auto scaleOf = [&](int c) -> float {
		if ((c != 2 && c != 4) || gUiRightT <= 0.f || gUiRightB >= 1.f) return gUiScale;
		return std::max(1.f, std::min(gUiScale, 1.f / (gUiRightT + 1.f - gUiRightB) * 0.99f));
	};
	// only pieces of the column itself (not the command bar that opens to the left of the unit panel)
	// The HUD panels keep their size relative to the screen HEIGHT: zone limits are written as shares of a 16:9 width
	// measured from their own edge, so on a wider picture (wide screen mode) they stay at the panels instead of
	// spreading over the battle (a tooltip left of the right column fell into the column's zone and was split)
	const float W16 = std::min((float)W, H * 16.f / 9.f);
	auto fromL = [&](float f) { return (float)vx + f * W16; };
	auto fromR = [&](float f) { return (float)vx + W - (1.f - f) * W16; };
	auto noteRight = [&](int c, float a0, float b0, float a1, float b1) {
		if (a0 < fromR(0.75f)) return;
		if (c == 2) gUiCurT = std::max(gUiCurT, (b1 - vy) / H);
		if (c == 4 && a1 >= fromR(0.97f)) gUiCurB = std::min(gUiCurB, (b0 - vy) / H);
	};
	// corner of a rectangle: 0 none, else 1 + (right) + 2 * (bottom)
	// corner zones; on the right the minimap column with the open build menu reaches two thirds down the screen
	auto corner = [&](float a0, float b0, float a1, float b1) -> int {
		// the unit panel's command bar opens to the left along the bottom edge
		if (a0 >= fromR(0.50f) && b0 >= vy + 0.86f * H) return 4;
		bool l = a1 <= fromL(0.40f), r = a0 >= fromR(0.75f);
		if (!(l || r)) return 0;
		// left: the special powers column under the portrait grows downwards with every new power (it used to fall
		// out of the top-left zone from the fourth button on)
		bool t = b1 <= vy + (r ? 0.66f : 0.75f) * H, b = b0 >= vy + (r ? 0.70f : 0.78f) * H;
		if (!(t || b)) return 0;
		return 1 + (r ? 1 : 0) + (b ? 2 : 0);
	};
	auto anchorOf = [&](int c, float& cx, float& cy) { cx = (c - 1) & 1 ? (float)vx + W : (float)vx; cy = (c - 1) & 2 ? (float)vy + H : (float)vy; };
	auto addRect = [&](float a0, float b0, float a1, float b1, float cx, float cy, float s) { if (gUiCur.size() < 4000) gUiCur.push_back({cx + (a0 - cx) * s, cy + (b0 - cy) * s, cx + (a1 - cx) * s, cy + (b1 - cy) * s, cx, cy, s}); };
	// text: many glyph-sized triangles. The game draws each text twice (dark shadow, then the letters) in batches of
	// different extent, so a corner decision per batch scaled one and not the other (doubled, offset messages and
	// tooltips). Texts follow the panel under each glyph instead (the mixed path below).
	bool textLike = false;
	if (dc.prim == 4 && nIdx >= 24 && nIdx % 3 == 0)
	{
		textLike = true;
		const float maxA = 0.0015f * W * H;
		for (uint32_t t = 0; t < nIdx && textLike; t += 3)
		{
			float a0 = std::min({sxy[2 * t], sxy[2 * t + 2], sxy[2 * t + 4]}), a1 = std::max({sxy[2 * t], sxy[2 * t + 2], sxy[2 * t + 4]});
			float b0 = std::min({sxy[2 * t + 1], sxy[2 * t + 3], sxy[2 * t + 5]}), b1 = std::max({sxy[2 * t + 1], sxy[2 * t + 3], sxy[2 * t + 5]});
			if ((a1 - a0) * (b1 - b0) > maxA) textLike = false;
		}
	}
	int c = textLike && gUiHud ? 0 : corner(x0, y0, x1, y1);
	// anything inside a tooltip scaled earlier in this frame (its frame pieces, the requirement tree of a locked
	// building: bracket lines and the building icon) moves and grows exactly with the tooltip
	if (!c && !textLike && gUiHud && !gUiMenu && !gUiPops.empty())
	{
		const float tol = 0.01f * W16;
		for (size_t q = gUiPops.size(); q-- > 0;)
		{
			const UiRect& r = gUiPops[q];
			if (x0 < r.x0 - tol || x1 > r.x1 + tol || y0 < r.y0 - tol || y1 > r.y1 + tol) continue;
			ax = r.ax; ay = r.ay;
			addRect(x0, y0, x1, y1, ax, ay, r.sc);
			axN = (ax - vx) / W * 2.f - 1.f; ayN = 1.f - (ay - vy) / H * 2.f;
			if (diag) log("ui-diag: box %.0f,%.0f-%.0f,%.0f inside tooltip %zu: scaled with it", x0, y0, x1, y1, q);
			return r.sc - 1.f;
		}
	}
	// a tooltip or popup that ends at the left edge of the right column belongs to it (it is anchored there);
	// tooltips of the lower build rows reach well below the middle of the screen
	// only after the HUD has started (health bars come first) and only popup-sized: the health bars of all units
	// are one batch, and when a unit stood next to the column the whole batch was "attached" and scaled with it
	// (the tooltips of the buttons under the minimap - repair, sell, objectives - end up to 12% short of the column)
	bool attached = !c && !textLike && gUiHud && (x1 - x0) <= 0.40f * W16 && gUiColLeft < 1e8f && x1 >= gUiColLeft - 0.12f * W16 && x0 < gUiColLeft && x1 <= gUiColLeft + 0.20f * W16 && y1 <= vy + 0.90f * H;
	if (attached) c = 2;
	// a tooltip of the unit panel / command bar opens above it and reaches down onto it: it moves with that panel
	// (otherwise its texts, lying on the scaled command bar, moved while the box stayed)
	// (long ones - the planning mode's - start well above the middle of the screen)
	bool attachedLow = !c && !textLike && gUiHud && x0 >= fromR(0.50f) && y0 >= vy + 0.30f * H && y1 >= vy + 0.88f * H && (x1 - x0) <= 0.40f * W16;
	if (attachedLow) { c = 4; attached = true; }
	if (c == 2 && !attached) gUiCurColLeft = std::min(gUiCurColLeft, x0);
	// unit markers, health bars and names come first in a frame: only what follows the first big corner panel is HUD
	if (!gUiHud && c && !attached && (x1 - x0) * (y1 - y0) >= 0.01f * W * H) gUiHud = true;
	if (!gUiHud) { if (diag) log("ui-diag: vs %016llx box %.0f,%.0f-%.0f,%.0f before the HUD", (unsigned long long)dev.vs->hash, x0, y0, x1, y1); return 0.f; }
	// a large window over the battle (pause / episode menu): nothing more is scaled this frame, and touches go to the
	// game unchanged (the HUD under it is covered)
	if (gUiMenu) return 0.f;
	// (the special abilities window is about a quarter of a wide screen: from 20%)
	if (apt && (x1 - x0) * (y1 - y0) >= 0.20f * W * H && (x1 - x0) < 0.98f * W)
	{
		gUiMenu = true; gUiCur.clear(); gUiPieces.clear(); gUiPops.clear();
		if (diag) log("ui-diag: big window %.0f,%.0f-%.0f,%.0f: scaling off for this frame", x0, y0, x1, y1);
		return 0.f;
	}
	if (diag) log("ui-diag: vs %016llx%s prim %u n %u idx %u box %.0f,%.0f-%.0f,%.0f corner %d", (unsigned long long)dev.vs->hash, apt ? " (apt)" : " (pass)", dc.prim, dc.primCount, nIdx, x0, y0, x1, y1, c);
	if (c)
	{
		anchorOf(c, ax, ay);
		if (!attached) noteRight(c, x0, y0, x1, y1);
		float s = scaleOf(c);
		if (attached && gUiScale > s + 0.01f)
		{
			// a tooltip of the right column, whose scale is limited so the column fits the screen height: the popup
			// itself gets the full HUD size (its text as big as everywhere else). It stays at its button - the corner
			// next to the panel (top right; bottom right for the command bar's) moves with the panel - and is kept
			// on the screen.
			float st = gUiScale;
			float q1 = ax + (x1 - ax) * s, r0 = ay + (y0 - ay) * s, r1 = ay + (y1 - ay) * s;
			float w = (x1 - x0) * st, h = (y1 - y0) * st;
			float f1 = q1, f0 = f1 - w;
			if (f0 < vx) { f0 = (float)vx; f1 = f0 + w; }
			float g0 = c == 4 ? r1 - h : r0;
			g0 = std::max((float)vy, std::min(g0, (float)vy + H - h));
			// the anchor that maps the box (x0, y0) onto (f0, g0) at scale st
			ax = (f0 - st * x0) / (1.f - st); ay = (g0 - st * y0) / (1.f - st);
			s = st;
			addRect(x0, y0, x1, y1, ax, ay, s);
			if (gUiPieces.size() < 2000) gUiPieces.push_back({x0 - 0.15f * W16, y0, x1, y1, ax, ay, s});
			if (gUiPops.size() < 64) gUiPops.push_back({x0, y0, x1, y1, ax, ay, s});
			axN = (ax - vx) / W * 2.f - 1.f; ayN = 1.f - (ay - vy) / H * 2.f;
			return s - 1.f;
		}
		addRect(x0, y0, x1, y1, ax, ay, s);
		// a tooltip's text can start left of its box: texts get a wider catch area to the left of attached popups
		if (gUiPieces.size() < 2000) gUiPieces.push_back({attached ? x0 - 0.15f * W16 : x0, y0, x1, y1, ax, ay, s});
		if (attached && gUiPops.size() < 64) gUiPops.push_back({x0, y0, x1, y1, ax, ay, s});
		axN = (c - 1) & 1 ? 1.f : -1.f; ayN = (c - 1) & 2 ? -1.f : 1.f;
		return s - 1.f;
	}
	if (dc.prim != 4 || nIdx % 3) return fail("mixed corners, not a triangle list");
	// mixed draw (the texts of the whole HUD in one batch): a triangle that lies on a panel scaled earlier in this frame
	// moves with that panel; texts elsewhere (subtitles, unit names) stay
	auto pieceOf = [&](float a0, float b0, float a1, float b1) -> const UiRect* {
		float cxm = (a0 + a1) * 0.5f, cym = (b0 + b1) * 0.5f;
		for (size_t q = gUiPieces.size(); q-- > 0;)
		{
			const UiRect& r = gUiPieces[q];
			if (cxm >= r.x0 && cxm <= r.x1 && cym >= r.y0 && cym <= r.y1) return &r;
		}
		return nullptr;
	};
	bool any = false;
	for (uint32_t t = 0; t < nIdx && !any; t += 3)
	{
		float a0 = std::min({sxy[2 * t], sxy[2 * t + 2], sxy[2 * t + 4]}), a1 = std::max({sxy[2 * t], sxy[2 * t + 2], sxy[2 * t + 4]});
		float b0 = std::min({sxy[2 * t + 1], sxy[2 * t + 3], sxy[2 * t + 5]}), b1 = std::max({sxy[2 * t + 1], sxy[2 * t + 3], sxy[2 * t + 5]});
		any = pieceOf(a0, b0, a1, b1) != nullptr;
	}
	if (!any) return fail("mixed draw, nothing on a panel");
	split.resize((size_t)nIdx * stride);
	for (uint32_t t = 0; t < nIdx; t += 3)
	{
		float a0 = std::min({sxy[2 * t], sxy[2 * t + 2], sxy[2 * t + 4]}), a1 = std::max({sxy[2 * t], sxy[2 * t + 2], sxy[2 * t + 4]});
		float b0 = std::min({sxy[2 * t + 1], sxy[2 * t + 3], sxy[2 * t + 5]}), b1 = std::max({sxy[2 * t + 1], sxy[2 * t + 3], sxy[2 * t + 5]});
		const UiRect* pr = pieceOf(a0, b0, a1, b1);
		bool tc = pr != nullptr;
		float cx = 0, cy = 0, s = 1.f;
		if (tc) { cx = pr->ax; cy = pr->ay; s = pr->sc; }
		for (uint32_t k = t; k < t + 3; k++)
		{
			uint8_t* d = split.data() + (size_t)k * stride;
			{ uint64_t at = (uint64_t)vix[k] * stride; memset(d, 0, stride); memcpy(d, vb + at, (size_t)std::min<uint64_t>(stride, vbSize - at)); }
			if (!tc) continue;
			// new screen position -> clip space -> object x, y (z kept)
			float nx = cx + (sxy[2 * k] - cx) * s, ny = cy + (sxy[2 * k + 1] - cy) * s;
			float w = wv[k];
			float qx = ((nx - vx) / W * 2.f - 1.f) * w, qy = (1.f - (ny - vy) / H * 2.f) * w;
			float p[4] = {0, 0, 0, 1}; memcpy(p, d + pe->offset, posBytes);
			float rx = qx - m[2] * p[2] - m[3] * p[3], ry = qy - m[6] * p[2] - m[7] * p[3];
			p[0] = (rx * m[5] - m[1] * ry) / det;
			p[1] = (m[0] * ry - rx * m[4]) / det;
			memcpy(d + pe->offset, p, 8);
		}
	}
	return 0.f;
}
void uiEndFrame(uint32_t gFrameNo)
{
	{
		// interface scaling: the panel rectangles of this frame become the touch mapping for the next one
		std::lock_guard<std::mutex> l(gUiMu);
		gUiScale = std::max(1.f, std::min(2.f, rt::config.uiScale));
		gUiPub.swap(gUiCur); gUiCur.clear();
		{
			static int diagFrames = 0;
			bool inGame = gUiWorldDraws >= 50;
			// /sdcard/ra3_uidiag: a frame every 1.5 s (40 in all), to catch a tooltip held open
			static const bool uiDiagFlag = access("/sdcard/ra3_uidiag", F_OK) == 0;
			gUiDiag = inGame && gUiScale > 1.001f && (uiDiagFlag ? diagFrames < 40 && gFrameNo % 45 == 0 : diagFrames < 3 && gFrameNo % 900 == 0 && verbose());
			if (gUiDiag) { diagFrames++; log("ui-diag: frame %u", gFrameNo + 1); }
		}
		// the right column's extents: a menu that opens shrinks the column at once (it has to fit); after it closes the
		// column grows back to the full HUD size, once the smaller extents have held for half a second (no flicker while
		// switching tabs)
		if (gUiHud)
		{
			static int smaller = 0;
			if (gUiCurT >= gUiRightT - 0.005f && gUiCurB <= gUiRightB + 0.005f) smaller = 0;
			gUiRightT = std::max(gUiRightT, gUiCurT); gUiRightB = std::min(gUiRightB, gUiCurB);
			if ((gUiCurT < gUiRightT - 0.005f || gUiCurB > gUiRightB + 0.005f) && ++smaller >= 15) { gUiRightT = gUiCurT; gUiRightB = gUiCurB; smaller = 0; }
			if (gUiCurColLeft < 1e8f) gUiColLeft = gUiCurColLeft;
		}
		gUiCurT = 0.f; gUiCurB = 1.f; gUiCurColLeft = 1e9f;
		gUiWorldDraws = 0; gAptDraws = gVideoDraws = 0; gUiHud = false; gUiAptSeen = false; gUiPieces.clear(); gUiPops.clear(); gUiMenu = false;
		static int nl = 0;
		if (gUiScale > 1.001f && !gUiPub.empty() && nl < 3 && gFrameNo > 600) { nl++; log("ui: interface scale %.2f, %zu panel pieces scaled this frame", gUiScale, gUiPub.size()); }
	}
}
void sideBars(bool& inGameOut, bool& blurOut)
{
	// battle vs. menu with a few frames of hysteresis both ways: no jump when a mission starts or ends
	static int streak = 0; static bool inGame = false;
	bool now = gUiWorldDraws >= 50;
	streak = now != inGame ? streak + 1 : 0;
	if (streak >= (now ? 1 : 3)) { inGame = now; streak = 0; }
	// menus get blurred side bars in the game's colours; loading screens and videos (few interface draws) stay black
	static int bstreak = 0; static bool blur = false;
	bool bnow = !inGame && gAptDraws >= 12;
	bstreak = bnow != blur ? bstreak + 1 : 0;
	if (bstreak >= 3)
	{
		blur = bnow; bstreak = 0;
		static int nl = 0;
		if (nl++ < 40) log("wide: side bars %s (interface draws %u, video %u, world %u)", blur ? "blurred" : "black", gAptDraws, gVideoDraws, gUiWorldDraws);
	}
	inGameOut = inGame; blurOut = blur;
}
void hudRects(int sw, int sh, std::vector<HudRect>& out)
{
	out.clear();
	{
		input::setScreen(sw, sh);
		if (gCamRegsNew)
		{
			// registers hold the matrix columns: back to the row-vector form (clip = pos * M)
			gCamRegsNew = false;
			float m[16];
			for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) m[a * 4 + b] = gCamRegs[b * 4 + a];
			static int nl = 0;
			if (nl++ < 3) log("input: camera [%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
			input::setViewProj(m);
		}
		input::dragTick();
		int kx, ky, kw, kh;
		input::kbButtonRect(kx, ky, kw, kh);
		// translucent buttons (the map shows through them in wide screen mode): dark fills are see-through, frames
		// and letters nearly opaque; the button editor stays opaque
		bool opaque = hud::editingNow();
		auto rect = [&](int x, int y, int w, int h, float r, float g, float b) {
			float lum = 0.3f * r + 0.59f * g + 0.11f * b;
			float a = opaque ? 1.f : (lum < 0.2f ? 0.3f : lum < 0.6f ? 0.6f : 0.9f) * std::min(1.f, rt::config.buttonOpacity / 100.f * (lum < 0.6f ? 1.f : 1.15f));   // launcher: button opacity (letters fade less)
			out.push_back(HudRect{x, y, w, h, r, g, b, a});
		};
		rect(kx, ky, kw, 2, 0.75f, 0.75f, 0.78f); rect(kx, ky + kh - 2, kw, 2, 0.75f, 0.75f, 0.78f);
		rect(kx, ky + 2, 2, kh - 4, 0.75f, 0.75f, 0.78f); rect(kx + kw - 2, ky + 2, 2, kh - 4, 0.75f, 0.75f, 0.78f);
		rect(kx + 2, ky + 2, kw - 4, kh - 4, 0.12f, 0.12f, 0.14f);
		int pad = kh / 8, rows = 3, cols = 7, cw = (kw - 4 - pad * 2) / cols, rh = (kh - 4 - pad * 2) / rows;
		for (int r = 0; r < rows; r++)
			for (int cc = 0; cc < cols; cc++)
				if (!(r == 2 && cc > 0 && cc < cols - 1) || cc == 3) rect(kx + 2 + pad + cc * cw + 1, ky + 2 + pad + r * rh + 1, (r == 2 && cc == 3 ? cw * 3 : cw) - 2, rh - 2, 0.85f, 0.85f, 0.88f);
		hud::draw(rect);
	}
}
}
}
