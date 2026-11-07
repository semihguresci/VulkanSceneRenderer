#include "Container/geometry/OpenVdbLoader.h"
#include "Container/geometry/OpenVdbPoints.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/geometric.hpp>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#endif

namespace {

namespace vdb = container::geometry::vdb;

TEST(OpenVdbLoader, ReportsBuildAvailability) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  EXPECT_TRUE(vdb::IsAvailable());
#else
  EXPECT_FALSE(vdb::IsAvailable());
#endif
}

TEST(OpenVdbLoader, DisabledBuildExplainsHowToEnableImport) {
  if (vdb::IsAvailable())
    GTEST_SKIP() << "OpenVDB is enabled";
  try {
    (void)vdb::LoadFromFile("missing.vdb");
    FAIL() << "Disabled importer must throw";
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),
              std::string::npos);
    EXPECT_NE(std::string(error.what()).find("unavailable"), std::string::npos);
  }
}

TEST(OpenVdbLoader, DisabledExplicitPreviewsReportAvailabilityBeforeModesOptionsOrPaths) {
  if (vdb::IsAvailable())
    GTEST_SKIP() << "OpenVDB is enabled";
  for (unsigned modes = 1; modes < 8; ++modes) {
    vdb::ImportOptions options;
    if (modes & 1) options.vectorPreview.emplace();
    if (modes & 2) options.slicePreview.emplace();
    if (modes & 4) options.nativePointPreview.emplace();
    options.maxInputBytes = 0;
    try {
      (void)vdb::LoadFromFile("missing.vdb", 0, options);
      FAIL();
    } catch (const std::runtime_error &error) {
      EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
    }
  }
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB

class OpenVdbImportTest : public ::testing::Test {
protected:
  void SetUp() override {
    openvdb::initialize();
    static std::atomic<unsigned> sequence{0};
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    directory = std::filesystem::temp_directory_path() /
                ("container-openvdb-" + std::to_string(stamp) + "-" +
                 std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(directory);
  }

  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }

  std::filesystem::path write(const openvdb::GridPtrVec &grids,
                              const std::string &name = "fixture.vdb") {
    const auto path = directory / name;
    openvdb::io::File file(path.string());
    file.write(grids);
    file.close();
    return path;
  }

  template <typename Grid = openvdb::FloatGrid>
  typename Grid::Ptr sphere(const std::string &name = "surface") {
    using Value = typename Grid::ValueType;
    auto grid = Grid::create(Value(3));
    grid->setName(name);
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    grid->setTransform(openvdb::math::Transform::createLinearTransform(0.25));
    auto accessor = grid->getAccessor();
    for (int z = -8; z <= 8; ++z) {
      for (int y = -8; y <= 8; ++y) {
        for (int x = -8; x <= 8; ++x) {
          const double distance =
              std::sqrt(double(x * x + y * y + z * z)) - 4.0;
          const openvdb::Coord coord(x, y, z);
          if (std::abs(distance) < 3.0) {
            accessor.setValue(coord, Value(distance));
          } else if (distance < 0.0) {
            accessor.setValueOff(coord, Value(-3));
          }
        }
      }
    }
    return grid;
  }

  openvdb::FloatGrid::Ptr fog() {
    auto grid = openvdb::FloatGrid::create(0.0f);
    grid->setName("density");
    grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    grid->setTransform(openvdb::math::Transform::createLinearTransform(0.25));
    auto accessor = grid->getAccessor();
    for (int z = -6; z <= 6; ++z) {
      for (int y = -6; y <= 6; ++y) {
        for (int x = -6; x <= 6; ++x) {
          const double density =
              1.0 - std::sqrt(double(x * x + y * y + z * z)) / 6.0;
          if (density > 0.0) {
            accessor.setValue(openvdb::Coord(x, y, z), float(density));
          }
        }
      }
    }
    return grid;
  }

  static void
  expectFiniteSurface(const container::geometry::dotbim::Model &model) {
    ASSERT_FALSE(model.indices.empty());
    ASSERT_EQ(model.indices.size() % 3u, 0u);
    for (const auto &vertex : model.vertices) {
      for (int axis = 0; axis < 3; ++axis) {
        EXPECT_TRUE(std::isfinite(vertex.position[axis]));
        EXPECT_TRUE(std::isfinite(vertex.normal[axis]));
        EXPECT_TRUE(std::isfinite(vertex.tangent[axis]));
      }
      EXPECT_NEAR(glm::length(vertex.normal), 1.0f, 1e-5f);
      EXPECT_NEAR(glm::dot(glm::vec3(vertex.tangent), vertex.normal), 0.0f,
                  1e-5f);
    }
    for (size_t i = 0; i < model.indices.size(); i += 3u) {
      const auto &a = model.vertices.at(model.indices[i]);
      const auto &b = model.vertices.at(model.indices[i + 1]);
      const auto &c = model.vertices.at(model.indices[i + 2]);
      const auto geometricNormal = glm::normalize(
          glm::cross(b.position - a.position, c.position - a.position));
      EXPECT_GT(glm::dot(a.normal, geometricNormal), 0.999f);
    }
    for (const auto &range : model.meshRanges) {
      EXPECT_GT(range.boundsRadius, 0.0f);
      EXPECT_TRUE(std::isfinite(range.boundsRadius));
      EXPECT_LE(size_t(range.firstIndex) + range.indexCount,
                model.indices.size());
      for (size_t i = range.firstIndex;
           i < size_t(range.firstIndex) + range.indexCount; ++i) {
        const auto &point = model.vertices[model.indices[i]].position;
        EXPECT_LE(glm::distance(point, range.boundsCenter),
                  range.boundsRadius + 1e-5f);
      }
    }
    ASSERT_FALSE(model.materials.empty());
    EXPECT_TRUE(std::isfinite(model.materials[0].pbr.roughnessFactor));
    EXPECT_TRUE(std::isfinite(model.materials[0].pbr.attenuationDistance));
  }

  std::filesystem::path directory;
};

TEST_F(OpenVdbImportTest, ExtractsTransformedScaledLevelSetWithOutwardNormals) {
  auto grid = sphere();
  grid->transform().postTranslate(openvdb::Vec3d(2, -3, 4));
  const auto model = vdb::LoadFromFile(write({grid}), 2.0f);
  ASSERT_EQ(model.meshRanges.size(), 1u);
  ASSERT_EQ(model.elements.size(), 1u);
  expectFiniteSurface(model);
  EXPECT_NEAR(model.meshRanges[0].boundsCenter.x, 4.0f, 0.05f);
  EXPECT_NEAR(model.meshRanges[0].boundsCenter.y, -6.0f, 0.05f);
  EXPECT_NEAR(model.meshRanges[0].boundsCenter.z, 8.0f, 0.05f);
  EXPECT_NEAR(model.meshRanges[0].boundsRadius, 2.0f, 0.15f);
  EXPECT_EQ(model.elements[0].displayName, "surface");
  EXPECT_EQ(model.elements[0].meshId, model.meshRanges[0].meshId);
  EXPECT_EQ(model.elements[0].materialIndex, 0u);
  EXPECT_EQ(model.elements[0].transform, glm::mat4(1.0f));
  EXPECT_FLOAT_EQ(model.unitMetadata.effectiveImportScale, 2.0f);
  EXPECT_EQ(model.importReport.completeness,
            container::geometry::ImportCompleteness::Complete);
  EXPECT_EQ(model.importReport.sourceProductCount, 1u);
  EXPECT_EQ(model.importReport.importedProductCount, 1u);
  EXPECT_NE(model.importReport.summary().find("OpenVDB"), std::string::npos);
  for (const auto &vertex : model.vertices) {
    EXPECT_GT(glm::dot(vertex.normal, vertex.position - glm::vec3(4, -6, 8)),
              0.0f);
  }
}

TEST_F(OpenVdbImportTest, PreservesReflectedNonuniformGridTransformAndWinding) {
  auto grid = sphere();
  grid->transform().postScale(openvdb::Vec3d(-2.0, 1.0, 0.5));
  const auto model = vdb::LoadFromFile(write({grid}));
  expectFiniteSurface(model);
  for (const auto &vertex : model.vertices) {
    EXPECT_GT(glm::dot(vertex.normal, vertex.position), 0.0f);
  }
  glm::vec3 extent(0.0f);
  for (const auto &vertex : model.vertices)
    extent = glm::max(extent, glm::abs(vertex.position));
  EXPECT_NEAR(extent.x, 2.0f, 0.15f);
  EXPECT_NEAR(extent.y, 1.0f, 0.1f);
  EXPECT_NEAR(extent.z, 0.5f, 0.05f);
}

TEST_F(OpenVdbImportTest,
       ImportsDoubleAndFloatGridsAsSeparateSelectableMeshes) {
  auto second = sphere<openvdb::DoubleGrid>("second");
  second->transform().postTranslate(openvdb::Vec3d(3, 0, 0));
  const auto model = vdb::LoadFromFile(write({sphere(), second}));
  ASSERT_EQ(model.elements.size(), 2u);
  ASSERT_EQ(model.meshRanges.size(), 2u);
  EXPECT_NE(model.elements[0].meshId, model.elements[1].meshId);
  EXPECT_NE(model.elements[0].guid, model.elements[1].guid);
  EXPECT_EQ(model.importReport.importedProductCount, 2u);
  expectFiniteSurface(model);
}

TEST_F(OpenVdbImportTest, DuplicateDisplayNamesRetainSeparateGridSurfaces) {
  const auto path = write({sphere("duplicate"), sphere("duplicate")});
  const auto model = vdb::LoadFromFile(path);
  ASSERT_EQ(model.elements.size(), 2u);
  EXPECT_NE(model.elements[0].guid, model.elements[1].guid);
  EXPECT_NE(model.elements[0].sourceId, model.elements[1].sourceId);

  vdb::ImportOptions options{};
  options.gridName = "duplicate";
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1.0f, options),
               std::runtime_error);
  options.gridName = "descriptor:" + model.elements.front().sourceId;
  const auto selectedFirst = vdb::LoadFromFile(path, 1.0f, options);
  ASSERT_EQ(selectedFirst.elements.size(), 1u);
  EXPECT_EQ(selectedFirst.elements.front().sourceId, model.elements.front().sourceId);
  for (const auto &element : model.elements) {
    options.gridName = "descriptor:" + element.sourceId;
    EXPECT_EQ(vdb::LoadFromFile(path, 1.0f, options).elements.size(), 1u);
  }
}

