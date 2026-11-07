#include "Container/physics/PhysicsElementBindings.h"

#include "Container/utility/SceneGraph.h"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace container::physics {
namespace {

using scene::SceneGraph;

bool finite(glm::vec3 value) {
  return std::isfinite(value.x) && std::isfinite(value.y) &&
         std::isfinite(value.z);
}

bool finiteAffine(const glm::mat4 &matrix) {
  for (int column = 0; column < 4; ++column)
    for (int row = 0; row < 4; ++row)
      if (!std::isfinite(matrix[column][row]))
        return false;
  return std::abs(matrix[0][3]) <= 1e-5f &&
         std::abs(matrix[1][3]) <= 1e-5f &&
         std::abs(matrix[2][3]) <= 1e-5f &&
         std::abs(matrix[3][3] - 1.0f) <= 1e-5f;
}

bool publishableFragmentModel(const glm::mat4 &matrix) {
  if (!finiteAffine(matrix) || matrix[0][3] != 0.0f ||
      matrix[1][3] != 0.0f || matrix[2][3] != 0.0f || matrix[3][3] != 1.0f)
    return false;
  // Runtime BIM publication requires an invertible affine model and finite
  // float normals. Normalize before testing the determinant, matching that
  // contract without rejecting small but valid imported unit scales.
  glm::dmat3 basis{matrix};
  for (int column = 0; column < 3; ++column) {
    const double length = glm::length(basis[column]);
    if (!std::isfinite(length) || length <= 0.0)
      return false;
    basis[column] /= length;
  }
  const double determinant = glm::determinant(basis);
  if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-8)
    return false;
  const glm::mat3 normal{glm::transpose(glm::inverse(glm::dmat3{matrix}))};
  for (int column = 0; column < 3; ++column)
    if (!finite(normal[column]))
      return false;
  return true;
}

void validateFragment(const PhysicsElementFragment &fragment) {
  if (fragment.objectIndex == UINT32_MAX || !publishableFragmentModel(fragment.model) ||
      !finite(fragment.worldMin) || !finite(fragment.worldMax) ||
      fragment.worldMin.x > fragment.worldMax.x ||
      fragment.worldMin.y > fragment.worldMax.y ||
      fragment.worldMin.z > fragment.worldMax.z)
    throw std::invalid_argument("Physics product has invalid fragment bounds or transforms");
}

glm::mat4 translated(glm::vec3 position) {
  return glm::translate(glm::mat4{1.0f}, position);
}

} // namespace

struct PhysicsElementBindings::Impl {
  struct Product {
    uint32_t node{SceneGraph::kInvalidNode};
    PhysicsBodyId body{kInvalidPhysicsBodyId};
    glm::mat4 anchorInverse{1.0f};
    glm::mat4 authoredProxyLocal{1.0f};
    glm::vec3 localMin{0.0f};
    glm::vec3 localMax{0.0f};
    std::vector<PhysicsElementFragment> fragments;
    std::vector<uint32_t> objects;
  };

  std::unordered_map<uint32_t, Product> products;
  std::unordered_map<uint32_t, uint32_t> objectProducts;
  std::vector<uint32_t> freeNodes;
  uint64_t graphIdentity{0};
  uint64_t generation{0};
  uint32_t root{SceneGraph::kInvalidNode};

  bool matches(const SceneGraph &graph) const noexcept {
    return graphIdentity == graph.identity();
  }

  void retireObsoleteGraph(const SceneGraph &graph, PhysicsScene &physics) const {
    // Reset and scale edits recreate body handles. Graph identity, rather
    // than the attachment-time handle, determines whether the remaining
    // world belongs to the replaced graph. A world already bound to the new
    // graph can contain unrelated primary bodies at these same node indices.
    if (!matches(graph) && graphIdentity != 0u &&
        physics.graphIdentity() == graphIdentity)
      physics.clear();
  }

