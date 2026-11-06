#include "Container/renderer/bim/BimObjectTransforms.h"

#include "Container/scene/SceneProvider.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace container::renderer {
namespace {

bool finite(glm::vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

template <typename Matrix>
bool finiteMatrix(const Matrix &value) {
  for (typename Matrix::length_type column = 0; column < value.length(); ++column)
    for (typename Matrix::col_type::length_type row = 0; row < value[column].length(); ++row)
      if (!std::isfinite(value[column][row])) return false;
  return true;
}

bool negativeHandedness(const glm::mat4 &model) {
  if (!finiteMatrix(model) || model[0][3] != 0.0f || model[1][3] != 0.0f ||
      model[2][3] != 0.0f || model[3][3] != 1.0f)
    throw std::invalid_argument("BIM object transforms must be finite affine matrices");
  // Normalize columns before testing conditioning so small, valid unit scales
  // do not look singular merely because their determinant is small.
  glm::dmat3 basis{model};
  for (int column = 0; column < 3; ++column) {
    const double length = glm::length(basis[column]);
    if (!std::isfinite(length) || length <= 0.0)
      throw std::invalid_argument("BIM object transforms must be nonsingular");
    basis[column] /= length;
  }
  const double determinant = glm::determinant(basis);
  if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-8)
    throw std::invalid_argument("BIM object transforms must be nonsingular");
  return determinant < 0.0;
}

BimElementBounds boundsFromRange(glm::dvec3 minimum, glm::dvec3 maximum) {
  BimElementBounds result;
  result.valid = true;
  result.min = glm::vec3{minimum};
  result.max = glm::vec3{maximum};
  result.center = glm::vec3{(minimum + maximum) * 0.5};
  result.size = glm::vec3{maximum - minimum};
  result.radius = static_cast<float>(glm::length((maximum - minimum) * 0.5));
  result.floorElevation = result.min.y;
  if (!finite(result.min) || !finite(result.max) || !finite(result.center) ||
      !finite(result.size) || !std::isfinite(result.radius))
    throw std::invalid_argument("BIM object bounds exceed the supported range");
  return result;
}

BimElementBounds transformedBounds(const BimElementBounds &bounds,
                                    const glm::dmat4 &delta) {
  glm::dvec3 minimum{std::numeric_limits<double>::max()};
  glm::dvec3 maximum{std::numeric_limits<double>::lowest()};
  for (unsigned corner = 0; corner < 8u; ++corner) {
    const glm::dvec3 point{
        (corner & 1u) ? bounds.max.x : bounds.min.x,
        (corner & 2u) ? bounds.max.y : bounds.min.y,
        (corner & 4u) ? bounds.max.z : bounds.min.z};
    const glm::dvec3 transformed{delta * glm::dvec4{point, 1.0}};
    minimum = glm::min(minimum, transformed);
    maximum = glm::max(maximum, transformed);
  }
  return boundsFromRange(minimum, maximum);
}

BimElementBounds translatedBounds(BimElementBounds bounds, glm::vec3 delta) {
  if (!bounds.valid || delta == glm::vec3{0}) return bounds;
  return boundsFromRange(glm::dvec3{bounds.min} + glm::dvec3{delta},
                         glm::dvec3{bounds.max} + glm::dvec3{delta});
}

bool validBounds(const BimElementBounds &bounds) {
  return bounds.valid && finite(bounds.min) && finite(bounds.max) &&
      glm::all(glm::greaterThanEqual(bounds.max, bounds.min));
}

} // namespace

