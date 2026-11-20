// Android touch / keys -> Win32 window messages for the guest message pump (PeekMessageA / DispatchMessageA).
#include "input.h"
#include "hud.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <unistd.h>
#include <deque>
#include <mutex>

#include "rt.h"

using namespace rt;

namespace
{
constexpr uint32_t kHwnd = 0x30001;
enum { WM_ACTIVATE = 0x6, WM_SETFOCUS = 0x7, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
       WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208, WM_MOUSEWHEEL = 0x20a, WM_ACTIVATEAPP = 0x1c };

struct Msg { uint32_t hwnd, msg, wp, lp; int64_t readyAt; int32_t x, y; };

std::mutex mu;
std::deque<Msg> q;
int64_t lastReady = 0;
bool keys[256];
int curX = 0, curY = 0;
bool lDown = false, rDown = false, mDown = false;
bool lGot = false, rGot = false, mGot = false;   // button state as of the messages the game has already received
bool started = false;
bool kbToggle = false;
uint32_t wndProc = 0;

int scrW = 1280, scrH = 720;
bool swallow = false;
int vx = 0, vy = 0, vw = 1, vh = 1, bw = 1280, bh = 720;

int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// set-1 scan code (bits 0-7) and extended-key flag (bit 8) of a virtual key, as Windows puts them into lParam
uint32_t scanOf(uint32_t vk)
{
	static const uint8_t letters[26] = {0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c};
	if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
	if (vk >= '1' && vk <= '9') return 0x02 + (vk - '1');
	if (vk == '0') return 0x0b;
	if (vk >= 0x70 && vk <= 0x79) return 0x3b + (vk - 0x70);   // F1..F10
	switch (vk)
	{
	case 0x7a: return 0x57; case 0x7b: return 0x58;          // F11, F12
	case 0x1b: return 0x01; case 0x08: return 0x0e; case 0x09: return 0x0f; case 0x0d: return 0x1c;
	case 0x10: return 0x2a; case 0x11: return 0x1d; case 0x12: return 0x38; case 0x20: return 0x39; case 0x14: return 0x3a;
	case 0xbd: return 0x0c; case 0xbb: return 0x0d; case 0xdb: return 0x1a; case 0xdd: return 0x1b; case 0xba: return 0x27;
	case 0xde: return 0x28; case 0xc0: return 0x29; case 0xdc: return 0x2b; case 0xbc: return 0x33; case 0xbe: return 0x34; case 0xbf: return 0x35;
	case 0x13: return 0x45;
	case 0x24: return 0x147; case 0x26: return 0x148; case 0x21: return 0x149; case 0x25: return 0x14b;   // Home, Up, PgUp, Left
	case 0x27: return 0x14d; case 0x23: return 0x14f; case 0x28: return 0x150; case 0x22: return 0x151;   // Right, End, Down, PgDn
	case 0x2d: return 0x152; case 0x2e: return 0x153;                                                     // Insert, Delete
	}
	return 0;
}

void push(uint32_t msg, uint32_t wp, uint32_t lp, int gapMs)
{
	// keyboard messages carry the scan code like on Windows: the game maps hotkeys by scan code
	if ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && !(lp & 0x01ff0000u)) lp |= (scanOf(wp) & 0x1ff) << 16;
	if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR)
	{
		static int nk = 0;
		if (nk++ < 40) log("input: %s vk/char 0x%x lParam 0x%08x", msg == WM_KEYDOWN ? "KEYDOWN" : msg == WM_KEYUP ? "KEYUP" : "CHAR", wp, lp);
	}
	int64_t t = std::max(nowMs(), lastReady + gapMs);
	lastReady = t;
	q.push_back({kHwnd, msg, wp, lp, t, curX, curY});
}
uint32_t mouseFlags() { return (lDown ? 1u : 0u) | (rDown ? 2u : 0u) | (mDown ? 0x10u : 0u) | (keys[0x10] ? 4u : 0u) | (keys[0x11] ? 8u : 0u); }
uint32_t xy() { return (uint32_t)(curX & 0xffff) | ((uint32_t)(curY & 0xffff) << 16); }

