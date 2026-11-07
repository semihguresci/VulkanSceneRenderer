#include "Container/geometry/OpenVdbNativePointPreview.h"
#include "Container/geometry/OpenVdbPoints.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/points/PointGroup.h>
#endif

namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbNativePointPreviewAvailability, DisabledBeforeAllOptionsAndPaths) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::ProcessingOptions p;p.maxInputBytes=0;vdb::NativePointPreviewOptions o;o.radiusScale=-1;
  try{(void)vdb::LoadNativePointsPreviewFromFile("missing","",-1,o,p);FAIL();}
  catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points=openvdb::points;using Grid=points::PointDataGrid;using Op=vdb::PointEditOperation;
class OpenVdbNativePointPreview:public ::testing::Test {
 protected:
  void SetUp()override {
    static std::atomic_uint64_t serial{0};directory=std::filesystem::temp_directory_path()/("container-native-point-preview-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));std::filesystem::create_directory(directory);
    p.voxelSize=1;p.gridName="points";std::vector<vdb::PointRecord> records(3);
    for(size_t i=0;i<3;++i){records[i].position={double(i)*.1,0,0};records[i].radius=double(i+1)*.01;}
    (void)vdb::WritePoints(records,path("raw.vdb"),p);
    const std::array<vdb::PointAttributeEdit,7> attributes{{
      {Op::Add,"id",{},std::vector<int64_t>{INT64_MAX-2,INT64_MAX-1,INT64_MAX}},
      {Op::Add,"extreme",{},std::vector<double>{-DBL_MAX,0,DBL_MAX}},
      {Op::Add,"flag",{},std::vector<uint8_t>{0,1,0}},
      {Op::Add,"radius",{},std::vector<float>{.01f,.02f,.03f}},
      {Op::Add,"direction",{},std::vector<std::array<double,3>>{{1,0,0},{-1,0,0},{0,0,0}}},
      {Op::Add,"label",{},std::vector<std::string>{"blue",std::string("a\0b",3),"blue"}},
      {Op::Add,"invalid",{},std::vector<double>{1,2,3}}}};
    const std::array<vdb::PointGroupEdit,2> groups{{{Op::Add,"keep",{},{1,1,0}},{Op::Add,"empty",{},{0}}}};
    (void)vdb::WritePointAttributeEdits(path("raw.vdb"),path("source.vdb"),attributes,groups,{},p);
    auto source=read();source->tree().setActiveState({0,0,0},false);
    source->insertMeta("artist",openvdb::StringMetadata(std::string("raw\0value",9)));
    points::makeDescriptorUnique(source->tree())->getMetadata().insertMeta("custom:descriptor",openvdb::StringMetadata("preserved"));
    for(auto leaf=source->tree().beginLeaf();leaf;++leaf)leaf->attributeArray("invalid").setHidden(true);
    points::appendAttribute<double>(source->tree(),"pair",.5,2,true);
    write("source.vdb",{source});
  }
  void TearDown()override{std::error_code ignored;for(const auto& entry:std::filesystem::directory_iterator(directory))std::filesystem::remove(entry.path(),ignored);std::filesystem::remove(directory,ignored);}
  auto path(const std::string& n)const{return directory/n;}
  Grid::Ptr read(const std::string& file="source.vdb"){openvdb::io::File input(path(file).string());input.open(false);auto grid=openvdb::gridPtrCast<Grid>(input.readGrid("points"));input.close();return grid;}
  void write(const std::string& file,const openvdb::GridPtrVec& grids){openvdb::io::File output(path(file).string());output.write(grids);}
  static std::string bytes(const std::filesystem::path& p){std::ifstream stream(p,std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}};}
  auto preview(vdb::NativePointPreviewOptions o={},float scale=1,const std::string& file="source.vdb"){return vdb::LoadNativePointsPreviewFromFile(path(file),{},scale,o,p);}
  static glm::vec3 center(const container::geometry::dotbim::Model& model,size_t point){return (model.vertices[point*6].position+model.vertices[point*6+1].position)*.5f;}
  static std::string property(const container::geometry::dotbim::Model& model,const std::string& scope,const std::string& name) {
    for(const auto& p:model.elements.at(0).properties)if(p.set==scope && p.name==name)return p.value;return {};
  }
  std::filesystem::path directory;vdb::ProcessingOptions p;
};

