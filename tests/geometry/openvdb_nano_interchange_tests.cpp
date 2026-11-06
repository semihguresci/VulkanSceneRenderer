#include "Container/geometry/OpenVdbNanoInterchange.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <gtest/gtest.h>

#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
#include "Container/geometry/detail/OpenVdbNanoCompatibility.h"
#include <nanovdb/io/IO.h>
#include <nanovdb/tools/CreatePrimitives.h>
#include <nanovdb/tools/GridValidator.h>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#endif
#endif

namespace {
namespace vdb = container::geometry::vdb;
TEST(OpenVdbNanoInterchangeAvailability,
     NativeReaderAvailabilityPrecedesInputs) {
  const auto capabilities = vdb::NanoVdbInterchangeCapabilities();
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
  EXPECT_TRUE(capabilities.nativeIo);
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  EXPECT_FALSE(capabilities.openVdbConversion);
#endif
#else
  EXPECT_FALSE(capabilities.nativeIo);
  vdb::NanoVdbLimits limits;
  limits.maxInputBytes = 0;
  for (int operation = 0; operation < 7; ++operation) {
    try {
      if (operation == 0)
        (void)vdb::InspectNanoVdbFile("missing", limits);
      if (operation == 1)
        (void)vdb::ReadNanoVdbFile("missing", {}, limits);
      if (operation == 2)
        (void)vdb::ValidateNanoVdbFile("missing", {}, limits);
      if (operation == 3)
        (void)vdb::WriteNanoVdbFile({}, "missing", {}, limits);
      if (operation == 4)
        (void)vdb::RewriteNanoVdbChecksums("missing", "missing",
                                           vdb::NanoVdbChecksum::Full, limits);
      if (operation == 5)
        (void)vdb::WriteNanoVdbFromVdb("missing", "missing", {}, {}, limits);
      if (operation == 6)
        (void)vdb::WriteVdbFromNanoVdb("missing", "missing", {}, false, limits);
      FAIL();
    } catch (const std::runtime_error &error) {
      EXPECT_NE(std::string(error.what()).find("ENABLE_NANOVDB"),
                std::string::npos);
    }
  }
#endif
}

#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
namespace nv = nanovdb;
class OpenVdbNanoInterchange : public ::testing::Test {
protected:
  std::filesystem::path directory;
  void SetUp() override {
    static std::atomic_uint64_t serial{};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-nano-test-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
  }
  void TearDown() override {
    std::error_code ignored;
    if (std::filesystem::is_symlink(directory, ignored) || ignored)
      return;
    const auto resolved = std::filesystem::weakly_canonical(directory, ignored);
    if (ignored)
      return;
    const auto parent = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path(), ignored);
    if (!ignored && resolved.parent_path() == parent &&
        resolved.filename() == directory.filename() &&
        resolved.filename().string().starts_with("container-nano-test-"))
      std::filesystem::remove_all(resolved, ignored);
  }
  std::filesystem::path path(const char *name) const {
    return directory / name;
  }
  template <class T = float>
  auto sphere(const std::string &name = "sphere",
              nv::CheckMode checksum = nv::CheckMode::Full) {
    return nv::tools::createLevelSetSphere<T>(
        3.0, nv::Vec3d(0), 1.0, 2.0, nv::Vec3d(0), name,
        nv::tools::StatsMode::All, checksum);
  }
  template <class Buffer>
  void segmented(const std::filesystem::path &output,
                 const nv::GridHandle<Buffer> &grid) {
    std::ofstream stream(output, std::ios::binary);
    nv::io::writeGrid(stream, grid, nv::io::Codec::NONE);
    ASSERT_TRUE(stream.good());
  }
  template <class Buffer>
  void raw(const char *name, const nv::GridHandle<Buffer> &grid) {
    std::ofstream stream(path(name), std::ios::binary);
    stream.write(static_cast<const char *>(grid.data()),
                 std::streamsize(grid.size()));
    ASSERT_TRUE(stream.good());
  }
  template <class T>
  void patch(const char *name, uint64_t offset, const T &value) {
    std::fstream stream(path(name),
                        std::ios::binary | std::ios::in | std::ios::out);
    stream.seekp(std::streamoff(offset));
    stream.write(reinterpret_cast<const char *>(&value), sizeof(value));
    ASSERT_TRUE(stream.good());
  }
};

TEST_F(OpenVdbNanoInterchange,
       IndependentFloatAndDoubleRawAndSegmentedArchivesValidate) {
  auto f = sphere<float>();
  auto d = sphere<double>("double");
  raw("float-raw.nvdb", f);
  segmented(path("float.nvdb"), f);
  segmented(path("double.nvdb"), d);
  for (const char *name : {"float-raw.nvdb", "float.nvdb", "double.nvdb"}) {
    SCOPED_TRACE(name);
    const auto inspection = vdb::InspectNanoVdbFile(path(name));
    ASSERT_EQ(inspection.grids.size(), 1u);
    EXPECT_TRUE(inspection.grids[0].supported);
    EXPECT_FALSE(inspection.grids[0].structureValidated);
    EXPECT_EQ(inspection.grids[0].checksumModeKnown, inspection.raw);
    const auto archive = vdb::ReadNanoVdbFile(path(name));
    ASSERT_EQ(archive.info().grids.size(), 1u);
    EXPECT_TRUE(archive.info().grids[0].structureValidated);
    EXPECT_TRUE(archive.info().grids[0].checksumVerified);
    EXPECT_EQ(archive.info().grids[0].activeVoxels,
              inspection.grids[0].activeVoxels);
    const auto retained = archive;
    EXPECT_EQ(retained.info().nativeBytes, archive.info().nativeBytes);
  }
}

