#include <Container/geometry/GltfDracoWorker.h>
#include <Container/geometry/GltfDracoProtocol.h>
#include <Container/utility/ModelImportCancellation.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <limits>
#include <string>
#include <thread>

#ifndef CONTAINER_HAS_DRACO
#define CONTAINER_HAS_DRACO 0
#endif
#if CONTAINER_HAS_DRACO
#include <draco/compression/encode.h>
#include <draco/mesh/mesh.h>
#endif

namespace {
using namespace container::geometry::gltf;
namespace protocol = container::geometry::gltf::draco_worker;

class Directory {
public:
  Directory() {
    static std::atomic<uint64_t> sequence{};
    path = std::filesystem::temp_directory_path() / ("container-draco-tests-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directory(path);
  }
  ~Directory() {
    // Only files written by these tests; no recursive deletion or reparse traversal.
    std::error_code ignored;
    for (const auto* name : {"request.bin", "response.bin"}) std::filesystem::remove(path / name, ignored);
    std::filesystem::remove(path, ignored);
  }
  std::filesystem::path path;
};

DracoDecodeLimits limits() {
  DracoDecodeLimits value;
  value.maxInputBytes = 1024 * 1024; value.maxOutputBytes = 4096;
  value.maxPoints = 3; value.maxFaces = 1;
  value.processMemoryBytes = 64ull * 1024 * 1024; value.timeoutMs = 3000;
  return value;
}

DracoDecodedMesh wireMesh() {
  DracoDecodedMesh mesh;
  mesh.pointCount = 3; mesh.faceIndices = {2, 0, 1};
  DracoDecodedAttribute attribute;
  attribute.uniqueId = 39; attribute.scalarType = DracoScalarType::UInt32;
  attribute.componentCount = 1; attribute.normalized = true;
  const std::array<uint32_t, 3> raw{16'777'217u, 0xffffffffu, 257u};
  attribute.values.resize(sizeof(raw)); std::memcpy(attribute.values.data(), raw.data(), sizeof(raw));
  mesh.attributes.push_back(std::move(attribute));
  return mesh;
}

std::filesystem::path executable(const char* variable, const char* definition) {
  if (const auto* value = std::getenv(variable)) return std::filesystem::absolute(value);
  return std::filesystem::absolute(definition);
}
#ifndef CONTAINER_DRACO_WORKER_EXECUTABLE
#define CONTAINER_DRACO_WORKER_EXECUTABLE "ContainerDracoWorker.exe"
#endif
#ifndef CONTAINER_DRACO_PROBE_EXECUTABLE
#define CONTAINER_DRACO_PROBE_EXECUTABLE "ContainerDracoWorkerProbe.exe"
#endif

TEST(DracoWorkerProtocol, RoundTripsVersionedRequestAndExactTypedResponse) {
  Directory directory;
  const std::array<std::byte, 4> compressed{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const std::array attributes{DracoAttributeRequest{39, 1}};
  protocol::WriteRequest(directory.path / "request.bin", compressed, attributes, limits());
  const auto request = protocol::ReadRequest(directory.path / "request.bin");
  EXPECT_EQ(request.compressed, std::vector<std::byte>(compressed.begin(), compressed.end()));
  EXPECT_EQ(request.attributes[0].uniqueId, 39u);
  EXPECT_EQ(request.limits.processMemoryBytes, limits().processMemoryBytes);
  const auto mesh = wireMesh();
  protocol::WriteResponse(directory.path / "response.bin", mesh, limits());
  EXPECT_EQ(std::filesystem::file_size(directory.path / "response.bin"), 96u);
  const auto decoded = protocol::ReadResponse(directory.path / "response.bin", attributes, limits());
  EXPECT_EQ(decoded.faceIndices, mesh.faceIndices);
  EXPECT_EQ(decoded.attributes[0].values, mesh.attributes[0].values);
  EXPECT_TRUE(decoded.attributes[0].normalized);
}

TEST(DracoWorkerProtocol, RejectsTruncationOverflowUnknownVersionAndTrailingBytes) {
  Directory directory;
  const auto file = directory.path / "response.bin";
  const std::array attributes{DracoAttributeRequest{39, 1}};
  const auto original = wireMesh();
  const auto reject = [&](uint64_t offset, unsigned char value) {
    protocol::WriteResponse(file, original, limits());
    std::fstream stream(file, std::ios::binary | std::ios::in | std::ios::out);
    stream.seekp(offset); stream.put(static_cast<char>(value)); stream.close();
    EXPECT_THROW((void)protocol::ReadResponse(file, attributes, limits()), std::runtime_error) << offset;
  };
  reject(4, 2);      // protocol version
  reject(24, 255);   // decoded point count exceeds declared maximum
  reject(48, 255);   // face index out of bounds
  reject(64, 255);   // scalar enum
  reject(72, 2);     // normalized flag
  reject(76, 255);   // raw byte length
  protocol::WriteResponse(file, original, limits());
  std::filesystem::resize_file(file, 50);
  EXPECT_THROW((void)protocol::ReadResponse(file, attributes, limits()), std::runtime_error);
  protocol::WriteResponse(file, original, limits());
  std::ofstream extra(file, std::ios::binary | std::ios::app); extra.put('x'); extra.close();
  EXPECT_THROW((void)protocol::ReadResponse(file, attributes, limits()), std::runtime_error);
}

TEST(DracoWorkerProtocol, RejectsUnboundedLimitsDuplicateIdsAndUnexpectedAttributes) {
  Directory directory;
  const std::array input{std::byte{1}};
  const std::array duplicates{DracoAttributeRequest{1, 1}, DracoAttributeRequest{1, 2}};
  EXPECT_THROW(protocol::ValidateRequest(input, duplicates, limits()), std::runtime_error);
  const std::array attributes{DracoAttributeRequest{39, 1}};
  auto invalid = limits(); invalid.maxOutputBytes = std::numeric_limits<uint64_t>::max();
  EXPECT_THROW(protocol::ValidateRequest(input, attributes, invalid), std::runtime_error);
  invalid = limits(); invalid.timeoutMs = 0;
  EXPECT_THROW(protocol::ValidateRequest(input, attributes, invalid), std::runtime_error);
  protocol::WriteResponse(directory.path / "response.bin", wireMesh(), limits());
  const std::array wrong{DracoAttributeRequest{40, 1}};
  EXPECT_THROW((void)protocol::ReadResponse(directory.path / "response.bin", wrong, limits()), std::runtime_error);
  invalid = limits(); invalid.maxOutputBytes = 95;
  EXPECT_THROW((void)protocol::ReadResponse(directory.path / "response.bin", attributes, invalid), std::runtime_error);
}

#ifdef _WIN32
TEST(DracoWorkerClient, EnforcesProcessMemoryBeforeRunningHiddenWorkerAndCleansOwnedFiles) {
  Directory directory;
  const std::array attributes{DracoAttributeRequest{39, 1}};
  const std::array input{std::byte{5}};
  DracoWorkerOptions options{executable("CONTAINER_DRACO_PROBE_EXECUTABLE", CONTAINER_DRACO_PROBE_EXECUTABLE), directory.path};
  ASSERT_TRUE(std::filesystem::is_regular_file(options.workerExecutable));
  const auto decoded = DecodeDracoInWorker(input, attributes, limits(), options);
  std::array<uint32_t, 3> values{};
  std::memcpy(values.data(), decoded.attributes[0].values.data(), sizeof(values));
  EXPECT_EQ(values, (std::array<uint32_t, 3>{1, 1, 1}));
  EXPECT_TRUE(std::filesystem::is_empty(directory.path));
}

TEST(DracoWorkerClient, TimeoutsCrashesOversizedAndMalformedResponsesLeaveNoFiles) {
  Directory directory;
  const std::array attributes{DracoAttributeRequest{39, 1}};
  DracoWorkerOptions options{executable("CONTAINER_DRACO_PROBE_EXECUTABLE", CONTAINER_DRACO_PROBE_EXECUTABLE), directory.path};
  ASSERT_TRUE(std::filesystem::is_regular_file(options.workerExecutable));
  for (const auto action : std::array<unsigned char, 4>{1, 2, 3, 4}) {
    SCOPED_TRACE(action);
    const std::array input{static_cast<std::byte>(action)};
    auto budget = limits(); if (action == 1) budget.timeoutMs = 100;
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_THROW((void)DecodeDracoInWorker(input, attributes, budget, options), std::runtime_error);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(6));
    EXPECT_TRUE(std::filesystem::is_empty(directory.path));
  }
}

TEST(DracoWorkerClient, CancelsAStartedDecoderAndCleansOwnedFiles) {
  using namespace std::chrono_literals;
  Directory directory;
  const std::array input{std::byte{7}};
  const std::array attributes{DracoAttributeRequest{39, 1}};
  const DracoWorkerOptions options{
      executable("CONTAINER_DRACO_PROBE_EXECUTABLE", CONTAINER_DRACO_PROBE_EXECUTABLE), directory.path};
  ASSERT_TRUE(std::filesystem::is_regular_file(options.workerExecutable));
  auto budget = limits();
  budget.timeoutMs = 9000;
  std::stop_source source;
  auto decode = std::async(std::launch::async, [&] {
    return DecodeDracoInWorker(input, attributes, budget, options, source.get_token());
  });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  bool started = false;
  while (!started && std::chrono::steady_clock::now() < deadline &&
         decode.wait_for(0ms) != std::future_status::ready) {
    for (const auto& entry : std::filesystem::directory_iterator(directory.path)) {
      std::error_code ignored;
      if (std::filesystem::is_regular_file(entry.path() / "response.bin", ignored)) {
        started = true;
        break;
      }
    }
    if (!started) std::this_thread::sleep_for(2ms);
  }
  source.request_stop();
  EXPECT_TRUE(started);
  EXPECT_EQ(decode.wait_for(3s), std::future_status::ready);
  EXPECT_THROW((void)decode.get(), container::util::ModelImportCancelled);
  EXPECT_TRUE(std::filesystem::is_empty(directory.path));
}

TEST(DracoWorkerClient, CleanupNeverDeletesVictimFilesThroughWorkerCreatedJunction) {
  Directory directory;
  const auto victim = directory.path / "victim";
  std::filesystem::create_directory(victim);
  for (const auto* file : {"request.bin", "response.bin"}) {
    std::ofstream output(victim / file, std::ios::binary);
    output << "victim must survive worker cleanup";
  }
  const std::array input{std::byte{6}};
  const std::array attributes{DracoAttributeRequest{39, 1}};
  DracoWorkerOptions options{executable("CONTAINER_DRACO_PROBE_EXECUTABLE", CONTAINER_DRACO_PROBE_EXECUTABLE), directory.path};
  EXPECT_THROW((void)DecodeDracoInWorker(input, attributes, limits(), options), std::runtime_error);
  for (const auto* file : {"request.bin", "response.bin"}) {
    ASSERT_TRUE(std::filesystem::is_regular_file(victim / file));
    EXPECT_EQ(std::filesystem::file_size(victim / file), 34u);
    std::filesystem::remove(victim / file);
  }
  std::filesystem::remove(victim);
  EXPECT_TRUE(std::filesystem::is_empty(directory.path));
}

TEST(DracoWorkerClient, MissingAndRelativeExecutablesFailBeforeCreatingTemporaryFiles) {
  Directory directory;
  const std::array input{std::byte{0}};
  const std::array attributes{DracoAttributeRequest{39, 1}};
  EXPECT_THROW((void)DecodeDracoInWorker(input, attributes, limits(),
      {directory.path / "missing.exe", directory.path}), std::runtime_error);
  EXPECT_THROW((void)DecodeDracoInWorker(input, attributes, limits(),
      {"relative.exe", directory.path}), std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_empty(directory.path));
}
#else
TEST(DracoWorkerClient, UnsupportedPlatformFailsExplicitlyWithoutInProcessDecoder) {
  EXPECT_FALSE(DracoWorkerPlatformSupported());
  const std::array input{std::byte{0}};
  const std::array attributes{DracoAttributeRequest{39, 1}};
  EXPECT_THROW((void)DecodeDracoInWorker(input, attributes, limits()), std::runtime_error);
}
#endif

#if CONTAINER_HAS_DRACO && defined(_WIN32)
std::vector<std::byte> encodedTriangle() {
  draco::Mesh mesh;
  mesh.set_num_points(3); mesh.SetNumFaces(1);
  mesh.SetFace(draco::FaceIndex(0), {draco::PointIndex(0), draco::PointIndex(1), draco::PointIndex(2)});
  draco::GeometryAttribute positions;
  positions.Init(draco::GeometryAttribute::POSITION, nullptr, 3, draco::DT_FLOAT32, false, 12, 0);
  const int position = mesh.AddAttribute(positions, true, 3);
  mesh.attribute(position)->set_unique_id(7);
  const float xyz[3][3]{{0, 0, 0}, {2, 0, 0}, {0, 3, 0}};
  for (uint32_t i = 0; i < 3; ++i) mesh.attribute(position)->SetAttributeValue(draco::AttributeValueIndex(i), xyz[i]);
  draco::GeometryAttribute colors;
  colors.Init(draco::GeometryAttribute::COLOR, nullptr, 4, draco::DT_UINT8, true, 4, 0);
  const int color = mesh.AddAttribute(colors, true, 3); mesh.attribute(color)->set_unique_id(19);
  const unsigned char rgba[3][4]{{255, 0, 0, 0}, {0, 255, 0, 128}, {0, 0, 255, 255}};
  for (uint32_t i = 0; i < 3; ++i) mesh.attribute(color)->SetAttributeValue(draco::AttributeValueIndex(i), rgba[i]);
  draco::GeometryAttribute ids;
  ids.Init(draco::GeometryAttribute::GENERIC, nullptr, 1, draco::DT_UINT32, false, 4, 0);
  const int id = mesh.AddAttribute(ids, true, 3); mesh.attribute(id)->set_unique_id(39);
  // Draco's encoder converts portable integers through int32_t. Keep the
  // fixture encodable while using odd values beyond float32's exact range.
  // Protocol/client coverage above separately preserves UINT32_MAX verbatim.
  const uint32_t exact[3]{16'777'217u, 16'777'219u, 257u};
  for (uint32_t i = 0; i < 3; ++i) mesh.attribute(id)->SetAttributeValue(draco::AttributeValueIndex(i), &exact[i]);
  draco::Encoder encoder; encoder.SetSpeedOptions(10, 10);
  encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 14);
  draco::EncoderBuffer buffer;
  const auto status = encoder.EncodeMeshToBuffer(mesh, &buffer);
  if (!status.ok()) throw std::runtime_error(status.error_msg_string());
  const auto* data = reinterpret_cast<const std::byte*>(buffer.data());
  return {data, data + buffer.size()};
}

TEST(DracoWorkerDecoder, RealEncoderRoundTripPreservesMappedRawRgbaAndExactUnsignedValues) {
  Directory directory;
  DracoWorkerOptions options{executable("CONTAINER_DRACO_WORKER_EXECUTABLE", CONTAINER_DRACO_WORKER_EXECUTABLE), directory.path};
  ASSERT_TRUE(std::filesystem::is_regular_file(options.workerExecutable));
  const auto encoded = encodedTriangle();
  const std::array attributes{DracoAttributeRequest{7, 3}, DracoAttributeRequest{19, 4}, DracoAttributeRequest{39, 1}};
  const auto decoded = DecodeDracoInWorker(encoded, attributes, limits(), options);
  ASSERT_EQ(decoded.pointCount, 3u); ASSERT_EQ(decoded.faceIndices.size(), 3u);
  ASSERT_EQ(decoded.attributes.size(), 3u);
  EXPECT_EQ(decoded.attributes[0].scalarType, DracoScalarType::Float32);
  EXPECT_EQ(decoded.attributes[1].scalarType, DracoScalarType::UInt8);
  EXPECT_TRUE(decoded.attributes[1].normalized);
  EXPECT_EQ(decoded.attributes[2].scalarType, DracoScalarType::UInt32);
  std::array<uint32_t, 3> exact{};
  std::memcpy(exact.data(), decoded.attributes[2].values.data(), sizeof(exact));
  // Encoder may reorder points. Match all attributes by their decoded position.
  for (uint32_t point = 0; point < decoded.pointCount; ++point) {
    std::array<float, 3> position{};
    std::memcpy(position.data(), decoded.attributes[0].values.data() + point * sizeof(position), sizeof(position));
    const uint32_t source = position[0] == 2 ? 1 : position[1] == 3 ? 2 : 0;
    const uint32_t expected[3]{16'777'217u, 16'777'219u, 257u};
    EXPECT_EQ(exact[point], expected[source]);
    const auto alpha = std::to_integer<unsigned char>(decoded.attributes[1].values[point * 4 + 3]);
    EXPECT_EQ(alpha, (std::array<unsigned char, 3>{0, 128, 255})[source]);
  }
  EXPECT_TRUE(std::filesystem::is_empty(directory.path));
}

TEST(DracoWorkerDecoder, InvalidStreamsCountOutputAndAttributeLimitsFailTransactionally) {
  Directory directory;
  DracoWorkerOptions options{executable("CONTAINER_DRACO_WORKER_EXECUTABLE", CONTAINER_DRACO_WORKER_EXECUTABLE), directory.path};
  ASSERT_TRUE(std::filesystem::is_regular_file(options.workerExecutable));
  const auto encoded = encodedTriangle();
  const std::array attributes{DracoAttributeRequest{7, 3}};
  const std::array invalid{std::byte{0}, std::byte{1}, std::byte{255}};
  EXPECT_THROW((void)DecodeDracoInWorker(invalid, attributes, limits(), options), std::runtime_error);
  EXPECT_THROW((void)DecodeDracoInWorker(std::span(encoded).first(encoded.size() / 2), attributes, limits(), options), std::runtime_error);
  auto budget = limits(); budget.maxPoints = 2;
  EXPECT_THROW((void)DecodeDracoInWorker(encoded, attributes, budget, options), std::runtime_error);
  budget = limits(); budget.maxOutputBytes = 48;
  EXPECT_THROW((void)DecodeDracoInWorker(encoded, attributes, budget, options), std::runtime_error);
  const std::array absent{DracoAttributeRequest{888, 3}};
  EXPECT_THROW((void)DecodeDracoInWorker(encoded, absent, limits(), options), std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_empty(directory.path));
}
#endif
} // namespace