TEST_F(OpenVdbNativePointPreview, ConstantRadiusMarkersNativeInactiveOrderMetadataAndOpacity) {
  const auto before=bytes(path("source.vdb"));vdb::NativePointPreviewOptions o;o.constantRadius=.025;o.opacity=.4f;
  const auto model=preview(o,2);ASSERT_EQ(model.vertices.size(),18u);EXPECT_EQ(model.indices.size(),72u);ASSERT_EQ(model.elements.size(),1u);
  EXPECT_EQ(model.elements[0].geometryKind,container::geometry::dotbim::GeometryKind::Mesh);
  EXPECT_TRUE(model.nativePointRanges.empty());EXPECT_FLOAT_EQ(center(model,0).x,0);EXPECT_NEAR(center(model,1).x,.2,1e-7);
  EXPECT_NEAR(model.vertices[0].position.x-center(model,0).x,.05,1e-7);
  EXPECT_EQ(property(model,"OpenVDB","Selected inactive points"),"3");
  EXPECT_EQ(property(model,"OpenVDB metadata","artist"),std::string("raw\0value",9));
  EXPECT_EQ(property(model,"OpenVDB descriptor metadata","custom:descriptor"),"preserved");
  EXPECT_NE(property(model,"OpenVDB attributes","invalid").find("hidden=1"),std::string::npos);
  EXPECT_NE(property(model,"OpenVDB attributes","pair").find("stride=2"),std::string::npos);
  EXPECT_EQ(property(model,"OpenVDB groups","keep"),"Native group membership retained in source");
  ASSERT_EQ(model.materials.size(),1u);EXPECT_EQ(model.materials[0].pbr.alphaMode,container::material::AlphaMode::Blend);
  EXPECT_FLOAT_EQ(model.materials[0].pbr.opacityFactor,.4f);EXPECT_FLOAT_EQ(model.materials[0].pbr.baseColor.a,1);
  EXPECT_FLOAT_EQ(model.vertices[0].colorAlpha,1);EXPECT_EQ(bytes(path("source.vdb")),before);
  for(size_t i=0;i<model.indices.size();i+=3){const auto a=model.vertices[model.indices[i]].position,b=model.vertices[model.indices[i+1]].position,c=model.vertices[model.indices[i+2]].position;const auto local=(a+b+c)/3.f-center(model,model.indices[i]/6);EXPECT_GT(glm::dot(glm::cross(b-a,c-a),local),0);}
}

TEST_F(OpenVdbNativePointPreview, ExactAdjacentInt64AndOpposingDoubleScalarColors) {
  vdb::NativePointPreviewOptions o;o.colorMode=vdb::NativePointColor::ScalarRamp;o.colorAttribute="id";
  const auto integers=preview(o);EXPECT_EQ(integers.vertices[0].color,glm::vec3(.1f,.25f,1));
  EXPECT_EQ(integers.vertices[6].color,glm::vec3(.55f,.225f,.525f));EXPECT_EQ(integers.vertices[12].color,glm::vec3(1,.2f,.05f));
  o.colorAttribute="extreme";const auto floating=preview(o);
  for(size_t n=0;n<3;++n)EXPECT_EQ(floating.vertices[6*n].color,integers.vertices[6*n].color);
  o.colorAttribute="flag";const auto boolean=preview(o);EXPECT_EQ(boolean.vertices[0].color,integers.vertices[0].color);EXPECT_EQ(boolean.vertices[6].color,integers.vertices[12].color);
  auto source=read();for(auto leaf=source->tree().beginLeaf();leaf;++leaf){points::AttributeWriteHandle<int64_t> ids(leaf->attributeArray("id"));ids.set(0,INT64_MIN);ids.set(1,INT64_MIN+1);ids.set(2,INT64_MIN+2);}write("negative.vdb",{source});
  o.colorAttribute="id";const auto negative=preview(o,1,"negative.vdb");for(size_t n=0;n<3;++n)EXPECT_EQ(negative.vertices[6*n].color,integers.vertices[6*n].color);
}

TEST_F(OpenVdbNativePointPreview, ExplicitPhysicalRadiusAndGroupSelection) {
  vdb::NativePointPreviewOptions o;o.radiusAttribute="radius";o.radiusScale=2;o.groups.anyOf={"keep"};
  const auto model=preview(o,3);ASSERT_EQ(model.vertices.size(),12u);EXPECT_NEAR(model.vertices[0].position.x,.06,1e-7);
  EXPECT_NEAR(model.vertices[6].position.x-center(model,1).x,.12,1e-7);
  o.groups.allOf={"empty"};EXPECT_TRUE(preview(o).vertices.empty());
  o.groups={};o.radiusAttribute="id";EXPECT_NO_THROW((void)preview(o)); // Explicit Int64 physical radius is supported.
  o.radiusAttribute="flag";EXPECT_THROW((void)preview(o),std::invalid_argument); // False is not a positive physical radius.
  o.radiusAttribute="P";EXPECT_THROW((void)preview(o),std::invalid_argument);
  o.radiusAttribute="pair";EXPECT_THROW((void)preview(o),std::invalid_argument);
}