  glm::mat4 rootMatrix(const SceneGraph &graph) const {
    const auto *entry = graph.getNode(root);
    if (entry == nullptr || entry->retired || entry->parent != SceneGraph::kInvalidNode ||
        !finiteAffine(entry->localTransform))
      throw std::invalid_argument("Physics auxiliary root was removed or reparented");
    return entry->localTransform;
  }

  glm::mat4 proxyMatrix(const SceneGraph &graph, const Product &product) const {
    const auto *entry = graph.getNode(product.node);
    if (entry == nullptr || entry->retired || entry->parent != root)
      throw std::invalid_argument("Physics product proxy was removed or reparented");
    const auto matrix = rootMatrix(graph) * entry->localTransform;
    if (!finiteAffine(matrix))
      throw std::invalid_argument("Physics product proxy has an invalid transform");
    return matrix;
  }

  void appendTransforms(const Product &product, const glm::mat4 &proxy,
                        std::vector<gpu::ObjectTransformUpdate> &updates) const {
    const auto delta = proxy * product.anchorInverse;
    for (const auto &fragment : product.fragments) {
      const auto model = delta * fragment.model;
      if (!finiteAffine(model))
        throw std::invalid_argument("Physics product transform exceeds the scene range");
      updates.push_back({.objectIndex = fragment.objectIndex, .model = model});
    }
  }

  void ensureRoot(SceneGraph &graph) {
    if (root != SceneGraph::kInvalidNode) {
      (void)rootMatrix(graph);
      return;
    }
    root = graph.createNode(glm::mat4{1.0f}, 0u, false,
                            SceneGraph::kInvalidNode, "Auxiliary physics bindings");
    graph.setVisible(root, false);
  }

  uint32_t takeNode(SceneGraph &graph, const glm::mat4 &local,
                    std::string_view label) {
    ensureRoot(graph);
    uint32_t node;
    if (!freeNodes.empty()) {
      node = freeNodes.back();
      freeNodes.pop_back();
    } else {
      node = graph.createNode(glm::mat4{1.0f}, 0u);
    }
    try {
      graph.setLocalTransform(node, local);
      graph.setName(node, label.empty() ? "Auxiliary physics product"
                                        : std::string{label});
      if (graph.getNode(node)->parent != root && !graph.setParent(node, root))
        throw std::invalid_argument("Cannot parent a physics product proxy");
      graph.setRenderable(node, false);
      graph.setVisible(node, false);
    } catch (...) {
      freeNodes.push_back(node);
      throw;
    }
    return node;
  }

  void retire(SceneGraph &graph, PhysicsScene &physics, const Product &product) {
    // A fresh graph can already contain a primary body at the same node index.
    // Process-unique body IDs prevent accidentally detaching that replacement.
    // In the original graph the proxy remains ours even when reset or a scale
    // edit has recreated its body and changed the process-unique handle.
    if ((matches(graph) && physics.hasBody(product.node)) ||
        physics.bodyId(product.node) == product.body)
      (void)physics.detach(graph, product.node, false);
  }
};

PhysicsElementBindings::PhysicsElementBindings()
    : impl_(std::make_unique<Impl>()) {}
PhysicsElementBindings::~PhysicsElementBindings() = default;
PhysicsElementBindings::PhysicsElementBindings(PhysicsElementBindings &&) noexcept = default;
PhysicsElementBindings &PhysicsElementBindings::operator=(PhysicsElementBindings &&) noexcept = default;

void PhysicsElementBindings::synchronizePublication(
    SceneGraph &graph, PhysicsScene &physics, uint64_t modelGeneration) {
  if (impl_->matches(graph) && impl_->generation == modelGeneration)
    return;
  const bool reuse = impl_->matches(graph);
  if (reuse)
    impl_->freeNodes.reserve(impl_->freeNodes.size() + impl_->products.size());
  impl_->retireObsoleteGraph(graph, physics);
  for (const auto &[productId, product] : impl_->products) {
    (void)productId;
    impl_->retire(graph, physics, product);
    if (reuse)
      impl_->freeNodes.push_back(product.node);
  }
  impl_->products.clear();
  impl_->objectProducts.clear();
  if (!reuse) {
    impl_->freeNodes.clear();
    impl_->root = SceneGraph::kInvalidNode;
  } else if (impl_->root != SceneGraph::kInvalidNode) {
    graph.setLocalTransform(impl_->root, glm::mat4{1.0f});
  }
  impl_->graphIdentity = graph.identity();
  impl_->generation = modelGeneration;
}

