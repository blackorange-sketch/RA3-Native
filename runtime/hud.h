// Custom on-screen keyboard buttons (side panels) with an in-game editor.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace hud
{
struct KeyEv { int vk; bool down; bool ctrl; };
using DrawRect = std::function<void(int x, int y, int w, int h, float r, float g, float b)>;

void setScreen(int sw, int sh);
// Touch handling; returns true when the event belongs to the HUD (it is then not passed to the game). Key events to inject go to out.
bool onTouch(int action, int idx, int count, const float* xs, const float* ys, std::vector<KeyEv>& out);
void draw(const DrawRect& rect);
// Performance overlay (launcher option or /sdcard/ra3_overlay): renderers report per-frame times from any thread.
bool overlayOn();
void setGpuNs(uint64_t ns);        // GPU time of one finished frame
void setRenderNs(uint64_t ns);     // renderer CPU time of one frame (Vulkan)
// Screen width taken by the button columns on the left and right (wide screen mode keeps the game picture out of them).
void reserves(int sw, int sh, int& left, int& right);
bool editingNow();
int buttonUnit();
bool mouseMode(int b);   // 0: LMB button lit (drag selects at once), 1: RMB button lit (tap = right click)   // the on-screen buttons' size unit (launcher: button size)   // the button editor is open
// Wide screen: the keyboard button's place in the top row (false: the default top-left corner).
bool kbRect(int& x, int& y, int& w, int& h);
}
