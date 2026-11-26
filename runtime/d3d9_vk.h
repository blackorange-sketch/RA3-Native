// Vulkan renderer (docs/VULKAN_PLAN.md, stages 3-5): the same entry points as the OpenGL one (d3d9_gl.cpp), which
// forwards to these when the Vulkan renderer runs (gfx::vulkan()).
#pragma once
#include "d3d9.h"
namespace d3d
{
namespace vkr
{
bool init();   // false: cannot draw with Vulkan (logged)
void clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
void draw(const DrawCall& dc);
void stretchRect(Obj* src, const int32_t* srcRect, Obj* dst, const int32_t* dstRect, uint32_t filter);
// the frame: the back buffer letterboxed into the screen, side bars, the on-screen buttons; false: nothing drawn
// yet (the caller shows the start-up console instead)
bool present(int screenW, int screenH);
uint32_t drawsThisFrame();
}
}
