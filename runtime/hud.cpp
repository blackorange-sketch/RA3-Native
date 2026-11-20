#include <atomic>
#include "hud.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "rt.h"

namespace
{
const char* kCfg = "/sdcard/ra3_buttons.cfg";

struct Btn { std::string label; int vk; bool ctrl; int side; };   // side 0 = left, 1 = right
struct R { int x, y, w, h; };

std::mutex mu;
int scrW = 1280, scrH = 720;
std::vector<Btn> btns;
bool loaded = false;
bool editing = false;
int addSide = 0;
int page = 0;
int pressed = -1, pressedPtr = -1;   // index of the held custom button, and the pointer index that holds it
int flash = 0;                       // frames of "added" feedback
// Shift / Ctrl / Alt buttons latch: a tap holds the key down (button lit) until the next tap on it, so it can be
// combined with taps on the map (Shift = queue waypoints, Ctrl = force fire, Alt = formation / reverse move)
bool latched[3];
int modIdx(int vk) { return vk == 0x10 ? 0 : vk == 0x11 ? 1 : vk == 0x12 ? 2 : -1; }
// LMB / RMB buttons latch a touch mode instead of a key: LMB = a drag draws a selection box at once (no hold),
// RMB = a tap is a right click (and a long press a left click)
bool mouseLatched[2];
int mouseIdx(int vk) { return vk == 0x01 ? 0 : vk == 0x02 ? 1 : -1; }
void releaseLatched(std::vector<hud::KeyEv>& out) { for (int i = 0; i < 3; i++) if (latched[i]) { latched[i] = false; out.push_back({0x10 + i, false, false}); } }

struct Key { const char* label; int vk; bool ctrl; };
std::vector<std::vector<Key>> pages;

void buildPages()
{
	if (!pages.empty()) return;
	static char lab[64][4];
	int n = 0;
	std::vector<Key> p1, p2;
	for (int d = 1; d <= 10; d++) { int v = d == 10 ? '0' : '0' + d; snprintf(lab[n], 4, "%c", v); p1.push_back({lab[n], v, false}); n++; }
	for (int d = 1; d <= 10; d++) { int v = d == 10 ? '0' : '0' + d; snprintf(lab[n], 4, "C%c", v); p1.push_back({lab[n], v, true}); n++; }
	static char fl[12][4];
	for (int f = 1; f <= 12; f++) { snprintf(fl[f - 1], 4, "F%d", f); p1.push_back({fl[f - 1], 0x6f + f, false}); }
	p1.push_back({"ESC", 0x1b, false}); p1.push_back({"TAB", 0x09, false}); p1.push_back({"SPC", 0x20, false}); p1.push_back({"DEL", 0x2e, false}); p1.push_back({"ENT", 0x0d, false});
	p1.push_back({"SHIFT", 0x10, false}); p1.push_back({"CTRL", 0x11, false}); p1.push_back({"ALT", 0x12, false});
	static char lt[26][2];
	for (int c = 0; c < 26; c++) { lt[c][0] = 'A' + c; lt[c][1] = 0; p2.push_back({lt[c], 'A' + c, false}); }
	p2.push_back({"^", 0x26, false}); p2.push_back({"v", 0x28, false}); p2.push_back({"<", 0x25, false}); p2.push_back({">", 0x27, false});
	p2.push_back({"+", 0xbb, false}); p2.push_back({"-", 0xbd, false}); p2.push_back({"HOM", 0x24, false}); p2.push_back({"END", 0x23, false});
	p2.push_back({"LMB", 0x01, false}); p2.push_back({"RMB", 0x02, false});
	pages.push_back(p1); pages.push_back(p2);
}

// ---- 5x7 font (column bytes, LSB = top row)
const uint8_t* glyph(char ch)
{
	static const uint8_t digits[10][5] = {{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
		{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E}};
	static const uint8_t letters[26][5] = {{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
		{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
		{0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
		{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
		{0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}};
	static const uint8_t blank[5] = {0, 0, 0, 0, 0};
	static const uint8_t plus[5] = {0x08,0x08,0x3E,0x08,0x08}, minus[5] = {0x08,0x08,0x08,0x08,0x08}, up[5] = {0x04,0x02,0x01,0x02,0x04}, dn[5] = {0x10,0x20,0x40,0x20,0x10},
		lt[5] = {0x08,0x14,0x22,0x41,0x00}, gt[5] = {0x00,0x41,0x22,0x14,0x08}, colon[5] = {0x00,0x36,0x36,0x00,0x00}, slash[5] = {0x20,0x10,0x08,0x04,0x02};
	if (ch >= '0' && ch <= '9') return digits[ch - '0'];
	if (ch >= 'A' && ch <= 'Z') return letters[ch - 'A'];
	if (ch >= 'a' && ch <= 'z') return letters[ch - 'a'];
	switch (ch) { case '+': return plus; case '-': return minus; case '^': return up; case 'v': return dn; case '<': return lt; case '>': return gt; case ':': return colon; case '/': return slash; }
	return blank;
}

void text(const hud::DrawRect& rect, const std::string& s, R box, float r, float g, float b)
{
	if (s.empty()) return;
	int n = (int)s.size();
	int scale = std::max(1, std::min((box.h * 5 / 10) / 7, (box.w - 8) / (6 * n)));
	int tw = (6 * n - 1) * scale, th = 7 * scale;
	int x0 = box.x + (box.w - tw) / 2, y0 = box.y + (box.h - th) / 2;
	for (int i = 0; i < n; i++)
	{
		const uint8_t* gl = glyph(s[i]);
		for (int row = 0; row < 7; row++)
			for (int col = 0; col < 5;)
			{
				if (!(gl[col] >> row & 1)) { col++; continue; }
				int run = 1;
				while (col + run < 5 && (gl[col + run] >> row & 1)) run++;
				rect(x0 + (i * 6 + col) * scale, y0 + row * scale, run * scale, scale, r, g, b);
				col += run;
			}
	}
}

int baseUnit() { return std::max(40, scrH / 15); }   // the button editor panel
int unit() { return baseUnit() * rt::config.buttonSize / 100; }   // buttons (launcher: button size)
// Wide screen: one horizontal row centred at the top (keyboard, EDIT, then the buttons), between the game's top-left
// panel and the minimap; otherwise columns at the screen edges.
bool topRow() { return rt::config.wideScreen; }
R slotRect(int slot, int nslots)
{
	int u = unit(); int w = u * 3 / 2, h = u * 3 / 4, gap = u / 6;
	int total = nslots * w + (nslots - 1) * gap;
	return {(scrW - total) / 2 + slot * (w + gap), u / 4, w, h};
}
R editRect()
{
	if (topRow()) return slotRect(1, 2 + (int)btns.size());
	int u = unit(); int h = u * 3 / 4; return {u / 3, u / 3 + u + u / 4, u * 3 / 2, h};
}
R btnRect(int side, int index)
{
	if (topRow()) return slotRect(2 + index, 2 + std::max((int)btns.size(), index + 1));   // index: position in the row
	int u = unit(); int w = u * 3 / 2, h = u * 3 / 4, gap = u / 6;
	if (side == 0) return {u / 3, editRect().y + (index + 1) * (h + gap), w, h};
	return {scrW - u / 3 - w, u / 3 + index * (h + gap), w, h};
}
bool fits(const R& r) { if (topRow()) return r.x >= scrW * 18 / 100 && r.x + r.w <= scrW * 80 / 100; return r.y + r.h <= scrH - unit() / 4; }
int countSide(int side) { int n = 0; for (auto& b : btns) if (b.side == side) n++; return n; }
// index of the button within its side, for layout
std::vector<std::pair<int, R>> layout()
{
	std::vector<std::pair<int, R>> v;
	int cnt[2] = {0, 0};
	for (size_t i = 0; i < btns.size(); i++)
	{
		R r = topRow() ? btnRect(0, (int)i) : btnRect(btns[i].side, cnt[btns[i].side]++);
		if (fits(r)) v.push_back({(int)i, r});
	}
	return v;
}

// palette geometry
const int kCols = 9, kRows = 5;
R panelRect() { int u = baseUnit(); int c = u, g = u / 8; int w = kCols * (c + g) + g, h = (kRows + 1) * (c + g) + g; return {(scrW - w) / 2, (scrH - h) / 2, w, h}; }
R cellRect(int col, int row, int spanCols = 1)
{
	R p = panelRect(); int u = baseUnit(); int c = u, g = u / 8;
	return {p.x + g + col * (c + g), p.y + g + row * (c + g), spanCols * c + (spanCols - 1) * g, c};
}
bool inside(const R& r, float x, float y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

void save()
{
	FILE* f = fopen(kCfg, "w");
	if (!f) { rt::log("hud: cannot write %s", kCfg); return; }
	for (auto& b : btns) fprintf(f, "%c %d %d %s\n", b.side ? 'R' : 'L', b.vk, b.ctrl ? 1 : 0, b.label.c_str());
	fclose(f);
}
void load()
{
	if (loaded) return;
	loaded = true;
	FILE* f = fopen(kCfg, "r");
	if (!f) return;
	char side, lab[32]; int vk, ctrl;
	while (fscanf(f, " %c %d %d %31s", &side, &vk, &ctrl, lab) == 4) btns.push_back({lab, vk, ctrl != 0, side == 'R' ? 1 : 0});
	fclose(f);
	rt::log("hud: %zu custom buttons loaded", btns.size());
}
}

namespace hud
{
void setScreen(int sw, int sh) { std::lock_guard<std::mutex> g(mu); scrW = sw; scrH = sh; load(); buildPages(); }
bool editingNow() { return editing; }
int buttonUnit() { return unit(); }
bool mouseMode(int b) { std::lock_guard<std::mutex> g(mu); return b >= 0 && b < 2 && mouseLatched[b]; }
bool kbRect(int& x, int& y, int& w, int& h)
{
	if (!topRow()) return false;
	std::lock_guard<std::mutex> g(mu);
	if (btns.empty()) load();
	R r = slotRect(0, 2 + (int)btns.size());
	x = r.x; y = r.y; w = r.w; h = r.h;
	return true;
}
void reserves(int sw, int sh, int& left, int& right)
{
	std::lock_guard<std::mutex> g(mu);
	if (btns.empty()) load();
	if (rt::config.wideScreen) { left = right = 0; (void)sw; return; }   // wide: the picture fills the screen, buttons on top
	int u = std::max(40, sh / 15), col = u / 3 + u * 3 / 2 + u / 4;
	bool r = false;
	for (auto& b : btns) if (b.side) r = true;
	left = col;   // the keyboard button is always on the left
	right = r ? col : 0;
	(void)sw;
}

bool onTouch(int action, int idx, int count, const float* xs, const float* ys, std::vector<KeyEv>& out)
{
	std::lock_guard<std::mutex> g(mu);
	load(); buildPages();
	enum { DOWN = 0, UP = 1, CANCEL = 3, PDOWN = 5, PUP = 6 };
	bool isDown = action == DOWN || action == PDOWN;
	bool isUp = action == UP || action == PUP || action == CANCEL;
	float x = xs[idx], y = ys[idx];
	if (isUp && pressed >= 0 && (action != PUP || idx == pressedPtr))
	{
		const Btn& b = btns[pressed];
		out.push_back({b.vk, false, b.ctrl});
		pressed = -1; pressedPtr = -1;
		return true;
	}
	if (editing)
	{
		if (!isDown) return true;   // the editor swallows everything
		R p = panelRect();
		// header: side / page / done
		if (inside(cellRect(0, 0, 3), x, y)) { addSide ^= 1; return true; }
		if (inside(cellRect(3, 0, 3), x, y)) { page = (page + 1) % (int)pages.size(); return true; }
		if (inside(cellRect(6, 0, 3), x, y)) { editing = false; save(); return true; }
		if (inside(p, x, y))
		{
			auto& pg = pages[page];
			for (size_t i = 0; i < pg.size(); i++)
			{
				if (!inside(cellRect((int)i % kCols, 1 + (int)i / kCols), x, y)) continue;
				R slot = topRow() ? slotRect(2 + (int)btns.size(), 3 + (int)btns.size()) : btnRect(addSide, countSide(addSide));
				if (fits(slot)) { btns.push_back({pg[i].label, pg[i].vk, pg[i].ctrl, addSide}); flash = 10; }
				return true;
			}
			return true;
		}
		for (auto& e : layout())
			if (inside(e.second, x, y)) { btns.erase(btns.begin() + e.first); return true; }
		if (inside(editRect(), x, y)) { editing = false; save(); }
		return true;
	}
	if (isDown)
	{
		if (inside(editRect(), x, y)) { releaseLatched(out); editing = true; page = 0; return true; }
		for (auto& e : layout())
			if (inside(e.second, x, y))
			{
				int m = modIdx(btns[e.first].vk);
				if (m >= 0) { latched[m] = !latched[m]; out.push_back({btns[e.first].vk, latched[m], false}); return true; }
				if (int mb = mouseIdx(btns[e.first].vk); mb >= 0) { mouseLatched[mb] = !mouseLatched[mb]; return true; }
				pressed = e.first; pressedPtr = idx;
				const Btn& b = btns[pressed];
				out.push_back({b.vk, true, b.ctrl});
				return true;
			}
	}
	return false;
}

// frame as four edges, then the fill: a full-size frame under a see-through fill would tint it grey
static void box(const DrawRect& rect, const R& r, float fr, float fg, float fb, float r2, float g2, float b2)
{
	rect(r.x, r.y, r.w, 2, fr, fg, fb); rect(r.x, r.y + r.h - 2, r.w, 2, fr, fg, fb);
	rect(r.x, r.y + 2, 2, r.h - 4, fr, fg, fb); rect(r.x + r.w - 2, r.y + 2, 2, r.h - 4, fr, fg, fb);
	rect(r.x + 2, r.y + 2, r.w - 4, r.h - 4, r2, g2, b2);
}
void draw(const DrawRect& rect)
{
	std::lock_guard<std::mutex> g(mu);
	load(); buildPages();
	{
		// small frames-per-second counter in the top right corner (averaged over about half a second)
		static auto t0 = std::chrono::steady_clock::now();
		static int frames = 0, shown = 0;
		frames++;
		double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (dt >= 0.5) { shown = (int)(frames / dt + 0.5); frames = 0; t0 = std::chrono::steady_clock::now(); }
		int h = std::max(18, scrH / 36), w = h * 2;
		rect(scrW - w - 6, 6, w, h, 0.05f, 0.05f, 0.06f);
		text(rect, std::to_string(shown), {scrW - w - 6, 6, w, h}, shown >= 28 ? 0.45f : 0.95f, shown >= 28 ? 0.95f : (shown >= 20 ? 0.85f : 0.35f), 0.35f);
	}
	R e = editRect();
	box(rect, e, editing ? 0.95f : 0.6f, editing ? 0.75f : 0.6f, editing ? 0.1f : 0.65f, 0.12f, 0.12f, 0.14f);
	text(rect, editing ? "DONE" : "EDIT", {e.x + 2, e.y + 2, e.w - 4, e.h - 4}, 0.9f, 0.9f, 0.92f);
	for (auto& en : layout())
	{
		const Btn& b = btns[en.first]; const R& r = en.second;
		bool on = en.first == pressed;
		int m = modIdx(b.vk);
		if ((m >= 0 && latched[m]) || (mouseIdx(b.vk) >= 0 && mouseLatched[mouseIdx(b.vk)])) { box(rect, r, 1.f, 0.78f, 0.1f, 0.55f, 0.38f, 0.02f); text(rect, b.label, {r.x + 2, r.y + 2, r.w - 4, r.h - 4}, 1.f, 1.f, 1.f); continue; }
		box(rect, r, 0.75f, 0.75f, 0.78f, on ? 0.55f : 0.12f, on ? 0.55f : 0.12f, on ? 0.6f : 0.14f);
		text(rect, b.label, {r.x + 2, r.y + 2, r.w - 4, r.h - 4}, 0.95f, 0.95f, 0.97f);
		if (editing)
		{
			int s = r.h / 3;
			rect(r.x + r.w - s, r.y, s, s, 0.85f, 0.15f, 0.15f);
			text(rect, "X", {r.x + r.w - s, r.y, s, s}, 1.f, 1.f, 1.f);
		}
	}
	if (editing)
	{
		R p = panelRect();
		rect(p.x, p.y, p.w, p.h, 0.7f, 0.7f, 0.75f);
		rect(p.x + 3, p.y + 3, p.w - 6, p.h - 6, 0.06f, 0.06f, 0.08f);
		R a = cellRect(0, 0, 3), b = cellRect(3, 0, 3), d = cellRect(6, 0, 3);
		rect(a.x, a.y, a.w, a.h, addSide ? 0.25f : 0.2f, addSide ? 0.45f : 0.6f, addSide ? 0.9f : 0.3f);
		text(rect, addSide ? "RIGHT" : "LEFT", a, 1.f, 1.f, 1.f);
		rect(b.x, b.y, b.w, b.h, 0.35f, 0.35f, 0.4f);
		text(rect, page == 0 ? "KEYS 1" : "KEYS 2", b, 1.f, 1.f, 1.f);
		rect(d.x, d.y, d.w, d.h, 0.8f, 0.55f, 0.1f);
		text(rect, "DONE", d, 1.f, 1.f, 1.f);
		auto& pg = pages[page];
		for (size_t i = 0; i < pg.size(); i++)
		{
			R c = cellRect((int)i % kCols, 1 + (int)i / kCols);
			rect(c.x, c.y, c.w, c.h, 0.4f, 0.4f, 0.45f);
			rect(c.x + 2, c.y + 2, c.w - 4, c.h - 4, 0.14f, 0.14f, 0.17f);
			text(rect, pg[i].label, c, 0.92f, 0.92f, 0.95f);
		}
		if (flash > 0) { flash--; rect(p.x + p.w / 2 - 20, p.y - 14, 40, 8, 0.2f, 0.9f, 0.3f); }
	}
}
}
