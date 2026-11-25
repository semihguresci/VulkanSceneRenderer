#pragma once

#include "Container/renderer/bim/BimManager.h"

namespace container::renderer {

// Restores geometry generated from the freshly parsed model. Existing CPU and
// GPU publications are independent of the cache, including on cancellation.
[[nodiscard]] bool restoreBimDerivedCache(
    BimManager::PreparedScene &prepared, const std::filesystem::path &directory,
    const std::filesystem::path &sourcePath, bool generateGeometryLods,
    std::vector<BimElementBounds> &bounds, std::stop_token stop);

void saveBimDerivedCache(BimManager::PreparedScene &prepared,
                         std::span<const BimElementBounds> bounds, std::stop_token stop);

} // namespace container::renderer
