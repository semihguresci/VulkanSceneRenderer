#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

enum class Mode { Visible, Covered, Hidden };

struct Swapchain {
    unsigned generation;
    VkExtent2D extent;
    bool clipped;
};

struct RenderingScope {
    VkSwapchainKHR swapchain;
    unsigned frame;
    bool loggedScissor = false;
};

// This fixture is for one renderer instance, device and surface. The old and
// new swapchains can coexist during recreation. Match only their color views.
std::mutex mutex;
VkInstance instance = VK_NULL_HANDLE;
VkDevice device = VK_NULL_HANDLE;
VkSurfaceKHR targetSurface = VK_NULL_HANDLE;
PFN_vkGetInstanceProcAddr nextGipa = nullptr;
PFN_GetPhysicalDeviceProcAddr nextGpdpa = nullptr;
PFN_vkGetDeviceProcAddr nextGdpa = nullptr;
std::unordered_map<VkSwapchainKHR, Swapchain> swapchains;
std::unordered_map<VkImage, VkSwapchainKHR> imageOwners;
std::unordered_map<VkImageView, VkSwapchainKHR> viewOwners;
std::unordered_map<VkCommandBuffer, RenderingScope> renderingScopes;
unsigned nextGeneration = 1;
unsigned renderedFrames = 0;
unsigned startFrame = 1;
Mode mode = Mode::Visible;
const char* modeName = "visible";
bool forceTrue = false;

bool configure() {
    char value[32]{};
    if (GetEnvironmentVariableA("CONTAINER_CLIP_TEST_MODE", value, sizeof(value)) >= sizeof(value))
        return false;
    if (std::strcmp(value, "covered") == 0) {
        mode = Mode::Covered;
        modeName = "covered";
    } else if (std::strcmp(value, "hidden") == 0) {
        mode = Mode::Hidden;
        modeName = "hidden";
    } else if (value[0] == '\0' || std::strcmp(value, "visible") == 0) {
        mode = Mode::Visible;
        modeName = "visible";
    } else {
        return false;
    }
    value[0] = '\0';
    if (GetEnvironmentVariableA("CONTAINER_CLIP_TEST_FORCE_TRUE", value, sizeof(value)) >= sizeof(value))
        return false;
    forceTrue = std::strcmp(value, "1") == 0;
    if (!forceTrue && value[0] != '\0' && std::strcmp(value, "0") != 0) return false;

    const DWORD length = GetEnvironmentVariableA("CONTAINER_CLIP_TEST_START_FRAME", value, sizeof(value));
    startFrame = 1;
    if (length == 0) return true;
    if (length >= sizeof(value)) return false;
    const auto parsed = std::from_chars(value, value + length, startFrame);
    return parsed.ec == std::errc{} && parsed.ptr == value + length && startFrame > 0;
}

template<class Function>
Function deviceProc(VkDevice value, const char* name) {
    return reinterpret_cast<Function>(nextGdpa(value, name));
}

