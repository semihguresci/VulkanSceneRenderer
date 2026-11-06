#include <Container/geometry/GltfMergeGeometry.h>
#include <Container/utility/SourcePrimitiveMapping.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace container::geometry::gltf {
namespace {
[[noreturn]] void fail(const char* message) {
  throw std::runtime_error(std::string("glTF merge geometry: ") + message);
}
uint64_t add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX-a) fail("count overflow");
  return a+b;
}
template<glm::length_t N> bool finite(const glm::vec<N,float,glm::defaultp>& value) {
  for (glm::length_t i=0;i<N;++i) if (!std::isfinite(value[i])) return false;
  return true;
}
struct Budget {
  const GltfMergeGeometryLimits& limits;
  uint64_t bytes{}, vertices{}, indices{};
  void charge(uint64_t count, uint64_t width) {
    if (count > (std::min<uint64_t>(limits.maxOutputBytes,SIZE_MAX)-bytes)/width)
      fail("output exceeds its byte budget");
    bytes += count*width;
  }
  void mesh(const Mesh& mesh) {
    vertices=add(vertices,mesh.vertices().size());
    indices=add(indices,mesh.indices().size());
    if (vertices>std::min<uint64_t>(limits.maxVertices,UINT32_MAX) ||
        indices>std::min<uint64_t>(limits.maxIndices,UINT32_MAX))
      fail("flattened geometry exceeds its index/count budget");
    if (mesh.empty() || mesh.indices().size()%3) fail("expected nonempty triangle geometry");
    for (const auto index:mesh.indices())
      if (index>=mesh.vertices().size()) fail("mesh index is outside its vertex stream");
    for (const auto& v:mesh.vertices())
      if (!finite(v.position) || !finite(v.color) || !finite(v.texCoord) ||
          !finite(v.texCoord1) || !finite(v.normal) || !finite(v.tangent) ||
          !std::isfinite(v.colorAlpha)) fail("mesh contains a nonfinite vertex attribute");
    charge(mesh.vertices().size(),2*sizeof(Vertex));
    charge(mesh.indices().size(),2*sizeof(uint32_t));
  }
};
int32_t material(int32_t local, const GltfMergePlan& plan) {
  if (local==-1) return -1;
  if (local<0 || size_t(local)>=plan.imported.materials.size()) fail("incoming material identity is invalid");
  const auto global=plan.imported.materials[size_t(local)];
  if (global<0 || size_t(global)>=plan.model.materials.size()) fail("mapped material identity is invalid");
  return global;
}
void sourceMesh(const Mesh& mesh, const tinygltf::Primitive& source,
                const tinygltf::Model& model, int32_t expectedMaterial) {
  const auto position=source.attributes.find("POSITION");
  if (position==source.attributes.end() || position->second<0 ||
      size_t(position->second)>=model.accessors.size() ||
      model.accessors[size_t(position->second)].count!=mesh.vertices().size() ||
      expectedMaterial!=source.material)
    fail("source primitive and runtime geometry disagree");
}
} // namespace

