#include "Container/geometry/OpenVdbMeshExport.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string_view>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <tbb/task_arena.h>
#endif

namespace container::geometry::vdb {
namespace {

std::string safeGroupName(size_t ordinal, std::string_view source) {
  std::string result = "grid_" + std::to_string(ordinal) + "_";
  if (source.empty()) return result + "surface";
  for (unsigned char character : source.substr(0, 64)) {
    const bool safe = (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '_' || character == '-' || character == '.';
    result += safe ? char(character) : '_';
  }
  return result;
}

std::string commentText(std::string_view source) {
  std::string result;
  result.reserve(source.size());
  for (unsigned char character : source)
    result += character < 32 || character == 127 || character == '\\' ? ' ' : char(character);
  return result;
}

bool finite(const glm::vec3& vector) {
  return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

void validateSurface(const dotbim::Model& model) {
  if (model.vertices.empty() || model.indices.empty() || model.indices.size() % 3 != 0 ||
      model.meshRanges.empty() || model.meshRanges.size() != model.elements.size())
    throw std::runtime_error("OpenVDB loader returned an invalid surface model");
  for (const auto& vertex : model.vertices) {
    const double normalLength = std::hypot(double(vertex.normal.x), double(vertex.normal.y), double(vertex.normal.z));
    if (!finite(vertex.position) || !finite(vertex.normal) || !std::isfinite(normalLength) || normalLength == 0.0)
      throw std::runtime_error("OpenVDB surface contains a non-finite position or invalid normal");
  }
  size_t nextIndex = 0;
  for (size_t grid = 0; grid < model.meshRanges.size(); ++grid) {
    const auto& range = model.meshRanges[grid];
    if (range.meshId != model.elements[grid].meshId || range.firstIndex != nextIndex ||
        range.indexCount == 0 || range.indexCount % 3 != 0 ||
        range.indexCount > model.indices.size() - nextIndex)
      throw std::runtime_error("OpenVDB surface has an invalid grid index range");
    nextIndex += range.indexCount;
  }
  if (nextIndex != model.indices.size())
    throw std::runtime_error("OpenVDB surface has unassigned triangle indices");
  for (uint32_t index : model.indices)
    if (index >= model.vertices.size())
      throw std::runtime_error("OpenVDB surface has an invalid vertex index");
}
} // namespace

MeshExportResult WriteObjSurface(const std::filesystem::path& input,
                                 const std::filesystem::path& output,
                                 const ImportOptions& options) {
  if (!IsAvailable())
    throw std::runtime_error("OpenVDB mesh export is unavailable in this build. Configure with -DENABLE_OPENVDB=ON.");
  if (options.vectorPreview || options.slicePreview || options.nativePointPreview)
    throw std::invalid_argument("OBJ scalar isosurface export does not accept explicit field or point preview modes");
  if (output.empty()) throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(output))
    throw std::invalid_argument("output already exists; choose a new OBJ path");
  if (!output.parent_path().empty() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("output directory does not exist");
  auto scalarOptions = options;
  scalarOptions.includePoints = false;
  auto model = [&] {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
    tbb::task_arena arena(1);
    return arena.execute([&] { return LoadFromFile(input, 1.0f, scalarOptions); });
#else
    return LoadFromFile(input, 1.0f, scalarOptions);
#endif
  }();
  MeshExportResult result;
  for (const auto& diagnostic : model.importReport.diagnostics) {
    if (diagnostic.representationType != "OpenVDB.EmptySurface")
      throw std::runtime_error("Cannot export an incomplete OpenVDB surface: " + diagnostic.reason);
    ++result.skippedEmptyGrids;
    result.diagnostics.push_back(diagnostic.reason);
  }
  if (model.importReport.partialProductCount != 0 ||
      model.importReport.skippedProductCount != result.skippedEmptyGrids ||
      model.importReport.completeness == ImportCompleteness::Failed)
    throw std::runtime_error("Cannot export an incomplete OpenVDB surface: " + model.importReport.summary());
  validateSurface(model);
  result.gridCount = model.meshRanges.size();
  result.vertexCount = model.vertices.size();
  result.triangleCount = model.indices.size() / 3;
  std::ofstream stream;
  bool created = false;
  try {
    // Exclusive creation protects existing paths and aliases even if a path
    // appears between the initial validation and this open operation.
    stream.open(output, std::ios::out | std::ios::binary | std::ios::noreplace);
    created = stream.is_open();
    if (!stream) throw std::runtime_error("Could not exclusively create OBJ output");
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<float>::max_digits10);
    stream << "# OpenVDB world-space scalar surface\n# grids " << result.gridCount
           << "\n# vertices " << result.vertexCount << "\n# triangles " << result.triangleCount << '\n';
    for (const auto& diagnostic : result.diagnostics)
      stream << "# warning: " << commentText(diagnostic) << '\n';
    for (const auto& vertex : model.vertices)
      stream << "v " << vertex.position.x << ' ' << vertex.position.y << ' ' << vertex.position.z << '\n';
    for (const auto& vertex : model.vertices)
      stream << "vn " << vertex.normal.x << ' ' << vertex.normal.y << ' ' << vertex.normal.z << '\n';
    stream << "s off\n";
    for (size_t grid = 0; grid < model.meshRanges.size(); ++grid) {
      const auto& range = model.meshRanges[grid];
      stream << "g " << safeGroupName(grid + 1, model.elements[grid].sourceId) << '\n';
      const size_t end = size_t(range.firstIndex) + range.indexCount;
      for (size_t face = range.firstIndex; face < end; face += 3) {
        stream << 'f';
        for (size_t corner = 0; corner < 3; ++corner) {
          const uint64_t index = uint64_t(model.indices[face + corner]) + 1;
          stream << ' ' << index << "//" << index;
        }
        stream << '\n';
      }
    }
    stream.flush();
    stream.close();
  } catch (...) {
    stream.exceptions(std::ios::goodbit);
    if (stream.is_open()) stream.close();
    if (created) {
      std::error_code ignored;
      std::filesystem::remove(output, ignored);
    }
    throw;
  }
  return result;
}
} // namespace container::geometry::vdb