VKAPI_ATTR VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkInstance* resultInstance) {
    if (instance != VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
    if (!configure()) {
        std::fputs("[clip-fixture] invalid mode, force flag, or start frame\n", stderr);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto* chain = reinterpret_cast<const VkLayerInstanceCreateInfo*>(info->pNext);
    while (chain && (chain->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                     chain->function != VK_LAYER_LINK_INFO)) {
        chain = reinterpret_cast<const VkLayerInstanceCreateInfo*>(chain->pNext);
    }
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto* link = chain->u.pLayerInfo;
    nextGipa = link->pfnNextGetInstanceProcAddr;
    nextGpdpa = link->pfnNextGetPhysicalDeviceProcAddr;
    const auto nextCreate = reinterpret_cast<PFN_vkCreateInstance>(nextGipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;
    const_cast<VkLayerInstanceCreateInfo*>(chain)->u.pLayerInfo = link->pNext;
    const auto status = nextCreate(info, allocator, resultInstance);
    if (status == VK_SUCCESS) instance = *resultInstance;
    return status;
}

VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance value, const VkAllocationCallbacks* allocator) {
    const auto nextDestroy = reinterpret_cast<PFN_vkDestroyInstance>(nextGipa(value, "vkDestroyInstance"));
    nextDestroy(value, allocator);
    instance = VK_NULL_HANDLE;
}

VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDevice* resultDevice) {
    if (device != VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;
    auto* chain = reinterpret_cast<const VkLayerDeviceCreateInfo*>(info->pNext);
    while (chain && (chain->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                     chain->function != VK_LAYER_LINK_INFO)) {
        chain = reinterpret_cast<const VkLayerDeviceCreateInfo*>(chain->pNext);
    }
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    const auto* link = chain->u.pLayerInfo;
    nextGdpa = link->pfnNextGetDeviceProcAddr;
    const auto nextCreate = reinterpret_cast<PFN_vkCreateDevice>(
        link->pfnNextGetInstanceProcAddr(instance, "vkCreateDevice"));
    if (!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;
    const_cast<VkLayerDeviceCreateInfo*>(chain)->u.pLayerInfo = link->pNext;
    const auto status = nextCreate(physical, info, allocator, resultDevice);
    if (status == VK_SUCCESS) device = *resultDevice;
    return status;
}

VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice value, const VkAllocationCallbacks* allocator) {
    deviceProc<PFN_vkDestroyDevice>(value, "vkDestroyDevice")(value, allocator);
    std::lock_guard lock(mutex);
    renderingScopes.clear();
    viewOwners.clear();
    imageOwners.clear();
    swapchains.clear();
    nextGeneration = 1;
    renderedFrames = 0;
    targetSurface = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
}

VKAPI_ATTR VkResult VKAPI_CALL createSwapchain(VkDevice value, const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* resultSwapchain) {
    bool target;
    {
        std::lock_guard lock(mutex);
        if (targetSurface == VK_NULL_HANDLE) targetSurface = info->surface;
        target = targetSurface == info->surface;
    }
    VkSwapchainCreateInfoKHR request = *info;
    if (target && forceTrue) request.clipped = VK_TRUE;
    const auto status = deviceProc<PFN_vkCreateSwapchainKHR>(value, "vkCreateSwapchainKHR")(
        value, &request, allocator, resultSwapchain);
    if (target && status == VK_SUCCESS) {
        std::lock_guard lock(mutex);
        const auto generation = nextGeneration++;
        swapchains.emplace(*resultSwapchain, Swapchain{generation, request.imageExtent, request.clipped == VK_TRUE});
        std::fprintf(stderr, "[clip-fixture] swapchain generation=%u requestedClipped=%u effectiveClipped=%u "
                             "format=%d extent=%ux%u mode=%s startFrame=%u\n",
            generation, info->clipped, request.clipped, static_cast<int>(request.imageFormat),
            request.imageExtent.width, request.imageExtent.height, modeName, startFrame);
        std::fflush(stderr);
    }
    return status;
}

VKAPI_ATTR void VKAPI_CALL destroySwapchain(VkDevice value, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* allocator) {
    {
        std::lock_guard lock(mutex);
        swapchains.erase(swapchain);
        const auto owned = [swapchain](const auto& entry) { return entry.second == swapchain; };
        std::erase_if(imageOwners, owned);
        std::erase_if(viewOwners, owned);
        std::erase_if(renderingScopes, [swapchain](const auto& entry) { return entry.second.swapchain == swapchain; });
    }
    deviceProc<PFN_vkDestroySwapchainKHR>(value, "vkDestroySwapchainKHR")(value, swapchain, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL getSwapchainImages(VkDevice value, VkSwapchainKHR swapchain,
    uint32_t* imageCount, VkImage* images) {
    const auto status = deviceProc<PFN_vkGetSwapchainImagesKHR>(value, "vkGetSwapchainImagesKHR")(
        value, swapchain, imageCount, images);
    if (images && (status == VK_SUCCESS || status == VK_INCOMPLETE)) {
        std::lock_guard lock(mutex);
        if (swapchains.contains(swapchain)) {
            for (uint32_t i = 0; i < *imageCount; ++i) imageOwners.insert_or_assign(images[i], swapchain);
        }
    }
    return status;
}

VKAPI_ATTR VkResult VKAPI_CALL createImageView(VkDevice value, const VkImageViewCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkImageView* view) {
    const auto status = deviceProc<PFN_vkCreateImageView>(value, "vkCreateImageView")(value, info, allocator, view);
    if (status == VK_SUCCESS) {
        std::lock_guard lock(mutex);
        if (const auto owner = imageOwners.find(info->image); owner != imageOwners.end()) {
            viewOwners.insert_or_assign(*view, owner->second);
        }
    }
    return status;
}

VKAPI_ATTR void VKAPI_CALL destroyImageView(VkDevice value, VkImageView view,
    const VkAllocationCallbacks* allocator) {
    {
        std::lock_guard lock(mutex);
        viewOwners.erase(view);
    }
    deviceProc<PFN_vkDestroyImageView>(value, "vkDestroyImageView")(value, view, allocator);
}

void enterRendering(VkCommandBuffer cmd, const VkRenderingInfo* info) {
    std::lock_guard lock(mutex);
    renderingScopes.erase(cmd);
    for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
        const auto owner = viewOwners.find(info->pColorAttachments[i].imageView);
        if (owner != viewOwners.end()) {
            renderingScopes.emplace(cmd, RenderingScope{owner->second, ++renderedFrames});
            return;
        }
    }
}

void leaveRendering(VkCommandBuffer cmd) {
    std::lock_guard lock(mutex);
    renderingScopes.erase(cmd);
}

VKAPI_ATTR void VKAPI_CALL beginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info) {
    enterRendering(cmd, info);
    deviceProc<PFN_vkCmdBeginRendering>(device, "vkCmdBeginRendering")(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL beginRenderingKhr(VkCommandBuffer cmd, const VkRenderingInfo* info) {
    enterRendering(cmd, info);
    deviceProc<PFN_vkCmdBeginRenderingKHR>(device, "vkCmdBeginRenderingKHR")(cmd, info);
}

VKAPI_ATTR void VKAPI_CALL endRendering(VkCommandBuffer cmd) {
    deviceProc<PFN_vkCmdEndRendering>(device, "vkCmdEndRendering")(cmd);
    leaveRendering(cmd);
}

VKAPI_ATTR void VKAPI_CALL endRenderingKhr(VkCommandBuffer cmd) {
    deviceProc<PFN_vkCmdEndRenderingKHR>(device, "vkCmdEndRenderingKHR")(cmd);
    leaveRendering(cmd);
}

VkRect2D exposedScissor(VkRect2D requested, VkExtent2D extent) {
    if (mode == Mode::Hidden) return {requested.offset, {0, 0}};
    const int64_t split = extent.width / 2;
    const int64_t start = std::clamp<int64_t>(requested.offset.x, 0, split);
    const int64_t end = std::clamp<int64_t>(int64_t(requested.offset.x) + requested.extent.width, start, split);
    requested.offset.x = static_cast<int32_t>(start);
    requested.extent.width = static_cast<uint32_t>(end - start);
    return requested;
}

VKAPI_ATTR void VKAPI_CALL setScissor(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkRect2D* rects) {
    std::vector<VkRect2D> clipped;
    {
        std::lock_guard lock(mutex);
        const auto scope = renderingScopes.find(cmd);
        if (scope != renderingScopes.end() && count != 0) {
            auto& rendering = scope->second;
            const auto& swapchain = swapchains.at(rendering.swapchain);
            const bool apply = swapchain.clipped && mode != Mode::Visible && rendering.frame >= startFrame;
            if (apply) {
                clipped.reserve(count);
                for (uint32_t i = 0; i < count; ++i) clipped.push_back(exposedScissor(rects[i], swapchain.extent));
            }
            if (!rendering.loggedScissor) {
                const auto original = rects[0];
                const auto result = apply ? clipped[0] : original;
                std::fprintf(stderr, "[clip-fixture] scissor generation=%u mode=%s applied=%d frame=%u "
                                     "x=%d y=%d width=%u height=%u clippedX=%d clippedY=%d clippedWidth=%u clippedHeight=%u\n",
                    swapchain.generation, modeName, int(apply), rendering.frame, original.offset.x, original.offset.y,
                    original.extent.width, original.extent.height, result.offset.x, result.offset.y,
                    result.extent.width, result.extent.height);
                std::fflush(stderr);
                rendering.loggedScissor = true;
            }
        }
    }
    deviceProc<PFN_vkCmdSetScissor>(device, "vkCmdSetScissor")(cmd, first, count, clipped.empty() ? rects : clipped.data());
}

PFN_vkVoidFunction deviceIntercept(const char* name, PFN_vkVoidFunction next) {
    if (!next) return nullptr;
#define HOOK(command, implementation) \
    if (std::strcmp(name, #command) == 0) return reinterpret_cast<PFN_vkVoidFunction>(implementation)
    HOOK(vkDestroyDevice, destroyDevice);
    HOOK(vkCreateSwapchainKHR, createSwapchain);
    HOOK(vkDestroySwapchainKHR, destroySwapchain);
    HOOK(vkGetSwapchainImagesKHR, getSwapchainImages);
    HOOK(vkCreateImageView, createImageView);
    HOOK(vkDestroyImageView, destroyImageView);
    HOOK(vkCmdBeginRendering, beginRendering);
    HOOK(vkCmdEndRendering, endRendering);
    HOOK(vkCmdBeginRenderingKHR, beginRenderingKhr);
    HOOK(vkCmdEndRenderingKHR, endRenderingKhr);
    HOOK(vkCmdSetScissor, setScissor);
#undef HOOK
    return next;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice value, const char* name) {
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gdpa);
    return nextGdpa ? deviceIntercept(name, nextGdpa(value, name)) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gpdpa(VkInstance value, const char* name) {
    return nextGpdpa ? nextGpdpa(value, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance value, const char* name) {
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gipa);
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gdpa);
    if (std::strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(createInstance);
    if (std::strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(destroyInstance);
    if (std::strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(createDevice);
    if (std::strcmp(name, "vk_layerGetPhysicalDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(gpdpa);
    return nextGipa ? deviceIntercept(name, nextGipa(value, name)) : nullptr;
}

}  // namespace

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info) {
    if (info->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || info->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    info->loaderLayerInterfaceVersion = 2;
    info->pfnGetInstanceProcAddr = gipa;
    info->pfnGetDeviceProcAddr = gdpa;
    info->pfnGetPhysicalDeviceProcAddr = gpdpa;
    return VK_SUCCESS;
}
