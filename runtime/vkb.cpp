// Vulkan renderer, stage 1: see vkb.h
#ifdef __ANDROID__
#include "vkb.h"
#include "rt.h"
#include <android/native_window.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

using rt::log;

namespace vkb
{
namespace
{
std::mutex mu;
std::string why;
VkInstance inst = VK_NULL_HANDLE;
VkPhysicalDevice phys = VK_NULL_HANDLE;
VkDevice dev = VK_NULL_HANDLE;
uint32_t qfam = 0;
VkQueue queue = VK_NULL_HANDLE;
VkSurfaceKHR surf = VK_NULL_HANDLE;
VkSwapchainKHR swap = VK_NULL_HANDLE;
VkFormat fmt = VK_FORMAT_UNDEFINED;
VkExtent2D ext{0, 0};
VkRenderPass rpass = VK_NULL_HANDLE;
std::vector<VkImage> images;
std::vector<VkImageView> views;
std::vector<VkFramebuffer> fbs;
VkCommandPool pool = VK_NULL_HANDLE;
VkCommandBuffer cmd = VK_NULL_HANDLE;
VkFence fence = VK_NULL_HANDLE;
VkSemaphore semAcquire = VK_NULL_HANDLE;
std::vector<VkSemaphore> semDone;   // one per swapchain image: the present of an image may still wait on its semaphore
ANativeWindow* win = nullptr;
bool dirty = false;   // swapchain out of date: rebuilt before the next frame
// staging buffer for the console picture
VkBuffer stage = VK_NULL_HANDLE;
VkDeviceMemory stageMem = VK_NULL_HANDLE;
VkDeviceSize stageSize = 0;
void* stageMap = nullptr;
std::vector<uint32_t> pic;   // the console picture
int picW = 0, picH = 0;
bool picDirty = false;

bool check(VkResult r, const char* what)
{
	if (r == VK_SUCCESS) return true;
	why = std::string(what) + " failed (" + std::to_string((int)r) + ")";
	log("vulkan: %s", why.c_str());
	return false;
}

bool initDevice()
{
	if (dev) return true;
	VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	ai.pApplicationName = "RA3 Native"; ai.apiVersion = VK_API_VERSION_1_1;
	const char* iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &ai; ici.enabledExtensionCount = 2; ici.ppEnabledExtensionNames = iext;
	if (!check(vkCreateInstance(&ici, nullptr, &inst), "vkCreateInstance")) return false;
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(inst, &n, nullptr);
	if (!n) { why = "no Vulkan device"; return false; }
	std::vector<VkPhysicalDevice> pds(n);
	vkEnumeratePhysicalDevices(inst, &n, pds.data());
	phys = pds[0];
	VkPhysicalDeviceProperties pp;
	vkGetPhysicalDeviceProperties(phys, &pp);
	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
	std::vector<VkQueueFamilyProperties> qs(qn);
	vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qs.data());
	qfam = ~0u;
	for (uint32_t i = 0; i < qn; i++) if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
	if (qfam == ~0u) { why = "no graphics queue"; return false; }
	float prio = 1.f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
	const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci; dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dext;
	if (!check(vkCreateDevice(phys, &dci, nullptr, &dev), "vkCreateDevice")) return false;
	vkGetDeviceQueue(dev, qfam, 0, &queue);
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = qfam;
	if (!check(vkCreateCommandPool(dev, &pci, nullptr, &pool), "vkCreateCommandPool")) return false;
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
	if (!check(vkAllocateCommandBuffers(dev, &cai, &cmd), "vkAllocateCommandBuffers")) return false;
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	vkCreateFence(dev, &fci, nullptr, &fence);
	VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	vkCreateSemaphore(dev, &sci, nullptr, &semAcquire);
	log("vulkan: %s, Vulkan %u.%u.%u, driver %u.%u.%u", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion),
		VK_VERSION_MAJOR(pp.driverVersion), VK_VERSION_MINOR(pp.driverVersion), VK_VERSION_PATCH(pp.driverVersion));
	return true;
}

void dropSwapchain()
{
	if (!dev) return;
	vkDeviceWaitIdle(dev);
	for (auto f : fbs) vkDestroyFramebuffer(dev, f, nullptr);
	for (auto v : views) vkDestroyImageView(dev, v, nullptr);
	for (auto x : semDone) vkDestroySemaphore(dev, x, nullptr);
	fbs.clear(); views.clear(); images.clear(); semDone.clear();
	if (rpass) { vkDestroyRenderPass(dev, rpass, nullptr); rpass = VK_NULL_HANDLE; }
	if (swap) { vkDestroySwapchainKHR(dev, swap, nullptr); swap = VK_NULL_HANDLE; }
}