TEST_F(OpenVdbImportTest,
       FogUsesHalfDensityByDefaultAndSupportsIsovalueOverride) {
  const auto path = write({fog()});
  const auto standard = vdb::LoadFromFile(path);
  vdb::ImportOptions options{};
  options.isoValue = 0.25;
  const auto larger = vdb::LoadFromFile(path, 1.0f, options);
  expectFiniteSurface(standard);
  EXPECT_NEAR(standard.meshRanges[0].boundsRadius, 0.75f, 0.12f);
  EXPECT_GT(larger.meshRanges[0].boundsRadius,
            standard.meshRanges[0].boundsRadius * 1.3f);
  for (const auto &vertex : standard.vertices) {
    EXPECT_GT(glm::dot(vertex.normal, vertex.position), 0.0f);
  }
}

TEST_F(OpenVdbImportTest, AdaptiveReflectedFogKeepsOutwardTriangleWinding) {
  auto grid = fog();
  grid->transform().postScale(openvdb::Vec3d(-2.0, 1.0, 0.5));
  grid->transform().postTranslate(openvdb::Vec3d(3, -2, 1));
  vdb::ImportOptions options{};
  options.adaptivity = 0.5;
  const auto model = vdb::LoadFromFile(write({grid}), 1.0f, options);
  expectFiniteSurface(model);
  for (const auto &vertex : model.vertices) {
    EXPECT_GT(glm::dot(vertex.normal, vertex.position - glm::vec3(3, -2, 1)),
              0.0f);
  }
}

