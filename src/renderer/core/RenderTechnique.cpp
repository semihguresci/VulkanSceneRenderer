#include "Container/renderer/core/RenderTechnique.h"

#include "Container/renderer/deferred/DeferredRasterTechnique.h"
#include "Container/renderer/forward/ForwardRasterTechnique.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace container::renderer {

namespace {

constexpr std::array kTechniqueNames{
    std::string_view{"deferred-raster"},
    std::string_view{"forward-raster"},
    std::string_view{"ray-tracing"},
    std::string_view{"path-tracing"},
    std::string_view{"gaussian-splatting"},
    std::string_view{"radiance-field"},
};

constexpr std::array kTechniqueDisplayNames{
    std::string_view{"Deferred raster"},
    std::string_view{"Forward rendering"},
    std::string_view{"Ray tracing"},
    std::string_view{"Path tracing"},
    std::string_view{"Gaussian splatting"},
    std::string_view{"Radiance field"},
};

constexpr std::array<RenderTechniqueDescriptor, kTechniqueNames.size()>
    kTechniqueDescriptors{{
        {.id = RenderTechniqueId::DeferredRaster,
         .name = "deferred-raster",
         .displayName = "Deferred raster",
         .implemented = true},
        {.id = RenderTechniqueId::ForwardRaster,
         .name = "forward-raster",
         .displayName = "Forward rendering",
         .implemented = true},
        {.id = RenderTechniqueId::RayTracing,
         .name = "ray-tracing",
         .displayName = "Ray tracing",
         .requiresRayTracing = true},
        {.id = RenderTechniqueId::PathTracing,
         .name = "path-tracing",
         .displayName = "Path tracing",
         .requiresRayTracing = true,
         .requiresPathTracing = true},
        {.id = RenderTechniqueId::GaussianSplatting,
         .name = "gaussian-splatting",
         .displayName = "Gaussian splatting"},
        {.id = RenderTechniqueId::RadianceField,
         .name = "radiance-field",
         .displayName = "Radiance field"},
    }};

[[nodiscard]] constexpr size_t techniqueIndex(RenderTechniqueId id) {
  return static_cast<size_t>(id);
}

}  // namespace

std::string_view renderTechniqueName(RenderTechniqueId id) {
  const size_t index = techniqueIndex(id);
  if (index >= kTechniqueNames.size()) {
    return "unknown";
  }
  return kTechniqueNames[index];
}

std::string_view renderTechniqueDisplayName(RenderTechniqueId id) {
  const size_t index = techniqueIndex(id);
  if (index >= kTechniqueDisplayNames.size()) {
    return "Unknown";
  }
  return kTechniqueDisplayNames[index];
}

std::span<const RenderTechniqueDescriptor> knownRenderTechniqueDescriptors() {
  return kTechniqueDescriptors;
}

std::optional<RenderTechniqueId> renderTechniqueIdFromName(
    std::string_view name) {
  if (name == "forward-rendering" || name == "real-time-rendering" ||
      name == "realtime-rendering" ||
      name == "real-time") {
    return RenderTechniqueId::ForwardRaster;
  }

  const auto it = std::ranges::find(kTechniqueNames, name);
  if (it == kTechniqueNames.end()) {
    return std::nullopt;
  }
  return static_cast<RenderTechniqueId>(
      static_cast<size_t>(std::distance(kTechniqueNames.begin(), it)));
}

void RenderTechniqueRegistry::registerTechnique(
    std::unique_ptr<RenderTechnique> technique) {
  if (!technique) {
    throw std::invalid_argument(
        "RenderTechniqueRegistry cannot register a null technique");
  }
  if (find(technique->id()) != nullptr) {
    throw std::invalid_argument(
        "RenderTechniqueRegistry duplicate technique registration");
  }
  techniques_.push_back(std::move(technique));
}

RenderTechnique* RenderTechniqueRegistry::find(RenderTechniqueId id) {
  const auto it = std::ranges::find_if(
      techniques_, [id](const std::unique_ptr<RenderTechnique>& technique) {
        return technique != nullptr && technique->id() == id;
      });
  return it == techniques_.end() ? nullptr : it->get();
}

const RenderTechnique* RenderTechniqueRegistry::find(
    RenderTechniqueId id) const {
  const auto it = std::ranges::find_if(
      techniques_, [id](const std::unique_ptr<RenderTechnique>& technique) {
        return technique != nullptr && technique->id() == id;
      });
  return it == techniques_.end() ? nullptr : it->get();
}

RenderTechniqueSelection RenderTechniqueRegistry::select(
    RenderTechniqueId requested,
    const RenderSystemContext& context) {
  RenderTechniqueSelection selection{};
  selection.requested = requested;
  selection.selected = requested;

  RenderTechnique* requestedTechnique = find(requested);
  if (requestedTechnique != nullptr) {
    const RenderTechniqueAvailability availability =
        requestedTechnique->availability(context);
    if (availability.available) {
      selection.technique = requestedTechnique;
      return selection;
    }
    selection.unavailableReason = availability.reason;
  } else {
    selection.unavailableReason = "requested technique is not registered";
  }

  RenderTechnique* fallback = find(RenderTechniqueId::DeferredRaster);
  if (fallback == nullptr) {
    return selection;
  }

  const RenderTechniqueAvailability fallbackAvailability =
      fallback->availability(context);
  if (!fallbackAvailability.available) {
    if (selection.unavailableReason.empty()) {
      selection.unavailableReason = fallbackAvailability.reason;
    }
    return selection;
  }

  selection.technique = fallback;
  selection.selected = RenderTechniqueId::DeferredRaster;
  selection.usedFallback = selection.selected != selection.requested;
  return selection;
}

RenderTechniqueRegistry createDefaultRenderTechniqueRegistry() {
  RenderTechniqueRegistry registry;
  registry.registerTechnique(std::make_unique<DeferredRasterTechnique>());
  registry.registerTechnique(std::make_unique<ForwardRasterTechnique>());
  return registry;
}

}  // namespace container::renderer