void mapTo(float sx, float sy)
{
	int x = (int)((sx - vx) * bw / std::max(1, vw)), y = (int)((sy - vy) * bh / std::max(1, vh));
	curX = std::max(0, std::min(bw - 1, x));
	curY = std::max(0, std::min(bh - 1, y));
	rt::uiUnmap(curX, curY);   // enlarged HUD panels: back to where the game has them
}

// the minimap in the top right corner of the in-game HUD (the HUD scales with the back buffer height)
bool inRadar(int x, int y) { return x >= bw - bh * 275 / 1000 && x <= bw - bh * 15 / 1000 && y >= bh * 10 / 1000 && y <= bh * 280 / 1000; }

// gesture state
int prevCount = 0;
int64_t downAt = 0, twoAt = 0, threeAt = 0;
float startX = 0, startY = 0, twoStartY = 0, twoStartX = 0, wheelAcc = 0, lastTwoY = 0;
bool twoPan = false;
float lastDist = 0;
bool ctrlSticky = false, altSticky = false;
bool moved = false, twoMoved = false, threeSeen = false, pending = false;
bool onRadar = false;   // the gesture started on the minimap
int dragMode = 0;   // 0 none, 1 left button (selection / UI drag), 2 right button (camera pan), 3 right button held on the minimap
int startCX = 0, startCY = 0;
}


namespace
{
void clickWithMods(bool right)
{
	bool c = ctrlSticky, a = altSticky;
	if (c) { keys[0x11] = true; push(WM_KEYDOWN, 0x11, 1, 0); }
	if (a) { keys[0x12] = true; push(WM_KEYDOWN, 0x12, 1, 0); }
	if (right) { rDown = true; push(WM_RBUTTONDOWN, mouseFlags(), xy(), 40); rDown = false; push(WM_RBUTTONUP, mouseFlags(), xy(), 40); }
	else { lDown = true; push(WM_LBUTTONDOWN, mouseFlags(), xy(), 40); lDown = false; push(WM_LBUTTONUP, mouseFlags(), xy(), 40); }
	if (c) { keys[0x11] = false; push(WM_KEYUP, 0x11, 1u | 0xc0000000u, 0); ctrlSticky = false; }
	if (a) { keys[0x12] = false; push(WM_KEYUP, 0x12, 1u | 0xc0000000u, 0); altSticky = false; }
}
}

namespace
{
constexpr float kPanGain = 0.35f;
float panPX = 0, panPY = 0;
int panVX = 0, panVY = 0;
void panBegin(float sx, float sy) { panPX = sx; panPY = sy; panVX = curX; panVY = curY; }
// relative pan: the cursor moves kPanGain times faster than the finger; near an edge the right button is released and re-grabbed at the centre
void panMove(float sx, float sy, bool mid = false)
{
	uint32_t dn = mid ? WM_MBUTTONDOWN : WM_RBUTTONDOWN, up = mid ? WM_MBUTTONUP : WM_RBUTTONUP, bit = mid ? 0x10u : 2u;
	float dir = (!mid && rt::config.panInvert) ? -1.f : 1.f;   // launcher option: the map follows the finger
	panVX += (int)(dir * (sx - panPX) * bw / std::max(1, vw) * kPanGain); panVY += (int)(dir * (sy - panPY) * bh / std::max(1, vh) * kPanGain);
	panPX = sx; panPY = sy;
	int mx = bw / 8, my = bh / 8;
	if (panVX < mx || panVX > bw - mx || panVY < my || panVY > bh - my)
	{
		curX = std::max(0, std::min(bw - 1, panVX)); curY = std::max(0, std::min(bh - 1, panVY));
		push(WM_MOUSEMOVE, mouseFlags(), xy(), 0);
		push(up, mouseFlags() & ~bit, xy(), 0);
		panVX = bw / 2; panVY = bh / 2; curX = panVX; curY = panVY;
		push(WM_MOUSEMOVE, mouseFlags() & ~bit, xy(), 0);
		push(dn, mouseFlags(), xy(), 0);
		return;
	}
	curX = panVX; curY = panVY;
	push(WM_MOUSEMOVE, mouseFlags(), xy(), 0);
}
}

