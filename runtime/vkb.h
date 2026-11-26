// Vulkan renderer, stage 1 (docs/VULKAN_PLAN.md): the device and the swapchain on the Android window. The frame is
// still black apart from the on-screen buttons and the start-up console picture; the game's own drawing comes in the
// next stages. Chosen in the launcher (renderer=vulkan) or with /sdcard/ra3_vulkan; when Vulkan cannot start the
// game falls back to OpenGL ES.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>

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

#ifdef VK_VERSION_1_0
// ---- for the renderer (d3d9_vk.cpp): the device, and frames it records itself
struct Handles
{
	VkInstance instance = VK_NULL_HANDLE; VkPhysicalDevice phys = VK_NULL_HANDLE; VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE; uint32_t family = 0, apiVersion = 0, uboAlign = 256;
	bool bc = false, aniso = false; float maxAniso = 1.f, timestampPeriod = 0.f;   // ns per timestamp tick (0: none)
};
const Handles& handles();
bool rebuildPending();   // the swapchain is rebuilt at the next acquire
std::recursive_mutex& lock();   // held around queue use
// the next swapchain image (signals `signal` when it may be written); false: none (window gone, swapchain rebuilt)
bool acquire(VkSemaphore signal, uint32_t& idx, VkImage& img, VkExtent2D& ext, VkFormat& fmt, uint32_t& generation);
// submits the command buffers (fence signalled when done); presentIdx >= 0: waits for waitAcquire, presents that image
void submit(const VkCommandBuffer* cmds, uint32_t n, VkFence fence, VkSemaphore waitAcquire, int presentIdx);
#endif
}
