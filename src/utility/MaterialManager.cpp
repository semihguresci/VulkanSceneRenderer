#include <Container/utility/MaterialManager.h>
#include <Container/utility/Material.h>

namespace container::material {

uint32_t MaterialManager::createMaterial(const Material& material) {
  materials_.push_back(material);
  return static_cast<uint32_t>(materials_.size() - 1);
}

bool MaterialManager::updateMaterial(uint32_t index, const Material& material) noexcept {
  if (index >= materials_.size()) return false;
  materials_[index] = material;
  return true;
}

const Material* MaterialManager::getMaterial(uint32_t index) const {
  if (index >= materials_.size()) return nullptr;
  return &materials_[index];
}

}  // namespace container::material