TEST_F(OpenVdbImportTest, UnsupportedGridProducesStructuredPartialReport) {
  auto unsupported = openvdb::Int32Grid::create(0);
  unsupported->setName("labels");
  unsupported->getAccessor().setValue(openvdb::Coord(0), 1);
  const auto model = vdb::LoadFromFile(write({unsupported, sphere()}));
  EXPECT_EQ(model.importReport.sourceProductCount, 2u);
  EXPECT_EQ(model.importReport.importedProductCount, 1u);
  EXPECT_EQ(model.importReport.skippedProductCount, 1u);
  EXPECT_EQ(model.importReport.completeness,
            container::geometry::ImportCompleteness::Partial);
  ASSERT_EQ(model.importReport.diagnostics.size(), 1u);
  EXPECT_EQ(model.importReport.diagnostics[0].representationType,
            "OpenVDB.UnsupportedGridType");
  EXPECT_NE(model.importReport.diagnostics[0].reason.find("labels"),
            std::string::npos);
  EXPECT_EQ(model.importReport.representationWarnings.at(
                "OpenVDB.UnsupportedGridType"),
            1u);
}

TEST_F(OpenVdbImportTest, GridSelectionExcludesOtherGridsAndTheirWarnings) {
  auto unsupported = openvdb::Int32Grid::create(0);
  unsupported->setName("labels");
  const auto path = write({sphere(), fog(), unsupported});
  vdb::ImportOptions options{};
  options.gridName = "density";
  const auto model = vdb::LoadFromFile(path, 1.0f, options);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.elements[0].displayName, "density");
  EXPECT_EQ(model.importReport.sourceProductCount, 1u);
  EXPECT_EQ(model.importReport.skippedProductCount, 0u);
  EXPECT_EQ(model.importReport.representationWarnings.count(
                "OpenVDB.UnsupportedGridType"),
            0u);
  // The radial fog fixture crosses exact voxel isovalues and can produce
  // degenerate triangles. Preserve those diagnostics for the selected grid;
  // selection must only exclude diagnostics belonging to the other grids.
  for (const auto &diagnostic : model.importReport.diagnostics) {
    EXPECT_EQ(diagnostic.representationType, "OpenVDB.DegenerateTriangles")
        << diagnostic.reason;
    EXPECT_EQ(diagnostic.productGuid, model.elements[0].guid)
        << diagnostic.reason;
    EXPECT_NE(diagnostic.reason.find("density"), std::string::npos)
        << diagnostic.reason;
  }
  options.gridName = "missing";
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1.0f, options),
               std::runtime_error);
  options.gridName = "labels";
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1.0f, options),
               std::runtime_error);
}

