#include "Container/geometry/OpenVdbMeshExport.h"
#include "Container/geometry/OpenVdbPoints.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;

struct Obj {
  std::vector<glm::dvec3> positions, normals;
  std::vector<std::array<size_t, 3>> faces, normalIndices;
  std::vector<std::string> groups;
};

Obj readObj(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read OBJ fixture");
  Obj result;
  std::string line;
  while (std::getline(input, line)) {
    std::istringstream fields(line);
    fields.imbue(std::locale::classic());
    std::string tag;
    fields >> tag;
    if (tag == "v" || tag == "vn") {
      glm::dvec3 vector;
      if (!(fields >> vector.x >> vector.y >> vector.z)) throw std::runtime_error("invalid OBJ vector");
      (tag == "v" ? result.positions : result.normals).push_back(vector);
    } else if (tag == "g") {
      std::string group;
      fields >> group;
      result.groups.push_back(group);
    } else if (tag == "f") {
      std::array<size_t, 3> vertices, normals;
      for (size_t corner = 0; corner < 3; ++corner) {
        std::string index;
        fields >> index;
        const auto split = index.find("//");
        if (split == std::string::npos) throw std::runtime_error("expected indexed v//vn face");
        vertices[corner] = std::stoull(index.substr(0, split));
        normals[corner] = std::stoull(index.substr(split + 2));
        if (!vertices[corner] || vertices[corner] > result.positions.size() ||
            !normals[corner] || normals[corner] > result.normals.size())
          throw std::runtime_error("OBJ index is out of range");
        --vertices[corner]; --normals[corner];
      }
      std::string extra;
      if (fields >> extra) throw std::runtime_error("expected triangular OBJ faces");
      result.faces.push_back(vertices);
      result.normalIndices.push_back(normals);
    }
  }
  return result;
}