TEST_F(OpenVdbNanoInterchange, NativeUnicodePathsAndLongNamesRemainExact) {
  const std::string name(400, 'x');
  const auto file =
      directory / std::filesystem::path(u8"\u539f\u751f-\u00e9.nvdb");
  auto grid = sphere(name);
  segmented(file, grid);
  const auto info = vdb::ValidateNanoVdbFile(file);
  ASSERT_EQ(info.grids.size(), 1u);
  EXPECT_EQ(info.grids[0].name, name);
}

TEST_F(OpenVdbNanoInterchange, SegmentedLongNamesAreBoundedBeforeStringTraversal) {
  auto handle=sphere(std::string(400,'n'));
  segmented(path("broken-name.nvdb"), handle);
  const auto &header=*handle.gridData();
  const auto *base=reinterpret_cast<const uint8_t *>(&header);
  for (uint32_t index=0;index<header.mBlindMetadataCount;++index) {
    const auto &b=*reinterpret_cast<const nv::GridBlindMetaData *>(
        base+header.mBlindMetadataOffset+index*sizeof(nv::GridBlindMetaData));
    if (b.mDataClass!=nv::GridBlindDataClass::GridName) continue;
    const auto fileOffset=sizeof(nv::io::FileHeader)+sizeof(nv::io::FileMetaData)+401;
    const auto payload=uint64_t(reinterpret_cast<const uint8_t *>(&b)-base)+uint64_t(b.mDataOffset);
    patch("broken-name.nvdb",fileOffset+payload+b.mValueCount-1,'x');
  }
  EXPECT_EQ(vdb::InspectNanoVdbFile(path("broken-name.nvdb")).grids[0].name,std::string(400,'n'));
  EXPECT_THROW((void)vdb::ReadNanoVdbFile(path("broken-name.nvdb")),std::invalid_argument);
}

TEST_F(OpenVdbNanoInterchange, ExactIntegerVectorAndMaskTreesValidate) {
  const auto check =
      [&]<class T>(
          const typename nv::tools::build::Grid<T>::ValueType &background,
          const typename nv::tools::build::Grid<T>::ValueType &first,
          const typename nv::tools::build::Grid<T>::ValueType &second) {
        char text[64];
        const std::string type = nv::toStr(text, nv::toGridType<T>());
        SCOPED_TRACE(type);
        nv::tools::build::Grid<T> source(background, type,
                                         std::is_same_v<T, nv::ValueMask>
                                             ? nv::GridClass::Topology
                                             : nv::GridClass::Unknown);
        auto accessor = source.getAccessor();
        accessor.setValue(nv::Coord(-9, 2, 3), first);
        accessor.setValue(nv::Coord(0, 0, 0), second);
        auto handle = nv::tools::createNanoGrid(
            source, nv::tools::StatsMode::All, nv::CheckMode::Full);
        const auto file = directory / (type + ".nvdb");
        segmented(file, handle);
        const auto information = vdb::ValidateNanoVdbFile(file);
        ASSERT_EQ(information.grids.size(), 1u);
        EXPECT_EQ(information.grids[0].valueType, type);
        EXPECT_EQ(information.grids[0].activeVoxels, 2u);
        EXPECT_TRUE(information.grids[0].checksumVerified);
      };
  check.template operator()<int32_t>(INT32_MIN, INT32_MAX, -3);
  check.template operator()<int64_t>(INT64_MIN, INT64_MAX - 1, -3);
  // The pinned native GridBuilder's LeafNode<bool> aliases BuildType to
  // ValueMask and cannot instantiate its setters. Boolean fixtures belong in
  // the independent OpenVDB-to-Nano conversion checks, not this builder probe.
  check.template operator()<nv::Vec3f>(nv::Vec3f(0), nv::Vec3f(1, -2, 3),
                                       nv::Vec3f(-4, 5, -6));
  check.template operator()<nv::Vec3d>(nv::Vec3d(0), nv::Vec3d(1, -2, 3),
                                       nv::Vec3d(-4, 5, -6));
  check.template operator()<nv::ValueMask>(false, true, false);
}

TEST_F(OpenVdbNanoInterchange,
       MultiSegmentDuplicateNamesRequireIndexSelection) {
  auto a = sphere("duplicate"), b = sphere("duplicate");
  {
    std::ofstream stream(path("multi.nvdb"), std::ios::binary);
    nv::io::writeGrid(stream, a, nv::io::Codec::NONE);
    nv::io::writeGrid(stream, b, nv::io::Codec::NONE);
  }
  EXPECT_EQ(vdb::InspectNanoVdbFile(path("multi.nvdb")).grids.size(), 2u);
  vdb::NanoVdbReadOptions options;
  options.inputGrid = "duplicate";
  EXPECT_THROW((void)vdb::ReadNanoVdbFile(path("multi.nvdb"), options),
               std::invalid_argument);
  options.inputGrid = "index:1";
  const auto selected = vdb::ReadNanoVdbFile(path("multi.nvdb"), options);
  ASSERT_EQ(selected.info().grids.size(), 1u);
  EXPECT_EQ(selected.info().grids[0].index, 1u);
  EXPECT_FALSE(selected.info().diagnostics.empty());
}