TEST_F(OpenVdbImportTest, EnforcesVoxelAndAggregateTriangleBudgets) {
  const auto path = write({sphere()});
  vdb::ImportOptions options{};
  options.maxActiveVoxels = 1;
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1.0f, options),
               std::runtime_error);
  options = {};
  options.maxTriangles = 1;
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1.0f, options),
               std::runtime_error);

  const auto model = vdb::LoadFromFile(path);
  options.maxTriangles = model.indices.size() / 3u;
  EXPECT_NO_THROW((void)vdb::LoadFromFile(path, 1.0f, options));
  const auto pair = write({sphere(), sphere("second")}, "pair.vdb");
  EXPECT_THROW((void)vdb::LoadFromFile(pair, 1.0f, options),
               std::runtime_error);
}

TEST_F(OpenVdbImportTest, RejectsInvalidOptionsAndMalformedFiles) {
  const auto path = write({sphere()});
  for (const float scale : {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()}) {
    EXPECT_THROW((void)vdb::LoadFromFile(path, scale), std::invalid_argument);
  }
  vdb::ImportOptions options{};
  for (const double adaptivity :
       {-0.1, 1.1, std::numeric_limits<double>::quiet_NaN()}) {
    options.adaptivity = adaptivity;
    EXPECT_THROW((void)vdb::LoadFromFile(path, 1.0f, options),
                 std::invalid_argument);
  }
  options = {};
  options.isoValue = std::numeric_limits<double>::infinity();
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1.0f, options),
               std::invalid_argument);
  options = {};
  options.maxTriangles = 0;
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1.0f, options),
               std::invalid_argument);
  options = {};
  options.maxActiveVoxels = 0;
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1.0f, options),
               std::invalid_argument);
  EXPECT_THROW((void)vdb::LoadFromFile(directory / "missing.vdb"),
               std::runtime_error);
  std::ofstream(directory / "bad.vdb") << "this is not an OpenVDB archive";
  EXPECT_THROW((void)vdb::LoadFromFile(directory / "bad.vdb"),
               std::runtime_error);
  const auto empty = write({}, "empty.vdb");
  EXPECT_THROW((void)vdb::LoadFromFile(empty), std::runtime_error);
}