PhysicsBodyId PhysicsElementBindings::attachOrReplace(
    SceneGraph &graph, PhysicsScene &physics, uint64_t modelGeneration,
    uint32_t productIdentityId,
    std::span<const PhysicsElementFragment> fragments,
    PhysicsBodySettings settings, std::string_view label) {
  synchronizePublication(graph, physics, modelGeneration);
  if (productIdentityId == 0u || fragments.empty())
    throw std::invalid_argument("Physics product requires an identity and fragments");
  std::vector<PhysicsElementFragment> sorted{fragments.begin(), fragments.end()};
  for (const auto &fragment : sorted)
    validateFragment(fragment);
  std::sort(sorted.begin(), sorted.end(), [](const auto &first, const auto &second) {
    return first.objectIndex < second.objectIndex;
  });
  for (size_t index = 0; index < sorted.size(); ++index) {
    const auto object = sorted[index].objectIndex;
    if (index != 0u && sorted[index - 1u].objectIndex == object)
      throw std::invalid_argument("Physics product contains a duplicate fragment");
    const auto owner = impl_->objectProducts.find(object);
    if (owner != impl_->objectProducts.end() && owner->second != productIdentityId)
      throw std::invalid_argument("Physics fragment already belongs to another body");
  }
  if (const auto found = impl_->products.find(productIdentityId);
      found != impl_->products.end()) {
    auto &product = found->second;
    if (sorted.size() != product.objects.size() ||
        !std::equal(sorted.begin(), sorted.end(), product.objects.begin(),
                    [](const auto &fragment, uint32_t object) {
                      return fragment.objectIndex == object;
                    }))
      throw std::invalid_argument("Physics product fragments changed within one model publication");
    const auto body = physics.replaceBox(graph, product.node, product.localMin,
                                         product.localMax, settings);
    product.body = body;
    return body;
  }

  glm::vec3 minimum = sorted.front().worldMin;
  glm::vec3 maximum = sorted.front().worldMax;
  for (const auto &fragment : sorted) {
    minimum = glm::min(minimum, fragment.worldMin);
    maximum = glm::max(maximum, fragment.worldMax);
  }
  const glm::dvec3 centerDouble =
      (glm::dvec3{minimum} + glm::dvec3{maximum}) * 0.5;
  const glm::vec3 center{centerDouble};
  Impl::Product product;
  product.localMin = glm::vec3{glm::dvec3{minimum} - centerDouble};
  product.localMax = glm::vec3{glm::dvec3{maximum} - centerDouble};
  if (!finite(center) || !finite(product.localMin) || !finite(product.localMax))
    throw std::invalid_argument("Physics product bounds exceed the supported range");
  product.anchorInverse = translated(-center);
  product.fragments = std::move(sorted);
  product.objects.reserve(product.fragments.size());
  for (const auto &fragment : product.fragments)
    product.objects.push_back(fragment.objectIndex);

  impl_->freeNodes.reserve(impl_->freeNodes.size() + 1u);
  impl_->products.reserve(impl_->products.size() + 1u);
  impl_->objectProducts.reserve(impl_->objectProducts.size() + product.objects.size());
  impl_->ensureRoot(graph);
  product.authoredProxyLocal = glm::inverse(impl_->rootMatrix(graph)) * translated(center);
  product.node = impl_->takeNode(graph, product.authoredProxyLocal, label);
  const auto node = product.node;
  try {
    auto [entry, inserted] = impl_->products.emplace(productIdentityId, std::move(product));
    (void)inserted;
    auto &stored = entry->second;
    for (const auto object : stored.objects)
      impl_->objectProducts.emplace(object, productIdentityId);
    stored.body = physics.attachBox(graph, node, stored.localMin, stored.localMax, settings);
    return stored.body;
  } catch (...) {
    if (const auto found = impl_->products.find(productIdentityId);
        found != impl_->products.end()) {
      for (const auto object : found->second.objects)
        impl_->objectProducts.erase(object);
      impl_->products.erase(found);
    }
    impl_->freeNodes.push_back(node);
    throw;
  }
}