TEST_F(OpenVdbNanoInterchange,
       QuantizedRepresentationIsVisibleButNotMisinterpreted) {
  auto grid = nv::tools::createLevelSetSphere<nv::Fp16>(3.0);
  segmented(path("half.nvdb"), grid);
  const auto inspection = vdb::InspectNanoVdbFile(path("half.nvdb"));
  ASSERT_EQ(inspection.grids.size(), 1u);
  EXPECT_FALSE(inspection.grids[0].supported);
  EXPECT_THROW((void)vdb::ReadNanoVdbFile(path("half.nvdb")),
               std::invalid_argument);
}

TEST_F(OpenVdbNanoInterchange, ConstantGridsChargeRootsAgainstSharedNodeCap) {
  nv::tools::build::Grid<float> source(2.0f, "constant");
  auto handle = nv::tools::createNanoGrid(source);
  {
    std::ofstream stream(path("constants.nvdb"), std::ios::binary);
    nv::io::writeGrid(stream, handle, nv::io::Codec::NONE);
    nv::io::writeGrid(stream, handle, nv::io::Codec::NONE);
  }
  const auto archive = vdb::ReadNanoVdbFile(path("constants.nvdb"));
  vdb::NanoVdbLimits limits;
  limits.maxNodes = 1;
  EXPECT_THROW((void)vdb::InspectNanoVdbFile(path("constants.nvdb"), limits),
               std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteNanoVdbFile(archive, path("too-many.nvdb"), {},
                                          limits), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("too-many.nvdb")));
  limits.maxNodes = 2;
  EXPECT_NO_THROW((void)vdb::ValidateNanoVdbFile(path("constants.nvdb"), {},
                                               limits));
}

TEST_F(OpenVdbNanoInterchange, TileOnlyInternalArraysUseCompleteIndependentBoundsChecks) {
  nv::tools::build::Grid<double> source(0.,"tiles");
  // Seed an inactive branch: the pinned builder otherwise initializes a new
  // lower node with the first requested tile's active value/state.
  source.tree().root().addTile<1>(nv::Coord(0),0.,false);
  source.tree().root().addTile<1>(nv::Coord(0),2.,true);
  source.tree().root().addTile<1>(nv::Coord(8,0,0),6.,true);
  auto handle=nv::tools::createNanoGrid(source,nv::tools::StatsMode::BBox,nv::CheckMode::Full);
  raw("tiles.nvdb",handle);
  const auto archive=vdb::ReadNanoVdbFile(path("tiles.nvdb"));
  ASSERT_EQ(archive.info().grids[0].storedLeafVoxels,0u);
  EXPECT_EQ(archive.info().grids[0].activeVoxels,1024u);
  EXPECT_TRUE(archive.info().grids[0].checksumVerified);
  EXPECT_TRUE(std::any_of(archive.info().diagnostics.begin(),archive.info().diagnostics.end(),[](const auto& s) {
    return s.find("absent-array bounds bug")!=std::string::npos;
  }));
  (void)vdb::WriteNanoVdbFile(archive,path("rewritten.nvdb"));
  EXPECT_EQ(vdb::ValidateNanoVdbFile(path("rewritten.nvdb")).grids[0].activeVoxels,1024u);
  patch("tiles.nvdb",sizeof(nv::GridData)+offsetof(nv::TreeData,mNodeOffset)+2*sizeof(int64_t),INT64_MAX);
  vdb::NanoVdbReadOptions unsafe; unsafe.verifyChecksums=false;
  EXPECT_THROW((void)vdb::ReadNanoVdbFile(path("tiles.nvdb"),unsafe),std::invalid_argument);
}

TEST_F(OpenVdbNanoInterchange, FullModeTinyTailGapIsExplicitAndStrictCoverageRejectsIt) {
  nv::tools::build::Grid<double> source(2.,"constant");
  auto handle=nv::tools::createNanoGrid(source,nv::tools::StatsMode::All,nv::CheckMode::Full);
  raw("small.nvdb",handle);
  const auto info=vdb::ValidateNanoVdbFile(path("small.nvdb"));
  ASSERT_EQ(info.grids[0].checksum,vdb::NanoVdbChecksum::Full);
  EXPECT_TRUE(info.grids[0].checksumVerified);
  EXPECT_FALSE(info.grids[0].checksumCoversAllData);
  vdb::NanoVdbReadOptions strict; strict.requireFullChecksum=true;
  EXPECT_THROW((void)vdb::ReadNanoVdbFile(path("small.nvdb"),strict),std::invalid_argument);
  patch("small.nvdb",sizeof(nv::GridData)+sizeof(nv::TreeData)+offsetof(nv::NanoRoot<double>::DataType,mBackground),7.);
  // This is an upstream format hole, not proof that the changed value is
  // authentic. The mode still compares equal; strict coverage still rejects.
  const auto modified=vdb::ValidateNanoVdbFile(path("small.nvdb"));
  EXPECT_TRUE(modified.grids[0].checksumVerified);
  EXPECT_FALSE(modified.grids[0].checksumCoversAllData);
  EXPECT_THROW((void)vdb::ReadNanoVdbFile(path("small.nvdb"),strict),std::invalid_argument);
}