bool makeSwapchain()
{
	VkBool32 sup = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(phys, qfam, surf, &sup);
	if (!sup) { why = "the graphics queue cannot present"; return false; }
	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps);
	uint32_t nf = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, nullptr);
	std::vector<VkSurfaceFormatKHR> fs(nf);
	vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, fs.data());
	if (!nf) { why = "no surface formats"; return false; }
	VkSurfaceFormatKHR sf = fs[0];
	for (auto& f : fs) if (f.format == VK_FORMAT_R8G8B8A8_UNORM) { sf = f; break; }
	fmt = sf.format;
	// the window's own size, landscape; the compositor turns the picture when the display's natural orientation differs
	ext.width = (uint32_t)ANativeWindow_getWidth(win);
	ext.height = (uint32_t)ANativeWindow_getHeight(win);
	if (caps.currentExtent.width != 0xffffffffu && !(caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)))
		ext = caps.currentExtent;
	ext.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, ext.width));
	ext.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, ext.height));
	uint32_t count = std::max(caps.minImageCount, 3u);
	if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
	VkCompositeAlphaFlagBitsKHR ca = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (!(caps.supportedCompositeAlpha & ca)) ca = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	sci.surface = surf; sci.minImageCount = count; sci.imageFormat = sf.format; sci.imageColorSpace = sf.colorSpace;
	sci.imageExtent = ext; sci.imageArrayLayers = 1;
	sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
	sci.compositeAlpha = ca; sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; sci.clipped = VK_TRUE;
	if (!check(vkCreateSwapchainKHR(dev, &sci, nullptr, &swap), "vkCreateSwapchainKHR")) return false;
	uint32_t ni = 0;
	vkGetSwapchainImagesKHR(dev, swap, &ni, nullptr);
	images.resize(ni);
	vkGetSwapchainImagesKHR(dev, swap, &ni, images.data());
	VkAttachmentDescription ad{};
	ad.format = fmt; ad.samples = VK_SAMPLE_COUNT_1_BIT;
	ad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	ad.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ad.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription sd{};
	sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sd.colorAttachmentCount = 1; sd.pColorAttachments = &ar;
	VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rci.attachmentCount = 1; rci.pAttachments = &ad; rci.subpassCount = 1; rci.pSubpasses = &sd;
	if (!check(vkCreateRenderPass(dev, &rci, nullptr, &rpass), "vkCreateRenderPass")) return false;
	for (VkImage img : images)
	{
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
		vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView v; vkCreateImageView(dev, &vci, nullptr, &v); views.push_back(v);
		VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fci.renderPass = rpass; fci.attachmentCount = 1; fci.pAttachments = &v; fci.width = ext.width; fci.height = ext.height; fci.layers = 1;
		VkFramebuffer fb; vkCreateFramebuffer(dev, &fci, nullptr, &fb); fbs.push_back(fb);
		VkSemaphoreCreateInfo sci2{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		VkSemaphore sm; vkCreateSemaphore(dev, &sci2, nullptr, &sm); semDone.push_back(sm);
	}
	log("vulkan: swapchain %ux%u, %u images, format %d, transform %u (surface %u)", ext.width, ext.height, ni, (int)fmt, (unsigned)sci.preTransform, (unsigned)caps.currentTransform);
	dirty = false;
	return true;
}

void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.oldLayout = from; b.newLayout = to; b.srcAccessMask = srcA; b.dstAccessMask = dstA;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool ensureStage(VkDeviceSize size)
{
	if (stageSize >= size) return true;
	if (stage) { vkUnmapMemory(dev, stageMem); vkDestroyBuffer(dev, stage, nullptr); vkFreeMemory(dev, stageMem, nullptr); stage = VK_NULL_HANDLE; stageSize = 0; }
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = size; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (!check(vkCreateBuffer(dev, &bci, nullptr, &stage), "vkCreateBuffer")) return false;
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, stage, &mr);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(phys, &mp);
	uint32_t ti = ~0u;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { ti = i; break; }
	if (ti == ~0u) { why = "no host-visible memory"; return false; }
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size; mai.memoryTypeIndex = ti;
	if (!check(vkAllocateMemory(dev, &mai, nullptr, &stageMem), "vkAllocateMemory")) return false;
	vkBindBufferMemory(dev, stage, stageMem, 0);
	vkMapMemory(dev, stageMem, 0, size, 0, &stageMap);
	stageSize = size;
	return true;
}