TEST_F(OpenVdbNativePointPreview, SignedVectorColorAndStableUtf8StringPalette) {
  vdb::NativePointPreviewOptions o;o.colorMode=vdb::NativePointColor::VectorDirection;o.colorAttribute="direction";
  auto model=preview(o);EXPECT_EQ(model.vertices[0].color,glm::vec3(1,.5f,.5f));EXPECT_EQ(model.vertices[6].color,glm::vec3(0,.5f,.5f));EXPECT_EQ(model.vertices[12].color,glm::vec3(.5f));
  o.colorMode=vdb::NativePointColor::StringPalette;o.colorAttribute="label";model=preview(o);
  // Independent Python integer FNV-1a oracle: blue -> index5, a-NUL-b -> index6.
  EXPECT_EQ(model.vertices[0].color,glm::vec3(.2f,.75f,.76f));EXPECT_EQ(model.vertices[12].color,model.vertices[0].color);
  EXPECT_EQ(model.vertices[6].color,glm::vec3(.86f,.4f,.6f));
  const auto again=preview(o);EXPECT_EQ(again.vertices[6].color,model.vertices[6].color);
  auto source=read();const std::array<vdb::PointAttributeEdit,1> invalid{{{Op::Set,"label",{},std::vector<std::string>{std::string("\xc0\xaf",2)}}}};
  (void)vdb::WritePointAttributeEdits(path("source.vdb"),path("bad-utf8.vdb"),invalid,{}, {},p);
  EXPECT_THROW((void)preview(o,1,"bad-utf8.vdb"),std::invalid_argument);
}

TEST_F(OpenVdbNativePointPreview, RegisteredFixedPositionAndScalarCodecsDecodeWithoutNullAssumption) {
  using PackedP=points::TypedAttributeArray<openvdb::Vec3f,points::TruncateCodec>;
  using PackedS=points::TypedAttributeArray<float,points::TruncateCodec>;
  if(!PackedP::isRegistered())PackedP::registerType();if(!PackedS::isRegistered())PackedS::registerType();
  auto source=read();
  // Replace P's descriptor type and native array together, preserving offsets.
  auto descriptor=std::make_shared<points::AttributeSet::Descriptor>(source->tree().cbeginLeaf()->attributeSet().descriptor());
  const auto position=descriptor->find("P");descriptor=descriptor->duplicateDrop({position});descriptor=descriptor->duplicateAppend("P",PackedP::attributeType());
  for(auto leaf=source->tree().beginLeaf();leaf;++leaf) {
    auto attrs=std::make_unique<points::AttributeSet>(descriptor,openvdb::Index(leaf->pointCount()));
    for(const auto& [name,index]:leaf->attributeSet().descriptor().map())if(name!="P")attrs->replace(name,leaf->constAttributeArray(index).copy());
    points::AttributeWriteHandle<openvdb::Vec3f,points::TruncateCodec> p(*attrs->get("P"));p.collapse({.125f,-.25f,.375f});
    leaf->replaceAttributeSet(attrs.release(),true);
  }
  points::appendAttribute<float,points::TruncateCodec>(source->tree(),"packed",.25f);
  write("packed.vdb",{source});vdb::NativePointPreviewOptions o;o.radiusAttribute="packed";o.colorMode=vdb::NativePointColor::ScalarRamp;o.colorAttribute="packed";
  const auto actual=preview(o,1,"packed.vdb");ASSERT_EQ(actual.vertices.size(),18u);EXPECT_EQ(center(actual,0),glm::vec3(.125f,-.25f,.375f));
  EXPECT_EQ(actual.vertices[0].color,glm::vec3(.55f,.225f,.525f));EXPECT_NE(property(actual,"OpenVDB attributes","P").find("codec=trnc"),std::string::npos);
}

TEST_F(OpenVdbNativePointPreview, RemoteIntegerOriginCompensationAndAffinePosition) {
  std::array<vdb::PointRecord,1> initial{{{{1e9,0,0},{},.25}}};(void)vdb::WritePoints(initial,path("remote.vdb"),p);
  auto source=read("remote.vdb");auto leaf=source->tree().beginLeaf();points::AttributeWriteHandle<openvdb::Vec3f> positions(leaf->attributeArray("P"));positions.set(0,{.1234567f,-.2f,.3f});
  source->transform().postTranslate({-1e9,0,0});write("recentered.vdb",{source});
  auto actual=preview({},1,"recentered.vdb");EXPECT_FLOAT_EQ(center(actual,0).x,.1234567f);EXPECT_FLOAT_EQ(center(actual,0).y,-.2f);EXPECT_FLOAT_EQ(center(actual,0).z,.3f);
  source=read();source->transform().postRotate(.3,openvdb::math::Z_AXIS);source->transform().postScale({.4,.7,1.2});source->transform().postTranslate({1,2,3});write("affine.vdb",{source});
  actual=preview({},2,"affine.vdb");const auto expected=source->transform().indexToWorld(openvdb::Vec3d(.1f,0,0))*2;
  EXPECT_NEAR(center(actual,1).x,expected.x(),1e-6);EXPECT_NEAR(center(actual,1).y,expected.y(),1e-6);
}

