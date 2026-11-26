// Vulkan renderer: Direct3D 9 draws recorded into Vulkan command buffers (see d3d9_vk.h, docs/VULKAN_PLAN.md).
//
// Frames: two in flight, each with its own command buffers, a host-visible ring (vertices and indices of dynamic
// buffers, shader constants), descriptor pools and a list of objects to destroy once the GPU has finished it.
// Texture uploads go into a separate command buffer submitted before the frame's draws, so they never break a render
// pass; an image the frame already used is replaced by a new one instead of being overwritten.
// Render passes use dynamic rendering (Vulkan 1.3): a pass starts when the target changes and ends before copies,
// layout changes and the present. Depth/stencil, cull mode, topology, viewport and the like are dynamic state, so a
// pipeline is one per shader pair, vertex layout, blend state and target formats.
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include "d3d9_vk.h"
#include "d3d9_sh.h"
#include "d3d9_tex.h"
#include "d3d9_ui.h"
#include "hud.h"
#include "input.h"
#include "rt.h"
#include "vkb.h"
#include "vksh.h"
#include "third_party/vma/vk_mem_alloc.h"
#include <algorithm>
#include <sys/stat.h>
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

using rt::log;

namespace d3d
{
namespace glr { uint32_t frameNo(); }
namespace vkr
{
using namespace ui;
using namespace tex;
namespace
{
// ---- Vulkan 1.3 entry points (Android's libvulkan for API 29 exports 1.1 only): loaded from the device
PFN_vkCmdBeginRendering pBeginRendering;
PFN_vkCmdEndRendering pEndRendering;
PFN_vkCmdSetCullMode pSetCullMode;
PFN_vkCmdSetFrontFace pSetFrontFace;
PFN_vkCmdSetPrimitiveTopology pSetTopology;
PFN_vkCmdSetDepthTestEnable pSetDepthTest;
PFN_vkCmdSetDepthWriteEnable pSetDepthWrite;
PFN_vkCmdSetDepthCompareOp pSetDepthCompare;
PFN_vkCmdSetStencilTestEnable pSetStencilTest;
PFN_vkCmdSetStencilOp pSetStencilOp;
PFN_vkCmdSetDepthBiasEnable pSetDepthBiasEnable;

std::recursive_mutex gMu;
bool gInit = false, gOk = false;
VkDevice D = VK_NULL_HANDLE;
VmaAllocator gA = VK_NULL_HANDLE;
uint32_t gUboAlign = 256;
VkFormat gDepthFmt = VK_FORMAT_D24_UNORM_S8_UINT;
VkDescriptorSetLayout gConstLayout = VK_NULL_HANDLE, gTexLayout = VK_NULL_HANDLE;
VkPipelineLayout gLayout = VK_NULL_HANDLE;
VkDescriptorPool gConstPool = VK_NULL_HANDLE;
VkPipelineCache gPipeCache = VK_NULL_HANDLE;
uint32_t gNewPipes = 0;
uint64_t gSerial = 1, gDone = 0;   // serial of the frame being recorded / of the last one the GPU finished
uint32_t gDraws = 0, gDrawsTotal = 0;

struct Push { float ui[4]; float hp[2]; int32_t bgra; uint32_t vsb; float at[4]; uint32_t psb; };
static_assert(sizeof(Push) == shdr::kVkPushSize, "push constant layout");

// ---- images and buffers
struct VTex
{
	VkImage img = VK_NULL_HANDLE;
	VmaAllocation mem = VK_NULL_HANDLE;
	VkFormat fmt = VK_FORMAT_UNDEFINED;
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	VkImageViewType vtype = VK_IMAGE_VIEW_TYPE_2D;
	uint32_t w = 1, h = 1, d = 1, levels = 1, layers = 1;
	VkImageView sampled = VK_NULL_HANDLE;
	std::unordered_map<uint32_t, VkImageView> att;   // attachment views: level | layer << 8
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	uint64_t useSerial = 0;     // last frame whose commands use it
	uint32_t ver = 0;           // uploaded content version
	bool rt = false, depth = false, stencil = false, noFilter = false;
	VkComponentMapping swz{};
	uint32_t content = 0;       // render target: bumped by every draw / clear into it
	VTex* fb = nullptr;         // its copy for draws that sample the target they draw to (water reads the scene)
	uint32_t fbContent = ~0u;
};
struct VBuf
{
	VkBuffer buf = VK_NULL_HANDLE;
	VmaAllocation mem = VK_NULL_HANDLE;
	uint8_t* map = nullptr;
	uint32_t size = 0, ver = 0;
	uint64_t useSerial = 0;
};

// ---- per-frame resources
struct Chunk { VkBuffer buf = VK_NULL_HANDLE; VmaAllocation mem = VK_NULL_HANDLE; uint8_t* map = nullptr; VkDeviceSize size = 0, pos = 0; VkDescriptorSet constSet = VK_NULL_HANDLE; };
struct Dump { VkBuffer buf; VmaAllocation mem; uint32_t w, h; VkFormat fmt; std::string name; };
struct Frame
{
	std::vector<Dump> dumps;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE, up = VK_NULL_HANDLE;
	bool upUsed = false;
	VkFence fence = VK_NULL_HANDLE;
	VkSemaphore acq = VK_NULL_HANDLE;
	std::vector<VkDescriptorPool> texPools;
	size_t texPool = 0;
	std::vector<Chunk> chunks;
	size_t chunk = 0;
	std::vector<std::function<void()>> trash;
	uint64_t serial = 0;
};
const int kFrames = 2;
Frame gF[kFrames];
int gFi = 0;
Frame* F = nullptr;
VkCommandBuffer C = VK_NULL_HANDLE;
const VkDeviceSize kChunk = 16u << 20;

void later(std::function<void()> f) { F->trash.push_back(std::move(f)); }

bool vkOk(VkResult r, const char* what)
{
	if (r == VK_SUCCESS) return true;
	static int n = 0;
	if (n++ < 30) log("vulkan: %s failed (%d)", what, (int)r);
	return false;
}

// ---- ring allocations (vertices, indices, constants) from the frame's chunks
struct RingPos { VkBuffer buf; VkDeviceSize off; uint8_t* ptr; size_t chunk; };
bool newChunk(Frame& f)
{
	Chunk c;
	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = kChunk;
	bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo inf;
	if (!vkOk(vmaCreateBuffer(gA, &bi, &ai, &c.buf, &c.mem, &inf), "ring buffer")) return false;
	c.map = (uint8_t*)inf.pMappedData; c.size = kChunk;
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = gConstPool; dai.descriptorSetCount = 1; dai.pSetLayouts = &gConstLayout;
	if (!vkOk(vkAllocateDescriptorSets(D, &dai, &c.constSet), "constant set")) return false;
	VkDescriptorBufferInfo b[2] = {{c.buf, 0, shdr::kVkConstRange}, {c.buf, 0, shdr::kVkConstRange}};
	VkWriteDescriptorSet w[2] = {};
	for (int i = 0; i < 2; i++)
	{
		w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = c.constSet; w[i].dstBinding = i; w[i].descriptorCount = 1;
		w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[i].pBufferInfo = &b[i];
	}
	vkUpdateDescriptorSets(D, 2, w, 0, nullptr);
	f.chunks.push_back(c);
	static int n = 0;
	if (n++ < 6 || f.chunks.size() > 4) log("vulkan: frame ring chunk #%zu (%u MB)", f.chunks.size(), (unsigned)(kChunk >> 20));
	return true;
}
bool ringAlloc(VkDeviceSize size, VkDeviceSize align, RingPos& out, VkDeviceSize reserve = 0)
{
	if (size + reserve > kChunk) return false;
	for (;;)
	{
		if (F->chunk >= F->chunks.size() && !newChunk(*F)) return false;
		Chunk& c = F->chunks[F->chunk];
		VkDeviceSize at = (c.pos + align - 1) / align * align;
		if (at + size + reserve <= c.size)
		{
			c.pos = at + size;
			out = {c.buf, at, c.map + at, F->chunk};
			return true;
		}
		F->chunk++;
	}
}

// ---- formats
struct Fmt { VkFormat vk = VK_FORMAT_B8G8R8A8_UNORM; VkComponentMapping swz{}; bool convert = false, bc = false, depth = false, stencil = false, none = false, noFilter = false; };
Fmt fmtOf(uint32_t f)
{
	Fmt t;
	auto sw = [&](VkComponentSwizzle r, VkComponentSwizzle g, VkComponentSwizzle b, VkComponentSwizzle a) { t.swz = {r, g, b, a}; };
	const auto R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, Z = VK_COMPONENT_SWIZZLE_ZERO, O = VK_COMPONENT_SWIZZLE_ONE, I = VK_COMPONENT_SWIZZLE_IDENTITY;
	switch (f)
	{
	case 21: t.vk = VK_FORMAT_B8G8R8A8_UNORM; break;                    // A8R8G8B8: B, G, R, A in memory
	case 22: t.vk = VK_FORMAT_B8G8R8A8_UNORM; sw(I, I, I, O); break;    // X8R8G8B8
	case 23: t.vk = VK_FORMAT_R5G6B5_UNORM_PACK16; break;
	case 25: t.vk = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
	case 24: t.vk = VK_FORMAT_A1R5G5B5_UNORM_PACK16; sw(I, I, I, O); break;
	case 28: t.vk = VK_FORMAT_R8_UNORM; sw(Z, Z, Z, R); break;          // A8
	case 50: t.vk = VK_FORMAT_R8_UNORM; sw(R, R, R, O); break;          // L8
	case 51: t.vk = VK_FORMAT_R8G8_UNORM; sw(R, R, R, G); break;        // A8L8
	case 111: t.vk = VK_FORMAT_R16_SFLOAT; break;
	case 112: t.vk = VK_FORMAT_R16G16_SFLOAT; break;
	case 113: t.vk = VK_FORMAT_R16G16B16A16_SFLOAT; break;
	case 114: t.vk = VK_FORMAT_R32_SFLOAT; t.noFilter = true; break;
	case 115: t.vk = VK_FORMAT_R32G32_SFLOAT; t.noFilter = true; break;
	case 116: t.vk = VK_FORMAT_R32G32B32A32_SFLOAT; t.noFilter = true; break;
	case 0x31545844: t.vk = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; t.bc = true; break;
	case 0x33545844: t.vk = VK_FORMAT_BC2_UNORM_BLOCK; t.bc = true; break;
	case 0x35545844: t.vk = VK_FORMAT_BC3_UNORM_BLOCK; t.bc = true; break;
	case 80: t.vk = VK_FORMAT_D16_UNORM; t.depth = true; break;
	case 71: t.vk = VK_FORMAT_D32_SFLOAT; t.depth = true; break;
	case 75: case 77: case 0x5a544e49: case 0x34324644: case 0x36314644: t.vk = gDepthFmt; t.depth = true; t.stencil = true; break;
	case 0x4c4c554e: t.none = true; break;   // "NULL" render target
	default: t.vk = VK_FORMAT_R8G8B8A8_UNORM; t.convert = true; break;   // expanded to RGBA8 on the CPU (expandLevel)
	}
	if (t.bc && !vkb::handles().bc) { t.bc = false; t.convert = true; t.vk = VK_FORMAT_R8G8B8A8_UNORM; }
	return t;
}

VkImageView makeView(VTex* t, VkImageViewType type, uint32_t level, uint32_t nLevels, uint32_t layer, uint32_t nLayers, VkImageAspectFlags aspect, VkComponentMapping swz)
{
	VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = t->img; vi.viewType = type; vi.format = t->fmt; vi.components = swz;
	vi.subresourceRange = {aspect, level, nLevels, layer, nLayers};
	VkImageView v = VK_NULL_HANDLE;
	vkOk(vkCreateImageView(D, &vi, nullptr, &v), "vkCreateImageView");
	return v;
}

void destroyTex(VTex* t)
{
	if (!t) return;
	if (t->fb) { destroyTex(t->fb); t->fb = nullptr; }
	VkImageView s = t->sampled; auto att = t->att; VkImage img = t->img; VmaAllocation m = t->mem;
	later([s, att, img, m] {
		if (s) vkDestroyImageView(D, s, nullptr);
		for (auto& kv : att) vkDestroyImageView(D, kv.second, nullptr);
		if (img) vmaDestroyImage(gA, img, m);
	});
	delete t;
}
void destroyBuf(VBuf* b)
{
	if (!b) return;
	VkBuffer buf = b->buf; VmaAllocation m = b->mem;
	later([buf, m] { vmaDestroyBuffer(gA, buf, m); });
	delete b;
}

VTex* createTex(uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t layers, bool cube, bool vol, const Fmt& f, bool rt, VkComponentMapping swz)
{
	VTex* t = new VTex;
	t->fmt = f.vk; t->w = w; t->h = h; t->d = d; t->levels = std::max(1u, levels); t->layers = layers; t->rt = rt;
	t->depth = f.depth; t->stencil = f.stencil; t->noFilter = f.noFilter; t->swz = swz;
	t->aspect = f.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
	VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = vol ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	ii.format = f.vk; ii.extent = {w, h, vol ? d : 1}; ii.mipLevels = t->levels; ii.arrayLayers = layers;
	ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (rt) ii.usage |= f.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (cube) ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	if (!vkOk(vmaCreateImage(gA, &ii, &ai, &t->img, &t->mem, nullptr), "vmaCreateImage"))
	{
		log("vulkan: image %ux%ux%u fmt %d levels %u layers %u rt %d could not be made", w, h, d, (int)f.vk, levels, layers, (int)rt);
		delete t;
		return nullptr;
	}
	t->vtype = cube ? VK_IMAGE_VIEW_TYPE_CUBE : vol ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
	// sampled: depth only for a depth/stencil image
	t->sampled = makeView(t, t->vtype, 0, t->levels, 0, layers, f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, f.depth ? VkComponentMapping{} : swz);
	return t;
}
VkImageView attView(VTex* t, uint32_t level, uint32_t layer)
{
	uint32_t k = level | layer << 8;
	auto it = t->att.find(k);
	if (it != t->att.end()) return it->second;
	VkImageView v = makeView(t, VK_IMAGE_VIEW_TYPE_2D, level, 1, layer, 1, t->aspect, VkComponentMapping{});
	t->att[k] = v;
	return v;
}

// ---- layouts: one layout per image (all levels and layers), full barriers (correct first, fast later)
void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, uint32_t level = 0, uint32_t nLevels = VK_REMAINING_MIP_LEVELS)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = from; b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img; b.subresourceRange = {aspect, level, nLevels, 0, VK_REMAINING_ARRAY_LAYERS};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// ---- render pass state
struct Target { VTex* color = nullptr; uint32_t cLevel = 0, cLayer = 0; VTex* depth = nullptr; uint32_t w = 0, h = 0; };
bool gInPass = false;
Target gPass;
// ---- frame dump (/sdcard/ra3_vkdump, optionally holding a frame number; default 1800): every render pass of that
// frame saved to /sdcard/ra3_dump/vkNN_*.bmp when it ends, and its draws logged
uint32_t gDumpFrame = 0;
int gDumpPass = 0;
bool dumping()
{
	static const uint32_t f = [] {
		uint32_t n = 0;
		if (FILE* fp = fopen("/sdcard/ra3_vkdump", "r")) { if (fscanf(fp, "%u", &n) != 1 || n < 10) n = 1800; fclose(fp); mkdir("/sdcard/ra3_dump", 0777); log("vulkan: frame %u will be dumped to /sdcard/ra3_dump", n); }
		return n;
	}();
	gDumpFrame = f;
	return f && glr::frameNo() == f;
}
void dumpTarget(VTex* t, uint32_t level, uint32_t layer, const char* what)
{
	if (!t || t->depth || gDumpPass >= 60) return;
	uint32_t w = std::max(1u, t->w >> level), h = std::max(1u, t->h >> level);
	uint32_t bpp = t->fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : t->fmt == VK_FORMAT_R32G32B32A32_SFLOAT ? 16 : (t->fmt == VK_FORMAT_R16_SFLOAT || t->fmt == VK_FORMAT_R5G6B5_UNORM_PACK16) ? 2 : 4;
	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = (VkDeviceSize)w * h * bpp; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_AUTO; ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	Dump d; d.w = w; d.h = h; d.fmt = t->fmt;
	if (vmaCreateBuffer(gA, &bi, &ai, &d.buf, &d.mem, nullptr) != VK_SUCCESS) return;
	VkImageLayout l = t->layout;
	barrier(C, t->img, t->aspect, l, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy c{};
	c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}; c.imageExtent = {w, h, 1};
	vkCmdCopyImageToBuffer(C, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d.buf, 1, &c);
	barrier(C, t->img, t->aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l);
	char nm[96];
	snprintf(nm, sizeof nm, "/sdcard/ra3_dump/vk%02d_%s_%ux%u_f%d.bmp", gDumpPass++, what, w, h, (int)t->fmt);
	d.name = nm;
	F->dumps.push_back(d);
}
float hf(uint16_t h)
{
	uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 1023, f;
	if (e == 0) f = s; else if (e == 31) f = s | 0x7f800000u; else f = s | (e - 15 + 127) << 23 | m << 13;
	float r; memcpy(&r, &f, 4); return r;
}
void writeDumps(Frame& f)
{
	for (Dump& d : f.dumps)
	{
		VmaAllocationInfo inf; vmaGetAllocationInfo(gA, d.mem, &inf);
		vmaInvalidateAllocation(gA, d.mem, 0, VK_WHOLE_SIZE);
		const uint8_t* px = (const uint8_t*)inf.pMappedData;
		uint32_t sc = d.w > 1200 ? 2 : 1, ow = d.w / sc, oh = d.h / sc, rowB = (ow * 3 + 3) & ~3u, imgB = rowB * oh;
		std::vector<uint8_t> bmp(54 + imgB, 0);
		auto w32 = [&](size_t o, uint32_t v) { memcpy(&bmp[o], &v, 4); };
		bmp[0] = 'B'; bmp[1] = 'M'; w32(2, 54 + imgB); w32(10, 54); w32(14, 40); w32(18, ow); w32(22, oh); bmp[26] = 1; bmp[28] = 24; w32(34, imgB);
		float lo = 1e30f, hi = -1e30f;
		if (d.fmt == VK_FORMAT_R32_SFLOAT)
			for (uint32_t i = 0; i < d.w * d.h; i++) { float v; memcpy(&v, px + 4 * i, 4); if (v == v && v > -1e20f && v < 1e20f) { lo = std::min(lo, v); hi = std::max(hi, v); } }
		for (uint32_t y = 0; y < oh; y++)
			for (uint32_t x = 0; x < ow; x++)
			{
				size_t i = (size_t)(sc * y) * d.w + sc * x;
				float r = 0, g = 0, b = 0;
				switch (d.fmt)
				{
				case VK_FORMAT_B8G8R8A8_UNORM: b = px[4 * i] / 255.f; g = px[4 * i + 1] / 255.f; r = px[4 * i + 2] / 255.f; break;
				case VK_FORMAT_R8G8B8A8_UNORM: r = px[4 * i] / 255.f; g = px[4 * i + 1] / 255.f; b = px[4 * i + 2] / 255.f; break;
				case VK_FORMAT_R16G16B16A16_SFLOAT: { uint16_t v[4]; memcpy(v, px + 8 * i, 8); r = hf(v[0]); g = hf(v[1]); b = hf(v[2]); break; }
				case VK_FORMAT_R32_SFLOAT: { float v; memcpy(&v, px + 4 * i, 4); r = g = b = hi > lo ? (v - lo) / (hi - lo) : 0.f; break; }
				case VK_FORMAT_R16G16_SFLOAT: { uint16_t v[2]; memcpy(v, px + 4 * i, 4); r = hf(v[0]); g = hf(v[1]); break; }
				default: { uint8_t v = px[4 * i]; r = g = b = v / 255.f; break; }
				}
				uint8_t* o = &bmp[54 + (size_t)(oh - 1 - y) * rowB + x * 3];
				auto cl = [](float v) { return (uint8_t)std::max(0.f, std::min(255.f, v * 255.f)); };
				o[0] = cl(b); o[1] = cl(g); o[2] = cl(r);
			}
		writeWhole(d.name, bmp);
		log("vulkan: dumped %s%s", d.name.c_str(), d.fmt == VK_FORMAT_R32_SFLOAT ? (" (R32F range " + std::to_string(lo) + ".." + std::to_string(hi) + ")").c_str() : "");
		vmaDestroyBuffer(gA, d.buf, d.mem);
	}
	f.dumps.clear();
}

void endPass()
{
	if (!gInPass) return;
	pEndRendering(C);
	gInPass = false;
	if (dumping()) dumpTarget(gPass.color, gPass.cLevel, gPass.cLayer, "pass");
}
void toLayout(VTex* t, VkImageLayout l)
{
	if (t->layout == l) return;
	endPass();
	barrier(C, t->img, t->aspect, t->layout, l);
	t->layout = l;
}

// ---- back buffer
VTex* gBack = nullptr;
VTex* gBackDepth = nullptr;
void ensureBack()
{
	if (gBack && gBack->w == backW && gBack->h == backH) return;
	endPass();
	destroyTex(gBack); destroyTex(gBackDepth);
	Fmt c; c.vk = VK_FORMAT_B8G8R8A8_UNORM;
	Fmt d; d.vk = gDepthFmt; d.depth = true; d.stencil = true;
	gBack = createTex(backW, backH, 1, 1, 1, false, false, c, true, VkComponentMapping{});
	gBackDepth = createTex(backW, backH, 1, 1, 1, false, false, d, true, VkComponentMapping{});
	log("vulkan: back buffer %ux%u", backW, backH);
}

// ---- textures of the game
Obj* texOwner(Obj* o) { if (o->iface == ISurface && o->parent) if (Obj* p = objectAt(o->parent)) return p; return o; }
bool isBack(Obj* rt) { return !rt || rt == gBackBuffer; }

void upload(Obj* o, VTex* t, const Fmt& f)
{
	// staging: every level and face of the guest copy (DXT as BC, the "convert" formats expanded to RGBA8)
	struct Reg { VkDeviceSize off; uint32_t level, layer, w, h, d; };
	std::vector<Reg> regs;
	std::vector<uint8_t> tmp;
	VkDeviceSize total = 0;
	auto lvl = [&](uint32_t l, uint32_t& w, uint32_t& h, uint32_t& d) { w = std::max(1u, o->w >> l); h = std::max(1u, o->h >> l); d = std::max(1u, o->d >> l); };
	for (uint32_t fc = 0; fc < o->faces; fc++)
		for (uint32_t l = 0; l < o->levels; l++)
		{
			uint32_t w, h, d; lvl(l, w, h, d);
			VkDeviceSize sz = f.convert ? (VkDeviceSize)w * h * d * 4 : (VkDeviceSize)levelBytes(o->fmt, w, h) * d;
			total = (total + 15) & ~(VkDeviceSize)15;
			regs.push_back({total, l, fc, w, h, d});
			total += sz;
		}
	VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = std::max<VkDeviceSize>(total, 16); bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO; ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VkBuffer sb; VmaAllocation sm; VmaAllocationInfo inf;
	if (!vkOk(vmaCreateBuffer(gA, &bi, &ai, &sb, &sm, &inf), "staging buffer")) return;
	uint8_t* map = (uint8_t*)inf.pMappedData;
	std::vector<VkBufferImageCopy> copies;
	for (auto& r : regs)
	{
		const uint8_t* src = (const uint8_t*)guest::host(o->data + o->levelOff[r.layer * o->levels + r.level]);
		if (f.convert)
		{
			size_t slice = (size_t)levelBytes(o->fmt, r.w, r.h);
			for (uint32_t z = 0; z < r.d; z++)
			{
				expandLevel(o->fmt, src + z * slice, r.w, r.h, tmp);
				memcpy(map + r.off + (size_t)z * r.w * r.h * 4, tmp.data(), (size_t)r.w * r.h * 4);
			}
		}
		else memcpy(map + r.off, src, (size_t)levelBytes(o->fmt, r.w, r.h) * r.d);
		VkBufferImageCopy c{};
		c.bufferOffset = r.off;
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, r.level, r.layer, 1};
		c.imageExtent = {r.w, r.h, r.d};
		copies.push_back(c);
	}
	if (!F->upUsed)
	{
		VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(F->up, &b);
		F->upUsed = true;
	}
	barrier(F->up, t->img, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	vkCmdCopyBufferToImage(F->up, sb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)copies.size(), copies.data());
	barrier(F->up, t->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	later([sb, sm] { vmaDestroyBuffer(gA, sb, sm); });
}

VTex* ensureTex(Obj* o0)
{
	Obj* o = texOwner(o0);
	o->lastUse = o0->lastUse = glr::frameNo();
	if (o->iface == ISurface && !o->rtTex && o->levelOff.empty()) o->rtTex = true;   // stand-alone surface: only as an attachment
	VTex* t = (VTex*)o->vk;
	Fmt f = fmtOf(o->fmt);
	if (f.none) return nullptr;
	if (!t)
	{
		bool cube = o->iface == ICube, vol = o->iface == IVolume;
		Fmt cf = f;
		if (o->rtTex && !f.depth && (f.convert || f.bc)) { cf.vk = VK_FORMAT_B8G8R8A8_UNORM; cf.convert = cf.bc = false; }
		t = createTex(o->w, o->h, o->d, o->levels, cube ? 6 : 1, cube, vol, cf, o->rtTex, f.swz);
		if (!t) return nullptr;
		o->vk = t;
		if (o->rtTex) t->ver = o->ver;
	}
	if (!o->rtTex && t->ver != o->ver && o->data)
	{
		if (t->useSerial == gSerial)
		{
			// this frame already draws with the old contents: a new image for the new ones
			VTex* n = createTex(t->w, t->h, t->d, t->levels, t->layers, t->vtype == VK_IMAGE_VIEW_TYPE_CUBE, t->vtype == VK_IMAGE_VIEW_TYPE_3D, f, false, f.swz);
			if (n) { destroyTex(t); t = n; o->vk = t; }
		}
		upload(o, t, f);
		t->ver = o->ver;
	}
	return t;
}

VBuf* ensureBuf(Obj* o, VkBufferUsageFlags usage)
{
	o->lastUse = glr::frameNo();
	VBuf* b = (VBuf*)o->vk;
	if (b && b->size < o->size) { destroyBuf(b); b = nullptr; o->vk = nullptr; }
	auto make = [&]() -> VBuf* {
		VBuf* n = new VBuf;
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = std::max(16u, o->size); bi.usage = usage;
		VmaAllocationCreateInfo ai{};
		ai.usage = VMA_MEMORY_USAGE_AUTO; ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo inf;
		if (!vkOk(vmaCreateBuffer(gA, &bi, &ai, &n->buf, &n->mem, &inf), "buffer")) { delete n; return nullptr; }
		n->map = (uint8_t*)inf.pMappedData; n->size = std::max(16u, o->size);
		return n;
	};
	if (!b) { b = make(); if (!b) return nullptr; o->vk = b; b->ver = 0; }
	if (b->ver != o->ver && o->data)
	{
		bool inFlight = b->useSerial > gDone;
		if (inFlight)
		{
			VBuf* n = make();
			if (n) { destroyBuf(b); b = n; o->vk = b; o->dirtyLo = 0xffffffffu; }
		}
		const uint8_t* src = (const uint8_t*)guest::host(o->data);
		if (!inFlight && b->ver && o->dirtyLo < o->dirtyHi && o->dirtyHi <= o->size) memcpy(b->map + o->dirtyLo, src + o->dirtyLo, o->dirtyHi - o->dirtyLo);
		else memcpy(b->map, src, o->size);
		vmaFlushAllocation(gA, b->mem, 0, VK_WHOLE_SIZE);
		o->dirtyLo = 0xffffffffu; o->dirtyHi = 0;
		b->ver = o->ver;
	}
	b->useSerial = gSerial;
	return b;
}

// ---- dummy textures for samplers without one
VTex* gDummy[3] = {};   // 2D, cube, 3D
void makeDummies()
{
	Fmt f; f.vk = VK_FORMAT_R8G8B8A8_UNORM;
	for (int k = 0; k < 3; k++)
	{
		gDummy[k] = createTex(1, 1, 1, 1, k == 1 ? 6 : 1, k == 1, k == 2, f, false, VkComponentMapping{});
		if (!gDummy[k]) continue;
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = 64; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_AUTO; ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VkBuffer sb; VmaAllocation sm; VmaAllocationInfo inf;
		if (vmaCreateBuffer(gA, &bi, &ai, &sb, &sm, &inf) != VK_SUCCESS) continue;
		for (int i = 0; i < 6; i++) ((uint32_t*)inf.pMappedData)[i] = 0xff000000u;   // black, opaque (as OpenGL)
		if (!F->upUsed) { VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; vkBeginCommandBuffer(F->up, &b); F->upUsed = true; }
		barrier(F->up, gDummy[k]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkBufferImageCopy c{};
		c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, k == 1 ? 6u : 1u}; c.imageExtent = {1, 1, 1};
		vkCmdCopyBufferToImage(F->up, sb, gDummy[k]->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		barrier(F->up, gDummy[k]->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		gDummy[k]->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		later([sb, sm] { vmaDestroyBuffer(gA, sb, sm); });
	}
	// a vertex buffer for shader inputs the vertex layout does not have: (0, 0, 0, 1) as OpenGL's constant attribute
}
VkBuffer gZeroVb = VK_NULL_HANDLE; VmaAllocation gZeroVbMem = VK_NULL_HANDLE;

// ---- samplers
std::unordered_map<uint64_t, VkSampler> gSamplers;
VkSamplerAddressMode addr(uint32_t a)
{
	switch (a) { case 2: case 5: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; default: return VK_SAMPLER_ADDRESS_MODE_REPEAT; }
}
VkSampler sampler(const uint32_t* s, bool hasMips, bool noFilter)
{
	uint32_t mag = s[5], mn = s[6], mip = s[7];
	const vkb::Handles& H = vkb::handles();
	float aniso = 1.f;
	if (!noFilter && H.aniso && H.maxAniso > 1.f && (mn == 3 || mag == 3)) aniso = std::max(1.f, std::min((float)std::max(1u, s[10]), H.maxAniso));
	bool border = s[1] == 4 || s[2] == 4 || s[3] == 4;
	uint32_t bc = border ? s[4] : 0;
	VkBorderColor bcol = (bc >> 24) < 128 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : (bc & 0xffffff) >= 0x808080 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
	uint64_t key = (uint64_t)(s[1] & 7) | (uint64_t)(s[2] & 7) << 3 | (uint64_t)(s[3] & 7) << 6 | (uint64_t)(mag & 3) << 9 | (uint64_t)(mn & 3) << 11 | (uint64_t)(mip & 3) << 13 |
		(uint64_t)hasMips << 15 | (uint64_t)noFilter << 16 | (uint64_t)bcol << 17 | (uint64_t)(aniso * 4.f) << 24;
	auto it = gSamplers.find(key);
	if (it != gSamplers.end()) return it->second;
	VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = (noFilter || mag == 1) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	si.minFilter = (noFilter || mn == 1) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	si.mipmapMode = (noFilter || mip != 2) ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
	si.addressModeU = addr(s[1]); si.addressModeV = addr(s[2]); si.addressModeW = addr(s[3]);
	si.anisotropyEnable = aniso > 1.f; si.maxAnisotropy = aniso;
	si.minLod = 0.f; si.maxLod = (!hasMips || mip == 0) ? 0.f : VK_LOD_CLAMP_NONE;
	si.borderColor = bcol;
	VkSampler sm = VK_NULL_HANDLE;
	vkOk(vkCreateSampler(D, &si, nullptr, &sm), "vkCreateSampler");
	gSamplers[key] = sm;
	return sm;
}

// ---- shaders and pipelines
uint64_t fnv(const void* d, size_t n, uint64_t h = 1469598103934665603ull)
{
	const uint8_t* p = (const uint8_t*)d;
	for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
	return h;
}
std::unordered_map<uint64_t, VkShaderModule> gModules;
VkShaderModule module(const std::string& src, bool vertex, uint64_t key)
{
	auto it = gModules.find(key);
	if (it != gModules.end()) return it->second;
	std::vector<uint32_t> spv;
	std::string err;
	VkShaderModule m = VK_NULL_HANDLE;
	if (vksh::compile(src, vertex, spv, err))
	{
		VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		ci.codeSize = spv.size() * 4; ci.pCode = spv.data();
		vkOk(vkCreateShaderModule(D, &ci, nullptr, &m), "vkCreateShaderModule");
	}
	else { static int n = 0; if (n++ < 5) log("vulkan: shader does not compile: %s\n%s", err.c_str(), src.c_str()); }
	gModules[key] = m;
	return m;
}
VkShaderModule vsModule(ShaderObj* vs, uint32_t intMask, uint32_t uintMask)
{
	uint64_t k = (vs->hash * 31 + 1) ^ ((uint64_t)intMask << 20) * 0x9e3779b97f4a7c15ull ^ ((uint64_t)uintMask << 40) * 0xc2b2ae3d27d4eb4full;
	auto it = gModules.find(k);
	if (it != gModules.end()) return it->second;
	return module(shdr::composeVk(vs->info, nullptr, false, intMask, uintMask), true, k);
}
bool gScaled = false;   // the GPU reads USCALED/SSCALED vertex formats
VkShaderModule psModule(ShaderObj* ps, ShaderObj* vs, bool precise)
{
	uint64_t k = ps->hash * 31 + 2;
	for (auto& n : vs->info.outputs) k = fnv(n.data(), n.size(), k);
	if (precise) k ^= 0x5bd1e9955bd1e995ull;
	auto it = gModules.find(k);
	if (it != gModules.end()) return it->second;
	return module(shdr::composeVk(ps->info, &vs->info.outputs, precise), false, k);
}

VkFormat attrFormat(uint8_t type)
{
	switch (type)
	{
	case 0: return VK_FORMAT_R32_SFLOAT; case 1: return VK_FORMAT_R32G32_SFLOAT; case 2: return VK_FORMAT_R32G32B32_SFLOAT; case 3: return VK_FORMAT_R32G32B32A32_SFLOAT;
	case 4: return VK_FORMAT_B8G8R8A8_UNORM;   // D3DCOLOR: the hardware swaps red and blue (no u_bgra swizzle)
	case 5: return gScaled ? VK_FORMAT_R8G8B8A8_USCALED : VK_FORMAT_R8G8B8A8_UINT;
	case 6: return gScaled ? VK_FORMAT_R16G16_SSCALED : VK_FORMAT_R16G16_SINT;
	case 7: return gScaled ? VK_FORMAT_R16G16B16A16_SSCALED : VK_FORMAT_R16G16B16A16_SINT;
	case 8: return VK_FORMAT_R8G8B8A8_UNORM; case 9: return VK_FORMAT_R16G16_SNORM; case 10: return VK_FORMAT_R16G16B16A16_SNORM;
	case 11: return VK_FORMAT_R16G16_UNORM; case 12: return VK_FORMAT_R16G16B16A16_UNORM;
	case 13: return gScaled ? VK_FORMAT_A2B10G10R10_USCALED_PACK32 : VK_FORMAT_A2B10G10R10_UINT_PACK32; case 14: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
	case 15: return VK_FORMAT_R16G16_SFLOAT; case 16: return VK_FORMAT_R16G16B16A16_SFLOAT;
	}
	return VK_FORMAT_UNDEFINED;
}
VkBlendFactor blendFactor(uint32_t b, bool alpha)
{
	switch (b)
	{
	case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case 9: return VK_BLEND_FACTOR_DST_COLOR; case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return alpha ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	case 12: return VK_BLEND_FACTOR_SRC_ALPHA; case 13: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; default: return VK_BLEND_FACTOR_ONE;
	}
}
VkBlendOp blendOp(uint32_t o) { switch (o) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; } }
VkCompareOp cmpOp(uint32_t f) { switch (f) { case 1: return VK_COMPARE_OP_NEVER; case 2: return VK_COMPARE_OP_LESS; case 3: return VK_COMPARE_OP_EQUAL; case 4: return VK_COMPARE_OP_LESS_OR_EQUAL; case 5: return VK_COMPARE_OP_GREATER; case 6: return VK_COMPARE_OP_NOT_EQUAL; case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL; default: return VK_COMPARE_OP_ALWAYS; } }
VkStencilOp stOp(uint32_t o) { switch (o) { case 2: return VK_STENCIL_OP_ZERO; case 3: return VK_STENCIL_OP_REPLACE; case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP; case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP; case 6: return VK_STENCIL_OP_INVERT; case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP; case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP; } }
VkPrimitiveTopology topo(uint32_t p) { switch (p) { case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; } }
int topoClass(uint32_t p) { return p == 1 ? 0 : (p == 2 || p == 3) ? 1 : 2; }

// ---- vertex formats the GPU cannot fetch (Adreno has no USCALED/SSCALED): the stream is rewritten on the CPU with
// every element as four floats, as Direct3D expands them
float h2f(uint16_t h)
{
	uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 31, m = h & 1023, f;
	if (e == 0) { if (!m) f = s; else { e = 127 - 15 + 1; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; f = s | e << 23 | m << 13; } }
	else if (e == 31) f = s | 0x7f800000u | m << 13;
	else f = s | (e - 15 + 127) << 23 | m << 13;
	float r; memcpy(&r, &f, 4); return r;
}
uint32_t attrBytes(uint8_t t)
{
	static const uint8_t n[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8};
	return t < 17 ? n[t] : 0;
}
void decodeAttr(uint8_t t, const uint8_t* p, float* o)
{
	o[0] = o[1] = o[2] = 0.f; o[3] = 1.f;
	switch (t)
	{
	case 0: case 1: case 2: case 3: memcpy(o, p, 4 * (t + 1)); break;
	case 4: o[0] = p[2] / 255.f; o[1] = p[1] / 255.f; o[2] = p[0] / 255.f; o[3] = p[3] / 255.f; break;   // D3DCOLOR
	case 5: for (int i = 0; i < 4; i++) o[i] = p[i]; break;
	case 6: case 7: { int16_t v[4]; memcpy(v, p, t == 6 ? 4 : 8); for (int i = 0; i < (t == 6 ? 2 : 4); i++) o[i] = v[i]; break; }
	case 8: for (int i = 0; i < 4; i++) o[i] = p[i] / 255.f; break;
	case 9: case 10: { int16_t v[4]; memcpy(v, p, t == 9 ? 4 : 8); for (int i = 0; i < (t == 9 ? 2 : 4); i++) o[i] = std::max(-1.f, v[i] / 32767.f); break; }
	case 11: case 12: { uint16_t v[4]; memcpy(v, p, t == 11 ? 4 : 8); for (int i = 0; i < (t == 11 ? 2 : 4); i++) o[i] = v[i] / 65535.f; break; }
	case 13: { uint32_t v; memcpy(&v, p, 4); o[0] = (float)(v & 1023); o[1] = (float)((v >> 10) & 1023); o[2] = (float)((v >> 20) & 1023); o[3] = 1.f; break; }
	case 14: { uint32_t v; memcpy(&v, p, 4); for (int i = 0; i < 3; i++) { int x = (int)((v >> (10 * i)) & 1023); if (x & 512) x -= 1024; o[i] = std::max(-1.f, x / 511.f); } o[3] = 1.f; break; }
	case 15: case 16: { uint16_t v[4]; memcpy(v, p, t == 15 ? 4 : 8); for (int i = 0; i < (t == 15 ? 2 : 4); i++) o[i] = h2f(v[i]); break; }
	}
}
// elements of one stream rewritten as float4s: `nEl` elements (types, source offsets)
struct ConvLayout { uint32_t n = 0; uint8_t type[16]; uint16_t off[16]; };
void convertVerts(const ConvLayout& cl, const uint8_t* src, uint32_t srcStride, uint32_t count, float* dst)
{
	for (uint32_t v = 0; v < count; v++, src += srcStride)
		for (uint32_t k = 0; k < cl.n; k++, dst += 4) decodeAttr(cl.type[k], src + cl.off[k], dst);
}
struct ConvBuf { uint32_t ver = 0; uint64_t key = 0; VBuf* buf = nullptr; };
std::unordered_map<Obj*, ConvBuf> gConv;

struct VtxLayout
{
	uint32_t nAttr = 0, nBind = 0;
	VkVertexInputAttributeDescription attr[16];
	VkVertexInputBindingDescription bind[17];
};
struct PipeKey
{
	VkShaderModule vs, ps;
	uint64_t vtx;
	uint32_t blend[7], colorMask;
	VkFormat cf, df;
	int topo;
	bool hasStencil;
};
std::unordered_map<uint64_t, VkPipeline> gPipes;
VkPipeline pipeline(const PipeKey& k, const VtxLayout& vl)
{
	uint64_t h = fnv(&k, sizeof k);
	auto it = gPipes.find(h);
	if (it != gPipes.end()) return it->second;
	VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = k.vs; st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = k.ps; st[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	vi.vertexBindingDescriptionCount = vl.nBind; vi.pVertexBindingDescriptions = vl.bind;
	vi.vertexAttributeDescriptionCount = vl.nAttr; vi.pVertexAttributeDescriptions = vl.attr;
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = k.topo == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k.topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1; vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	ds.minDepthBounds = 0.f; ds.maxDepthBounds = 1.f;
	VkPipelineColorBlendAttachmentState ba{};
	ba.blendEnable = k.blend[0] != 0;
	ba.srcColorBlendFactor = (VkBlendFactor)k.blend[1]; ba.dstColorBlendFactor = (VkBlendFactor)k.blend[2]; ba.colorBlendOp = (VkBlendOp)k.blend[3];
	ba.srcAlphaBlendFactor = (VkBlendFactor)k.blend[4]; ba.dstAlphaBlendFactor = (VkBlendFactor)k.blend[5]; ba.alphaBlendOp = (VkBlendOp)k.blend[6];
	ba.colorWriteMask = k.colorMask;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = k.cf != VK_FORMAT_UNDEFINED ? 1 : 0; cb.pAttachments = &ba;
	const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BIAS,
		VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE,
		VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
		VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE};
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = sizeof dyn / sizeof dyn[0]; dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = cb.attachmentCount; ri.pColorAttachmentFormats = &k.cf;
	ri.depthAttachmentFormat = k.df; ri.stencilAttachmentFormat = k.hasStencil ? k.df : VK_FORMAT_UNDEFINED;
	VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pi.pNext = &ri;
	pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
	pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pDepthStencilState = &ds; pi.pColorBlendState = &cb; pi.pDynamicState = &dy;
	pi.layout = gLayout;
	VkPipeline p = VK_NULL_HANDLE;
	vkOk(vkCreateGraphicsPipelines(D, gPipeCache, 1, &pi, nullptr, &p), "vkCreateGraphicsPipelines");
	gPipes[h] = p;
	gNewPipes++;
	static int n = 0;
	if (n++ < 6 || gPipes.size() % 100 == 0) log("vulkan: %zu pipelines", gPipes.size());
	return p;
}

std::string pipeCachePath() { return rt::config.dataRoot + "/vkpipelines.bin"; }
void savePipeCache()
{
	size_t n = 0;
	if (vkGetPipelineCacheData(D, gPipeCache, &n, nullptr) != VK_SUCCESS || !n) return;
	std::vector<uint8_t> d(n);
	if (vkGetPipelineCacheData(D, gPipeCache, &n, d.data()) != VK_SUCCESS) return;
	d.resize(n);
	writeWhole(pipeCachePath(), d);
	log("vulkan: pipeline cache saved (%zu pipelines, %zu KB)", gPipes.size(), n >> 10);
	gNewPipes = 0;
}

// ---- command buffer state (dynamic state, bindings), reset at the start of a frame
struct CmdState
{
	bool valid = false;
	VkPipeline pipe = VK_NULL_HANDLE;
	VkViewport vp{}; VkRect2D sc{};
	float blendC[4]; uint32_t cull, topo, dTest, dWrite, dCmp, sTest, sOps[4], sMaskR, sMaskW, sRef, biasOn; float bias[2];
	Push push; bool pushValid = false;
	VkDescriptorSet constSet = VK_NULL_HANDLE; uint32_t constOff[2] = {~0u, ~0u};
	VkDescriptorSet texSet = VK_NULL_HANDLE;
	VkImageView texViews[16]; VkSampler texSamp[16]; uint32_t texMask = 0;
};
CmdState S;

// constants: the last block written per stage, reused when the next draw's is the same
struct ConstCache { std::vector<uint8_t> data; VkDeviceSize off = 0; size_t chunk = ~(size_t)0; };
ConstCache gCC[2];
bool writeConsts(int stage, const uint8_t* data, uint32_t size, VkDeviceSize& off, size_t& chunk)
{
	ConstCache& c = gCC[stage];
	if (c.chunk < F->chunks.size() && c.data.size() == size && !memcmp(c.data.data(), data, size)) { off = c.off; chunk = c.chunk; return true; }
	RingPos rp;
	if (!ringAlloc(std::max<uint32_t>(size, 16), gUboAlign, rp, shdr::kVkConstRange)) return false;
	memcpy(rp.ptr, data, size);
	c.data.assign(data, data + size); c.off = rp.off; c.chunk = rp.chunk;
	off = rp.off; chunk = rp.chunk;
	return true;
}

// ---- frames
void beginFrame()
{
	F = &gF[gFi];
	if (F->serial)
	{
		vkWaitForFences(D, 1, &F->fence, VK_TRUE, UINT64_MAX);
		gDone = std::max(gDone, F->serial);
	}
	if (!F->dumps.empty()) writeDumps(*F);
	for (auto& f : F->trash) f();
	F->trash.clear();
	vkResetFences(D, 1, &F->fence);
	vkResetCommandPool(D, F->pool, 0);
	for (auto p : F->texPools) vkResetDescriptorPool(D, p, 0);
	F->texPool = 0;
	for (auto& c : F->chunks) c.pos = 0;
	F->chunk = 0;
	F->upUsed = false;
	F->serial = gSerial;
	VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(F->cmd, &b);
	C = F->cmd;
	S = CmdState();
	gCC[0].chunk = gCC[1].chunk = ~(size_t)0;
	gInPass = false;
}

VkDescriptorSet allocTexSet()
{
	for (;;)
	{
		if (F->texPool >= F->texPools.size())
		{
			VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * 1024};
			VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			pci.maxSets = 1024; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
			VkDescriptorPool p;
			if (!vkOk(vkCreateDescriptorPool(D, &pci, nullptr, &p), "descriptor pool")) return VK_NULL_HANDLE;
			F->texPools.push_back(p);
		}
		VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		ai.descriptorPool = F->texPools[F->texPool]; ai.descriptorSetCount = 1; ai.pSetLayouts = &gTexLayout;
		VkDescriptorSet s;
		VkResult r = vkAllocateDescriptorSets(D, &ai, &s);
		if (r == VK_SUCCESS) return s;
		F->texPool++;
	}
}

// A draw that samples the target it draws to (the water reads the scene for its refraction): OpenGL on a tiled GPU
// gives it the contents from before the pass, Vulkan would give garbage. It reads a copy instead, made again only
// when the target has been drawn to since.
VTex* feedback(VTex* t, uint32_t level, uint32_t layer)
{
	uint32_t w = std::max(1u, t->w >> level), h = std::max(1u, t->h >> level);
	if (t->fb && (t->fb->w != w || t->fb->h != h)) { destroyTex(t->fb); t->fb = nullptr; }
	if (!t->fb)
	{
		Fmt f; f.vk = t->fmt; f.depth = t->depth; f.stencil = t->stencil; f.noFilter = t->noFilter;
		t->fb = createTex(w, h, 1, 1, 1, false, false, f, false, t->swz);
		if (!t->fb) return gDummy[0];
		t->fbContent = ~0u;
		static int n = 0;
		if (n++ < 8) log("vulkan: a draw samples its own render target (%ux%u format %d): it reads a copy", w, h, (int)t->fmt);
	}
	if (t->fbContent != t->content)
	{
		toLayout(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		toLayout(t->fb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageCopy c{};
		c.srcSubresource = {t->aspect, level, layer, 1}; c.dstSubresource = {t->aspect, 0, 0, 1};
		c.extent = {w, h, 1};
		vkCmdCopyImage(C, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->fb->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
		t->fbContent = t->content;
		t->useSerial = gSerial;
	}
	return t->fb;
}

// begin rendering into the current target (layouts first: no barrier may fall inside a pass)
bool beginPass(const Target& t)
{
	if (gInPass && t.color == gPass.color && t.cLevel == gPass.cLevel && t.cLayer == gPass.cLayer && t.depth == gPass.depth) return true;
	endPass();
	if (t.color) toLayout(t.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	if (t.depth) toLayout(t.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
	ri.renderArea = {{0, 0}, {t.w, t.h}};
	ri.layerCount = 1;
	if (t.color)
	{
		ca.imageView = attView(t.color, t.cLevel, t.cLayer); ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
		t.color->useSerial = gSerial;
	}
	if (t.depth)
	{
		da.imageView = attView(t.depth, 0, 0); da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ri.pDepthAttachment = &da;
		if (t.depth->stencil) ri.pStencilAttachment = &da;
		t.depth->useSerial = gSerial;
	}
	pBeginRendering(C, &ri);
	gInPass = true;
	gPass = t;
	return true;
}

// the target of the device state
bool currentTarget(Target& t)
{
	ensureBack();
	Obj* rt = dev.rt0;
	if (isBack(rt))
	{
		t.color = gBack; t.depth = gBackDepth; t.w = backW; t.h = backH;
		return gBack && gBackDepth;
	}
	Obj* ow = texOwner(rt);
	Fmt f = fmtOf(ow->fmt);
	uint32_t lvl = rt->iface == ISurface ? rt->level : 0;
	t.w = std::max(1u, ow->w >> lvl); t.h = std::max(1u, ow->h >> lvl);
	if (!f.none)
	{
		t.color = ensureTex(rt);
		if (!t.color || t.color->depth) return false;
		t.cLevel = lvl; t.cLayer = ow->iface == ICube && rt->iface == ISurface ? rt->face : 0;
		if (ow->levels > 1 && (ow->usage & 0x400) && lvl == 0) ow->mipDirty = true;   // auto-mipmapped target
	}
	Obj* ds = dev.ds;
	if (ds && ds->fmt != 0x4c4c554e && fmtOf(texOwner(ds)->fmt).depth)
	{
		t.depth = ensureTex(ds);
		if (t.depth)
		{
			Obj* dw = texOwner(ds);
			t.w = std::min(t.w, dw->w); t.h = std::min(t.h, dw->h);   // the overlap (as OpenGL)
		}
	}
	return t.color || t.depth;
}

// mip chain of an auto-mipmapped render target, outside a pass
void genMips(VTex* t)
{
	endPass();
	barrier(C, t->img, t->aspect, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	for (uint32_t l = 1; l < t->levels; l++)
	{
		barrier(C, t->img, t->aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, l - 1, 1);
		VkImageBlit b{};
		b.srcSubresource = {t->aspect, l - 1, 0, t->layers}; b.dstSubresource = {t->aspect, l, 0, t->layers};
		b.srcOffsets[1] = {(int32_t)std::max(1u, t->w >> (l - 1)), (int32_t)std::max(1u, t->h >> (l - 1)), 1};
		b.dstOffsets[1] = {(int32_t)std::max(1u, t->w >> l), (int32_t)std::max(1u, t->h >> l), 1};
		vkCmdBlitImage(C, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
	}
	barrier(C, t->img, t->aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, t->levels - 1);
	barrier(C, t->img, t->aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, t->levels - 1, 1);
	t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// ---- the present pass: a small pipeline for the on-screen buttons (alpha-blended rectangles)
VkPipeline gRectPipe = VK_NULL_HANDLE;
VkPipelineLayout gRectLayout = VK_NULL_HANDLE;
VkFormat gRectFmt = VK_FORMAT_UNDEFINED;
std::unordered_map<VkImage, VkImageView> gSwapViews;
void rectPipeline(VkFormat fmt)
{
	if (gRectPipe && gRectFmt == fmt) return;
	if (!gRectLayout)
	{
		VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
		VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
		vkCreatePipelineLayout(D, &li, nullptr, &gRectLayout);
	}
	const char* vs = "#version 450\nlayout(push_constant) uniform P { vec4 r; vec4 c; };\nvoid main() { vec2 p = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1)); gl_Position = vec4(mix(r.x, r.z, p.x), mix(r.y, r.w, p.y), 0.0, 1.0); }\n";
	const char* fs = "#version 450\nlayout(push_constant) uniform P { vec4 r; vec4 c; };\nlayout(location = 0) out vec4 o;\nvoid main() { o = c; }\n";
	VkShaderModule v = module(vs, true, 0x7265637476ull), f = module(fs, false, 0x7265637466ull);
	if (!v || !f) return;
	VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = v; st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = f; st[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vp.viewportCount = 1; vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.f; rs.cullMode = VK_CULL_MODE_NONE;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState ba{};
	ba.blendEnable = VK_TRUE;
	ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; ba.colorBlendOp = VK_BLEND_OP_ADD;
	ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; ba.alphaBlendOp = VK_BLEND_OP_ADD;
	ba.colorWriteMask = 15;
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1; cb.pAttachments = &ba;
	const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
	VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pi.pNext = &ri; pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
	pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pColorBlendState = &cb; pi.pDynamicState = &dy; pi.layout = gRectLayout;
	if (gRectPipe) { VkPipeline old = gRectPipe; later([old] { vkDestroyPipeline(D, old, nullptr); }); }
	gRectPipe = VK_NULL_HANDLE;
	vkOk(vkCreateGraphicsPipelines(D, gPipeCache, 1, &pi, nullptr, &gRectPipe), "rect pipeline");
	gRectFmt = fmt;
}

// ---- released objects: Vulkan resources and the guest copy freed (as the OpenGL renderer's reapReleased)
void reap(uint32_t frame)
{
	static uint64_t freedObjs = 0, freedBytes = 0;
	for (Obj* o : takeReleased())
	{
		if (o->ref > 0) { o->released = false; continue; }
		bool bound = o == dev.ib || o == dev.rt0 || o == dev.ds || o == gBackBuffer;
		for (int i = 0; i < 16 && !bound; i++) bound = dev.tex[i] == o || dev.streams[i].vb == o;
		if (bound || frame - o->lastUse < 1800 || o->lastUse > frame) { requeueReleased(o); continue; }
		{
			auto it = gConv.find(o);
			if (it != gConv.end()) { destroyBuf(it->second.buf); gConv.erase(it); }
		}
		if (o->vk)
		{
			if (o->iface == IVB || o->iface == IIB) destroyBuf((VBuf*)o->vk);
			else destroyTex((VTex*)o->vk);
			o->vk = nullptr;
		}
		if (o->ownsData && o->data) { freedBytes += o->size; rt::gfree(o->data); o->data = 0; }
		freedObjs++;
		o->released = false;
	}
	static uint32_t last = 0;
	if (frame - last >= 3600) { last = frame; log("mem: guest heap %llu MiB live; reclaimed %llu d3d objects, %llu MiB so far (Vulkan)", (unsigned long long)(rt::heapLiveBytes() >> 20), (unsigned long long)freedObjs, (unsigned long long)(freedBytes >> 20)); }
}
}   // namespace

// ---------------------------------------------------------------------------------------------------------------
bool init()
{
	std::lock_guard<std::recursive_mutex> l(gMu);
	if (gInit) return gOk;
	gInit = true;
	const vkb::Handles& H = vkb::handles();
	D = H.device;
	if (!D) { log("vulkan: no device"); return false; }
	if (!vksh::available()) { log("vulkan: this build has no shader compiler"); return false; }
	pBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(D, "vkCmdBeginRendering");
	pEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(D, "vkCmdEndRendering");
	pSetCullMode = (PFN_vkCmdSetCullMode)vkGetDeviceProcAddr(D, "vkCmdSetCullMode");
	pSetFrontFace = (PFN_vkCmdSetFrontFace)vkGetDeviceProcAddr(D, "vkCmdSetFrontFace");
	pSetTopology = (PFN_vkCmdSetPrimitiveTopology)vkGetDeviceProcAddr(D, "vkCmdSetPrimitiveTopology");
	pSetDepthTest = (PFN_vkCmdSetDepthTestEnable)vkGetDeviceProcAddr(D, "vkCmdSetDepthTestEnable");
	pSetDepthWrite = (PFN_vkCmdSetDepthWriteEnable)vkGetDeviceProcAddr(D, "vkCmdSetDepthWriteEnable");
	pSetDepthCompare = (PFN_vkCmdSetDepthCompareOp)vkGetDeviceProcAddr(D, "vkCmdSetDepthCompareOp");
	pSetStencilTest = (PFN_vkCmdSetStencilTestEnable)vkGetDeviceProcAddr(D, "vkCmdSetStencilTestEnable");
	pSetStencilOp = (PFN_vkCmdSetStencilOp)vkGetDeviceProcAddr(D, "vkCmdSetStencilOp");
	pSetDepthBiasEnable = (PFN_vkCmdSetDepthBiasEnable)vkGetDeviceProcAddr(D, "vkCmdSetDepthBiasEnable");
	if (!pBeginRendering || !pEndRendering || !pSetCullMode || !pSetFrontFace || !pSetTopology || !pSetDepthTest || !pSetDepthWrite || !pSetDepthCompare ||
		!pSetStencilTest || !pSetStencilOp || !pSetDepthBiasEnable)
	{ log("vulkan: Vulkan 1.3 functions missing"); return false; }
	gUboAlign = std::max(16u, H.uboAlign);
	{
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(H.phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
		const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
		gDepthFmt = (fp.optimalTilingFeatures & need) == need ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
		vkGetPhysicalDeviceFormatProperties(H.phys, VK_FORMAT_R8G8B8A8_USCALED, &fp);
		bool usc = fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
		vkGetPhysicalDeviceFormatProperties(H.phys, VK_FORMAT_R16G16_SSCALED, &fp);
		gScaled = usc && (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
		log("vulkan: depth format %s, scaled vertex formats %s", gDepthFmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", gScaled ? "yes" : "no (integer attributes converted in the shader)");
	}
	VmaAllocatorCreateInfo ai{};
	ai.physicalDevice = H.phys; ai.device = D; ai.instance = H.instance; ai.vulkanApiVersion = VK_API_VERSION_1_1;
	if (!vkOk(vmaCreateAllocator(&ai, &gA), "vmaCreateAllocator")) return false;
	// layouts: set 0 = the two constant blocks (dynamic offsets), set 1 = 16 textures, push constants
	{
		VkDescriptorSetLayoutBinding b[2] = {};
		for (int i = 0; i < 2; i++) { b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; b[i].descriptorCount = 1; b[i].stageFlags = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; }
		VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		li.bindingCount = 2; li.pBindings = b;
		if (!vkOk(vkCreateDescriptorSetLayout(D, &li, nullptr, &gConstLayout), "const layout")) return false;
		VkDescriptorSetLayoutBinding t[16] = {};
		for (int i = 0; i < 16; i++) { t[i].binding = i; t[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; t[i].descriptorCount = 1; t[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; }
		li.bindingCount = 16; li.pBindings = t;
		if (!vkOk(vkCreateDescriptorSetLayout(D, &li, nullptr, &gTexLayout), "texture layout")) return false;
		VkDescriptorSetLayout sets[2] = {gConstLayout, gTexLayout};
		VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, shdr::kVkPushSize};
		VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pli.setLayoutCount = 2; pli.pSetLayouts = sets; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
		if (!vkOk(vkCreatePipelineLayout(D, &pli, nullptr, &gLayout), "pipeline layout")) return false;
		VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * 256};
		VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pci.maxSets = 256; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
		if (!vkOk(vkCreateDescriptorPool(D, &pci, nullptr, &gConstPool), "const pool")) return false;
	}
	{
		std::vector<uint8_t> blob;
		readWhole(pipeCachePath(), blob);
		VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
		ci.initialDataSize = blob.size(); ci.pInitialData = blob.empty() ? nullptr : blob.data();
		if (vkCreatePipelineCache(D, &ci, nullptr, &gPipeCache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = nullptr; vkCreatePipelineCache(D, &ci, nullptr, &gPipeCache); }
		log("vulkan: pipeline cache %zu KB from disk", blob.size() >> 10);
	}
	for (auto& f : gF)
	{
		VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pci.queueFamilyIndex = H.family;
		if (!vkOk(vkCreateCommandPool(D, &pci, nullptr, &f.pool), "command pool")) return false;
		VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		cai.commandPool = f.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 2;
		VkCommandBuffer cb[2];
		if (!vkOk(vkAllocateCommandBuffers(D, &cai, cb), "command buffers")) return false;
		f.cmd = cb[0]; f.up = cb[1];
		VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		vkCreateFence(D, &fci, nullptr, &f.fence);
		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		vkCreateSemaphore(D, &sci, nullptr, &f.acq);
	}
	gFi = 0;
	beginFrame();
	makeDummies();
	{
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = 64; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		VmaAllocationCreateInfo vai{}; vai.usage = VMA_MEMORY_USAGE_AUTO; vai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo inf;
		if (!vkOk(vmaCreateBuffer(gA, &bi, &vai, &gZeroVb, &gZeroVbMem, &inf), "zero vertex buffer")) return false;
		float v[4] = {0.f, 0.f, 0.f, 1.f};
		memcpy(inf.pMappedData, v, 16);
	}
	gOk = true;
	log("vulkan: renderer ready");
	return true;
}

uint32_t drawsThisFrame() { return gDraws; }

void clear(uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil)
{
	std::lock_guard<std::recursive_mutex> l(gMu);
	if (!gOk) return;
	Target t;
	if (!currentTarget(t)) return;
	VkClearAttachment a[2]; uint32_t na = 0;
	if ((flags & 1) && t.color)
	{
		a[na].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; a[na].colorAttachment = 0;
		a[na].clearValue.color = {{((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, ((color >> 24) & 255) / 255.f}};
		na++;
	}
	if ((flags & 6) && t.depth)
	{
		VkImageAspectFlags as = 0;
		if (flags & 2) as |= VK_IMAGE_ASPECT_DEPTH_BIT;
		if ((flags & 4) && t.depth->stencil) as |= VK_IMAGE_ASPECT_STENCIL_BIT;
		if (as) { a[na].aspectMask = as; a[na].colorAttachment = 0; a[na].clearValue.depthStencil = {z, stencil & 255}; na++; }
	}
	if (!na) return;
	beginPass(t);
	std::vector<VkClearRect> cr;
	if (count && rects)
		for (uint32_t i = 0; i < count; i++)
		{
			int32_t x1 = std::max(0, guest::load<int32_t>(rects + 16 * i)), y1 = std::max(0, guest::load<int32_t>(rects + 16 * i + 4));
			int32_t x2 = std::min((int32_t)t.w, guest::load<int32_t>(rects + 16 * i + 8)), y2 = std::min((int32_t)t.h, guest::load<int32_t>(rects + 16 * i + 12));
			if (x2 > x1 && y2 > y1) cr.push_back({{{x1, y1}, {(uint32_t)(x2 - x1), (uint32_t)(y2 - y1)}}, 0, 1});
		}
	else cr.push_back({{{0, 0}, {t.w, t.h}}, 0, 1});
	if (!cr.empty()) vkCmdClearAttachments(C, na, a, (uint32_t)cr.size(), cr.data());
	if (dumping()) log("vkdump pass %d clear flags %x color %08x z %g rects %u rt %ux%u", gDumpPass, flags, color, (double)z, count, t.w, t.h);
	if (t.color && (flags & 1)) t.color->content++;
	if (t.depth && (flags & 6)) t.depth->content++;
}

void draw(const DrawCall& dc)
{
	std::lock_guard<std::recursive_mutex> l(gMu);
	if (!gOk) return;
	ShaderObj *vs = dev.vs, *ps = dev.ps;
	if (!vs || !ps || !vs->ok || !ps->ok || !dev.vdecl) return;
	// the frame's statistics the interface code relies on (as the OpenGL renderer)
	if (dev.rs[7] && !gUiNested) gUiWorldDraws++;
	if (!gUiNested) { if (vs->hash == kAptVs) gAptDraws++; else if (vs->hash == kVideoVs) gVideoDraws++; }
	DeclElem els[64];
	uint32_t nEls = 0;
	{
		size_t n = std::min<size_t>(dev.vdecl->blob.size() / 8, 64);
		for (size_t i = 0; i < n; i++) { DeclElem e; memcpy(&e, dev.vdecl->blob.data() + 8 * i, 8); if (e.stream == 0xff) break; els[nEls++] = e; }
	}
	// interface scaling (HUD panels in the corners)
	float axN = 0, ayN = 0, ax = 0, ay = 0, kUi = 0;
	{
		static std::vector<uint8_t> split;
		split.clear();
		kUi = uiScaleFor(dc, els, nEls, axN, ayN, ax, ay, split);
		if (!split.empty())
		{
			static std::vector<uint8_t> keep;
			keep.swap(split);
			DrawCall d2;
			d2.prim = 4; d2.primCount = dc.primCount; d2.upVerts = keep.data(); d2.upStride = dev.streams[0].stride;
			for (uint32_t q = 0; q < nEls; q++) if (els[q].usage == 0 && els[q].index == 0) { uint32_t s = dev.streams[els[q].stream < 16 ? els[q].stream : 0].stride; if (s) d2.upStride = s; }
			if (dc.upVerts) d2.upStride = dc.upStride;
			gUiNested = true;
			draw(d2);
			gUiNested = false;
			keep.swap(split);
			return;
		}
	}
	Target t;
	if (!currentTarget(t)) return;
	bool precise = rt::config.fastShaders && vs->info.relC;
	if (rt::config.fastShaders && !precise)
		for (int i = 0; i < 16 && !precise; i++) if (ps->info.samplerType[i] && dev.tex[i] && (dev.tex[i]->usage & 3)) precise = true;
	VkShaderModule pm = psModule(ps, vs, precise);
	if (!pm) return;

	// textures: uploaded, mip chains built and in the sampling layout before the pass starts
	VkImageView views[16]; VkSampler samps[16]; uint32_t mask = 0;
	for (int i = 0; i < 16; i++)
	{
		int st = ps->info.samplerType[i] ? ps->info.samplerType[i] : vs->info.samplerType[i];
		if (!st) continue;
		int kind = st == 3 ? 1 : st == 4 ? 2 : 0;
		VTex* tx = nullptr;
		Obj* o = dev.tex[i];
		if (o)
		{
			Obj* ow = texOwner(o);
			int okind = ow->iface == ICube ? 1 : ow->iface == IVolume ? 2 : 0;
			if (okind == kind) tx = ensureTex(o);
			if (tx && (tx == t.color || tx == t.depth)) tx = feedback(tx, tx == t.color ? t.cLevel : 0, tx == t.color ? t.cLayer : 0);   // reads its own target
			if (tx && ow->mipDirty && tx->levels > 1 && !tx->depth) { genMips(tx); ow->mipDirty = false; }
		}
		if (!tx) tx = gDummy[kind];
		if (!tx) return;
		toLayout(tx, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		tx->useSerial = gSerial;
		views[i] = tx->sampled;
		samps[i] = sampler(dev.samp[i], tx->levels > 1, tx->noFilter);
		mask |= 1u << i;
	}
	beginPass(t);

	// vertex layout
	VtxLayout vl;
	uint32_t streamsUsed = 0;
	bool instanced = false; uint32_t instCount = 1;
	if (!dc.upVerts)
		for (int si = 0; si < 16; si++)
		{
			uint32_t fq = dev.streams[si].freq;
			if (fq & 0x40000000u) { instCount = std::max<uint32_t>(1, fq & 0x3fffffffu); instanced = true; }
			else if (fq & 0x80000000u) instanced = true;
		}
	uint32_t locs = 0, intMask = 0, uintMask = 0;
	// streams with a format the GPU cannot fetch: rewritten as float4s
	ConvLayout conv[15];
	uint32_t convMask = 0;
	if (!gScaled)
		for (uint32_t k = 0; k < nEls; k++)
		{
			uint8_t ty = els[k].type;
			if ((ty == 5 || ty == 6 || ty == 7 || ty == 13) && shdr::attrLocation(els[k].usage, els[k].index) >= 0) convMask |= 1u << (els[k].stream < 15 ? els[k].stream : 0);
		}
	for (uint32_t k = 0; k < nEls; k++)
	{
		const DeclElem& e = els[k];
		int loc = shdr::attrLocation(e.usage, e.index);
		if (loc < 0 || (locs >> loc & 1)) continue;
		VkFormat f = attrFormat(e.type);
		if (f == VK_FORMAT_UNDEFINED) continue;
		uint32_t si = e.stream < 15 ? e.stream : 0;
		if (dc.upVerts ? si != 0 : !dev.streams[si].vb) continue;
		if (convMask >> si & 1)
		{
			ConvLayout& c = conv[si];
			if (c.n >= 16) continue;
			vl.attr[vl.nAttr++] = {(uint32_t)loc, si, VK_FORMAT_R32G32B32A32_SFLOAT, 16 * c.n};
			c.type[c.n] = e.type; c.off[c.n] = e.offset; c.n++;
			locs |= 1u << loc;
			streamsUsed |= 1u << si;
			continue;
		}
		vl.attr[vl.nAttr++] = {(uint32_t)loc, si, f, e.offset};
		locs |= 1u << loc;
		if (f == VK_FORMAT_R16G16_SINT || f == VK_FORMAT_R16G16B16A16_SINT) intMask |= 1u << loc;
		else if (f == VK_FORMAT_R8G8B8A8_UINT || f == VK_FORMAT_A2B10G10R10_UINT_PACK32) uintMask |= 1u << loc;
		streamsUsed |= 1u << si;
	}
	bool needZero = false;
	for (auto& kv : vs->info.inputs)
		if (kv.second >= 0 && kv.second < 16 && !(locs >> kv.second & 1)) { vl.attr[vl.nAttr++] = {(uint32_t)kv.second, 15, VK_FORMAT_R32G32B32A32_SFLOAT, 0}; locs |= 1u << kv.second; needZero = true; }
	for (uint32_t si = 0; si < 15; si++)
		if (streamsUsed >> si & 1)
		{
			bool perInst = instanced && (dev.streams[si].freq & 0x80000000u);
			uint32_t stride = (convMask >> si & 1) ? 16 * conv[si].n : dc.upVerts ? dc.upStride : dev.streams[si].stride;
			vl.bind[vl.nBind++] = {si, stride, perInst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
		}
	if (needZero) vl.bind[vl.nBind++] = {15, 0, VK_VERTEX_INPUT_RATE_VERTEX};

	VkShaderModule vm = vsModule(vs, intMask, uintMask);
	if (!vm) return;
	// pipeline
	const uint32_t* rs = dev.rs;
	PipeKey pk;
	memset(&pk, 0, sizeof pk);
	pk.vs = vm; pk.ps = pm;
	pk.vtx = fnv(vl.attr, sizeof(vl.attr[0]) * vl.nAttr, fnv(vl.bind, sizeof(vl.bind[0]) * vl.nBind, vl.nAttr * 131 + vl.nBind));
	bool blend = rs[27] != 0 && t.color;
	if (blend)
	{
		bool sep = rs[206] != 0;
		pk.blend[0] = 1;
		pk.blend[1] = blendFactor(rs[19], false); pk.blend[2] = blendFactor(rs[20], false); pk.blend[3] = blendOp(rs[171]);
		pk.blend[4] = blendFactor(sep ? rs[207] : rs[19], true); pk.blend[5] = blendFactor(sep ? rs[208] : rs[20], true); pk.blend[6] = blendOp(sep ? rs[209] : rs[171]);
	}
	if (blend)
	{
		// float32 targets cannot blend on every GPU
		static std::unordered_map<int, bool> canBlend;
		auto it = canBlend.find((int)t.color->fmt);
		if (it == canBlend.end())
		{
			VkFormatProperties fp;
			vkGetPhysicalDeviceFormatProperties(vkb::handles().phys, t.color->fmt, &fp);
			it = canBlend.emplace((int)t.color->fmt, (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0).first;
			if (!it->second) log("vulkan: format %d cannot blend: blending off for it", (int)t.color->fmt);
		}
		if (!it->second) memset(pk.blend, 0, sizeof pk.blend);
	}
	pk.colorMask = rs[168] & 15;
	pk.cf = t.color ? t.color->fmt : VK_FORMAT_UNDEFINED;
	pk.df = t.depth ? t.depth->fmt : VK_FORMAT_UNDEFINED;
	pk.hasStencil = t.depth && t.depth->stencil;
	pk.topo = topoClass(dc.prim);
	VkPipeline pipe = pipeline(pk, vl);
	if (!pipe) return;
	if (S.pipe != pipe) { vkCmdBindPipeline(C, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); S.pipe = pipe; }

	// dynamic state (only what changed)
	bool full = !S.valid;
	S.valid = true;
	bool hasDepth = t.depth != nullptr;
	{
		uint32_t vx = dev.vp[0], vy = dev.vp[1], vw = dev.vp[2], vh = dev.vp[3];
		if (!vw || !vh) { vx = vy = 0; vw = t.w; vh = t.h; }
		VkViewport v{(float)vx, (float)vy, (float)vw, (float)vh, dev.vpZ[0], dev.vpZ[1]};
		if (full || memcmp(&v, &S.vp, sizeof v)) { vkCmdSetViewport(C, 0, 1, &v); S.vp = v; }
		VkRect2D sc{{0, 0}, {t.w, t.h}};
		if (rs[174] && dev.scissor[2] > dev.scissor[0])
		{
			float x0 = dev.scissor[0], y0 = dev.scissor[1], x1 = dev.scissor[2], y1 = dev.scissor[3];
			if (kUi != 0.f) { float s = kUi + 1.f; x0 = ax + (x0 - ax) * s; y0 = ay + (y0 - ay) * s; x1 = ax + (x1 - ax) * s; y1 = ay + (y1 - ay) * s; }   // the panel's clip grows with it
			int32_t ix0 = std::max(0, (int32_t)x0), iy0 = std::max(0, (int32_t)y0), ix1 = std::min((int32_t)t.w, (int32_t)(x1 + 0.5f)), iy1 = std::min((int32_t)t.h, (int32_t)(y1 + 0.5f));
			sc = {{ix0, iy0}, {(uint32_t)std::max(0, ix1 - ix0), (uint32_t)std::max(0, iy1 - iy0)}};
		}
		if (full || memcmp(&sc, &S.sc, sizeof sc)) { vkCmdSetScissor(C, 0, 1, &sc); S.sc = sc; }
	}
	if (full) pSetFrontFace(C, VK_FRONT_FACE_CLOCKWISE);   // Direct3D's front faces are clockwise on screen, as in this frame
	{
		uint32_t cull = rs[22] == 2 ? VK_CULL_MODE_FRONT_BIT : rs[22] == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
		if (full || S.cull != cull) { pSetCullMode(C, cull); S.cull = cull; }
		uint32_t tp = topo(dc.prim);
		if (full || S.topo != tp) { pSetTopology(C, (VkPrimitiveTopology)tp); S.topo = tp; }
		uint32_t dt = rs[7] && hasDepth, dw = rs[14] && hasDepth, dcmp = cmpOp(rs[23]);
		if (full || S.dTest != dt) { pSetDepthTest(C, dt); S.dTest = dt; }
		if (full || S.dWrite != dw) { pSetDepthWrite(C, dw); S.dWrite = dw; }
		if (full || S.dCmp != dcmp) { pSetDepthCompare(C, (VkCompareOp)dcmp); S.dCmp = dcmp; }
		uint32_t stOn = rs[52] && pk.hasStencil;
		if (full || S.sTest != stOn) { pSetStencilTest(C, stOn); S.sTest = stOn; }
		uint32_t ops[4] = {(uint32_t)stOp(rs[53]), (uint32_t)stOp(rs[55]), (uint32_t)stOp(rs[54]), (uint32_t)cmpOp(rs[56])};
		if (full || memcmp(ops, S.sOps, sizeof ops)) { pSetStencilOp(C, VK_STENCIL_FACE_FRONT_AND_BACK, (VkStencilOp)ops[0], (VkStencilOp)ops[1], (VkStencilOp)ops[2], (VkCompareOp)ops[3]); memcpy(S.sOps, ops, sizeof ops); }
		uint32_t mr = rs[58] & 255, mw = rs[59] & 255, ref = rs[57] & 255;
		if (full || S.sMaskR != mr) { vkCmdSetStencilCompareMask(C, VK_STENCIL_FACE_FRONT_AND_BACK, mr); S.sMaskR = mr; }
		if (full || S.sMaskW != mw) { vkCmdSetStencilWriteMask(C, VK_STENCIL_FACE_FRONT_AND_BACK, mw); S.sMaskW = mw; }
		if (full || S.sRef != ref) { vkCmdSetStencilReference(C, VK_STENCIL_FACE_FRONT_AND_BACK, ref); S.sRef = ref; }
		float bias, slope;
		memcpy(&bias, &rs[195], 4); memcpy(&slope, &rs[175], 4);
		uint32_t bon = (bias != 0.f || slope != 0.f) && hasDepth;
		if (full || S.biasOn != bon) { pSetDepthBiasEnable(C, bon); S.biasOn = bon; }
		if (bon && (full || S.bias[0] != bias || S.bias[1] != slope)) { vkCmdSetDepthBias(C, bias * 16777216.f, 0.f, slope); S.bias[0] = bias; S.bias[1] = slope; }
		uint32_t bf = rs[193];
		float bc[4] = {((bf >> 16) & 255) / 255.f, ((bf >> 8) & 255) / 255.f, (bf & 255) / 255.f, ((bf >> 24) & 255) / 255.f};
		if (full || memcmp(bc, S.blendC, sizeof bc)) { vkCmdSetBlendConstants(C, bc); memcpy(S.blendC, bc, sizeof bc); }
	}

	// constants
	for (int attempt = 0;; attempt++)
	{
		static uint8_t blk[shdr::kVkConstRange];
		VkDeviceSize off[2] = {0, 0}; size_t chunk[2] = {~(size_t)0, ~(size_t)0};
		for (int stage = 0; stage < 2; stage++)
		{
			ShaderObj* sh = stage ? ps : vs;
			shdr::VkConstLayout cl = shdr::vkConstLayout(sh->info);
			if (!cl.size) continue;
			uint32_t nreg = (uint32_t)cl.nc, size = cl.size;
			if (cl.ints) memcpy(blk, stage ? dev.psI : dev.vsI, 256);
			const float* src = stage ? dev.psF : dev.vsF;
			if (!stage && sh->info.relC)
			{
				// relative addressing reads the shader's own def'd constants from the registers too (particle tables);
				// only as far as the shader can reach
				if (!sh->info.relN)
				{
					int m = sh->info.numCDirect;
					for (auto& c : sh->info.ctab) if (c.regSet == 2) m = std::max(m, (int)(c.regIndex + std::max(1u, c.regCount)));
					for (auto& d : sh->info.defF) m = std::max(m, d.first + 1);
					sh->info.relN = std::max(1, std::min(256, m));
				}
				nreg = std::min<uint32_t>(nreg, sh->info.relN);
				size = cl.cOff + 16 * nreg;
				memcpy(blk + cl.cOff, src, 16 * nreg);
				for (auto& d : sh->info.defF) if (d.first >= 0 && (uint32_t)d.first < nreg) memcpy(blk + cl.cOff + 16 * d.first, d.second.data(), 16);
			}
			else memcpy(blk + cl.cOff, src, 16 * nreg);
			if (!writeConsts(stage, blk, size, off[stage], chunk[stage])) return;
		}
		// both blocks must come from one chunk (one descriptor set): rewrite the other one if they differ
		if (chunk[0] != ~(size_t)0 && chunk[1] != ~(size_t)0 && chunk[0] != chunk[1])
		{
			if (attempt >= 2) return;
			gCC[0].chunk = gCC[1].chunk = ~(size_t)0;   // the ring moved to a new chunk: write both there
			continue;
		}
		size_t ch = chunk[0] != ~(size_t)0 ? chunk[0] : chunk[1] != ~(size_t)0 ? chunk[1] : F->chunk;
		if (ch >= F->chunks.size()) { RingPos rp; if (!ringAlloc(16, 16, rp)) return; ch = rp.chunk; }
		VkDescriptorSet cs = F->chunks[ch].constSet;
		uint32_t o2[2] = {(uint32_t)off[0], (uint32_t)off[1]};
		if (S.constSet != cs || S.constOff[0] != o2[0] || S.constOff[1] != o2[1])
		{
			vkCmdBindDescriptorSets(C, VK_PIPELINE_BIND_POINT_GRAPHICS, gLayout, 0, 1, &cs, 2, o2);
			S.constSet = cs; S.constOff[0] = o2[0]; S.constOff[1] = o2[1];
		}
		break;
	}
	{
		Push p{};
		p.ui[0] = axN; p.ui[1] = ayN; p.ui[2] = kUi;
		uint32_t vw = dev.vp[2] ? dev.vp[2] : t.w, vh = dev.vp[3] ? dev.vp[3] : t.h;
		p.hp[0] = vw ? 1.f / vw : 0.f; p.hp[1] = vh ? 1.f / vh : 0.f;
		p.bgra = 0;
		for (int i = 0; i < 16; i++) { if (dev.vsB[i]) p.vsb |= 1u << i; if (dev.psB[i]) p.psb |= 1u << i; }
		p.at[0] = rs[15] ? (float)rs[25] : 0.f; p.at[1] = (rs[24] & 255) / 255.f;
		if (!S.pushValid || memcmp(&p, &S.push, sizeof p)) { vkCmdPushConstants(C, gLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p); S.push = p; S.pushValid = true; }
	}
	// textures
	if (mask)
	{
		bool same = S.texSet && S.texMask == mask;
		for (int i = 0; i < 16 && same; i++) if (mask >> i & 1) same = S.texViews[i] == views[i] && S.texSamp[i] == samps[i];
		if (!same)
		{
			VkDescriptorSet set = allocTexSet();
			if (!set) return;
			VkDescriptorImageInfo ii[16]; VkWriteDescriptorSet w[16]; uint32_t nw = 0;
			for (int i = 0; i < 16; i++)
				if (mask >> i & 1)
				{
					ii[nw] = {samps[i], views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
					w[nw] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
					w[nw].dstSet = set; w[nw].dstBinding = i; w[nw].descriptorCount = 1; w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[nw];
					nw++;
				}
			vkUpdateDescriptorSets(D, nw, w, 0, nullptr);
			vkCmdBindDescriptorSets(C, VK_PIPELINE_BIND_POINT_GRAPHICS, gLayout, 1, 1, &set, 0, nullptr);
			S.texSet = set; S.texMask = mask;
			memcpy(S.texViews, views, sizeof views); memcpy(S.texSamp, samps, sizeof samps);
		}
	}

	// vertex and index data
	const uint32_t nv = (uint32_t)primVertices(dc.prim, dc.primCount);
	if (!nv) return;
	int32_t vtxOff = dc.indexed ? dc.baseVertex : 0;
	if (dc.upVerts)
	{
		uint32_t nverts = dc.indexed ? dc.minIndex + dc.numVertices : nv;
		RingPos rp;
		if (convMask & 1)
		{
			if (!ringAlloc((VkDeviceSize)nverts * 16 * conv[0].n, 16, rp)) return;
			convertVerts(conv[0], (const uint8_t*)dc.upVerts, dc.upStride, nverts, (float*)rp.ptr);
		}
		else
		{
			if (!ringAlloc((VkDeviceSize)nverts * dc.upStride, 16, rp)) return;
			memcpy(rp.ptr, dc.upVerts, (size_t)nverts * dc.upStride);
		}
		VkDeviceSize o = rp.off;
		vkCmdBindVertexBuffers(C, 0, 1, &rp.buf, &o);
		vtxOff = 0;
	}
	else
	{
		// dynamic streams: the vertices this draw uses copied to the ring (the game rewrites them for the next draw)
		uint32_t useMin = dc.minIndex, useCount = dc.numVertices;
		bool anyRing = false;
		for (uint32_t si = 0; si < 15; si++)
			if ((streamsUsed >> si & 1) && (dev.streams[si].vb->usage & 0x200) && !(instanced && (dev.streams[si].freq & 0x80000000u))) anyRing = true;
		if (anyRing && dc.indexed && !dc.upIdx && dev.ib && dev.ib->data && (uint64_t)(dc.startIndex + nv) * (dev.ib->fmt == 102 ? 4 : 2) <= dev.ib->size)
		{
			const uint8_t* ip = (const uint8_t*)guest::host(dev.ib->data) + (uint64_t)dc.startIndex * (dev.ib->fmt == 102 ? 4 : 2);
			uint32_t lo = 0xffffffffu, hi = 0;
			if (dev.ib->fmt == 102) for (uint32_t i = 0; i < nv; i++) { uint32_t v; memcpy(&v, ip + 4 * i, 4); lo = std::min(lo, v); hi = std::max(hi, v); }
			else for (uint32_t i = 0; i < nv; i++) { uint16_t v; memcpy(&v, ip + 2 * i, 2); lo = std::min<uint32_t>(lo, v); hi = std::max<uint32_t>(hi, v); }
			useMin = lo; useCount = hi - lo + 1;
		}
		if (anyRing && dc.indexed) vtxOff = -(int32_t)useMin;
		for (uint32_t si = 0; si < 15; si++)
		{
			if (!(streamsUsed >> si & 1)) continue;
			Stream& st = dev.streams[si];
			bool perInst = instanced && (st.freq & 0x80000000u);
			VkBuffer b = VK_NULL_HANDLE; VkDeviceSize o = 0;
			if (!perInst && (st.vb->usage & 0x200) && st.vb->data && st.stride)
			{
				int64_t first = dc.indexed ? (int64_t)dc.baseVertex + useMin : dc.baseVertex;
				int64_t count = dc.indexed ? useCount : nv;
				int64_t avail = ((int64_t)st.vb->size - (int64_t)st.offset) / (int64_t)st.stride - first;
				if (count > avail) count = avail;
				RingPos rp;
				bool cv = convMask >> si & 1;
				if (first >= 0 && count > 0 && ringAlloc((VkDeviceSize)count * (cv ? 16 * conv[si].n : st.stride), 16, rp))
				{
					const uint8_t* src = (const uint8_t*)guest::host(st.vb->data) + st.offset + (uint64_t)first * st.stride;
					if (cv) convertVerts(conv[si], src, st.stride, (uint32_t)count, (float*)rp.ptr);
					else memcpy(rp.ptr, src, (size_t)count * st.stride);
					b = rp.buf; o = rp.off;
				}
			}
			if (!b && (convMask >> si & 1))
			{
				// a static buffer rewritten once per version (and per layout)
				if (!st.vb->data || !st.stride || st.offset >= st.vb->size) return;
				ConvBuf& cb = gConv[st.vb];
				uint64_t key = fnv(&conv[si], sizeof conv[si], st.stride * 1000003ull + st.offset);
				uint32_t outStride = 16 * conv[si].n;
				if (!cb.buf || cb.ver != st.vb->ver || cb.key != key)
				{
					uint32_t n = (st.vb->size - st.offset) / st.stride;
					VBuf* nb = new VBuf;
					VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
					bi.size = std::max<VkDeviceSize>(16, (VkDeviceSize)n * outStride); bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
					VmaAllocationCreateInfo ai{}; ai.usage = VMA_MEMORY_USAGE_AUTO; ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
					VmaAllocationInfo inf;
					if (!vkOk(vmaCreateBuffer(gA, &bi, &ai, &nb->buf, &nb->mem, &inf), "converted vertex buffer")) { delete nb; return; }
					nb->map = (uint8_t*)inf.pMappedData; nb->size = (uint32_t)bi.size;
					convertVerts(conv[si], (const uint8_t*)guest::host(st.vb->data) + st.offset, st.stride, n, (float*)nb->map);
					vmaFlushAllocation(gA, nb->mem, 0, VK_WHOLE_SIZE);
					if (cb.buf) destroyBuf(cb.buf);
					cb.buf = nb; cb.ver = st.vb->ver; cb.key = key;
					static int nl = 0;
					if (nl++ < 6) log("vulkan: vertex buffer of %u vertices rewritten as floats (%u elements)", n, conv[si].n);
				}
				st.vb->lastUse = glr::frameNo();
				b = cb.buf->buf;
				int64_t base = perInst ? 0 : (dc.indexed ? (int64_t)dc.baseVertex - vtxOff : dc.baseVertex);
				if (base < 0) return;
				o = (VkDeviceSize)base * outStride;
			}
			if (!b)
			{
				VBuf* vb = ensureBuf(st.vb, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
				if (!vb) return;
				b = vb->buf;
				int64_t base = perInst ? 0 : (dc.indexed ? (int64_t)dc.baseVertex - vtxOff : dc.baseVertex);
				int64_t oo = (int64_t)st.offset + base * (int64_t)st.stride;
				if (oo < 0) return;
				o = (VkDeviceSize)oo;
			}
			vkCmdBindVertexBuffers(C, si, 1, &b, &o);
		}
	}
	if (needZero) { VkDeviceSize zo = 0; vkCmdBindVertexBuffers(C, 15, 1, &gZeroVb, &zo); }
	{
		// diagnostics while the Vulkan renderer is being brought up: the first draws of each shader / vertex layout
		// in a battle (frame 900 on): layout, streams, draw parameters and the first vertex as the game wrote it
		static std::unordered_map<uint64_t, int> seen;
		static int logged = 0;
		uint32_t fr = glr::frameNo();
		uint64_t key = vs->hash ^ fnv(dev.vdecl->blob.data(), dev.vdecl->blob.size()) * 31 ^ (uint64_t)dc.indexed << 63;
		if (fr >= 900 && logged < 150 && seen[key]++ == 0)
		{
			logged++;
			char b[256];
			std::string o;
			snprintf(b, sizeof b, "vkdiag #%d f%u vs %016llx ps %016llx prim %u n %u idx %d base %d start %u min %u nv %u inst %u up %d vtxOff %d rt %ux%u f%d z %u blend %u |",
				logged, fr, (unsigned long long)vs->hash, (unsigned long long)ps->hash, dc.prim, dc.primCount, (int)dc.indexed, (int)dc.baseVertex, dc.startIndex, dc.minIndex, dc.numVertices,
				instCount, dc.upVerts ? 1 : 0, (int)vtxOff, t.w, t.h, t.color ? (int)t.color->fmt : -1, rs[7], rs[27]);
			o += b;
			for (uint32_t k = 0; k < nEls; k++)
			{
				snprintf(b, sizeof b, " s%u+%u:t%u:u%u.%u->L%d", els[k].stream, els[k].offset, els[k].type, els[k].usage, els[k].index, shdr::attrLocation(els[k].usage, els[k].index));
				o += b;
			}
			o += " |";
			for (uint32_t si = 0; si < 15; si++)
				if (streamsUsed >> si & 1)
				{
					const Stream& st = dev.streams[si];
					snprintf(b, sizeof b, " stream%u stride %u off %u size %u usage %x freq %x", si, dc.upVerts ? dc.upStride : st.stride, st.offset, st.vb ? st.vb->size : 0, st.vb ? st.vb->usage : 0, st.freq);
					o += b;
				}
			if (dev.ib && dc.indexed && !dc.upIdx) { snprintf(b, sizeof b, " | ib fmt %u size %u usage %x", dev.ib->fmt, dev.ib->size, dev.ib->usage); o += b; }
			o += " | tex";
			for (int i = 0; i < 16; i++)
				if (mask >> i & 1)
				{
					Obj* tt = dev.tex[i] ? texOwner(dev.tex[i]) : nullptr;
					VTex* vt = tt ? (VTex*)tt->vk : nullptr;
					snprintf(b, sizeof b, " %d:%s%ux%u f%u l%u a%u/%u%s%s", i, tt ? "" : "none ", tt ? tt->w : 0, tt ? tt->h : 0, tt ? tt->fmt : 0, tt ? tt->levels : 0, dev.samp[i][1], dev.samp[i][2],
						vt && vt->rt ? (vt->content ? " RT" : " RT-EMPTY") : "", vt && (vt == t.color || vt == t.depth) ? " SELF" : "");
					o += b;
				}
			log("%s", o.c_str());
			// the first three vertices of the position stream
			for (uint32_t k = 0; k < nEls; k++)
			{
				if (els[k].usage != 0 || els[k].index != 0) continue;
				const Stream& st = dev.streams[els[k].stream < 15 ? els[k].stream : 0];
				const uint8_t* base = nullptr; uint32_t stride = 0, size = 0;
				if (dc.upVerts) { base = (const uint8_t*)dc.upVerts; stride = dc.upStride; size = 1u << 30; }
				else if (st.vb && st.vb->data) { base = (const uint8_t*)guest::host(st.vb->data) + st.offset; stride = st.stride; size = st.vb->size - st.offset; }
				if (!base) break;
				std::string v;
				for (uint32_t q = 0; q < 3; q++)
				{
					int64_t vi = dc.indexed ? 0 : (dc.upVerts ? 0 : dc.baseVertex);
					if (dc.indexed && dev.ib && dev.ib->data && !dc.upIdx)
					{
						const uint8_t* ip = (const uint8_t*)guest::host(dev.ib->data);
						vi = (int64_t)dc.baseVertex + (dev.ib->fmt == 102 ? ((const uint32_t*)ip)[dc.startIndex + q] : ((const uint16_t*)ip)[dc.startIndex + q]);
					}
					else vi += q;
					uint64_t off = (uint64_t)vi * stride + els[k].offset;
					if (vi < 0 || off + 16 > size) { v += " (out)"; break; }
					const uint8_t* p = base + off;
					if (els[k].type <= 3) { float f[4] = {0, 0, 0, 0}; memcpy(f, p, 4 * (els[k].type + 1)); snprintf(b, sizeof b, " v%lld(%g %g %g %g)", (long long)vi, f[0], f[1], f[2], f[3]); }
					else { int16_t h[4]; memcpy(h, p, 8); snprintf(b, sizeof b, " v%lld[%d %d %d %d]", (long long)vi, h[0], h[1], h[2], h[3]); }
					v += b;
				}
				log("vkdiag   position%s", v.c_str());
				break;
			}
		}
	}
	if (!dc.indexed) vkCmdDraw(C, nv, instCount, 0, 0);
	else if (dc.upIdx)
	{
		bool wide = dc.upIdxFmt == 102;
		RingPos rp;
		if (!ringAlloc((VkDeviceSize)nv * (wide ? 4 : 2), 16, rp)) return;
		memcpy(rp.ptr, dc.upIdx, (size_t)nv * (wide ? 4 : 2));
		vkCmdBindIndexBuffer(C, rp.buf, rp.off, wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(C, nv, 1, 0, vtxOff, 0);
	}
	else if (dev.ib)
	{
		bool wide = dev.ib->fmt == 102;
		uint32_t isz = wide ? 4 : 2;
		VkIndexType it = wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
		RingPos rp;
		if ((dev.ib->usage & 0x200) && dev.ib->data && (uint64_t)(dc.startIndex + nv) * isz <= dev.ib->size && ringAlloc((VkDeviceSize)nv * isz, 16, rp))
		{
			memcpy(rp.ptr, (const uint8_t*)guest::host(dev.ib->data) + (uint64_t)dc.startIndex * isz, (size_t)nv * isz);
			vkCmdBindIndexBuffer(C, rp.buf, rp.off, it);
			vkCmdDrawIndexed(C, nv, instCount, 0, vtxOff, 0);
		}
		else
		{
			VBuf* ib = ensureBuf(dev.ib, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
			if (!ib) return;
			vkCmdBindIndexBuffer(C, ib->buf, 0, it);
			vkCmdDrawIndexed(C, nv, instCount, dc.startIndex, vtxOff, 0);
		}
	}
	gDraws++; gDrawsTotal++;
	if (dumping())
		log("vkdump pass %d draw: vs %016llx ps %016llx prim %u n %u rt %ux%u f%d depth %d | z %u zw %u zf %u cull %u blend %u %u/%u cw %x st %u at %u/%u bias %x/%x vp %u,%u,%u,%u sc %u",
			gDumpPass, (unsigned long long)vs->hash, (unsigned long long)ps->hash, dc.prim, dc.primCount, t.w, t.h, t.color ? (int)t.color->fmt : -1, t.depth ? (int)t.depth->fmt : -1,
			rs[7], rs[14], rs[23], rs[22], rs[27], rs[19], rs[20], rs[168], rs[52], rs[15], rs[24], rs[195], rs[175], dev.vp[0], dev.vp[1], dev.vp[2], dev.vp[3], rs[174]);
	if (t.color) t.color->content++;
	if (t.depth) t.depth->content++;
}

void stretchRect(Obj* src, const int32_t* sr, Obj* dst, const int32_t* dr, uint32_t filter)
{
	std::lock_guard<std::recursive_mutex> l(gMu);
	if (!gOk) return;
	ensureBack();
	auto side = [&](Obj* o, VTex*& t, uint32_t& level, uint32_t& layer, uint32_t& w, uint32_t& h) {
		if (isBack(o)) { t = gBack; level = layer = 0; w = backW; h = backH; return; }
		Obj* ow = texOwner(o);
		t = ensureTex(o);
		level = o->iface == ISurface ? o->level : 0;
		layer = ow->iface == ICube && o->iface == ISurface ? o->face : 0;
		w = std::max(1u, ow->w >> level); h = std::max(1u, ow->h >> level);
	};
	VTex *s, *d; uint32_t sl, sa, sw, sh, dl, da, dw, dh;
	side(src, s, sl, sa, sw, sh);
	side(dst, d, dl, da, dw, dh);
	if (!s || !d || s == d) return;
	if (s->depth || d->depth) { static int n = 0; if (n++ < 4) log("vulkan: depth StretchRect not supported"); return; }
	int32_t a[4] = {0, 0, (int32_t)sw, (int32_t)sh}, b[4] = {0, 0, (int32_t)dw, (int32_t)dh};
	if (sr) memcpy(a, sr, 16);
	if (dr) memcpy(b, dr, 16);
	toLayout(s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	toLayout(d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	VkImageBlit bl{};
	bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, sl, sa, 1}; bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dl, da, 1};
	bl.srcOffsets[0] = {a[0], a[1], 0}; bl.srcOffsets[1] = {a[2], a[3], 1};
	bl.dstOffsets[0] = {b[0], b[1], 0}; bl.dstOffsets[1] = {b[2], b[3], 1};
	vkCmdBlitImage(C, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, filter == 2 && !s->noFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
	if (dumping()) { log("vkdump stretch %ux%u f%d [%d,%d,%d,%d] -> %ux%u f%d [%d,%d,%d,%d]", sw, sh, (int)s->fmt, a[0], a[1], a[2], a[3], dw, dh, (int)d->fmt, b[0], b[1], b[2], b[3]); d->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; dumpTarget(d, dl, da, "copy"); }
	s->useSerial = d->useSerial = gSerial;
}

bool present(int sw, int sh)
{
	std::lock_guard<std::recursive_mutex> l(gMu);
	if (!gOk) return false;
	static bool everDrew = false;
	if (gDrawsTotal) everDrew = true;
	uint32_t frame = glr::frameNo();
	if (!everDrew)
	{
		// nothing of the game yet: the caller shows the start-up console; the recorded frame (uploads) goes out now
		endPass();
		vkEndCommandBuffer(C);
		VkCommandBuffer cbs[2]; uint32_t n = 0;
		if (F->upUsed) { vkEndCommandBuffer(F->up); cbs[n++] = F->up; }
		cbs[n++] = C;
		vkb::submit(cbs, n, F->fence, VK_NULL_HANDLE, -1);
		gSerial++; gFi = (gFi + 1) % kFrames; beginFrame();
		return false;
	}
	ensureBack();
	endPass();
	uint32_t idx = 0; VkImage img = VK_NULL_HANDLE; VkExtent2D ext{}; VkFormat fmt = VK_FORMAT_UNDEFINED;
	uint32_t gen = 0;
	bool have = vkb::acquire(F->acq, idx, img, ext, fmt, gen);
	static uint32_t lastGen = 0;
	if (have && gen != lastGen)
	{
		// a new swapchain (the old images are gone, their handles may come back): new views
		for (auto& kv : gSwapViews) vkDestroyImageView(D, kv.second, nullptr);
		gSwapViews.clear();
		lastGen = gen;
	}
	if (have)
	{
		// letterbox the back buffer (as the OpenGL renderer)
		sw = (int)ext.width; sh = (int)ext.height;
		double sa = (double)sw / sh, ba = (double)backW / backH;
		int vw = sw, vh = sh, vx = 0, vy = 0;
		if (sa > ba) { vw = (int)(sh * ba); vx = (sw - vw) / 2; } else { vh = (int)(sw / ba); vy = (sh - vh) / 2; }
		{
			int rl = 0, rr = 0;
			hud::reserves(sw, sh, rl, rr);
			if (vx < rl || vx + vw > sw - rr)
			{
				int aw = sw - rl - rr;
				vw = std::min(aw, (int)(sh * ba)); vh = (int)(vw / ba);
				vx = rl + (aw - vw) / 2; vy = (sh - vh) / 2;
			}
		}
		input::setView(vx, vy, vw, vh, (int)backW, (int)backH);
		barrier(C, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkClearColorValue black{{0.f, 0.f, 0.f, 1.f}};
		VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdClearColorImage(C, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rng);
		toLayout(gBack, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkImageBlit bl{};
		bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		bl.srcOffsets[1] = {(int32_t)backW, (int32_t)backH, 1};
		bl.dstOffsets[0] = {vx, vy, 0}; bl.dstOffsets[1] = {vx + vw, vy + vh, 1};
		vkCmdBlitImage(C, gBack->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
		barrier(C, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		// the screen pass: side bars outside a battle (black for now), then the on-screen buttons
		VkImageView& view = gSwapViews[img];
		if (!view)
		{
			VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt; vi.subresourceRange = rng;
			vkCreateImageView(D, &vi, nullptr, &view);
		}
		rectPipeline(fmt);
		VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
		ca.imageView = view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
		ri.renderArea = {{0, 0}, ext}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
		pBeginRendering(C, &ri);
		{
			bool inGame, blur;
			sideBars(inGame, blur);
			int cw = vh * 16 / 9;
			if (!inGame && vw > cw + 4)
			{
				int cx = vx + (vw - cw) / 2;
				VkClearAttachment a{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
				a.clearValue.color = black;
				VkClearRect cr[2] = {{{{0, 0}, {(uint32_t)std::max(1, cx), (uint32_t)sh}}, 0, 1}, {{{cx + cw, 0}, {(uint32_t)std::max(1, sw - cx - cw), (uint32_t)sh}}, 0, 1}};
				vkCmdClearAttachments(C, 1, &a, 2, cr);
			}
		}
		if (gRectPipe)
		{
			static std::vector<HudRect> rects;
			hudRects(sw, sh, rects);
			if (!rects.empty())
			{
				vkCmdBindPipeline(C, VK_PIPELINE_BIND_POINT_GRAPHICS, gRectPipe);
				VkViewport v{0.f, 0.f, (float)sw, (float)sh, 0.f, 1.f};
				VkRect2D sc{{0, 0}, ext};
				vkCmdSetViewport(C, 0, 1, &v); vkCmdSetScissor(C, 0, 1, &sc);
				for (const HudRect& q : rects)
				{
					// Vulkan clip space: y down
					float pc[8] = {2.f * q.x / sw - 1.f, 2.f * q.y / sh - 1.f, 2.f * (q.x + q.w) / sw - 1.f, 2.f * (q.y + q.h) / sh - 1.f, q.r, q.g, q.b, q.a};
					vkCmdPushConstants(C, gRectLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
					vkCmdDraw(C, 4, 1, 0, 0);
				}
				S.pipe = VK_NULL_HANDLE; S.valid = false;
			}
		}
		pEndRendering(C);
		barrier(C, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	}
	vkEndCommandBuffer(C);
	VkCommandBuffer cbs[2]; uint32_t n = 0;
	if (F->upUsed) { vkEndCommandBuffer(F->up); cbs[n++] = F->up; }
	cbs[n++] = C;
	vkb::submit(cbs, n, F->fence, F->acq, have ? (int)idx : -1);
	gSerial++;
	gFi = (gFi + 1) % kFrames;
	beginFrame();
	gDraws = 0;
	if (frame % 600 == 0) reap(frame);
	if (gNewPipes && (frame == 300 || frame % 3600 == 0)) savePipeCache();
	return true;
}
}   // namespace vkr
}   // namespace d3d
#endif