TEST_F(OpenVdbNanoInterchange, LegacyNodeTailChecksumsExposeBlindDataCoverage) {
  for(bool longName:{false,true}) {
    SCOPED_TRACE(longName);
    auto handle=sphere(longName ? std::string(400,'l') : "legacy-nodes");
    auto* header=static_cast<nv::GridData*>(handle.buffer().data());
    header->mVersion=nv::Version(32,6,0);
    // SDK12's <=32.6 path implements the historic per-node CRC table. The
    // independent SDK11 fixture job covers its default blocked-tail variant.
    nv::tools::updateChecksum(header,nv::CheckMode::Full);
    const auto name=longName ? "legacy-blind.nvdb" : "legacy-nodes.nvdb";
    raw(name,handle);
    const auto info=vdb::ValidateNanoVdbFile(path(name));
    EXPECT_TRUE(info.grids[0].checksumVerified);
    EXPECT_EQ(info.grids[0].checksumCoversAllData,!longName);
    EXPECT_TRUE(std::any_of(info.diagnostics.begin(),info.diagnostics.end(),[](const auto& s) {
      return s.find("node-based checksum convention accepted")!=std::string::npos;
    }));
    vdb::NanoVdbReadOptions strict; strict.requireFullChecksum=true;
    if(longName) EXPECT_THROW((void)vdb::ReadNanoVdbFile(path(name),strict),std::invalid_argument);
    else EXPECT_NO_THROW((void)vdb::ReadNanoVdbFile(path(name),strict));
  }
}

TEST_F(OpenVdbNanoInterchange, MixedSegmentCodecsSurviveChecksumRewriting) {
  const auto caps = vdb::NanoVdbInterchangeCapabilities();
  auto first = sphere<float>("first");
  auto second = sphere<double>("second");
  auto third = sphere<float>("third");
  {
    std::ofstream stream(path("mixed.nvdb"), std::ios::binary);
    nv::io::writeGrid(stream, first, nv::io::Codec::NONE);
    nv::io::writeGrid(stream, second,
                       caps.zip ? nv::io::Codec::ZIP : nv::io::Codec::NONE);
    nv::io::writeGrid(stream, third,
                       caps.blosc ? nv::io::Codec::BLOSC : nv::io::Codec::NONE);
  }
  const auto before = vdb::ValidateNanoVdbFile(path("mixed.nvdb"));
  const auto after = vdb::RewriteNanoVdbChecksums(path("mixed.nvdb"),
                                                  path("rewritten.nvdb"));
  ASSERT_EQ(after.grids.size(), 3u);
  EXPECT_EQ(after.grids[1].codec, before.grids[1].codec);
  EXPECT_EQ(after.grids[2].codec, before.grids[2].codec);
  const auto checked = vdb::ValidateNanoVdbFile(path("rewritten.nvdb"));
  EXPECT_TRUE(checked.grids[2].checksumVerified);
  std::ifstream stream(path("rewritten.nvdb"), std::ios::binary);
  const auto independentlyRead = nv::io::readGrids<nv::HostBuffer>(stream);
  ASSERT_EQ(independentlyRead.size(), 3u);
  EXPECT_EQ(independentlyRead[1].grid<double>()->tree().getValue(nv::Coord(0)),
            second.grid<double>()->tree().getValue(nv::Coord(0)));
}

TEST_F(OpenVdbNanoInterchange,
       MetadataSizesAndAggregateLimitsFailBeforePayloadAllocation) {
  auto grid = sphere();
  segmented(path("oversized-name.nvdb"), grid);
  patch("oversized-name.nvdb",
        sizeof(nv::io::FileHeader) + offsetof(nv::io::FileMetaData, nameSize),
        UINT32_MAX);
  EXPECT_THROW((void)vdb::InspectNanoVdbFile(path("oversized-name.nvdb")),
               std::invalid_argument);
  raw("raw.nvdb", grid);
  vdb::NanoVdbLimits limits;
  limits.maxNativeBytes = 1;
  EXPECT_THROW((void)vdb::InspectNanoVdbFile(path("raw.nvdb"), limits),
               std::invalid_argument);
  limits = {};
  limits.maxStoredLeafVoxels = 1;
  EXPECT_THROW((void)vdb::ReadNanoVdbFile(path("raw.nvdb"), {}, limits),
               std::invalid_argument);
  limits = {};
  limits.maxWork = 1;
  EXPECT_THROW((void)vdb::ReadNanoVdbFile(path("raw.nvdb"), {}, limits),
               std::invalid_argument);
}

TEST_F(OpenVdbNanoInterchange,
       EscapingNodeOffsetsRejectBeforeUpstreamPointerTraversal) {
  auto grid = sphere();
  raw("broken.nvdb", grid);
  patch("broken.nvdb",
        sizeof(nv::GridData) + offsetof(nv::TreeData, mNodeOffset), UINT64_MAX);
  EXPECT_NO_THROW((void)vdb::InspectNanoVdbFile(path("broken.nvdb")));
  EXPECT_THROW((void)vdb::ValidateNanoVdbFile(path("broken.nvdb")),
               std::invalid_argument);
}

TEST_F(OpenVdbNanoInterchange,
       ChecksumMismatchIsIndependentOfStructuralValidation) {
  auto grid = sphere();
  raw("crc.nvdb", grid);
  const char changed = 'S';
  patch("crc.nvdb", offsetof(nv::GridData, mGridName), changed);
  EXPECT_THROW((void)vdb::ReadNanoVdbFile(path("crc.nvdb")),
               std::invalid_argument);
  vdb::NanoVdbReadOptions options;
  options.verifyChecksums = false;
  const auto archive = vdb::ReadNanoVdbFile(path("crc.nvdb"), options);
  EXPECT_TRUE(archive.info().grids[0].structureValidated);
  EXPECT_FALSE(archive.info().grids[0].checksumVerified);
  EXPECT_FALSE(archive.info().diagnostics.empty());
}