GltfMergeGeometry ComposeGltfMergeGeometry(const GltfMergeGeometryInput& base,
    const Model& incoming, const GltfMergePlan& plan, const GltfMergeGeometryLimits& limits) {
  if (!limits.maxPrimitives || !limits.maxVertices || !limits.maxIndices || !limits.maxOutputBytes)
    fail("all limits must be positive");
  const auto incomingMeshCount=plan.imported.primitives.size();
  if (plan.imported.meshes.size()!=incomingMeshCount || incomingMeshCount>plan.model.meshes.size())
    fail("incoming mesh maps disagree");
  const auto retainedMeshCount=plan.model.meshes.size()-incomingMeshCount;
  uint64_t retainedSourceCount{},incomingCount{};
  for (size_t m=0;m<retainedMeshCount;++m)
    retainedSourceCount=add(retainedSourceCount,plan.model.meshes[m].primitives.size());
  for (size_t m=0;m<incomingMeshCount;++m) {
    if (plan.imported.meshes[m]<0 || size_t(plan.imported.meshes[m])!=retainedMeshCount+m)
      fail("incoming mesh identities are not a canonical append");
    const auto& mapped=plan.imported.primitives[m];
    if (mapped.size()!=plan.model.meshes[retainedMeshCount+m].primitives.size())
      fail("incoming primitive map does not cover its source mesh");
    for (size_t p=0;p<mapped.size();++p)
      if (mapped[p].mesh!=plan.imported.meshes[m] || mapped[p].primitive<0 || size_t(mapped[p].primitive)!=p)
        fail("incoming primitive identity changed");
    incomingCount=add(incomingCount,mapped.size());
  }
  const auto runtimeCount=add(base.runtime.meshes().size(),incomingCount);
  const auto sourceCount=add(retainedSourceCount,incomingCount);
  if (runtimeCount>limits.maxPrimitives || runtimeCount>=UINT32_MAX || sourceCount>INT32_MAX ||
      incomingCount!=incoming.meshes().size()) fail("primitive tables exceed their count budget");
  Budget budget{limits};
  budget.charge(runtimeCount,sizeof(Mesh)+sizeof(PrimitiveRange)+sizeof(int32_t));
  budget.charge(sourceCount,sizeof(uint32_t));
  for (const auto& mesh:base.runtime.meshes()) budget.mesh(mesh);
  for (const auto& mesh:incoming.meshes()) budget.mesh(mesh);
  container::scene::ValidateSourcePrimitiveIds(base.sourcePrimitiveToRuntime,
      size_t(retainedSourceCount),base.runtime.meshes().size());
  auto mapping=container::scene::MakeRuntimePrimitiveMapping(size_t(retainedSourceCount),
      base.runtime.meshes().size(),base.runtimePrimitiveToSource);
  if (mapping.sourceToRuntime.size()!=base.sourcePrimitiveToRuntime.size() ||
      !std::equal(mapping.sourceToRuntime.begin(),mapping.sourceToRuntime.end(),base.sourcePrimitiveToRuntime.begin()))
    fail("forward and inverse primitive identities disagree");
  size_t flat{};
  for (size_t m=0;m<retainedMeshCount;++m) for (const auto& primitive:plan.model.meshes[m].primitives) {
    const auto& mesh=base.runtime.meshes()[mapping.sourceToRuntime[flat++]];
    sourceMesh(mesh,primitive,plan.model,mesh.materialIndex());
  }
  flat=0;
  for (size_t m=0;m<incomingMeshCount;++m) for (const auto& primitive:plan.model.meshes[retainedMeshCount+m].primitives) {
    const auto& mesh=incoming.meshes()[flat++];
    sourceMesh(mesh,primitive,plan.model,material(mesh.materialIndex(),plan));
  }
  // Payload validation/budgets finish before copying any geometry. FromMeshes
  // reserves exact flattened counts; its 32-bit sums are safe after preflight.
  std::vector<Mesh> meshes;
  meshes.reserve(size_t(runtimeCount));
  meshes.insert(meshes.end(),base.runtime.meshes().begin(),base.runtime.meshes().end());
  mapping.sourceToRuntime.reserve(size_t(sourceCount));
  mapping.runtimeToSource.reserve(size_t(runtimeCount));
  for (size_t p=0;p<incoming.meshes().size();++p) {
    const auto& mesh=incoming.meshes()[p];
    meshes.emplace_back(mesh.vertices(),mesh.indices(),material(mesh.materialIndex(),plan),mesh.disableBackfaceCulling());
    mapping.sourceToRuntime.push_back(uint32_t(base.runtime.meshes().size()+p));
    mapping.runtimeToSource.push_back(int32_t(retainedSourceCount+p));
  }
  return {Model::FromMeshes(std::move(meshes)),std::move(mapping.sourceToRuntime),
          std::move(mapping.runtimeToSource),budget.bytes};
}
} // namespace container::geometry::gltf