// one frame: acquire, record(img), submit, present; the caller holds mu
template <class F> void frame(F record)
{
	if (!swap || !dev) return;
	if (dirty) { dropSwapchain(); if (!makeSwapchain()) return; }
	vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
	uint32_t idx = 0;
	VkResult r = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, semAcquire, VK_NULL_HANDLE, &idx);
	if (r == VK_ERROR_OUT_OF_DATE_KHR) { dirty = true; return; }
	if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { check(r, "vkAcquireNextImageKHR"); return; }
	vkResetFences(dev, 1, &fence);
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);
	record(idx);
	vkEndCommandBuffer(cmd);
	VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = 1; si.pWaitSemaphores = &semAcquire; si.pWaitDstStageMask = &ws;
	si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
	si.signalSemaphoreCount = 1; si.pSignalSemaphores = &semDone[idx];
	if (!check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit")) return;
	VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &semDone[idx]; pi.swapchainCount = 1; pi.pSwapchains = &swap; pi.pImageIndices = &idx;
	r = vkQueuePresentKHR(queue, &pi);
	// SUBOPTIMAL comes every frame on a phone held in landscape: the picture is not pre-rotated (IDENTITY transform),
	// the compositor turns it. A new swapchain would be the same, so only OUT_OF_DATE (window resized) rebuilds it.
	if (r == VK_ERROR_OUT_OF_DATE_KHR) dirty = true;
}
}   // namespace

bool wanted()
{
	static const bool w = rt::config.renderer == "vulkan" || access("/sdcard/ra3_vulkan", F_OK) == 0;
	return w;
}

bool setWindow(void* w)
{
	std::lock_guard<std::mutex> l(mu);
	if (!w)
	{
		dropSwapchain();
		if (surf) { vkDestroySurfaceKHR(inst, surf, nullptr); surf = VK_NULL_HANDLE; }
		win = nullptr;
		return true;
	}
	win = (ANativeWindow*)w;
	if (!initDevice()) return false;
	VkAndroidSurfaceCreateInfoKHR aci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
	aci.window = win;
	if (!check(vkCreateAndroidSurfaceKHR(inst, &aci, nullptr, &surf), "vkCreateAndroidSurfaceKHR")) return false;
	return makeSwapchain();
}

bool ready() { std::lock_guard<std::mutex> l(mu); return swap != VK_NULL_HANDLE; }
int width() { return (int)ext.width; }
int height() { return (int)ext.height; }
const char* failure() { return why.c_str(); }

void postPicture(const uint32_t* rgba, int w, int h)
{
	std::lock_guard<std::mutex> l(mu);
	pic.assign(rgba, rgba + (size_t)w * h);
	picW = w; picH = h; picDirty = true;
}

void present(const Rect* rects, size_t n, bool picture)
{
	std::lock_guard<std::mutex> l(mu);
	picture = picture && !pic.empty();
	if (picture && !ensureStage((VkDeviceSize)picW * picH * 4)) picture = false;
	frame([&](uint32_t idx) {
		VkImage img = images[idx];
		barrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkClearColorValue black{{0.f, 0.f, 0.f, 1.f}};
		VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
		if (picture)
		{
			if (picDirty)   // the previous frame has finished with the buffer (frame() waited for its fence)
			{
				const uint8_t* s = (const uint8_t*)pic.data();
				uint8_t* d = (uint8_t*)stageMap;
				size_t px = (size_t)picW * picH;
				if (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB)
					for (size_t i = 0; i < px; i++) { d[4 * i] = s[4 * i + 2]; d[4 * i + 1] = s[4 * i + 1]; d[4 * i + 2] = s[4 * i]; d[4 * i + 3] = 255; }
				else memcpy(d, s, px * 4);
				picDirty = false;
			}
			VkBufferImageCopy c{};
			c.bufferRowLength = (uint32_t)picW; c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			c.imageExtent = {std::min((uint32_t)picW, ext.width), std::min((uint32_t)picH, ext.height), 1};
			vkCmdCopyBufferToImage(cmd, stage, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		}
		barrier(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rbi.renderPass = rpass; rbi.framebuffer = fbs[idx]; rbi.renderArea = {{0, 0}, ext};
		vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
		// stage 1 has no pipelines yet: the buttons are clears, their transparency folded into the colour as if over black
		for (size_t i = 0; i < n; i++)
		{
			const Rect& q = rects[i];
			int x0 = std::max(0, q.x), y0 = std::max(0, q.y), x1 = std::min((int)ext.width, q.x + q.w), y1 = std::min((int)ext.height, q.y + q.h);
			if (x1 <= x0 || y1 <= y0) continue;
			VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
			ca.clearValue.color = {{q.r * q.a, q.g * q.a, q.b * q.a, 1.f}};
			VkClearRect cr{{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, 0, 1};
			vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
		}
		vkCmdEndRenderPass(cmd);
	});
}
}
#endif