TEST_F(OpenVdbNativePointPreview, OnlySelectedValuesMustBeFiniteAndChosenOpaqueLayoutsFail) {
  auto source=read();auto leaf=source->tree().beginLeaf();points::AttributeWriteHandle<double> invalid(leaf->attributeArray("invalid"));invalid.set(2,NAN);
  points::AttributeWriteHandle<openvdb::Vec3f> position(leaf->attributeArray("P"));position.set(2,{NAN,0,0});write("invalid-values.vdb",{source});
  vdb::NativePointPreviewOptions o;o.groups.anyOf={"keep"};o.colorMode=vdb::NativePointColor::ScalarRamp;o.colorAttribute="invalid";
  EXPECT_EQ(preview(o,1,"invalid-values.vdb").vertices.size(),12u);o.groups={};EXPECT_THROW((void)preview(o,1,"invalid-values.vdb"),std::invalid_argument);
  o.colorAttribute="pair";EXPECT_THROW((void)preview(o),std::invalid_argument);
  o.colorAttribute="missing";EXPECT_THROW((void)preview(o),std::invalid_argument);
  o.colorMode=vdb::NativePointColor::VectorDirection;o.colorAttribute="direction";
  source=read();leaf=source->tree().beginLeaf();points::AttributeWriteHandle<openvdb::Vec3d> direction(leaf->attributeArray("direction"));direction.set(0,{1,NAN,0});write("invalid-vector.vdb",{source});EXPECT_THROW((void)preview(o,1,"invalid-vector.vdb"),std::invalid_argument);
}

TEST_F(OpenVdbNativePointPreview, SharedPointTriangleMetadataMemoryCatalogAndFloatPrecisionLimits) {
  vdb::NativePointPreviewOptions o;o.maxMetadataBytes=1;EXPECT_THROW((void)preview(o),std::invalid_argument);
  auto small=p;small.maxTriangles=23;EXPECT_THROW((void)vdb::LoadNativePointsPreviewFromFile(path("source.vdb"),{},1,{},small),std::invalid_argument);
  small=p;small.maxInputBytes=8*1024*1024;EXPECT_THROW((void)vdb::LoadNativePointsPreviewFromFile(path("source.vdb"),{},1,{},small),std::invalid_argument);
  vdb::PointAttributeOptions limits;limits.maxPoints=2;EXPECT_THROW((void)vdb::LoadNativePointsPreviewFromFile(path("source.vdb"),{},1,{},p,limits),std::invalid_argument);
  auto source=read();source->transform().postTranslate({1e6+.125,0,0});write("huge-origin.vdb",{source});EXPECT_THROW((void)preview({},1,"huge-origin.vdb"),std::invalid_argument);
  source=read();source->setTransform(openvdb::math::Transform::createFrustumTransform(openvdb::BBoxd({-1,-1,-1},{1,1,1}),.5,10,1));write("frustum.vdb",{source});EXPECT_THROW((void)preview({},1,"frustum.vdb"),std::invalid_argument);
  write("two.vdb",{read(),read()});o={};o.maxGrids=1;EXPECT_THROW((void)preview(o,1,"two.vdb"),std::invalid_argument);
  EXPECT_THROW((void)vdb::LoadNativePointsPreviewFromFile(path("two.vdb"),"points",1,{},p),std::invalid_argument);
  EXPECT_EQ(vdb::LoadNativePointsPreviewFromFile(path("two.vdb"),"descriptor:points[1]",1,{},p).vertices.size(),18u);
}

TEST_F(OpenVdbNativePointPreview, EmptySelectionAndArchivedSourceAreInformativeAndUnchanged) {
  const auto before=bytes(path("source.vdb"));vdb::NativePointPreviewOptions o;o.groups.anyOf={"empty"};
  const auto empty=preview(o);EXPECT_TRUE(empty.elements.empty());EXPECT_TRUE(empty.vertices.empty());EXPECT_FALSE(empty.importReport.diagnostics.empty());
  (void)vdb::WriteSelectedPoints(path("source.vdb"),path("archived.vdb"),o.groups,vdb::PointSelectionMode::KeepSelected,{},p);
  o.groups.anyOf={"keep"};const auto archived=preview(o,1,"archived.vdb");EXPECT_TRUE(archived.vertices.empty());EXPECT_FALSE(archived.importReport.diagnostics.empty());
  EXPECT_EQ(bytes(path("source.vdb")),before);
}
#endif
} // namespace
