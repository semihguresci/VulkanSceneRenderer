#pragma once

#include <string>
#include <vector>

namespace container::renderer {

struct RendererDeviceCapabilities {
  bool descriptorIndexing{false};
  bool bufferDeviceAddress{false};
  bool accelerationStructure{false};
  bool rayTracingPipeline{false};
  bool rayQuery{false};
  bool storageImageAtomics{false};
  bool dynamicRendering{false};

  [[nodiscard]] static RendererDeviceCapabilities rasterOnly() {
    RendererDeviceCapabilities capabilities{};
    capabilities.descriptorIndexing = true;
    return capabilities;
  }

  [[nodiscard]] bool supportsRayTracing() const {
    return descriptorIndexing && bufferDeviceAddress &&
           accelerationStructure && rayTracingPipeline;
  }

  [[nodiscard]] bool supportsRayQueries() const {
    return descriptorIndexing && bufferDeviceAddress &&
           accelerationStructure && rayQuery;
  }

  [[nodiscard]] bool supportsPathTracing() const {
    return supportsRayQueries();
  }

  [[nodiscard]] std::vector<std::string> missingRayTracingRequirements()
      const {
    std::vector<std::string> missing;
    if (!descriptorIndexing) {
      missing.push_back("descriptor indexing");
    }
    if (!bufferDeviceAddress) {
      missing.push_back("buffer device address");
    }
    if (!accelerationStructure) {
      missing.push_back("acceleration structure");
    }
    if (!rayTracingPipeline) {
      missing.push_back("ray tracing pipeline");
    }
    return missing;
  }

  [[nodiscard]] std::vector<std::string> missingPathTracingRequirements()
      const {
    std::vector<std::string> missing;
    if(!descriptorIndexing) missing.push_back("descriptor indexing");
    if(!bufferDeviceAddress) missing.push_back("buffer device address");
    if(!accelerationStructure) missing.push_back("acceleration structure");
    if(!rayQuery) missing.push_back("ray queries");
    return missing;
  }
};

}  // namespace container::renderer
