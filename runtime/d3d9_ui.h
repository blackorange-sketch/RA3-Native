// HUD scaling (d3d9_ui.cpp): the interface panels of a battle drawn larger around the screen corners, and the
// touch mapping back to the game's own coordinates. Renderer-independent: OpenGL and Vulkan both use it.
#pragma once
#include <cstdint>
#include <mutex>
#include <vector>
#include "d3d9.h"
namespace d3d
{
namespace ui
{
extern const uint64_t kAptVs, kVideoVs;   // interface (APT) and video vertex shaders
struct UiRect { float x0, y0, x1, y1, ax, ay, sc; };
extern float gUiScale;
extern bool gUiDiag, gUiNested;
extern uint32_t gUiWorldDraws, gAptDraws, gVideoDraws;   // this frame: depth-tested (world), interface and video draws
extern float gCamRegs[16]; extern uint32_t gCamRegsFrame; extern bool gCamRegsNew;   // camera of the world draws ("drag the map")
// Scale-1 for the u_ui uniform when the whole draw lies in one corner zone (anchor in clip space axN/ayN, pixels
// ax/ay); a draw spread over several zones comes back rebuilt in `split` (non-indexed triangle list).
float uiScaleFor(const DrawCall& dc, const DeclElem* els, uint32_t nEls, float& axN, float& ayN, float& ax, float& ay, std::vector<uint8_t>& split);
void uiUnmapImpl(int& x, int& y);
void uiEndFrame(uint32_t frameNo);   // after a presented frame: this frame's panels become the touch mapping
// Wide screen, once per presented frame (before uiEndFrame): a battle is showing (no side bars), and outside a
// battle whether the bars beside the 16:9 menu are blurred (menus) or black (loading screens, videos)
void sideBars(bool& inGame, bool& blur);
// The on-screen buttons of this frame as rectangles (screen pixels, colour with alpha); also hands the screen size
// and the world camera to input (call once per presented frame)
struct HudRect { int x, y, w, h; float r, g, b, a; };
void hudRects(int screenW, int screenH, std::vector<HudRect>& out);
}
}