std::optional<uint32_t>
PhysicsElementBindings::productForObject(uint32_t objectIndex) const noexcept {
  if (const auto found = impl_->objectProducts.find(objectIndex);
      found != impl_->objectProducts.end())
    return found->second;
  return std::nullopt;
}

std::optional<uint32_t>
PhysicsElementBindings::bodyNodeForProduct(uint32_t productIdentityId) const noexcept {
  if (const auto found = impl_->products.find(productIdentityId);
      found != impl_->products.end())
    return found->second.node;
  return std::nullopt;
}

std::optional<uint32_t>
PhysicsElementBindings::bodyNodeForObject(uint32_t objectIndex) const noexcept {
  const auto product = productForObject(objectIndex);
  return product ? bodyNodeForProduct(*product) : std::nullopt;
}

std::span<const uint32_t>
PhysicsElementBindings::objectsForNode(uint32_t node) const noexcept {
  for (const auto &[productId, product] : impl_->products) {
    (void)productId;
    if (product.node == node)
      return product.objects;
  }
  return {};
}

size_t PhysicsElementBindings::productCount() const noexcept {
  return impl_->products.size();
}

bool PhysicsElementBindings::ownsNode(const SceneGraph &graph,
                                      uint32_t node) const noexcept {
  if (!impl_->matches(graph) || node == SceneGraph::kInvalidNode)
    return false;
  if (node == impl_->root ||
      std::find(impl_->freeNodes.begin(), impl_->freeNodes.end(), node) != impl_->freeNodes.end())
    return true;
  for (const auto &[productId, product] : impl_->products) {
    (void)productId;
    if (product.node == node)
      return true;
  }
  return false;
}

std::vector<gpu::ObjectTransformUpdate>
PhysicsElementBindings::collectTransforms(const SceneGraph &graph) const {
  std::vector<gpu::ObjectTransformUpdate> updates;
  if (!impl_->matches(graph))
    return updates;
  updates.reserve(impl_->objectProducts.size());
  for (const auto &[productId, product] : impl_->products) {
    (void)productId;
    impl_->appendTransforms(product, impl_->proxyMatrix(graph, product), updates);
  }
  return updates;
}

std::vector<gpu::ObjectTransformUpdate>
PhysicsElementBindings::previewTransforms(const SceneGraph &graph, uint32_t node,
                                          const glm::mat4 &local) const {
  std::vector<gpu::ObjectTransformUpdate> updates;
  if (!impl_->matches(graph))
    return updates;
  const bool rootEdit = node == impl_->root;
  for (const auto &[productId, product] : impl_->products) {
    (void)productId;
    if (!rootEdit && product.node != node)
      continue;
    // The root is an independent graph root, and each active proxy remains
    // its direct child. Preserve collectTransforms' topology checks as well
    // as its fragment multiplication order for the prospective edit.
    const auto root = impl_->rootMatrix(graph);
    const auto *entry = graph.getNode(product.node);
    if (!entry || entry->parent != impl_->root)
      throw std::invalid_argument("Physics product proxy was removed or reparented");
    const auto proxy = rootEdit ? local * entry->localTransform : root * local;
    if (!finiteAffine(proxy))
      throw std::invalid_argument("Physics product proxy has an invalid transform");
    if (rootEdit && updates.empty())
      updates.reserve(impl_->objectProducts.size());
    else if (!rootEdit)
      updates.reserve(product.fragments.size());
    impl_->appendTransforms(product, proxy, updates);
  }
  return updates;
}