std::vector<BimObjectTransforms::Pending> BimObjectTransforms::prepare(
    std::span<const gpu::ObjectData> objects, BimMetadataTableView metadata,
    std::span<const gpu::ObjectTransformUpdate> updates, glm::vec3 rootTranslation) const {
  if (!finite(rootTranslation))
    throw std::invalid_argument("BIM root translation must be finite");
  std::vector<Pending> pending;
  pending.reserve(updates.size());
  std::unordered_set<uint32_t> seen;
  seen.reserve(updates.size());
  for (const auto &update : updates) {
    if (update.objectIndex >= objects.size() || update.objectIndex >= metadata.size())
      throw std::invalid_argument("BIM object transform index is outside imported metadata");
    if (!seen.insert(update.objectIndex).second)
      throw std::invalid_argument("BIM object transform batch contains a duplicate index");
    const auto &current = objects[update.objectIndex];
    if (negativeHandedness(update.model) != negativeHandedness(current.model))
      throw std::invalid_argument("BIM object transforms cannot change triangle winding");
    if (update.model == current.model) continue;

    Original original;
    if (const auto previous = originals_.find(update.objectIndex); previous != originals_.end()) {
      original = previous->second;
    } else {
      original.model = glm::translate(glm::mat4{1}, -rootTranslation) * current.model;
      original.bounds = metadata[update.objectIndex].bounds;
      if (!validBounds(original.bounds)) {
        const auto sphere = current.boundingSphere;
        if (!finite(glm::vec3{sphere}) || !std::isfinite(sphere.w) || sphere.w < 0.0f)
          throw std::invalid_argument("BIM object has no finite collider bounds");
        const glm::dvec3 center = glm::dvec3{sphere} - glm::dvec3{rootTranslation};
        original.bounds = boundsFromRange(center - double(sphere.w), center + double(sphere.w));
      }
    }
    const glm::dmat4 delta = glm::dmat4{update.model} * glm::inverse(glm::dmat4{original.model});
    if (!finiteMatrix(delta))
      throw std::invalid_argument("BIM object transform delta exceeds the supported range");
    original.currentBounds = transformedBounds(original.bounds, delta);
    gpu::ObjectData replacement = current;
    replacement.model = update.model;
    const glm::mat3 normal{glm::transpose(glm::inverse(glm::dmat3{update.model}))};
    if (!finiteMatrix(normal))
      throw std::invalid_argument("BIM normal transform exceeds the supported range");
    replacement.normalMatrix0 = glm::vec4{normal[0], 0};
    replacement.normalMatrix1 = glm::vec4{normal[1], 0};
    replacement.normalMatrix2 = glm::vec4{normal[2], 0};
    replacement.boundingSphere = glm::vec4{original.currentBounds.center, original.currentBounds.radius};
    pending.push_back({update.objectIndex, original, replacement});
  }
  return pending;
}

void BimObjectTransforms::validate(
    std::span<const gpu::ObjectData> objects, BimMetadataTableView metadata,
    std::span<const gpu::ObjectTransformUpdate> updates, glm::vec3 rootTranslation) const {
  (void)prepare(objects, metadata, updates, rootTranslation);
}

bool BimObjectTransforms::apply(
    std::span<gpu::ObjectData> objects, BimMetadataTableView metadata,
    std::span<const gpu::ObjectTransformUpdate> updates, glm::vec3 rootTranslation) {
  const auto pending = prepare(objects, metadata, updates, rootTranslation);
  if (pending.empty()) return false;

  // Finish allocations before publishing either models or existing overlays.
  originals_.reserve(originals_.size() + pending.size());
  std::vector<uint32_t> inserted;
  inserted.reserve(pending.size());
  try {
    for (const auto &entry : pending)
      if (originals_.try_emplace(entry.objectIndex, entry.original).second)
        inserted.push_back(entry.objectIndex);
  } catch (...) {
    for (const auto index : inserted) originals_.erase(index);
    throw;
  }
  for (const auto &entry : pending) {
    originals_.find(entry.objectIndex)->second = entry.original;
    objects[entry.objectIndex] = entry.object;
  }
  return true;
}

BimElementBounds BimObjectTransforms::boundsForObject(
    uint32_t objectIndex, BimElementBounds importedBounds, glm::vec3 rootTranslation) const {
  if (const auto entry = originals_.find(objectIndex); entry != originals_.end())
    return entry->second.currentBounds;
  return translatedBounds(importedBounds, rootTranslation);
}

void BimObjectTransforms::translate(glm::vec3 delta) {
  if (!finite(delta)) throw std::invalid_argument("BIM root translation must be finite");
  // Check all translated bounds before mutating the first overlay.
  for (const auto &[index, original] : originals_) {
    (void)index;
    (void)translatedBounds(original.currentBounds, delta);
  }
  for (auto &[index, original] : originals_) {
    (void)index;
    original.currentBounds = translatedBounds(original.currentBounds, delta);
  }
}

void BimObjectTransforms::extendSceneBounds(scene::SceneProviderBounds &bounds) const {
  for (const auto &[index, original] : originals_) {
    (void)index;
    const auto &moved = original.currentBounds;
    if (!bounds.valid) {
      bounds = {.min = moved.min, .max = moved.max, .valid = true};
    } else {
      bounds.min = glm::min(bounds.min, moved.min);
      bounds.max = glm::max(bounds.max, moved.max);
    }
  }
}

} // namespace container::renderer
