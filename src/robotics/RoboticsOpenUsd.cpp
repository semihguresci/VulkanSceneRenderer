#include "Container/robotics/RoboticsOpenUsd.h"

#include "Container/geometry/DotBimLoader.h"
#include "Container/utility/AtomicFile.h"
#include "Container/utility/ContentFingerprint.h"
#include "RoboticsOpenUsdData.h"

#include <glm/matrix.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace container::robotics {
namespace {

class StagedUsdLayer {
public:
  explicit StagedUsdLayer(const std::filesystem::path& destination) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      auto candidate = destination.parent_path() /
          (".usd-stage-" + std::to_string(stamp) + '-' + std::to_string(attempt));
      auto layer = candidate / "layer.usda";
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        directory_ = std::move(candidate);
        path_ = std::move(layer);
        return;
      }
      if (error && error != std::errc::file_exists)
        throw std::filesystem::filesystem_error("Creating USD export staging directory", candidate, error);
    }
    throw std::runtime_error("Could not create a unique USD export staging directory");
  }

  ~StagedUsdLayer() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    std::filesystem::remove(directory_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path directory_;
  std::filesystem::path path_;
};

void flushUsdLayer(const std::filesystem::path& path) {
#ifdef _WIN32
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) util::atomicFileError("opening staged USD layer", GetLastError());
  const bool flushed = FlushFileBuffers(file) != 0;
  const auto flushError = flushed ? ERROR_SUCCESS : GetLastError();
  const bool closed = CloseHandle(file) != 0;
  const auto closeError = closed ? ERROR_SUCCESS : GetLastError();
#else
  const int file = ::open(path.c_str(), O_WRONLY);
  if (file < 0) util::atomicFileError("opening staged USD layer", errno);
  int result;
  do { result = ::fsync(file); } while (result != 0 && errno == EINTR);
  const bool flushed = result == 0;
  const auto flushError = flushed ? 0 : errno;
  const bool closed = ::close(file) == 0;
  const auto closeError = closed ? 0 : errno;
#endif
  if (!flushed) util::atomicFileError("flushing staged USD layer", flushError);
  if (!closed) util::atomicFileError("closing staged USD layer", closeError);
}

void publishUsdLayer(const std::filesystem::path& output, const detail::OpenUsdData& stage) {
  const auto writerMutex = util::replacementMutex(output);
  const std::lock_guard writerLock(*writerMutex);
  StagedUsdLayer file(output);
  const auto& path = file.path();
  try {
    std::ofstream layer;
    layer.exceptions(std::ios::badbit | std::ios::failbit);
    layer.open(path, std::ios::binary | std::ios::out);
    detail::writeOpenUsdLayer(layer, stage);
    layer.close();
  } catch (const std::ios_base::failure& error) {
    throw std::runtime_error(std::string("Writing the staged OpenUSD layer failed: ") + error.what());
  }
  flushUsdLayer(path);
  util::replaceFileAtomically(path, output);
}

bool isParentRelationship(SemanticRelationshipKind kind) {
  return kind == SemanticRelationshipKind::Contains || kind == SemanticRelationshipKind::Aggregates ||
         kind == SemanticRelationshipKind::Nests || kind == SemanticRelationshipKind::Child;
}

std::string primName(const std::string& id) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(id.data());
  return "P_" + util::FingerprintContent({bytes, id.size()}).digest;
}

void buildHierarchy(detail::OpenUsdData& stage) {
  const auto& sources = stage.scene.entities;
  const size_t count = sources.size();
  stage.entities.resize(count);
  std::map<std::string_view, size_t> byId;
  for (size_t i = 0; i < count; ++i) {
    byId.emplace(sources[i].id, i);
    stage.entities[i].primName = primName(sources[i].id);
  }

  std::vector<size_t> parents(count, count);
  std::vector<bool> ambiguous(count, false);
  for (const auto& rel : stage.scene.relationships) {
    if (!isParentRelationship(rel.kind)) continue;
    const size_t parent = byId.at(rel.from);
    const size_t child = byId.at(rel.to);
    if (parents[child] == count) parents[child] = parent;
    else if (parents[child] != parent) ambiguous[child] = true;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!ambiguous[i]) continue;
    parents[i] = count;
    stage.diagnostics.push_back({"ambiguous-usd-parent", sources[i].sourceId,
        "Several products name this entity as a child. It remains at the stage root with its authored relationships."});
  }

  // A prim can have just one parent. Break every edge in a cycle while keeping
  // the original relationships available to USD consumers.
  enum class Visit { Unvisited, Active, Done };
  std::vector<Visit> visits(count, Visit::Unvisited);
  std::vector<size_t> chain;
  for (size_t i = 0; i < count; ++i) {
    size_t node = i;
    while (node != count && visits[node] == Visit::Unvisited) {
      visits[node] = Visit::Active;
      chain.push_back(node);
      node = parents[node];
    }
    if (node != count && visits[node] == Visit::Active) {
      const size_t start = node;
      do {
        stage.diagnostics.push_back({"cyclic-usd-parent", sources[node].sourceId,
            "The authored parent is in a cycle. This entity remains at the stage root with its authored relationships."});
        node = std::exchange(parents[node], count);
      } while (node != start);
    }
    for (const size_t visited : chain) visits[visited] = Visit::Done;
    chain.clear();
  }

  for (size_t i = 0; i < count; ++i) {
    if (parents[i] == count) stage.roots.push_back(i);
    else stage.entities[parents[i]].children.push_back(i);
  }
  auto pending = stage.roots;
  for (size_t next = 0; next < pending.size(); ++next) {
    const size_t index = pending[next];
    auto& entity = stage.entities[index];
    const auto* parent = parents[index] == count ? nullptr : &stage.entities[parents[index]];
    const auto parentWorld = parent ? parent->worldFromLocal : glm::dmat4(1.0);
    entity.primPath = (parent ? parent->primPath : "/World") + '/' + entity.primName;
    entity.worldFromLocal = sources[index].roboticsWorldFromLocal.value_or(parentWorld);
    if (sources[index].roboticsWorldFromLocal) {
      entity.parentFromLocal = parent ? glm::inverse(parentWorld) * entity.worldFromLocal : entity.worldFromLocal;
      for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
          if (!std::isfinite(entity.parentFromLocal[column][row]))
            throw std::invalid_argument("USD parent-relative transform cannot be represented for " + sources[index].id);
    }
    pending.insert(pending.end(), entity.children.begin(), entity.children.end());
  }
}

