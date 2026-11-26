// Vulkan Memory Allocator (third_party/vma, MIT): the implementation, for the Vulkan renderer
#ifdef __ANDROID__
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_VULKAN_VERSION 1001000   // Android 10's libvulkan exports Vulkan 1.1
#include <vulkan/vulkan.h>
#include "third_party/vma/vk_mem_alloc.h"
#endif
