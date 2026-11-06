#include <Container/geometry/GltfMergeGeometry.h>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <limits>

namespace {
using namespace container::geometry;
using namespace container::geometry::gltf;

struct Fixture {
  tinygltf::Model source;
  Model runtime;
  explicit Fixture(bool implicit=false, bool doubleSided=false) {
    std::vector<Vertex> vertices(3);
    vertices[0].position={0,0,0};vertices[1].position={1,0,0};vertices[2].position={0,1,0};
    for (auto& vertex:vertices) {vertex.normal={0,0,1};vertex.tangent={0,-1,0,-1};vertex.colorAlpha=.7f;}
    runtime=Model::FromMeshes({Mesh(vertices,{0,1,2},implicit?-1:0,doubleSided)});
    const std::array<float,9> positions{0,0,0,1,0,0,0,1,0};
    tinygltf::Buffer buffer;buffer.data.resize(sizeof(positions));
    std::memcpy(buffer.data.data(),positions.data(),sizeof(positions));source.buffers.push_back(std::move(buffer));
    tinygltf::BufferView view;view.buffer=0;view.byteLength=sizeof(positions);source.bufferViews.push_back(view);
    tinygltf::Accessor accessor;accessor.bufferView=0;accessor.componentType=TINYGLTF_COMPONENT_TYPE_FLOAT;
    accessor.type=TINYGLTF_TYPE_VEC3;accessor.count=3;source.accessors.push_back(accessor);
    tinygltf::Primitive primitive;primitive.mode=TINYGLTF_MODE_TRIANGLES;primitive.attributes["POSITION"]=0;
    primitive.material=implicit?-1:0;
    tinygltf::Mesh mesh;mesh.primitives.push_back(primitive);source.meshes.push_back(mesh);
    if (!implicit) {source.materials.emplace_back();source.materials[0].doubleSided=doubleSided;}
    tinygltf::Node node;node.mesh=0;source.nodes.push_back(node);
    tinygltf::Scene scene;scene.nodes={0};source.scenes.push_back(scene);source.defaultScene=0;
  }
};

TEST(GltfMergeGeometry, KeepsProceduralGapAndTranslatesIncomingSourceMaterial) {
  Fixture a,b;
  const auto procedural=Mesh(a.runtime.meshes()[0].vertices(),{0,1,2},99);
  auto edited=a.runtime.meshes()[0].vertices();edited[0].tangent={0,1,0,1};
  const auto live=Model::FromMeshes({Mesh(edited,{0,1,2},0),procedural});
  const std::array<uint32_t,1> forward{0};const std::array<int32_t,2> inverse{0,-1};
  const auto plan=PlanGltfMerge(a.source,b.source);
  const auto merged=ComposeGltfMergeGeometry({live,forward,inverse},b.runtime,plan);
  EXPECT_EQ(merged.sourcePrimitiveToRuntime,(std::vector<uint32_t>{0,2}));
  EXPECT_EQ(merged.runtimePrimitiveToSource,(std::vector<int32_t>{0,-1,1}));
  ASSERT_EQ(merged.runtime.meshes().size(),3u);
  EXPECT_EQ(merged.runtime.meshes()[0].vertices()[0].tangent,edited[0].tangent);
  EXPECT_EQ(merged.runtime.meshes()[1].materialIndex(),99);
  EXPECT_EQ(merged.runtime.meshes()[2].materialIndex(),plan.imported.materials[0]);
  EXPECT_EQ(merged.runtime.primitiveRanges()[2].firstIndex,6u);
  EXPECT_EQ(merged.runtime.primitiveRanges()[2].materialIndex,1);
  EXPECT_EQ(merged.runtime.indices(),(std::vector<uint32_t>{0,1,2,3,4,5,6,7,8}));
  EXPECT_EQ(live.meshes()[0].vertices()[0].tangent,edited[0].tangent);
  EXPECT_EQ(b.runtime.meshes()[0].materialIndex(),0);
}

TEST(GltfMergeGeometry, RepeatedMergeRetainsPriorRuntimeIdentitiesAndDoesNotClonePlacements) {
  Fixture a,b,c;
  const auto live=Model::FromMeshes({a.runtime.meshes()[0],Mesh(a.runtime.meshes()[0].vertices(),{0,1,2},90)});
  const std::array<uint32_t,1> forward{0};const std::array<int32_t,2> inverse{0,-1};
  const auto abPlan=PlanGltfMerge(a.source,b.source);
  const auto ab=ComposeGltfMergeGeometry({live,forward,inverse},b.runtime,abPlan);
  const auto abcPlan=PlanGltfMerge(abPlan.model,c.source);
  const auto abc=ComposeGltfMergeGeometry({ab.runtime,ab.sourcePrimitiveToRuntime,ab.runtimePrimitiveToSource},c.runtime,abcPlan);
  EXPECT_EQ(abc.sourcePrimitiveToRuntime,(std::vector<uint32_t>{0,2,3}));
  EXPECT_EQ(abc.runtimePrimitiveToSource,(std::vector<int32_t>{0,-1,1,2}));
  ASSERT_EQ(abc.runtime.meshes().size(),4u);
  EXPECT_EQ(abc.runtime.meshes()[1].materialIndex(),90);
  EXPECT_EQ(abc.runtime.meshes()[2].materialIndex(),1);
  EXPECT_EQ(abc.runtime.meshes()[3].materialIndex(),2);
  EXPECT_GT(abcPlan.model.nodes.size(),abc.runtime.meshes().size());
}

TEST(GltfMergeGeometry, PreservesImplicitMaterialAndDoubleSidedGeometry) {
  Fixture a,b(true,true);
  const std::array<uint32_t,1> forward{0};const std::array<int32_t,1> inverse{0};
  const auto plan=PlanGltfMerge(a.source,b.source);
  const auto merged=ComposeGltfMergeGeometry({a.runtime,forward,inverse},b.runtime,plan);
  EXPECT_EQ(merged.runtime.meshes()[1].materialIndex(),-1);
  EXPECT_TRUE(merged.runtime.meshes()[1].disableBackfaceCulling());
  EXPECT_EQ(merged.runtime.primitiveRanges()[1].materialIndex,-1);
}

TEST(GltfMergeGeometry, ProceduralOnlyBaseRetainsRuntimeMaterialsOutsideSourceTable) {
  Fixture incoming;
  tinygltf::Model empty;
  const auto base=Model::FromMeshes({Mesh(incoming.runtime.meshes()[0].vertices(),{0,1,2},123)});
  const std::array<int32_t,1> inverse{-1};
  const auto plan=PlanGltfMerge(empty,incoming.source);
  const auto result=ComposeGltfMergeGeometry({base,{},inverse},incoming.runtime,plan);
  EXPECT_EQ(result.sourcePrimitiveToRuntime,(std::vector<uint32_t>{1}));
  EXPECT_EQ(result.runtimePrimitiveToSource,(std::vector<int32_t>{-1,0}));
  EXPECT_EQ(result.runtime.meshes()[0].materialIndex(),123);
  EXPECT_EQ(result.runtime.meshes()[1].materialIndex(),0);
}

TEST(GltfMergeGeometry, InvalidMapsSourceAssociationsAndGeometryRejectWithoutInputMutation) {
  Fixture a,b;const auto plan=PlanGltfMerge(a.source,b.source);
  const std::array<uint32_t,1> forward{0},badForward{1};const std::array<int32_t,1> inverse{0};
  EXPECT_THROW((void)ComposeGltfMergeGeometry({a.runtime,badForward,inverse},b.runtime,plan),std::invalid_argument);
  const auto wrongMaterial=Model::FromMeshes({Mesh(b.runtime.meshes()[0].vertices(),{0,1,2},7)});
  EXPECT_THROW((void)ComposeGltfMergeGeometry({a.runtime,forward,inverse},wrongMaterial,plan),std::runtime_error);
  const auto badIndex=Model::FromMeshes({Mesh(b.runtime.meshes()[0].vertices(),{0,1,9},0)});
  EXPECT_THROW((void)ComposeGltfMergeGeometry({a.runtime,forward,inverse},badIndex,plan),std::runtime_error);
  auto vertices=b.runtime.meshes()[0].vertices();vertices[0].colorAlpha=std::numeric_limits<float>::infinity();
  const auto nonfinite=Model::FromMeshes({Mesh(vertices,{0,1,2},0)});
  EXPECT_THROW((void)ComposeGltfMergeGeometry({a.runtime,forward,inverse},nonfinite,plan),std::runtime_error);
  auto badPlan=plan;badPlan.imported.primitives[0][0].mesh=0;
  EXPECT_THROW((void)ComposeGltfMergeGeometry({a.runtime,forward,inverse},b.runtime,badPlan),std::runtime_error);
  EXPECT_EQ(a.runtime.indices(),(std::vector<uint32_t>{0,1,2}));
  EXPECT_EQ(b.runtime.meshes()[0].materialIndex(),0);
}

TEST(GltfMergeGeometry, CountsAndBothOwnedStreamCopiesAreBudgetedBeforeConstruction) {
  Fixture a,b;const auto plan=PlanGltfMerge(a.source,b.source);
  const std::array<uint32_t,1> forward{0};const std::array<int32_t,1> inverse{0};
  const auto valid=ComposeGltfMergeGeometry({a.runtime,forward,inverse},b.runtime,plan);
  for (int kind=0;kind<4;++kind) {
    GltfMergeGeometryLimits limits;
    if (kind==0) limits.maxOutputBytes=valid.outputBytes-1;
    if (kind==1) limits.maxPrimitives=1;
    if (kind==2) limits.maxVertices=5;
    if (kind==3) limits.maxIndices=5;
    EXPECT_THROW((void)ComposeGltfMergeGeometry({a.runtime,forward,inverse},b.runtime,plan,limits),std::runtime_error) << kind;
  }
  GltfMergeGeometryLimits exact;exact.maxOutputBytes=valid.outputBytes;
  EXPECT_EQ(ComposeGltfMergeGeometry({a.runtime,forward,inverse},b.runtime,plan,exact).outputBytes,valid.outputBytes);
}
} // namespace