TEST_F(OpenVdbNanoInterchange,
       FullChecksumRequirementRejectsDisabledAndHeaderOnly) {
  for (const auto mode : {nv::CheckMode::Disable, nv::CheckMode::Half}) {
    auto grid = sphere("sphere", mode);
    const char *name =
        mode == nv::CheckMode::Disable ? "disabled.nvdb" : "header.nvdb";
    raw(name, grid);
    EXPECT_NO_THROW((void)vdb::ReadNanoVdbFile(path(name)));
    vdb::NanoVdbReadOptions options;
    options.requireFullChecksum = true;
    EXPECT_THROW((void)vdb::ReadNanoVdbFile(path(name), options),
                 std::invalid_argument);
  }
}

TEST_F(OpenVdbNanoInterchange,
       NativeWritesRoundTripWithIndependentUpstreamReaders) {
  auto first = sphere<float>("float"), second = sphere<double>("double");
  {
    std::ofstream stream(path("source.nvdb"), std::ios::binary);
    nv::io::writeGrid(stream, first, nv::io::Codec::NONE);
    nv::io::writeGrid(stream, second, nv::io::Codec::NONE);
  }
  const auto archive = vdb::ReadNanoVdbFile(path("source.nvdb"));
  for (bool rawOutput : {false, true}) {
    SCOPED_TRACE(rawOutput);
    vdb::NanoVdbWriteOptions options;
    options.raw = rawOutput;
    const auto output =
        path(rawOutput ? "raw-output.nvdb" : "segmented-output.nvdb");
    const auto result = vdb::WriteNanoVdbFile(archive, output, options);
    EXPECT_EQ(result.raw, rawOutput);
    EXPECT_EQ(result.grids.size(), 2u);
    EXPECT_EQ(result.encodedBytes, std::filesystem::file_size(output));
    EXPECT_EQ(vdb::ValidateNanoVdbFile(output).grids.size(), 2u);
    // The pinned upstream batch raw reader rewinds incorrectly when raw grids
    // have different byte sizes. Its independent indexed API handles both
    // standard segments and heterogeneous raw groups correctly.
    for (int index = 0; index < 2; ++index) {
      std::ifstream stream(output, std::ios::binary);
      const auto handle =
          nv::io::readGrid<nv::HostBuffer>(stream, index, nv::HostBuffer{});
      ASSERT_EQ(handle.gridCount(), 1u);
      const auto *actual = handle.gridData();
      const auto *expected = index ? second.gridData() : first.gridData();
      EXPECT_EQ(actual->mGridSize, expected->mGridSize);
      EXPECT_EQ(std::memcmp(reinterpret_cast<const uint8_t *>(actual) +
                                sizeof(nv::GridData),
                            reinterpret_cast<const uint8_t *>(expected) +
                                sizeof(nv::GridData),
                            size_t(actual->mGridSize - sizeof(nv::GridData))),
                0);
      EXPECT_TRUE(nv::tools::validateChecksum(actual, nv::CheckMode::Full));
    }
  }
  // Rewriting never changes the source archive's original file ordinals.
  EXPECT_EQ(archive.info().grids[1].index, 1u);
}

TEST_F(OpenVdbNanoInterchange,
       NativeWriterPreservesLongNamesAndUnicodeParentDirectories) {
  auto grid = sphere(std::string(400, 'x'));
  segmented(path("source.nvdb"), grid);
  const auto archive = vdb::ReadNanoVdbFile(path("source.nvdb"));
  const auto parent =
      directory / std::filesystem::path(u8"\u4f53\u7a4d-\u00e9");
  ASSERT_TRUE(std::filesystem::create_directory(parent));
  const auto output = parent / std::filesystem::path(u8"\u51fa\u529b.nvdb");
  const auto report = vdb::WriteNanoVdbFile(archive, output);
  EXPECT_EQ(report.grids[0].name, std::string(400, 'x'));
  EXPECT_EQ(vdb::ValidateNanoVdbFile(output).grids[0].name,
            report.grids[0].name);
}

TEST_F(OpenVdbNanoInterchange,
       ExplicitChecksumRepairRejectsStructuralDamageAndPreservesInput) {
  auto grid = sphere();
  raw("damaged-crc.nvdb", grid);
  patch("damaged-crc.nvdb", offsetof(nv::GridData, mGridName), 'S');
  const auto bytes = [&] {
    std::ifstream f(path("damaged-crc.nvdb"), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
  };
  const auto original = bytes();
  const auto result = vdb::RewriteNanoVdbChecksums(path("damaged-crc.nvdb"),
                                                   path("repaired.nvdb"));
  EXPECT_EQ(bytes(), original);
  EXPECT_TRUE(result.grids[0].checksumVerified);
  EXPECT_TRUE(std::any_of(
      result.diagnostics.begin(), result.diagnostics.end(), [](const auto &s) {
        return s.find("Explicit checksum repair accepted") != std::string::npos;
      }));
  EXPECT_TRUE(vdb::ValidateNanoVdbFile(path("repaired.nvdb"))
                  .grids[0]
                  .checksumVerified);
  patch("damaged-crc.nvdb",
        sizeof(nv::GridData) + offsetof(nv::TreeData, mNodeOffset), UINT64_MAX);
  EXPECT_THROW((void)vdb::RewriteNanoVdbChecksums(path("damaged-crc.nvdb"),
                                                  path("bad.nvdb")),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("bad.nvdb")));
}