std::string bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(OpenVdbMeshExportAvailability, DisabledBuildFailsBeforeInspectingOutput) {
  if (vdb::IsAvailable()) GTEST_SKIP() << "OpenVDB is enabled";
  try {
    (void)vdb::WriteObjSurface("missing.vdb", {});
    FAIL() << "disabled export must fail before output validation";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
  }
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbMeshExport : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{0};
    directory = std::filesystem::temp_directory_path() /
        ("container-vdb-obj-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path file(const char* name) const { return directory / name; }
  std::filesystem::path write(const openvdb::GridPtrVec& grids, const char* name = "input.vdb") {
    const auto path = file(name);
    openvdb::io::File output(path.string());
    output.write(grids);
    output.close();
    return path;
  }
  template<class Grid = openvdb::FloatGrid>
  typename Grid::Ptr sphere(const std::string& name = "surface") {
    using Value = typename Grid::ValueType;
    auto grid = Grid::create(Value(3));
    grid->setName(name);
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    grid->setTransform(openvdb::math::Transform::createLinearTransform(0.25));
    auto accessor = grid->getAccessor();
    for (int x = -8; x <= 8; ++x)
      for (int y = -8; y <= 8; ++y)
        for (int z = -8; z <= 8; ++z) {
          const double distance = std::sqrt(double(x*x + y*y + z*z)) - 4.0;
          if (std::abs(distance) < 3.0) accessor.setValue({x,y,z}, Value(distance));
          else if (distance < 0) accessor.setValueOff({x,y,z}, Value(-3));
        }
    return grid;
  }
  static void expectOutward(const Obj& obj, glm::dvec3 center = glm::dvec3(0),
                             glm::dvec3 scale = glm::dvec3(1)) {
    ASSERT_FALSE(obj.faces.empty());
    for (size_t index = 0; index < obj.faces.size(); ++index) {
      const auto face = obj.faces[index];
      const auto a = obj.positions[face[0]], b = obj.positions[face[1]], c = obj.positions[face[2]];
      const auto cross = glm::cross(b-a, c-a);
      const auto outward = ((a+b+c) / 3.0 - center) / (scale * scale);
      EXPECT_GT(glm::dot(cross, outward), 0.0);
      for (size_t normalIndex : obj.normalIndices[index]) {
        const auto normal = obj.normals[normalIndex];
        EXPECT_NEAR(glm::length(normal), 1.0, 1e-6);
        EXPECT_GT(glm::dot(cross, normal), 0.0);
        EXPECT_NEAR(glm::dot(glm::normalize(cross), normal), 1.0, 1e-6);
      }
    }
  }
  std::filesystem::path directory;
};

TEST_F(OpenVdbMeshExport, IndexedObjRoundTripsNormalsWindingCountsAndDeterministicBytes) {
  const auto input = write({sphere()});
  const auto result = vdb::WriteObjSurface(input, file("surface.obj"));
  const auto obj = readObj(file("surface.obj"));
  EXPECT_EQ(result.gridCount, 1u);
  EXPECT_EQ(result.vertexCount, obj.positions.size());
  EXPECT_EQ(result.triangleCount, obj.faces.size());
  EXPECT_EQ(obj.positions.size(), obj.normals.size());
  EXPECT_EQ(obj.groups.size(), 1u);
  EXPECT_EQ(result.skippedEmptyGrids, 0u);
  EXPECT_TRUE(result.diagnostics.empty());
  expectOutward(obj);
  (void)vdb::WriteObjSurface(input, file("repeat.obj"));
  EXPECT_EQ(bytes(file("surface.obj")), bytes(file("repeat.obj")));
}

TEST_F(OpenVdbMeshExport, ReflectedAnisotropicAffineTransformHasWorldSpaceOutwardFaces) {
  auto source = sphere();
  source->transform().postScale(openvdb::Vec3d(-2,1.5,0.5));
  source->transform().postTranslate(openvdb::Vec3d(3,-4,2));
  (void)vdb::WriteObjSurface(write({source}), file("reflected.obj"));
  const auto obj = readObj(file("reflected.obj"));
  expectOutward(obj, {3,-4,2}, {-2,1.5,0.5});
  glm::dvec3 minimum(std::numeric_limits<double>::max()), maximum(std::numeric_limits<double>::lowest());
  for (const auto& position : obj.positions) {
    minimum = glm::min(minimum, position);
    maximum = glm::max(maximum, position);
  }
  EXPECT_NEAR(minimum.x, 1, 0.15); EXPECT_NEAR(maximum.x, 5, 0.15);
  EXPECT_NEAR(minimum.y, -5.5, 0.15); EXPECT_NEAR(maximum.y, -2.5, 0.15);
  EXPECT_NEAR(minimum.z, 1.5, 0.1); EXPECT_NEAR(maximum.z, 2.5, 0.1);
}

TEST_F(OpenVdbMeshExport, MultipleGridsHaveDistinctSafeGroupsAndExplicitEmptyDiagnostics) {
  auto first = sphere("name with spaces\n#comment");
  auto second = sphere("name/with/slashes");
  second->transform().postTranslate(openvdb::Vec3d(3,0,0));
  auto empty = openvdb::FloatGrid::create(0.0f);
  empty->setName("empty");
  const auto result = vdb::WriteObjSurface(write({first,second,empty}), file("groups.obj"));
  const auto obj = readObj(file("groups.obj"));
  ASSERT_EQ(obj.groups.size(), 2u);
  EXPECT_NE(obj.groups[0], obj.groups[1]);
  for (const auto& name : obj.groups) {
    EXPECT_TRUE(name.starts_with("grid_"));
    EXPECT_EQ(name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-"), std::string::npos);
  }
  EXPECT_EQ(result.gridCount, 2u);
  EXPECT_EQ(result.skippedEmptyGrids, 1u);
  EXPECT_EQ(result.diagnostics.size(), 1u);
  EXPECT_NE(bytes(file("groups.obj")).find("# warning:"), std::string::npos);
}

TEST_F(OpenVdbMeshExport, SelectedDoubleGridHonorsIsovalueAndAdaptivity) {
  auto other = sphere("other");
  auto selected = sphere<openvdb::DoubleGrid>("double");
  const auto input = write({other,selected});
  vdb::ImportOptions options;
  options.gridName = "double";
  options.isoValue = 1.0;
  const auto regular = vdb::WriteObjSurface(input, file("regular.obj"), options);
  const auto obj = readObj(file("regular.obj"));
  EXPECT_EQ(regular.gridCount, 1u);
  double radius = 0;
  for (const auto& position : obj.positions) radius = std::max(radius, glm::length(position));
  EXPECT_NEAR(radius, 1.25, 0.08);
  options.adaptivity = 1.0;
  const auto adapted = vdb::WriteObjSurface(input, file("adapted.obj"), options);
  EXPECT_GT(adapted.triangleCount, 0u);
  EXPECT_LT(adapted.triangleCount, regular.triangleCount);
}

TEST_F(OpenVdbMeshExport, FogIsovalueControlsSurfaceAndOutwardNormals) {
  auto source = openvdb::FloatGrid::create(0.0f);
  source->setName("fog");
  source->setGridClass(openvdb::GRID_FOG_VOLUME);
  source->setTransform(openvdb::math::Transform::createLinearTransform(0.25));
  for (int x = -6; x <= 6; ++x)
    for (int y = -6; y <= 6; ++y)
      for (int z = -6; z <= 6; ++z) {
        const double density = 1.0 - std::sqrt(double(x*x+y*y+z*z)) / 6.0;
        if (density > 0) source->tree().setValue({x,y,z}, float(density));
      }
  vdb::ImportOptions options;
  options.isoValue = 0.25;
  (void)vdb::WriteObjSurface(write({source}), file("fog.obj"), options);
  const auto obj = readObj(file("fog.obj"));
  expectOutward(obj);
  double maximumRadius = 0;
  for (const auto& position : obj.positions) maximumRadius = std::max(maximumRadius, glm::length(position));
  EXPECT_NEAR(maximumRadius, 1.125, 0.08);
}

TEST_F(OpenVdbMeshExport, UnsupportedGridsAreStrictUnlessExplicitlyUnselected) {
  auto wind = openvdb::Vec3SGrid::create(openvdb::Vec3f(1,0,0));
  wind->setName("wind");
  const auto input = write({sphere(),wind});
  EXPECT_THROW((void)vdb::WriteObjSurface(input, file("partial.obj")), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(file("partial.obj")));
  vdb::ImportOptions selected;
  selected.gridName = "surface";
  EXPECT_EQ(vdb::WriteObjSurface(input, file("selected.obj"), selected).gridCount, 1u);
  selected.gridName = "wind";
  EXPECT_THROW((void)vdb::WriteObjSurface(input, file("wind.obj"), selected), std::runtime_error);
  auto empty = openvdb::FloatGrid::create(0.0f);
  EXPECT_THROW((void)vdb::WriteObjSurface(write({empty}, "empty.vdb"), file("empty.obj")), std::runtime_error);
}

TEST_F(OpenVdbMeshExport, MixedPointGridsAreExcludedFromScalarOnlyExport) {
  const std::array<vdb::PointRecord,1> records{{{{0,0,0},{1,0,0},.5}}};
  vdb::ProcessingOptions pointOptions; pointOptions.gridName="points";
  (void)vdb::WritePoints(records,file("points.vdb"),pointOptions);
  openvdb::io::File pointsFile(file("points.vdb").string()); pointsFile.open(false);
  const auto points=pointsFile.readGrid("points"); pointsFile.close();
  const auto input=write({sphere(),points},"mixed.vdb");
  const auto exported=vdb::WriteObjSurface(input,file("scalar.obj"));
  EXPECT_EQ(exported.gridCount,1u); EXPECT_TRUE(exported.diagnostics.empty());
  const auto expected=vdb::WriteObjSurface(write({sphere()},"scalar.vdb"),file("expected.obj"));
  EXPECT_EQ(exported.triangleCount,expected.triangleCount);
  vdb::ImportOptions selected; selected.gridName="points";
  EXPECT_THROW((void)vdb::WriteObjSurface(input,file("points.obj"),selected),std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(file("points.obj")));
}

TEST_F(OpenVdbMeshExport, BudgetsInvalidOptionsAndOutputAliasesLeaveFilesUntouched) {
  const auto input = write({sphere()});
  const auto original = bytes(input);
  EXPECT_THROW((void)vdb::WriteObjSurface(input, input), std::invalid_argument);
  EXPECT_EQ(bytes(input), original);
  std::ofstream(file("existing.obj"), std::ios::binary) << "keep this output";
  EXPECT_THROW((void)vdb::WriteObjSurface(input, file("existing.obj")), std::invalid_argument);
  EXPECT_EQ(bytes(file("existing.obj")), "keep this output");
  for (const auto options : {vdb::ImportOptions{.maxActiveVoxels = 1},
                            vdb::ImportOptions{.maxInputBytes = 1},
                            vdb::ImportOptions{.maxTriangles = 1},
                            vdb::ImportOptions{.isoValue = NAN},
                            vdb::ImportOptions{.adaptivity = 2}}) {
    EXPECT_THROW((void)vdb::WriteObjSurface(input, file("bad.obj"), options), std::exception);
    EXPECT_FALSE(std::filesystem::exists(file("bad.obj")));
  }
  EXPECT_THROW((void)vdb::WriteObjSurface(input, file("absent/child.obj")), std::invalid_argument);
  EXPECT_EQ(bytes(input), original);
}

TEST_F(OpenVdbMeshExport, RejectsEveryExplicitPreviewModeBeforeLoading) {
  for (int mode=0;mode<3;++mode) {
    vdb::ImportOptions options;
    if (mode==0) options.vectorPreview.emplace();
    if (mode==1) options.slicePreview.emplace();
    if (mode==2) options.nativePointPreview.emplace();
    EXPECT_THROW((void)vdb::WriteObjSurface(file("missing.vdb"),file("preview.obj"),options),std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(file("preview.obj")));
  }
}
#endif
} // namespace
