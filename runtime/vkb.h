// Vulkan renderer, stage 1 (docs/VULKAN_PLAN.md): the device and the swapchain on the Android window. The frame is
// still black apart from the on-screen buttons and the start-up console picture; the game's own drawing comes in the
// next stages. Chosen in the launcher (renderer=vulkan) or with /sdcard/ra3_vulkan; when Vulkan cannot start the
// game falls back to OpenGL ES.
#pragma once
#include <cstddef>
#include <cstdint>

namespace vkb
{
bool wanted();                         // the player chose Vulkan
bool setWindow(void* aNativeWindow);   // platform thread: create (non-null) or drop (null) the swapchain; false: cannot start
bool ready();                          // a swapchain exists
int width();
int height();
const char* failure();                 // why setWindow failed
struct Rect { int x, y, w, h; float r, g, b, a; };
void postPicture(const uint32_t* rgba, int w, int h);   // the console picture (kept, shown by present when asked)
void present(const Rect* rects, size_t n, bool picture); // black frame, the console picture if asked, then the rectangles
}
