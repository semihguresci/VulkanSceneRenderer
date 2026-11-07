#pragma once

#include "Container/common/CommonVulkan.h"

#ifndef VMA_VULKAN_HPP
#define VMA_VULKAN_HPP 1
#endif

#ifndef VMA_DYNAMIC_VULKAN_FUNCTIONS
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#endif
#ifndef VMA_STATIC_VULKAN_FUNCTIONS
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#endif

#include <vk_mem_alloc.h>

#ifndef VMA_VERSION
#error "VMA release version is unavailable; check the selected include target"
#endif
static_assert(VMA_VERSION == VK_MAKE_VERSION(3, 5, 0), "This build requires VMA 3.5.0");
