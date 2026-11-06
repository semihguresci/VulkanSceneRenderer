#pragma once

#include <cstdint>
#include <vector>

#include "Container/utility/Material.h"

namespace container::scene { class SceneManager; }

namespace container::material {

class MaterialManager {
 public:
  uint32_t createMaterial(const Material& material);
  bool updateMaterial(uint32_t index, const Material& material) noexcept;
  [[nodiscard]] const Material* getMaterial(uint32_t index) const;
  [[nodiscard]] size_t materialCount() const { return materials_.size(); }

 private:
  friend class container::scene::SceneManager;
  std::vector<Material> materials_{};
};

}  // namespace container::material