TEST_F(OpenVdbImportTest, EmptySurfaceIsReportedWhenOtherGridsImport) {
  auto empty = openvdb::FloatGrid::create(0.0f);
  empty->setName("empty");
  const auto model = vdb::LoadFromFile(write({sphere(), empty}));
  EXPECT_EQ(model.importReport.importedProductCount, 1u);
  EXPECT_EQ(model.importReport.skippedProductCount, 1u);
  EXPECT_EQ(
      model.importReport.representationWarnings.at("OpenVDB.EmptySurface"), 1u);
  vdb::ImportOptions options{};
  options.gridName = "empty";
  EXPECT_THROW(
      (void)vdb::LoadFromFile(write({empty}, "only-empty.vdb"), 1.0f, options),
      std::runtime_error);
}

TEST_F(OpenVdbImportTest, RejectsNonfiniteVoxelDataAndScaledPositions) {
  auto grid = sphere();
  grid->getAccessor().setValue(openvdb::Coord(4, 0, 0),
                               std::numeric_limits<float>::quiet_NaN());
  EXPECT_THROW((void)vdb::LoadFromFile(write({grid})), std::runtime_error);
  auto translated = sphere();
  translated->transform().postTranslate(openvdb::Vec3d(10, 0, 0));
  EXPECT_THROW((void)vdb::LoadFromFile(write({translated}, "overflow.vdb"),
                                       std::numeric_limits<float>::max()),
               std::runtime_error);
}

TEST_F(OpenVdbImportTest, RejectsNonfiniteInactiveInteriorValue) {
  auto grid = sphere();
  grid->getAccessor().setValueOff(openvdb::Coord(0),
                                  std::numeric_limits<float>::quiet_NaN());
  EXPECT_THROW((void)vdb::LoadFromFile(write({grid})), std::runtime_error);
}

TEST_F(OpenVdbImportTest, AdaptiveExtractionStaysWithinBudget) {
  const auto path = write({sphere()});
  const auto uniform = vdb::LoadFromFile(path);
  vdb::ImportOptions options{};
  options.adaptivity = 0.5;
  const auto adaptive = vdb::LoadFromFile(path, 1.0f, options);
  expectFiniteSurface(adaptive);
  EXPECT_LE(adaptive.indices.size(), uniform.indices.size());
}

TEST_F(OpenVdbImportTest, BoundsEncodedInputAndInactiveStoredTopology) {
  const auto path = write({sphere()});
  vdb::ImportOptions options;
  options.maxInputBytes = 16;
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1.0f, options), std::runtime_error);
  options.maxInputBytes = 0;
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1.0f, options), std::invalid_argument);

  auto sparse = sphere();
  for (int leaf = 0; leaf < 300; ++leaf)
    sparse->tree().setValueOff(openvdb::Coord(128 + leaf * 8, 0, 0), 0.25f);
  options = {};
  options.maxActiveVoxels = 100'000;
  ASSERT_LT(sparse->activeVoxelCount(), options.maxActiveVoxels);
  EXPECT_THROW((void)vdb::LoadFromFile(write({sparse}, "inactive-leaves.vdb"),
                                      1.0f, options), std::runtime_error);
}

