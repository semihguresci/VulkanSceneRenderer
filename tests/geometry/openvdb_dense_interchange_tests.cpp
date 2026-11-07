#include "Container/geometry/OpenVdbDenseInterchange.h"
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/Stream.h>
#include <openvdb/openvdb.h>
#endif

namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbDenseInterchangeAvailability, DisabledPrecedesPathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::ProcessingOptions p;
  p.maxInputBytes = 0;
  vdb::DenseArrayVolume v;
  for (int operation = 0; operation < 4; ++operation)
    try {
      if (operation == 0)
        (void)vdb::ReadDenseVdb("missing", {}, {}, p);
      if (operation == 1)
        (void)vdb::WriteDenseVdb("", v, p);
      if (operation == 2)
        (void)vdb::ReadDenseArrayFile("missing", p);
      if (operation == 3)
        vdb::WriteDenseArrayFile("", v, p);
      FAIL();
    } catch (const std::exception &e) {
      EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"), std::string::npos);
    }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbDenseInterchange : public ::testing::Test {
protected:
  std::filesystem::path directory;
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{};
    for (int i = 0; i < 100; ++i) {
      directory = std::filesystem::temp_directory_path() /
                  ("container-dense-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                   "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(directory))
        return;
    }
    FAIL();
  }
  void TearDown() override {
    std::error_code error;
    for (const auto &e : std::filesystem::directory_iterator(directory))
      if (e.is_regular_file())
        std::filesystem::remove(e.path(), error);
    std::filesystem::remove(directory, error);
  }
  auto path(const std::string &n) const { return directory / n; }
  vdb::DenseArrayVolume volume() {
    vdb::DenseArrayVolume v;
    v.region = {{-2, 3, -1}, {3, 2, 2}};
    std::vector<float> values;
    for (int i = 0; i < 12; ++i)
      values.push_back(float(i) + .5f);
    v.values = values;
    v.background = -2.5f;
    v.active = {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0};
    v.gridName = "field";
    return v;
  }
  void write(const std::string &n, const openvdb::GridPtrVec &grids) {
    openvdb::io::File file(path(n).string());
    file.setCompression(openvdb::io::COMPRESS_ZIP);
    file.write(grids);
    file.close();
  }
  auto bytes(const std::string &n) {
    std::ifstream f(path(n), std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(f), {});
  }
  void bytes(const std::string &n, const std::vector<char> &b) {
    std::ofstream f(path(n), std::ios::binary);
    f.write(b.data(), std::streamsize(b.size()));
  }
  template <class T> void exact(const T &a, const T &b) {
    if constexpr (std::is_same_v<T, float>)
      EXPECT_EQ(std::bit_cast<uint32_t>(a), std::bit_cast<uint32_t>(b));
    else if constexpr (std::is_same_v<T, double>)
      EXPECT_EQ(std::bit_cast<uint64_t>(a), std::bit_cast<uint64_t>(b));
    else
      for (int k = 0; k < 3; ++k)
        exact(a[k], b[k]);
  }
  void same(const vdb::DenseArrayVolume &a, const vdb::DenseArrayVolume &b) {
    EXPECT_EQ(a.region.origin, b.region.origin);
    EXPECT_EQ(a.region.dimensions, b.region.dimensions);
    EXPECT_EQ(a.linear, b.linear);
    EXPECT_EQ(a.translation, b.translation);
    EXPECT_EQ(a.gridName, b.gridName);
    EXPECT_EQ(a.gridClass, b.gridClass);
    EXPECT_EQ(a.vectorType, b.vectorType);
    EXPECT_EQ(a.worldSpace, b.worldSpace);
    EXPECT_EQ(a.active, b.active);
    ASSERT_EQ(a.values.index(), b.values.index());
    std::visit(
        [&](const auto &values) {
          using T = typename std::decay_t<decltype(values)>::value_type;
          const auto &other = std::get<std::vector<T>>(b.values);
          ASSERT_EQ(values.size(), other.size());
          for (size_t i = 0; i < values.size(); ++i)
            exact(values[i], other[i]);
          exact(std::get<T>(a.background), std::get<T>(b.background));
        },
        a.values);
  }
};
TEST_F(OpenVdbDenseInterchange, UnicodeFilenameRoundTripUsesNativePathSnapshot) {
  const auto input = volume();
  const auto native = directory / std::filesystem::path(u8"\u5165\u529b-\u00e9.vdb");
  const auto binary = directory / std::filesystem::path(u8"\u51fa\u529b-\u00e9.cvarr");
  (void)vdb::WriteDenseVdb(native, input);
  const auto originalSize = std::filesystem::file_size(native);
  const auto loaded = vdb::ReadDenseVdb(native, input.region);
  same(input, loaded);
  vdb::WriteDenseArrayFile(binary, loaded);
  same(loaded, vdb::ReadDenseArrayFile(binary));
  EXPECT_EQ(std::filesystem::file_size(native), originalSize);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),
                          std::filesystem::directory_iterator()), 2);
}
TEST_F(OpenVdbDenseInterchange,
       AllFourNativeTypesKeepMasksInactiveValuesAndBitsThroughBothFormats) {
  for (int type = 0; type < 4; ++type) {
    SCOPED_TRACE(type);
    auto v = volume();
    if (type == 0) {
      auto &values = std::get<std::vector<float>>(v.values);
      values[0] = -0.f;
      values[1] = -0.f;
      values[2] = std::numeric_limits<float>::denorm_min();
      v.background = 0.f;
    }
    if (type == 1) {
      std::vector<double> values(12, 1 + 0x1p-40);
      values[0] = -0.;
      values[1] = -0.;
      values[2] = std::numeric_limits<double>::denorm_min();
      v.values = values;
      v.background = 0.;
    }
    if (type == 2) {
      std::vector<std::array<float, 3>> values(12, {1, 2, 3});
      values[0] = {-0.f, 0.f, std::numeric_limits<float>::denorm_min()};
      values[1] = {0.f, -0.f, 0.f};
      v.values = values;
      v.background = std::array<float, 3>{0, 0, 0};
      v.vectorType = vdb::DenseVectorType::Covariant;
      v.worldSpace = false;
    }
    if (type == 3) {
      std::vector<std::array<double, 3>> values(12, {1 + 0x1p-40, 2, 3});
      values[0] = {-0., 0., std::numeric_limits<double>::denorm_min()};
      values[1] = {0., -0., 0.};
      v.values = values;
      v.background = std::array<double, 3>{0, 0, 0};
      v.vectorType = vdb::DenseVectorType::ContravariantAbsolute;
    }
    const auto prefix = std::to_string(type);
    vdb::WriteDenseArrayFile(path(prefix + ".array"), v);
    auto array = vdb::ReadDenseArrayFile(path(prefix + ".array"));
    same(v, array);
    const auto result = vdb::WriteDenseVdb(path(prefix + ".vdb"), array);
    EXPECT_EQ(result.activeVoxelCount, 6u);
    auto imported = vdb::ReadDenseVdb(path(prefix + ".vdb"), v.region);
    same(v, imported);
  }
}
TEST_F(OpenVdbDenseInterchange, XFastestCoordinatesAndExplicitShearedReflectedAffineAreCorrect) {
  auto v = volume();
  v.linear = {0, 2, .25, -1, 0, 0, 0, 0, 3};
  v.translation = {4, -2, 7};
  (void)vdb::WriteDenseVdb(path("field.vdb"), v);
  openvdb::io::File file(path("field.vdb").string());
  file.open(false);
  auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("field"));
  file.close();
  EXPECT_FLOAT_EQ(grid->tree().getValue({-2, 3, -1}), .5f);
  EXPECT_FLOAT_EQ(grid->tree().getValue({0, 3, -1}), 2.5f);
  EXPECT_FLOAT_EQ(grid->tree().getValue({-2, 4, -1}), 3.5f);
  EXPECT_FLOAT_EQ(grid->tree().getValue({-2, 3, 0}), 6.5f);
  EXPECT_EQ(grid->indexToWorld(openvdb::Vec3d(-2, 3, -1)), openvdb::Vec3d(9.75, 0, 4));
  same(v, vdb::ReadDenseVdb(path("field.vdb"), v.region));
}
TEST_F(OpenVdbDenseInterchange, WhollyInactiveSignedZerosMaterializeTheirOwnLeaf) {
  for (int type = 0; type < 4; ++type)
    for (bool negativeBackground : {false, true}) {
      SCOPED_TRACE(type);
      SCOPED_TRACE(negativeBackground);
      vdb::DenseArrayVolume v;
      v.region = {{-8, 8, 16}, {1, 1, 1}};
      const float f = negativeBackground ? -0.f : 0.f;
      const double d = negativeBackground ? -0. : 0.;
      if (type == 0) {
        v.values = std::vector<float>{-f};
        v.background = f;
      }
      if (type == 1) {
        v.values = std::vector<double>{-d};
        v.background = d;
      }
      if (type == 2) {
        v.values = std::vector<std::array<float, 3>>{{-f, f, -f}};
        v.background = std::array<float, 3>{f, -f, f};
      }
      if (type == 3) {
        v.values = std::vector<std::array<double, 3>>{{-d, d, -d}};
        v.background = std::array<double, 3>{d, -d, d};
      }
      const auto name =
          std::to_string(type) + (negativeBackground ? "negative.vdb" : "positive.vdb");
      EXPECT_EQ(vdb::WriteDenseVdb(path(name), v).activeVoxelCount, 0u);
      same(v, vdb::ReadDenseVdb(path(name), v.region));
    }
}
TEST_F(OpenVdbDenseInterchange, ProductTranslationCancellationCannotHideWorldPrecisionLoss) {
  vdb::DenseArrayVolume v;
  v.region.origin = {1000000000, 0, 0};
  v.linear = {std::nextafter(1e6, 0.), 0, 0, 0, 1e-6, 0, 0, 0, 1};
  v.translation = {-1e15, 0, 0};
  EXPECT_THROW(vdb::WriteDenseArrayFile(path("bad.array"), v), std::exception);
  EXPECT_THROW((void)vdb::WriteDenseVdb(path("bad.vdb"), v), std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("bad.array")));
  EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
}
TEST_F(OpenVdbDenseInterchange, ConstantBackgroundExportUsesOnlyRequestedRegion) {
  auto grid = openvdb::DoubleGrid::create(7.25);
  grid->setName("constant");
  write("source.vdb", {grid});
  vdb::DenseRegion region{{-100, 7, 1}, {2, 3, 1}};
  auto v = vdb::ReadDenseVdb(path("source.vdb"), region);
  EXPECT_EQ(v.values.index(), 1u);
  EXPECT_EQ(std::get<std::vector<double>>(v.values), std::vector<double>(6, 7.25));
  EXPECT_EQ(v.active, std::vector<uint8_t>(6, 0));
  EXPECT_EQ(v.diagnostics.activeVoxelsOutside, 0u);
  EXPECT_FALSE(v.diagnostics.storedNonBackgroundOutside);
}
TEST_F(OpenVdbDenseInterchange,
       ExportReportsCroppedActivityInactiveValuesAndEveryCustomMetadataKey) {
  auto grid = openvdb::FloatGrid::create(0);
  grid->setName("source");
  grid->tree().setValue({-1, 0, 0}, 1);
  grid->tree().setValue({3, 0, 0}, 2);
  grid->tree().setValueOff({4, 0, 0}, 7);
  grid->insertMeta("artist", openvdb::StringMetadata("Alice"));
  grid->insertMeta("arbitrary", openvdb::DoubleMetadata(2));
  write("source.vdb", {grid});
  auto v = vdb::ReadDenseVdb(path("source.vdb"), {{-1, 0, 0}, {3, 1, 1}});
  EXPECT_EQ(v.diagnostics.activeVoxelsOutside, 1u);
  EXPECT_TRUE(v.diagnostics.storedNonBackgroundOutside);
  EXPECT_NE(std::find(v.diagnostics.omittedMetadata.begin(), v.diagnostics.omittedMetadata.end(),
                      "artist"),
            v.diagnostics.omittedMetadata.end());
  EXPECT_NE(std::find(v.diagnostics.omittedMetadata.begin(), v.diagnostics.omittedMetadata.end(),
                      "arbitrary"),
            v.diagnostics.omittedMetadata.end());
  vdb::WriteDenseArrayFile(path("crop.array"), v);
  auto again = vdb::ReadDenseArrayFile(path("crop.array"));
  EXPECT_EQ(again.diagnostics.activeVoxelsOutside, 1u);
  EXPECT_TRUE(again.diagnostics.storedNonBackgroundOutside);
  EXPECT_EQ(again.diagnostics.omittedMetadata, v.diagnostics.omittedMetadata);
}
TEST_F(OpenVdbDenseInterchange,
       BinarySchemaIsLittleEndianAndRejectsCorruptHeadersLengthsAndTrailingData) {
  auto v = volume();
  vdb::WriteDenseArrayFile(path("good.array"), v);
  const auto original = bytes("good.array");
  ASSERT_GT(original.size(), 210u);
  EXPECT_EQ(std::string(original.begin(), original.begin() + 7), "CVDBARR");
  EXPECT_EQ(uint8_t(original[8]), 1u);
  EXPECT_EQ(uint8_t(original[12]), 0u);
  EXPECT_EQ(uint8_t(original[32]), 254u);
  EXPECT_EQ(uint8_t(original[35]), 255u);
  for (size_t offset :
       {size_t(0), size_t(8), size_t(12), size_t(16), size_t(20), size_t(24), size_t(28),
        size_t(44), size_t(156), size_t(160), size_t(180), size_t(188), size_t(196)}) {
    SCOPED_TRACE(offset);
    auto corrupt = original;
    corrupt[offset] = char(255);
    bytes("bad.array", corrupt);
    EXPECT_THROW((void)vdb::ReadDenseArrayFile(path("bad.array")), std::exception);
  }
  auto shortFile = original;
  shortFile.resize(shortFile.size() - 1);
  bytes("short.array", shortFile);
  EXPECT_THROW((void)vdb::ReadDenseArrayFile(path("short.array")), std::exception);
  auto longFile = original;
  longFile.push_back('x');
  bytes("long.array", longFile);
  EXPECT_THROW((void)vdb::ReadDenseArrayFile(path("long.array")), std::exception);
}
TEST_F(OpenVdbDenseInterchange, InvalidValuesMasksMapsAndBudgetsLeaveNoOutput) {
  auto v = volume();
  auto bad = v;
  bad.active[0] = 2;
  EXPECT_THROW((void)vdb::WriteDenseVdb(path("out.vdb"), bad), std::exception);
  bad = v;
  bad.background = 0.;
  EXPECT_THROW(vdb::WriteDenseArrayFile(path("out.array"), bad), std::exception);
  bad = v;
  std::get<std::vector<float>>(bad.values)[0] = NAN;
  EXPECT_THROW((void)vdb::WriteDenseVdb(path("out.vdb"), bad), std::exception);
  bad = v;
  bad.linear[4] = 0;
  EXPECT_THROW(vdb::WriteDenseArrayFile(path("out.array"), bad), std::exception);
  bad = v;
  bad.translation[0] = 1e15;
  EXPECT_THROW((void)vdb::WriteDenseVdb(path("out.vdb"), bad), std::exception);
  bad = v;
  bad.region.origin[0] = INT32_MAX;
  EXPECT_THROW((void)vdb::WriteDenseVdb(path("out.vdb"), bad), std::exception);
  vdb::ProcessingOptions p;
  p.maxVoxelCount = 1;
  EXPECT_THROW((void)vdb::WriteDenseVdb(path("out.vdb"), v, p), std::exception);
  p = {};
  p.maxInputBytes = 1024;
  EXPECT_THROW(vdb::WriteDenseArrayFile(path("out.array"), v, p), std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  EXPECT_FALSE(std::filesystem::exists(path("out.array")));
}
TEST_F(OpenVdbDenseInterchange, ExclusiveOutputsAndSourceBytesAreProtected) {
  auto v = volume();
  vdb::WriteDenseArrayFile(path("source.array"), v);
  auto original = bytes("source.array");
  EXPECT_THROW(vdb::WriteDenseArrayFile(path("source.array"), v), std::exception);
  EXPECT_EQ(bytes("source.array"), original);
  (void)vdb::WriteDenseVdb(path("source.vdb"), v);
  original = bytes("source.vdb");
  EXPECT_THROW((void)vdb::WriteDenseVdb(path("source.vdb"), v), std::exception);
  auto array = vdb::ReadDenseVdb(path("source.vdb"), v.region);
  EXPECT_EQ(bytes("source.vdb"), original);
  same(v, array);
}
TEST_F(OpenVdbDenseInterchange, SelectorsRejectAmbiguityAndCatalogCollisionsBeforeReadingValues) {
  auto a = openvdb::FloatGrid::create(1), b = openvdb::FloatGrid::create(2),
       c = openvdb::FloatGrid::create(3);
  a->setName("field");
  b->setName("field");
  c->setName("field[0]");
  write("ambiguous.vdb", {a, b});
  EXPECT_THROW((void)vdb::ReadDenseVdb(path("ambiguous.vdb"), {}, "field"), std::exception);
  auto selected = vdb::ReadDenseVdb(path("ambiguous.vdb"), {}, "descriptor:field[1]");
  EXPECT_FLOAT_EQ(std::get<std::vector<float>>(selected.values)[0], 2);
  write("collision.vdb", {a, b, c});
  EXPECT_THROW((void)vdb::ReadDenseVdb(path("collision.vdb"), {}), std::exception);
  {
    std::ofstream out(path("stream.vdb"), std::ios::binary);
    openvdb::io::Stream stream(out);
    stream.write(openvdb::GridPtrVec{a});
  }
  EXPECT_THROW((void)vdb::ReadDenseVdb(path("stream.vdb"), {}), std::exception);
}
TEST_F(OpenVdbDenseInterchange, ExactMatricesRetainSubToleranceShearAndDistantNonuniformScales) {
  for(const bool shear:{false,true}) {
    vdb::DenseArrayVolume v; v.values=std::vector<double>{1.125}; v.background=0.; v.active={1};
    v.translation={1e8,0,0};
    if(shear) v.linear={1.,std::ldexp(1.,-60),0.,0.,1.,0.,0.,0.,1.};
    else v.linear={.1,0.,0.,0.,.2,0.,0.,0.,.4};
    const auto name=shear ? "shear.vdb" : "scale.vdb";
    (void)vdb::WriteDenseVdb(path(name),v);
    const auto read=vdb::ReadDenseVdb(path(name),v.region);
    EXPECT_EQ(read.linear,v.linear); EXPECT_EQ(read.translation,v.translation);
    EXPECT_EQ(std::get<std::vector<double>>(read.values)[0],1.125);
  }
}
#endif
} // namespace