std::vector<gpu::ObjectTransformUpdate>
PhysicsElementBindings::detach(SceneGraph &graph, PhysicsScene &physics,
                               uint32_t objectIndex, bool restore) {
  std::vector<gpu::ObjectTransformUpdate> updates;
  const auto productId = productForObject(objectIndex);
  if (!productId)
    return updates;
  if (!impl_->matches(graph)) {
    (void)clear(&graph, physics, false);
    return updates;
  }
  const auto found = impl_->products.find(*productId);
  auto &product = found->second;
  impl_->freeNodes.reserve(impl_->freeNodes.size() + 1u);
  if (restore) {
    updates.reserve(product.fragments.size());
    impl_->appendTransforms(product, impl_->rootMatrix(graph) * product.authoredProxyLocal,
                             updates);
  }
  if (physics.hasBody(product.node))
    (void)physics.detach(graph, product.node, restore);
  for (const auto object : product.objects)
    impl_->objectProducts.erase(object);
  impl_->freeNodes.push_back(product.node);
  impl_->products.erase(found);
  return updates;
}

std::vector<gpu::ObjectTransformUpdate>
PhysicsElementBindings::clear(SceneGraph *graph, PhysicsScene &physics,
                              bool restore) {
  std::vector<gpu::ObjectTransformUpdate> updates;
  const bool matching = graph != nullptr && impl_->matches(*graph);
  if (graph == nullptr && physics.graphIdentity() == impl_->graphIdentity) {
    for (const auto &[productId, product] : impl_->products) {
      (void)productId;
      if (physics.hasBody(product.node))
        throw std::invalid_argument("Retiring live auxiliary bodies requires their scene graph");
    }
  }
  if (graph != nullptr)
    impl_->retireObsoleteGraph(*graph, physics);
  if (matching) {
    impl_->freeNodes.reserve(impl_->freeNodes.size() + impl_->products.size());
    if (restore && !impl_->products.empty()) {
      updates.reserve(impl_->objectProducts.size());
      const auto root = impl_->rootMatrix(*graph);
      for (const auto &[productId, product] : impl_->products) {
        (void)productId;
        impl_->appendTransforms(product, root * product.authoredProxyLocal, updates);
      }
    }
  }
  for (const auto &[productId, product] : impl_->products) {
    (void)productId;
    if (graph != nullptr && ((matching && physics.hasBody(product.node)) ||
                            physics.bodyId(product.node) == product.body))
      (void)physics.detach(*graph, product.node, matching && restore);
    if (matching)
      impl_->freeNodes.push_back(product.node);
  }
  impl_->products.clear();
  impl_->objectProducts.clear();
  if (!matching) {
    impl_->freeNodes.clear();
    impl_->root = SceneGraph::kInvalidNode;
    impl_->graphIdentity = 0u;
    impl_->generation = 0u;
  }
  return updates;
}

void PhysicsElementBindings::translate(SceneGraph &graph, PhysicsScene &physics,
                                      glm::vec3 delta) {
  if (!finite(delta))
    throw std::invalid_argument("Physics auxiliary translation must be finite");
  if (delta == glm::vec3{0.0f} || impl_->products.empty() || !impl_->matches(graph))
    return;
  const auto movedRoot = translated(delta) * impl_->rootMatrix(graph);
  if (!finiteAffine(movedRoot))
    throw std::invalid_argument("Physics auxiliary translation exceeds the scene range");
  for (const auto &[productId, product] : impl_->products) {
    (void)productId;
    if (!finiteAffine(translated(delta) * impl_->proxyMatrix(graph, product)))
      throw std::invalid_argument("Physics product translation exceeds the scene range");
  }
  for (const auto &[productId, product] : impl_->products) {
    (void)productId;
    if (physics.motionType(product.node) == PhysicsMotionType::Dynamic) {
      physics.setPlaying(false);
      break;
    }
  }
  graph.setLocalTransform(impl_->root, movedRoot);
  graph.updateWorldTransforms();
}

} // namespace container::physics
