#include "Container/renderer/platform/VulkanContext.h"
#include "Container/utility/DebugMessengerExt.h"

namespace container::renderer {

VulkanContext::VulkanContext(VulkanContextResult result,
                             bool enableValidationLayers)
    : result_(std::move(result))
    , enableValidationLayers_(enableValidationLayers) {}

VulkanContext::~VulkanContext() {
  result_.deviceWrapper.reset();
  if (enableValidationLayers_ &&
      result_.debugMessenger != VK_NULL_HANDLE) {
    result_.ownedDebugMessenger.clear();
    result_.debugMessenger = VK_NULL_HANDLE;
  }

  if (result_.surface != VK_NULL_HANDLE) {
    result_.ownedSurface.clear();
    result_.surface = VK_NULL_HANDLE;
  }

  result_.instanceWrapper.reset();
}

}  // namespace container::renderer