namespace input
{
void setScreen(int sw, int sh) { scrW = sw; scrH = sh; hud::setScreen(sw, sh); }
void kbButtonRect(int& x, int& y, int& w, int& h) { if (hud::kbRect(x, y, w, h)) return; int u = hud::buttonUnit(); x = u / 3; y = u / 3; w = u * 3 / 2; h = u; }

void modButtonRect(int idx, int& x, int& y, int& w, int& h) { int u = std::max(40, scrH / 15); w = u; h = u * 3 / 4; x = u / 3; y = u / 3 + u + u / 4 + idx * (h + u / 6); }
bool modActive(int idx) { return idx == 0 ? ctrlSticky : altSticky; }
void setBackSize(int w, int h) { bw = w; bh = h; }
void backSize(int& w, int& h) { w = bw; h = bh; }
void setView(int x, int y, int w, int h, int backW, int backH) { vx = x; vy = y; vw = w; vh = h; bw = backW; bh = backH; }

bool takeKeyboardToggle() { std::lock_guard<std::mutex> g(mu); bool r = kbToggle; kbToggle = false; return r; }

void onTouch(int action, int idx, int count, const float* xs, const float* ys)
{
	std::lock_guard<std::mutex> g(mu);
	enum { DOWN = 0, UP = 1, MOVE = 2, CANCEL = 3, PDOWN = 5, PUP = 6 };
	int64_t now = nowMs();
	{
		std::vector<hud::KeyEv> kev;
		bool used = hud::onTouch(action, idx, count, xs, ys, kev);
		for (auto& k : kev)
		{
			// Alt arrives as WM_SYSKEYDOWN/UP with the context bit (29), as on Windows
			uint32_t dn = k.vk == 0x12 ? WM_SYSKEYDOWN : WM_KEYDOWN, up = k.vk == 0x12 ? WM_SYSKEYUP : WM_KEYUP, ctx = k.vk == 0x12 ? 0x20000000u : 0;
			if (k.down) { if (k.ctrl) { keys[0x11] = true; push(WM_KEYDOWN, 0x11, 1, 0); } keys[k.vk] = true; push(dn, k.vk, 1u | ctx, 0); }
			else { keys[k.vk] = false; push(up, k.vk, 1u | ctx | 0xc0000000u, 0); if (k.ctrl) { keys[0x11] = false; push(WM_KEYUP, 0x11, 1u | 0xc0000000u, 0); } }
		}
		if (used) { swallow = (action == 0 || action == 5) ? true : swallow; if (action == 1 || action == 3) swallow = false; return; }
	}
	{
		int bx, by, bw2, bh2;
		kbButtonRect(bx, by, bw2, bh2);
		if (action == DOWN && xs[0] >= bx && xs[0] < bx + bw2 && ys[0] >= by && ys[0] < by + bh2) { kbToggle = true; swallow = true; return; }
		if (swallow) { if (action == UP || action == CANCEL) swallow = false; return; }
	}
	if (action == DOWN)
	{
		mapTo(xs[0], ys[0]);
		log("input: DOWN screen %.0f,%.0f -> game %d,%d (back %dx%d view %d,%d %dx%d)", xs[0], ys[0], curX, curY, bw, bh, vx, vy, vw, vh);
		startX = xs[0]; startY = ys[0]; moved = false; downAt = now; twoMoved = false; threeSeen = false;
		push(WM_MOUSEMOVE, mouseFlags(), xy(), 0);
		// the button press is deferred: a tap clicks, a quick swipe pans the camera, hold-then-drag selects
		pending = true; dragMode = 0; startCX = curX; startCY = curY;
		onRadar = inRadar(curX, curY);
		if (onRadar) { static int nr = 0; if (nr++ < 10) log("input: touch on the minimap at %d,%d", curX, curY); }
	}
	else if (action == PDOWN)
	{
		if (count == 2)
		{
			// a second finger: abandon the one-finger gesture
			if (lDown) { lDown = false; push(WM_LBUTTONUP, mouseFlags(), xy(), 30); }
			if (rDown) { rDown = false; push(WM_RBUTTONUP, mouseFlags(), xy(), 30); }
			if (mDown) { mDown = false; push(WM_MBUTTONUP, mouseFlags(), xy(), 30); twoPan = false; }
			pending = false; dragMode = 0;
			twoAt = now; twoMoved = false; twoPan = false;
			twoStartX = (xs[0] + xs[1]) / 2; twoStartY = (ys[0] + ys[1]) / 2;
			lastDist = hypotf(xs[1] - xs[0], ys[1] - ys[0]); wheelAcc = 0;
		}
	}
	else if (action == MOVE)
	{
		if (count == 1 && !threeSeen)
		{
			mapTo(xs[0], ys[0]);
			if (pending && !moved && fabsf(xs[0] - startX) + fabsf(ys[0] - startY) > std::max(24, scrH / 50))
			{
				moved = true;
				int cx = curX, cy = curY;
				curX = startCX; curY = startCY;
				// minimap: the right button moves the camera there, held while the finger moves
				if (onRadar) { dragMode = 3; rDown = true; push(WM_RBUTTONDOWN, mouseFlags(), xy(), 0); }
				else if (now - downAt >= 250) { dragMode = 1; lDown = true; push(WM_LBUTTONDOWN, mouseFlags(), xy(), 0); }
				else { dragMode = 2; rDown = true; push(WM_RBUTTONDOWN, mouseFlags(), xy(), 0); panBegin(startX, startY); }
				curX = cx; curY = cy;
			}
			if (dragMode == 2) panMove(xs[0], ys[0]);
			else if (pending || dragMode) push(WM_MOUSEMOVE, mouseFlags(), xy(), 0);
		}
		else if (count == 2)
		{
			float cx = (xs[0] + xs[1]) / 2, cy = (ys[0] + ys[1]) / 2, d = hypotf(xs[1] - xs[0], ys[1] - ys[0]);
			if (fabsf(d - lastDist) > std::max(20, scrH / 40)) twoMoved = true;
			// pinch -> wheel
			wheelAcc += d - lastDist; lastDist = d;
			while (wheelAcc > 50) { wheelAcc -= 50; push(WM_MOUSEWHEEL, (uint32_t)(120 & 0xffff) << 16, xy(), 0); }
			while (wheelAcc < -50) { wheelAcc += 50; push(WM_MOUSEWHEEL, (uint32_t)(-120 & 0xffff) << 16, xy(), 0); }
			// centre drift -> camera pan (right-drag)
			if (!twoPan && fabsf(cx - twoStartX) + fabsf(cy - twoStartY) > std::max(60, scrH / 20))
			{
				twoMoved = true; twoPan = true;
				mapTo(twoStartX, twoStartY); push(WM_MOUSEMOVE, mouseFlags(), xy(), 0);
				mDown = true; push(WM_MBUTTONDOWN, mouseFlags(), xy(), 0); panBegin(twoStartX, twoStartY);
			}
			if (twoPan) panMove(cx, cy, true);
		}
	}
	else if (action == PUP)
	{
		if (count == 2 && twoPan) { twoPan = false; if (mDown) { mDown = false; push(WM_MBUTTONUP, mouseFlags(), xy(), 30); } }
		else if (count == 2 && !twoMoved && now - twoAt < 350)
		{
			// a tap of two fingers = cancel / command (right click)
			mapTo(twoStartX, twoStartY);
			push(WM_MOUSEMOVE, mouseFlags(), xy(), 0);
			clickWithMods(true);
		}
	}
	else if (action == UP || action == CANCEL)
	{
		// tap = left click, long press = right click; on the minimap the other way round: a tap (right click) moves the
		// camera, a long press (left click) sends the selected units there
		if (pending && dragMode == 0 && action == UP) { bool lp = now - downAt >= 600; clickWithMods(onRadar ? !lp : lp); }
		if (lDown) { lDown = false; push(WM_LBUTTONUP, mouseFlags(), xy(), 40); }
		if (rDown && (dragMode == 2 || dragMode == 3)) { rDown = false; push(WM_RBUTTONUP, mouseFlags(), xy(), 40); }
		if (mDown) { mDown = false; twoPan = false; push(WM_MBUTTONUP, mouseFlags(), xy(), 40); }
		pending = false; dragMode = 0;
		threeSeen = false;
	}
	prevCount = count;
}

namespace
{
int vkOf(int a, int& chr, bool shift)
{
	chr = 0;
	if (a >= 29 && a <= 54) { int l = a - 29; chr = shift ? 'A' + l : 'a' + l; return 'A' + l; }   // AKEYCODE_A..Z
	if (a >= 7 && a <= 16) { int d = a - 7; static const char sh[] = ")!@#$%^&*("; chr = shift ? sh[d] : '0' + d; return '0' + d; }
	switch (a)
	{
	case 62: chr = ' '; return 0x20;
	case 66: chr = 13; return 0x0d;
	case 67: chr = 8; return 0x08;
	case 111: case 4: return 0x1b;   // ESC / BACK
	case 61: chr = 9; return 0x09;
	case 19: return 0x26; case 20: return 0x28; case 21: return 0x25; case 22: return 0x27;
	case 112: return 0x2e; case 122: return 0x24; case 123: return 0x23; case 92: return 0x21; case 93: return 0x22;
	case 59: case 60: return 0x10; case 113: case 114: return 0x11; case 57: case 58: return 0x12;
	case 69: chr = shift ? '_' : '-'; return 0xbd; case 70: chr = shift ? '+' : '='; return 0xbb;
	case 55: chr = shift ? '<' : ','; return 0xbc; case 56: chr = shift ? '>' : '.'; return 0xbe;
	case 76: chr = shift ? '?' : '/'; return 0xbf; case 74: chr = shift ? ':' : ';'; return 0xba; case 75: chr = shift ? '"' : '\''; return 0xde;
	}
	if (a >= 131 && a <= 142) return 0x70 + (a - 131);   // F1..F12
	return 0;
}
}

void onChar(unsigned cu)
{
	std::lock_guard<std::mutex> g(mu);
	if (cu == '\n' || cu == '\r') return;   // Enter comes through onKey
	int vk = 0;
	if (cu >= 'a' && cu <= 'z') vk = 'A' + (cu - 'a'); else if (cu >= 'A' && cu <= 'Z') vk = cu; else if (cu >= '0' && cu <= '9') vk = cu; else if (cu == ' ') vk = 0x20;
	if (vk) push(WM_KEYDOWN, vk, 1, 0);
	push(WM_CHAR, cu, 1, 0);
	if (vk) push(WM_KEYUP, vk, 1u | 0xc0000000u, 0);
}

void onKey(bool down, int a, int meta)
{
	std::lock_guard<std::mutex> g(mu);
	bool shift = (meta & 0x1) != 0 || keys[0x10];
	int chr;
	int vk = vkOf(a, chr, shift);
	if (!vk) return;
	if (vk == 0x10 || vk == 0x11 || vk == 0x12) { keys[vk] = down; }
	if (down)
	{
		bool rep = keys[vk];
		keys[vk] = true;
		push(WM_KEYDOWN, vk, 1u | (rep ? 0x40000000u : 0u), 0);
		if (chr && !keys[0x11]) push(WM_CHAR, chr, 1, 0);
	}
	else { keys[vk] = false; push(WM_KEYUP, vk, 1u | 0xc0000000u, 0); }
}
}