TEST_F(OpenVdbNanoInterchange,
       WriterLimitsAndExclusiveOutputsProtectExistingFiles) {
  auto grid = sphere();
  raw("source.nvdb", grid);
  const auto archive = vdb::ReadNanoVdbFile(path("source.nvdb"));
  for (int boundary = 0; boundary < 4; ++boundary) {
    vdb::NanoVdbLimits limits;
    if (boundary == 0)
      limits.maxNativeBytes = 1;
    if (boundary == 1)
      limits.maxWorkspaceBytes = 1;
    if (boundary == 2)
      limits.maxWork = 1;
    if (boundary == 3)
      limits.maxInputBytes = 1;
    EXPECT_THROW(
        (void)vdb::WriteNanoVdbFile(archive, path("limited.nvdb"), {}, limits),
        std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path("limited.nvdb")));
  }
  const auto size = std::filesystem::file_size(path("source.nvdb"));
  EXPECT_THROW((void)vdb::WriteNanoVdbFile(archive, path("source.nvdb")),
               std::invalid_argument);
  EXPECT_EQ(std::filesystem::file_size(path("source.nvdb")), size);
  vdb::NanoVdbWriteOptions options;
  options.allowFloatMapRounding = true;
  EXPECT_THROW(
      (void)vdb::WriteNanoVdbFile(archive, path("ignored.nvdb"), options),
      std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("ignored.nvdb")));
}

TEST_F(OpenVdbNanoInterchange,
       RequestedCodecsRoundTripOrFailExplicitlyWhenUnavailable) {
  auto grid = sphere();
  raw("source.nvdb", grid);
  const auto archive = vdb::ReadNanoVdbFile(path("source.nvdb"));
  const auto capabilities = vdb::NanoVdbInterchangeCapabilities();
  for (const auto codec : {vdb::NanoVdbCodec::Zip, vdb::NanoVdbCodec::Blosc}) {
    const auto output =
        path(codec == vdb::NanoVdbCodec::Zip ? "zip.nvdb" : "blosc.nvdb");
    vdb::NanoVdbWriteOptions options;
    options.codec = codec;
    if (codec == vdb::NanoVdbCodec::Zip ? capabilities.zip
                                        : capabilities.blosc) {
      const auto written = vdb::WriteNanoVdbFile(archive, output, options);
      EXPECT_EQ(written.grids[0].codec, codec);
      EXPECT_TRUE(vdb::ValidateNanoVdbFile(output).grids[0].checksumVerified);
      std::ifstream stream(output, std::ios::binary);
      const auto independent = nv::io::readGrids<nv::HostBuffer>(stream);
      ASSERT_EQ(independent.size(), 1u);
      EXPECT_TRUE(nv::tools::isValid(independent[0].grid<float>(),
                                     nv::CheckMode::Full));
      const auto repair =
          path(codec == vdb::NanoVdbCodec::Zip ? "zip-header.nvdb"
                                               : "blosc-header.nvdb");
      const auto rewritten = vdb::RewriteNanoVdbChecksums(
          output, repair, vdb::NanoVdbChecksum::Header);
      EXPECT_EQ(rewritten.grids[0].codec, codec);
      EXPECT_EQ(vdb::ValidateNanoVdbFile(repair).grids[0].checksum,
                vdb::NanoVdbChecksum::Header);
    } else {
      EXPECT_THROW((void)vdb::WriteNanoVdbFile(archive, output, options),
                   std::invalid_argument);
      EXPECT_FALSE(std::filesystem::exists(output));
    }
    options.raw = true;
    EXPECT_THROW(
        (void)vdb::WriteNanoVdbFile(archive, path("bad-raw.nvdb"), options),
        std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path("bad-raw.nvdb")));
  }
}