void appendImportDiagnostics(const geometry::ImportReport& report,
                             std::vector<RoboticsDiagnostic>& diagnostics) {
  if (report.completeness == geometry::ImportCompleteness::Partial)
    diagnostics.push_back({"partial-source-import", {}, report.summary()});
  for (const auto& diagnostic : report.diagnostics) {
    auto source = diagnostic.productGuid;
    if (source.empty() && diagnostic.entityId != 0)
      source = '#' + std::to_string(diagnostic.entityId);
    auto message = diagnostic.reason;
    if (!diagnostic.representationType.empty())
      message = diagnostic.representationType + ": " + message;
    diagnostics.push_back({"source-import", std::move(source), std::move(message)});
  }
}

} // namespace

RoboticsOpenUsdResult writeRoboticsOpenUsd(const std::filesystem::path& output,
    const geometry::dotbim::Model& model, const RoboticsSceneOptions& options) {
  if (output.extension() != ".usda")
    throw std::invalid_argument("Robotics OpenUSD output must use the .usda extension");
  if (model.importReport.completeness == geometry::ImportCompleteness::Failed) {
    auto message = model.importReport.summary();
    if (!model.importReport.diagnostics.empty())
      message += ". " + model.importReport.diagnostics.front().reason;
    throw std::invalid_argument(std::move(message));
  }

  detail::OpenUsdData stage;
  stage.scene = buildRoboticsScene(model, options);
  if (!stage.scene.frame.roboticsFromRenderer)
    throw std::invalid_argument("USD metre scale is unknown. Provide --metres-per-renderer-unit or an input with physical units.");
  stage.diagnostics = stage.scene.diagnostics;
  appendImportDiagnostics(model.importReport, stage.diagnostics);
  buildHierarchy(stage);
  detail::appendOpenUsdMeshes(model, stage);

  auto collision = cookBimCollisionGeometry(model, stage.scene);
  stage.colliders = std::move(collision.colliders);
  for (auto& diagnostic : collision.diagnostics) {
    if (!diagnostic.representationId.empty())
      diagnostic.message += " Representation: " + diagnostic.representationId;
    stage.diagnostics.push_back({std::move(diagnostic.code),
        std::move(diagnostic.entityId), std::move(diagnostic.message)});
  }

  RoboticsOpenUsdResult result;
  result.entityCount = stage.entities.size();
  result.meshCount = stage.standaloneMeshes.size();
  for (const auto& entity : stage.entities) result.meshCount += entity.meshes.size();
  result.colliderCount = stage.colliders.size();
  if (result.entityCount == 0 && result.meshCount == 0)
    throw std::invalid_argument("No semantic entities or supported meshes are available for USD export");
  if (result.meshCount == 0)
    stage.diagnostics.push_back({"no-visual-meshes", {}, "This USD stage contains semantics without visual meshes."});
  const auto diagnosticKey = [](const RoboticsDiagnostic& diagnostic) {
    return std::tie(diagnostic.code, diagnostic.sourceId, diagnostic.message);
  };
  std::ranges::sort(stage.diagnostics, {}, diagnosticKey);
  const auto duplicates = std::ranges::unique(stage.diagnostics, {}, diagnosticKey);
  stage.diagnostics.erase(duplicates.begin(), duplicates.end());

  publishUsdLayer(output, stage);
  result.diagnostics = std::move(stage.diagnostics);
  return result;
}

} // namespace container::robotics