// ---- guest side: the Win32 message pump
namespace
{
bool pollMsg(Msg& out)
{
	std::lock_guard<std::mutex> g(mu);
	if (!started)
	{
		started = true;
		q.push_front({kHwnd, WM_SETFOCUS, 0, 0, 0, 0, 0});
		q.push_front({kHwnd, WM_ACTIVATE, 1, 0, 0, 0, 0});
		q.push_front({kHwnd, WM_ACTIVATEAPP, 1, 0, 0, 0, 0});
	}
	if (q.empty() || q.front().readyAt > nowMs()) return false;
	out = q.front();
	q.pop_front();
	switch (out.msg)
	{
	case WM_LBUTTONDOWN: lGot = true; break; case WM_LBUTTONUP: lGot = false; break;
	case WM_RBUTTONDOWN: rGot = true; break; case WM_RBUTTONUP: rGot = false; break;
	case WM_MBUTTONDOWN: mGot = true; break; case WM_MBUTTONUP: mGot = false; break;
	}
	return true;
}
void fillMsg(uint32_t p, const Msg& m)
{
	guest::store<uint32_t>(p, m.hwnd); guest::store<uint32_t>(p + 4, m.msg); guest::store<uint32_t>(p + 8, m.wp); guest::store<uint32_t>(p + 12, m.lp);
	guest::store<uint32_t>(p + 16, (uint32_t)nowMs()); guest::store<int32_t>(p + 20, m.x); guest::store<int32_t>(p + 24, m.y);
}
int16_t keyState(uint32_t vk)
{
	std::lock_guard<std::mutex> g(mu);
	// mouse buttons as the game's message queue sees them: a tap is queued as down+up at once, and the game must still
	// see the button held while it handles the down message (minimap camera jump)
	bool d = vk < 256 && (vk == 1 ? lGot : vk == 2 ? rGot : vk == 4 ? mGot : keys[vk]);
	return d ? (int16_t)0x8001 : 0;
}
}