TEST_F(OpenVdbNanoInterchange, ConversionAvailabilityIsIndependentOfNativeIo) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  EXPECT_TRUE(vdb::NanoVdbInterchangeCapabilities().openVdbConversion);
#else
  vdb::NanoVdbLimits limits;
  limits.maxInputBytes = 0;
  try {
    (void)vdb::WriteNanoVdbFromVdb("missing", "missing", {}, {}, limits);
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB"),
              std::string::npos);
  }
  try {
    (void)vdb::WriteVdbFromNanoVdb("missing", "missing", {}, false, limits);
    FAIL();
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB"),
              std::string::npos);
  }
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
TEST_F(OpenVdbNanoInterchange,
       AllExactVdbTypesRoundTripInactiveStatesAndNativeValues) {
  openvdb::initialize();
  const auto check = [&]<class Grid>(typename Grid::ValueType background,
                                     typename Grid::ValueType on,
                                     typename Grid::ValueType off) {
    SCOPED_TRACE(Grid::gridType());
    auto source = Grid::create(background);
    source->setName("typed");
    source->tree().setValueOn(openvdb::Coord(-9, 2, 3), on);
    source->tree().setValueOff(openvdb::Coord(1, -2, 4), off);
    source->insertMeta("exact-id", openvdb::Int64Metadata(INT64_MAX - 7));
    source->insertMeta("label", openvdb::StringMetadata(std::string("a\0b", 3)));
    openvdb::UnknownMetadata opaque("vendor:unknown-v1");
    opaque.setValue({0, 2, 255, 17});
    source->insertMeta("opaque", opaque);
    openvdb::MetaMap fileMeta;
    fileMeta.insertMeta("file-id", openvdb::Int64Metadata(INT64_MIN + 9));
    {
      openvdb::io::File file(path("source.vdb").string());
      file.setCompression(openvdb::io::COMPRESS_ZIP);
      file.write(openvdb::GridPtrVec{source}, fileMeta);
    }
    const auto written = vdb::WriteNanoVdbFromVdb(path("source.vdb"), path("typed.nvdb"));
    ASSERT_EQ(written.grids.size(), 1u);
    EXPECT_TRUE(written.grids[0].containerMetadata);
    EXPECT_TRUE(written.grids[0].checksumVerified);
    EXPECT_NO_THROW((void)vdb::WriteVdbFromNanoVdb(path("typed.nvdb"), path("roundtrip.vdb")));
    openvdb::io::File file(path("roundtrip.vdb").string());
    file.open(false);
    auto result = openvdb::gridPtrCast<Grid>(file.readGrid("typed"));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->background(), background);
    EXPECT_EQ(result->tree().getValue(openvdb::Coord(-9, 2, 3)), on);
    EXPECT_TRUE(result->tree().isValueOn(openvdb::Coord(-9, 2, 3)));
    EXPECT_EQ(result->tree().getValue(openvdb::Coord(1, -2, 4)), off);
    EXPECT_FALSE(result->tree().isValueOn(openvdb::Coord(1, -2, 4)));
    EXPECT_EQ(result->template metaValue<int64_t>("exact-id"), INT64_MAX - 7);
    EXPECT_EQ(result->template metaValue<std::string>("label"), std::string("a\0b", 3));
    EXPECT_EQ(file.getMetadata()->metaValue<int64_t>("file-id"), INT64_MIN + 9);
    const auto restoredOpaque =
        std::dynamic_pointer_cast<const openvdb::UnknownMetadata>((*result)["opaque"]);
    ASSERT_TRUE(restoredOpaque);
    EXPECT_EQ(restoredOpaque->typeName(), opaque.typeName());
    EXPECT_EQ(restoredOpaque->value(), opaque.value());
    if constexpr (std::is_floating_point_v<typename Grid::ValueType>)
      EXPECT_EQ(std::signbit(result->tree().getValue(openvdb::Coord(1, -2, 4))),
                std::signbit(off));
    file.close();
    std::filesystem::remove(path("source.vdb"));
    std::filesystem::remove(path("typed.nvdb"));
    std::filesystem::remove(path("roundtrip.vdb"));
  };
  check.template operator()<openvdb::FloatGrid>(2.f, 1.f, -0.f);
  check.template operator()<openvdb::DoubleGrid>(2., 1., -0.);
  check.template operator()<openvdb::Int32Grid>(INT32_MIN, INT32_MAX, -17);
  check.template operator()<openvdb::Int64Grid>(INT64_MIN, INT64_MAX - 1, -17);
  check.template operator()<openvdb::BoolGrid>(true, false, true);
  check.template operator()<openvdb::MaskGrid>(false, true, false);
  check.template operator()<openvdb::Vec3SGrid>(openvdb::Vec3f(0), openvdb::Vec3f(1,-2,3),
                                               openvdb::Vec3f(-0.f,2,-3));
  check.template operator()<openvdb::Vec3DGrid>(openvdb::Vec3d(0), openvdb::Vec3d(1,-2,3),
                                               openvdb::Vec3d(-0.,2,-3));
}

TEST_F(OpenVdbNanoInterchange, VectorFramesDoubleAffineAndUnicodePathsSurvive) {
  openvdb::initialize();
  auto source = openvdb::Vec3DGrid::create(openvdb::Vec3d(0));
  source->setName(std::string(400, 'v'));
  source->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  source->setIsInWorldSpace(true);
  source->tree().setValue(openvdb::Coord(-1,2,3), openvdb::Vec3d(2,-3,4));
  auto matrix = openvdb::math::Mat4d::identity();
  matrix[0][0] = 2; matrix[1][1] = -1; matrix[1][0] = .25;
  matrix[3][0] = 7; matrix[3][1] = -4; matrix[3][2] = 3;
  source->setTransform(openvdb::math::Transform::createLinearTransform(matrix));
  { openvdb::io::File file(path("source.vdb").string());
    file.write(openvdb::GridPtrVec{source}); }
  const auto parent = directory / std::filesystem::path(u8"\u4f53\u7a4d-\u00e9");
  ASSERT_TRUE(std::filesystem::create_directory(parent));
  const auto input = parent / std::filesystem::path(u8"\u5165\u529b.vdb");
  const auto native = parent / std::filesystem::path(u8"\u51fa\u529b.nvdb");
  const auto output = parent / std::filesystem::path(u8"\u623b\u3059.vdb");
  std::filesystem::copy_file(path("source.vdb"), input);
  (void)vdb::WriteNanoVdbFromVdb(input, native);
  (void)vdb::WriteVdbFromNanoVdb(native, output);
  std::filesystem::copy_file(output, path("roundtrip.vdb"));
  openvdb::io::File file(path("roundtrip.vdb").string());
  file.open(false);
  auto result = openvdb::gridPtrCast<openvdb::Vec3DGrid>(file.readGrid(source->getName()));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->getVectorType(), source->getVectorType());
  EXPECT_EQ(result->isInWorldSpace(), source->isInWorldSpace());
  EXPECT_EQ(result->transform().baseMap()->getAffineMap()->getMat4(), matrix);
  EXPECT_EQ(result->tree().getValue(openvdb::Coord(-1,2,3)), openvdb::Vec3d(2,-3,4));
}

