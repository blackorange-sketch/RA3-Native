// Android touch / keys -> Win32 window messages for the guest message pump (PeekMessageA / DispatchMessageA).
#include "input.h"

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
enum { WM_ACTIVATE = 0x6, WM_SETFOCUS = 0x7, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
       WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MOUSEWHEEL = 0x20a, WM_ACTIVATEAPP = 0x1c };

struct Msg { uint32_t hwnd, msg, wp, lp; int64_t readyAt; int32_t x, y; };

std::mutex mu;
std::deque<Msg> q;
int64_t lastReady = 0;
bool keys[256];
int curX = 0, curY = 0;
bool lDown = false, rDown = false;
bool started = false;
bool kbToggle = false;
uint32_t wndProc = 0;

int scrW = 1280, scrH = 720;
bool swallow = false;
int vx = 0, vy = 0, vw = 1, vh = 1, bw = 1280, bh = 720;

int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void push(uint32_t msg, uint32_t wp, uint32_t lp, int gapMs)
{
	int64_t t = std::max(nowMs(), lastReady + gapMs);
	lastReady = t;
	q.push_back({kHwnd, msg, wp, lp, t, curX, curY});
}
uint32_t mouseFlags() { return (lDown ? 1u : 0u) | (rDown ? 2u : 0u) | (keys[0x10] ? 4u : 0u) | (keys[0x11] ? 8u : 0u); }
uint32_t xy() { return (uint32_t)(curX & 0xffff) | ((uint32_t)(curY & 0xffff) << 16); }

void mapTo(float sx, float sy)
{
	int x = (int)((sx - vx) * bw / std::max(1, vw)), y = (int)((sy - vy) * bh / std::max(1, vh));
	curX = std::max(0, std::min(bw - 1, x));
	curY = std::max(0, std::min(bh - 1, y));
}

// gesture state
int prevCount = 0;
int64_t downAt = 0, twoAt = 0, threeAt = 0;
float startX = 0, startY = 0, twoStartY = 0, twoStartX = 0, wheelAcc = 0, lastTwoY = 0;
bool moved = false, twoMoved = false, threeSeen = false;
}

namespace input
{
void setScreen(int sw, int sh) { scrW = sw; scrH = sh; }
void kbButtonRect(int& x, int& y, int& w, int& h) { int u = std::max(48, scrH / 12); x = u / 3; y = u / 3; w = u * 3 / 2; h = u; }

void setView(int x, int y, int w, int h, int backW, int backH) { vx = x; vy = y; vw = w; vh = h; bw = backW; bh = backH; }

bool takeKeyboardToggle() { std::lock_guard<std::mutex> g(mu); bool r = kbToggle; kbToggle = false; return r; }

void onTouch(int action, int idx, int count, const float* xs, const float* ys)
{
	std::lock_guard<std::mutex> g(mu);
	enum { DOWN = 0, UP = 1, MOVE = 2, CANCEL = 3, PDOWN = 5, PUP = 6 };
	int64_t now = nowMs();
	{
		int bx, by, bw2, bh2;
		kbButtonRect(bx, by, bw2, bh2);
		if (action == DOWN && xs[0] >= bx && xs[0] < bx + bw2 && ys[0] >= by && ys[0] < by + bh2) { kbToggle = true; swallow = true; return; }
		if (swallow) { if (action == UP || action == CANCEL) swallow = false; return; }
	}
	if (action == DOWN)
	{
		mapTo(xs[0], ys[0]);
		startX = xs[0]; startY = ys[0]; moved = false; downAt = now; twoMoved = false; threeSeen = false;
		push(WM_MOUSEMOVE, mouseFlags(), xy(), 0);
		lDown = true;
		push(WM_LBUTTONDOWN, mouseFlags(), xy(), 40);   // let the UI see the hover first
	}
	else if (action == PDOWN)
	{
		if (count == 2)
		{
			// a second finger: cancel the left drag, a tap of two fingers becomes a right click
			if (lDown) { lDown = false; push(WM_LBUTTONUP, mouseFlags(), xy(), 30); }
			twoAt = now; twoMoved = false; twoStartX = xs[1]; twoStartY = lastTwoY = (ys[0] + ys[1]) / 2; wheelAcc = 0;
		}
	}
	else if (action == MOVE)
	{
		if (count == 1 && !threeSeen)
		{
			mapTo(xs[0], ys[0]);
			if (fabsf(xs[0] - startX) + fabsf(ys[0] - startY) > 12) moved = true;
			push(WM_MOUSEMOVE, mouseFlags(), xy(), 0);
		}
		else if (count == 2)
		{
			float my = (ys[0] + ys[1]) / 2;
			if (fabsf(my - twoStartY) > 24 || fabsf(xs[1] - twoStartX) > 60) twoMoved = true;
			wheelAcc += my - lastTwoY; lastTwoY = my;
			while (wheelAcc > 40) { wheelAcc -= 40; push(WM_MOUSEWHEEL, (uint32_t)(-120 & 0xffff) << 16, xy(), 0); }
			while (wheelAcc < -40) { wheelAcc += 40; push(WM_MOUSEWHEEL, (uint32_t)(120 & 0xffff) << 16, xy(), 0); }
		}
	}
	else if (action == PUP)
	{
		if (count == 2 && !twoMoved && now - twoAt < 350)
		{
			mapTo(xs[idx == 0 ? 1 : 0], ys[idx == 0 ? 1 : 0]);
			push(WM_MOUSEMOVE, mouseFlags(), xy(), 0);
			rDown = true; push(WM_RBUTTONDOWN, mouseFlags(), xy(), 40);
			rDown = false; push(WM_RBUTTONUP, mouseFlags(), xy(), 40);
		}
	}
	else if (action == UP || action == CANCEL)
	{
		if (lDown) { lDown = false; push(WM_LBUTTONUP, mouseFlags(), xy(), 40); }
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
	bool d = vk < 256 && (vk == 1 ? lDown : vk == 2 ? rDown : keys[vk]);
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
	guest::store<int32_t>(ARG(1), 0); guest::store<int32_t>(ARG(1) + 4, 0); guest::store<int32_t>(ARG(1) + 8, bw); guest::store<int32_t>(ARG(1) + 12, bh);
	SET(1);
}