IMP_ID("USER32.dll", "RegisterClassW", RegisterClassW, 4)
{
	uint32_t wc = ARG(0);
	if (wc) { wndProc = guest::load<uint32_t>(wc + 4); log("RegisterClassW: wndproc %x", wndProc); }
	SET(0xc001);
}
IMP_ID("USER32.dll", "PeekMessageA", PeekMessageA, 20)
{
	Msg m;
	if (pollMsg(m))
	{
		if (ARG(0)) fillMsg(ARG(0), m);
		if (!(ARG(4) & 1)) { std::lock_guard<std::mutex> g(mu); q.push_front(m); }   // PM_NOREMOVE
		SET(1);
	}
	else SET(0);
}
IMP_ID("USER32.dll", "GetMessageA", GetMessageA, 16)
{
	Msg m;
	for (int i = 0; i < 10 && !pollMsg(m); i++) usleep(1000);
	if (ARG(0)) { Msg n = m; if (m.hwnd == 0) n = Msg{0, 0, 0, 0, 0, 0, 0}; fillMsg(ARG(0), n); }
	SET(1);
}
IMP_ID("USER32.dll", "DispatchMessageA", DispatchMessageA, 4)
{
	uint32_t p = ARG(0);
	uint32_t msg = guest::load<uint32_t>(p + 4);
	if (wndProc && guest::load<uint32_t>(p) && msg)
	{
		uint32_t r = callGuestRet(c, wndProc, {guest::load<uint32_t>(p), msg, guest::load<uint32_t>(p + 8), guest::load<uint32_t>(p + 12)}, 16);
		SET(r);
	}
	else SET(0);
}
IMP_ID("USER32.dll", "GetKeyState", GetKeyState, 4) { SET((uint32_t)(int32_t)keyState(ARG(0))); }
IMP_ID("USER32.dll", "GetAsyncKeyState", GetAsyncKeyState, 4) { SET((uint32_t)(int32_t)keyState(ARG(0))); }
IMP_ID("USER32.dll", "GetClientRect", GetClientRect, 8)
{
	{ static int n = 0; if (n++ < 40) log("GetClientRect -> %dx%d", bw, bh); }
	guest::store<int32_t>(ARG(1), 0); guest::store<int32_t>(ARG(1) + 4, 0); guest::store<int32_t>(ARG(1) + 8, bw); guest::store<int32_t>(ARG(1) + 12, bh);
	SET(1);
}