TEST_F(OpenVdbImportTest, StandardPointFileLoadsRadiusAndVelocityMarkersThroughSceneRoute) {
  const auto path = directory / "points.vdb";
  const std::vector<vdb::PointRecord> points{{{-1,0,0},{1,0,0},.25},{{1,0,0},{-1,0,0},.5}};
  (void)vdb::WritePoints(points,path);
  const auto model = vdb::LoadFromFile(path,2);
  ASSERT_EQ(model.elements.size(),1u); ASSERT_EQ(model.meshRanges.size(),1u);
  EXPECT_EQ(model.vertices.size(),12u); EXPECT_EQ(model.indices.size(),48u);
  EXPECT_EQ(model.elements[0].type,"OpenVDB Points");
  EXPECT_EQ(model.elements[0].geometryKind,container::geometry::dotbim::GeometryKind::Mesh);
  EXPECT_TRUE(model.nativePointRanges.empty());
  EXPECT_EQ(model.importReport.importedProductCount,1u);
  EXPECT_EQ(model.importReport.completeness,container::geometry::ImportCompleteness::Complete);
  ASSERT_LT(model.elements[0].materialIndex,model.materials.size());
  EXPECT_EQ(model.materials[model.elements[0].materialIndex].pbr.baseColor,glm::vec4(1));
  EXPECT_EQ(model.vertices[0].position,glm::vec3(-1.5f,0,0));
  EXPECT_EQ(model.vertices[6].position,glm::vec3(3,0,0));
  EXPECT_EQ(model.vertices[0].color,glm::vec3(1,.5f,.5f));
  EXPECT_EQ(model.vertices[6].color,glm::vec3(0,.5f,.5f));
  vdb::ImportOptions options; options.includePoints=false;
  EXPECT_THROW((void)vdb::LoadFromFile(path,1,options),std::runtime_error);
}

TEST_F(OpenVdbImportTest, MixedScalarAndPointsKeepUniqueRangesMaterialsReportsAndSharedTriangleLimit) {
  const auto pointPath=directory/"points.vdb";
  (void)vdb::WritePoints(std::vector<vdb::PointRecord>{{{2,0,0},{1,0,0},.25}},pointPath);
  openvdb::io::File input(pointPath.string());input.open(false);
  auto points=input.readGrid("surface");input.close();points->setName("points");
  const auto path=write({sphere(),points},"mixed.vdb");
  vdb::ImportOptions scalarOptions;scalarOptions.gridName="surface";
  const auto scalar=vdb::LoadFromFile(path,1,scalarOptions);
  const auto model=vdb::LoadFromFile(path);
  ASSERT_EQ(model.elements.size(),2u);ASSERT_EQ(model.meshRanges.size(),2u);
  EXPECT_EQ(model.importReport.sourceProductCount,2u);
  EXPECT_EQ(model.importReport.importedProductCount,2u);
  EXPECT_EQ(model.importReport.skippedProductCount,0u);
  EXPECT_EQ(model.vertices.size(),scalar.vertices.size()+6);
  EXPECT_EQ(model.indices.size(),scalar.indices.size()+24);
  EXPECT_NE(model.elements[0].meshId,model.elements[1].meshId);
  EXPECT_NE(model.elements[0].materialIndex,model.elements[1].materialIndex);
  for(size_t index=0;index<model.meshRanges.size();++index) {
    EXPECT_EQ(model.meshRanges[index].meshId,model.elements[index].meshId);
    for(size_t i=model.meshRanges[index].firstIndex;i<model.meshRanges[index].firstIndex+model.meshRanges[index].indexCount;++i)
      EXPECT_LT(model.indices[i],model.vertices.size());
  }
  vdb::ImportOptions bounded;bounded.maxTriangles=scalar.indices.size()/3+7;
  EXPECT_THROW((void)vdb::LoadFromFile(path,1,bounded),std::runtime_error);
  bounded={};bounded.pointPreview.maxPoints=0;
  EXPECT_THROW((void)vdb::LoadFromFile(path,1,bounded),std::runtime_error);
}

TEST_F(OpenVdbImportTest, EmptyPointGridsAndCollidingDescriptorsRetainExplicitDiagnostics) {
  const auto pointPath=directory/"empty.vdb";
  (void)vdb::WritePoints(std::vector<vdb::PointRecord>{},pointPath);
  EXPECT_THROW((void)vdb::LoadFromFile(pointPath),std::runtime_error);
  openvdb::io::File input(pointPath.string());input.open(false);
  auto points=input.readGrid("surface");input.close();points->setName("points");
  const auto model=vdb::LoadFromFile(write({sphere(),points},"mixed-empty.vdb"));
  ASSERT_EQ(model.elements.size(),1u);ASSERT_EQ(model.importReport.diagnostics.size(),1u);
  EXPECT_EQ(model.importReport.skippedProductCount,1u);
  EXPECT_EQ(model.importReport.diagnostics[0].representationType,"OpenVDB.EmptyPoints");
  const auto literal=points->deepCopyGrid();literal->setName("points[0]");
  EXPECT_THROW((void)vdb::LoadFromFile(write({points,points->deepCopyGrid(),literal},"collision.vdb")),std::runtime_error);
}