TEST_F(OpenVdbNanoInterchange, FloatMapCacheRoundingRequiresExplicitConsent) {
  openvdb::initialize();
  auto source = openvdb::FloatGrid::create(0.f);
  source->setName("rounded");
  source->setTransform(openvdb::math::Transform::createLinearTransform(.1));
  source->tree().setValue(openvdb::Coord(0), 1.f);
  { openvdb::io::File file(path("source.vdb").string());
    file.write(openvdb::GridPtrVec{source}); }
  EXPECT_THROW((void)vdb::WriteNanoVdbFromVdb(path("source.vdb"), path("denied.nvdb")),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("denied.nvdb")));
  vdb::NanoVdbWriteOptions options;
  options.allowFloatMapRounding = true;
  const auto report = vdb::WriteNanoVdbFromVdb(path("source.vdb"), path("allowed.nvdb"), {}, options);
  EXPECT_TRUE(std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
    [](const auto &s) { return s.find("Float map cache rounded") != std::string::npos; }));
  (void)vdb::WriteVdbFromNanoVdb(path("allowed.nvdb"), path("restored.vdb"));
  openvdb::io::File file(path("restored.vdb").string());
  file.open(false);
  EXPECT_EQ(file.readGrid("rounded")->transform().voxelSize(), source->voxelSize());
}

TEST_F(OpenVdbNanoInterchange,
       UpstreamInactiveInternalOriginsAreValidatedWithoutChangingNativeBytes) {
  openvdb::initialize();
  auto source = openvdb::FloatGrid::create(2.f);
  source->setName("inactive");
  source->tree().setValueOn(openvdb::Coord(-9,2,3), 7.f);
  source->tree().setValueOff(openvdb::Coord(8001,2,3), -3.5f);
  auto handle = nv::tools::createNanoGrid(*source, nv::tools::StatsMode::BBox,
                                          nv::CheckMode::Full);
  raw("upstream.nvdb", handle);
  const auto archive = vdb::ReadNanoVdbFile(path("upstream.nvdb"));
  vdb::NanoVdbWriteOptions options;
  options.raw = true;
  (void)vdb::WriteNanoVdbFile(archive, path("retained.nvdb"), options);
  std::ifstream actual(path("retained.nvdb"), std::ios::binary);
  const std::string bytes(std::istreambuf_iterator<char>(actual), {});
  ASSERT_EQ(bytes.size(), handle.size());
  EXPECT_EQ(std::memcmp(bytes.data(), handle.data(), bytes.size()), 0);
  (void)vdb::WriteVdbFromNanoVdb(path("upstream.nvdb"), path("restored.vdb"));
  openvdb::io::File file(path("restored.vdb").string());
  file.open(false);
  auto result = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid("inactive"));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->tree().getValue(openvdb::Coord(8001,2,3)), -3.5f);
  EXPECT_FALSE(result->tree().isValueOn(openvdb::Coord(8001,2,3)));
  EXPECT_EQ(result->tree().getValue(openvdb::Coord(-9,2,3)), 7.f);
}

TEST_F(OpenVdbNanoInterchange, ReverseConversionRetainsSmallAffineShearExactly) {
  openvdb::initialize();
  auto source = openvdb::DoubleGrid::create(0.);
  source->setName("shear");
  source->tree().setValue(openvdb::Coord(0), 1.);
  auto matrix = openvdb::math::Mat4d::identity();
  matrix[1][0] = std::ldexp(1., -60); matrix[3][0]=1e8;
  std::stringstream encoded(std::ios::in|std::ios::out|std::ios::binary);
  openvdb::writeString(encoded,openvdb::math::AffineMap::mapType()); openvdb::math::AffineMap(matrix).write(encoded);
  encoded.seekg(0); openvdb::io::setCurrentVersion(encoded);
  auto exact=std::make_shared<openvdb::math::Transform>(); exact->read(encoded); source->setTransform(exact);
  { openvdb::io::File file(path("shear.vdb").string());
    file.write(openvdb::GridPtrVec{source}); }
  (void)vdb::WriteNanoVdbFromVdb(path("shear.vdb"), path("shear.nvdb"));
  (void)vdb::WriteVdbFromNanoVdb(path("shear.nvdb"), path("restored.vdb"));
  openvdb::io::File file(path("restored.vdb").string());
  file.open(false);
  const auto actual=file.readGrid("shear")->transform().baseMap()->getAffineMap()->getMat4();
  for(size_t r=0;r<4;++r) for(size_t c=0;c<4;++c) EXPECT_EQ(actual(r,c),matrix(r,c));
}

TEST_F(OpenVdbNanoInterchange, ExternalVectorsRequireExplicitFrameDefaults) {
  nv::tools::build::Grid<nv::Vec3f> source(nv::Vec3f(0), "external");
  source.getAccessor().setValue(nv::Coord(0), nv::Vec3f(1,2,3));
  auto handle = nv::tools::createNanoGrid(source);
  segmented(path("external.nvdb"), handle);
  EXPECT_THROW((void)vdb::WriteVdbFromNanoVdb(path("external.nvdb"), path("denied.vdb")),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("denied.vdb")));
  const auto report = vdb::WriteVdbFromNanoVdb(path("external.nvdb"), path("allowed.vdb"), {}, true);
  EXPECT_TRUE(std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
    [](const auto &s) { return s.find("Defaulted vector frame") != std::string::npos; }));
}
#endif
#endif
} // namespace