TEST_F(OpenVdbImportTest, ExplicitVectorModeRoutesOnlySelectedFieldAndHonorsOuterCaps) {
  auto wind=openvdb::Vec3DGrid::create(openvdb::Vec3d(1,0,0)); wind->setName("wind");
  wind->setIsInWorldSpace(true); wind->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  const auto path=write({sphere(),wind},"vector.vdb");
  vdb::ImportOptions options; options.gridName="wind"; options.vectorPreview.emplace();
  auto& preview=*options.vectorPreview; preview.hasBounds=true;
  preview.boundsMin={-1,-1,-1}; preview.boundsMax={1,1,1}; preview.seedCounts={1,1,1};
  const auto glyph=vdb::LoadFromFile(path,1,options);
  ASSERT_EQ(glyph.elements.size(),1u); EXPECT_FALSE(glyph.vertices.empty());
  EXPECT_EQ(glyph.importReport.sourceProductCount,1u);
  EXPECT_EQ(glyph.vertices.front().color,glm::vec3(1,.5f,.5f));
  preview.mode=vdb::VectorPreviewMode::Streamlines; preview.bothDirections=false; preview.maxSteps=4;
  const auto lines=vdb::LoadFromFile(path,1,options);
  EXPECT_FALSE(lines.vertices.empty()); EXPECT_NE(lines.indices.size(),glyph.indices.size());
  options.maxTriangles=1;
  EXPECT_THROW((void)vdb::LoadFromFile(path,1,options),std::exception);
  options.maxTriangles=1000; preview.hasBounds=false;
  EXPECT_THROW((void)vdb::LoadFromFile(path,1,options),std::invalid_argument);
}

TEST_F(OpenVdbImportTest, ExplicitSliceRoutesRawSelectedValuesAndMergesOuterCaps) {
  auto field = openvdb::DoubleGrid::create(7);
  field->setName("field");
  for (int x = 0; x < 3; ++x) field->tree().setValue({x, 0, 0}, 7);
  const auto path = write({sphere(), field}, "slice.vdb");
  vdb::ImportOptions options;
  options.gridName = "field";
  options.includePoints = false;
  options.slicePreview.emplace();
  auto &slice = *options.slicePreview;
  slice.sampleCounts = {3, 3};
  slice.colorMap = vdb::SliceColorMap::Gray;
  slice.colorMinimum = 0;
  slice.colorMaximum = 14;
  slice.opacity = .35f;
  const auto model = vdb::LoadFromFile(path, 2, options);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.elements[0].type, "OpenVDB Field Slice");
  EXPECT_EQ(model.elements[0].sourceId, "field");
  EXPECT_EQ(model.vertices.size(), 9u);
  EXPECT_EQ(model.indices.size(), 24u);
  EXPECT_EQ(model.vertices.front().position, glm::vec3(-2, -2, 0));
  for (const auto &vertex : model.vertices) EXPECT_EQ(vertex.color, glm::vec3(.5f));
  EXPECT_EQ(model.materials[0].pbr.alphaMode, container::material::AlphaMode::Blend);
  EXPECT_FLOAT_EQ(model.materials[0].pbr.baseColor.a, .35f);
  EXPECT_EQ(model.importReport.sourceProductCount, 1u);
  const auto baseline = options;
  options.maxTriangles = 7;
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1, options), std::invalid_argument);
  options = baseline;
  options.slicePreview->maxTriangles = 7;
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1, options), std::invalid_argument);
  options = baseline;
  options.maxActiveVoxels = 2;
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1, options), std::invalid_argument);
  options = baseline;
  options.slicePreview->maxActiveVoxels = 2;
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1, options), std::invalid_argument);
  options = baseline;
  options.maxInputBytes = 1;
  EXPECT_THROW((void)vdb::LoadFromFile(path, 1, options), std::invalid_argument);
}

TEST_F(OpenVdbImportTest, ExplicitNativePointAttributesUseSeparateCapsAndPhysicalMarkers) {
  const auto raw = directory / "native-raw.vdb", source = directory / "native.vdb";
  vdb::ProcessingOptions processing;
  processing.gridName = "native";
  const std::vector<vdb::PointRecord> points{
      {{-.5, 0, 0}, {}, .01}, {{.5, 0, 0}, {}, .01}};
  (void)vdb::WritePoints(points, raw, processing);
  const std::array<vdb::PointAttributeEdit, 2> edits{{
      {vdb::PointEditOperation::Add, "width", {}, std::vector<float>{.1f, .2f}},
      {vdb::PointEditOperation::Add, "id", {},
       std::vector<int64_t>{INT64_MAX - 1, INT64_MAX}}}};
  const std::array<vdb::PointGroupEdit, 1> groups{{
      {vdb::PointEditOperation::Add, "keep", {}, {0, 1}}}};
  (void)vdb::WritePointAttributeEdits(raw, source, edits, groups, {}, processing);
  vdb::ImportOptions options;
  options.gridName = "native";
  options.includePoints = false;
  options.pointPreview.maxPoints = 1; // Standard-route state does not constrain this mode.
  options.nativePointPreview.emplace();
  auto &preview = *options.nativePointPreview;
  preview.radiusAttribute = "width";
  preview.colorMode = vdb::NativePointColor::ScalarRamp;
  preview.colorAttribute = "id";
  preview.opacity = .25f;
  const auto model = vdb::LoadFromFile(source, 2, options);
  ASSERT_EQ(model.elements.size(), 1u);
  EXPECT_EQ(model.elements[0].type, "OpenVDB Native Points");
  EXPECT_EQ(model.vertices.size(), 12u);
  EXPECT_EQ(model.indices.size(), 48u);
  EXPECT_NEAR(model.vertices[0].position.x, -.8, 1e-7);
  EXPECT_NEAR(model.vertices[6].position.x, 1.4, 1e-7);
  EXPECT_EQ(model.vertices[0].color, glm::vec3(.1f, .25f, 1));
  EXPECT_EQ(model.vertices[6].color, glm::vec3(1, .2f, .05f));
  EXPECT_EQ(model.materials[0].pbr.alphaMode, container::material::AlphaMode::Blend);
  EXPECT_FLOAT_EQ(model.materials[0].pbr.opacityFactor, .25f);
  EXPECT_EQ(model.importReport.sourceProductCount, 1u);
  const auto baseline = options;
  preview.groups.anyOf = {"keep"};
  const auto selected = vdb::LoadFromFile(source, 2, options);
  ASSERT_EQ(selected.vertices.size(), 6u);
  EXPECT_NEAR(selected.vertices[0].position.x, 1.4, 1e-7);
  options = baseline;
  options.nativePointLimits.maxPoints = 1;
  EXPECT_THROW((void)vdb::LoadFromFile(source, 1, options), std::invalid_argument);
  options = baseline;
  options.maxTriangles = 15;
  EXPECT_THROW((void)vdb::LoadFromFile(source, 1, options), std::invalid_argument);
  options = baseline;
  options.maxActiveVoxels = 1;
  EXPECT_THROW((void)vdb::LoadFromFile(source, 1, options), std::invalid_argument);
  options = baseline;
  options.maxInputBytes = 1;
  EXPECT_THROW((void)vdb::LoadFromFile(source, 1, options), std::invalid_argument);
}

TEST_F(OpenVdbImportTest, ExplicitPreviewModesAreMutuallyExclusiveBeforeFilesystemAccess) {
  for (unsigned modes : {3u, 5u, 6u, 7u}) {
    vdb::ImportOptions options;
    if (modes & 1) options.vectorPreview.emplace();
    if (modes & 2) options.slicePreview.emplace();
    if (modes & 4) options.nativePointPreview.emplace();
    try {
      (void)vdb::LoadFromFile(directory / "missing.vdb", 1, options);
      FAIL();
    } catch (const std::invalid_argument &error) {
      EXPECT_NE(std::string(error.what()).find("mutually exclusive"), std::string::npos);
    }
  }
}

#else

TEST(OpenVdbLoader, ProceduralSurfaceFixtures) {
  GTEST_SKIP()
      << "Enable ENABLE_OPENVDB to run procedural .vdb extraction tests";
}

#endif

} // namespace
