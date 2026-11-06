#include "Container/geometry/OpenVdbParticles.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/ParticlesToLevelSet.h>
#include <tbb/task_arena.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <mutex>
#include <streambuf>
#include <vector>
#endif

namespace container::geometry::vdb {
namespace {

[[noreturn]] void unavailable() {
  throw std::runtime_error(
      "OpenVDB processing is unavailable in this build. Install the OpenVDB "
      "dependencies and configure with -DENABLE_OPENVDB=ON.");
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
void validateOptions(const ProcessingOptions& processing,
                     const ParticleRasterizationOptions& options) {
  if (!std::isfinite(processing.voxelSize) ||
      processing.voxelSize < 1.5e-5 || processing.voxelSize > 1e6)
    throw std::invalid_argument("particle voxel size must be in [1.5e-5, 1e6] world units");
  if (!std::isfinite(processing.halfWidth) || processing.halfWidth <= 1.0 ||
      processing.halfWidth > 64.0)
    throw std::invalid_argument("particle half width must be in (1, 64] voxels");
  if (processing.maxVoxelCount == 0 || processing.maxInputBytes == 0 ||
      options.maxParticles == 0)
    throw std::invalid_argument("particle processing budgets must be positive");
  if (processing.gridName.empty())
    throw std::invalid_argument("output grid name must not be empty");
}

void validateOutput(const std::filesystem::path& output) {
  if (output.empty()) throw std::invalid_argument("output path must not be empty");
  if (std::filesystem::exists(output))
    throw std::invalid_argument("output already exists; choose a new .vdb path");
  if (!output.parent_path().empty() &&
      !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("output directory does not exist");
}

using Leaf = openvdb::FloatTree::LeafNodeType;
using Upper = openvdb::FloatTree::RootNodeType::ChildNodeType;
using Lower = Upper::ChildNodeType;

struct Bounds {
  std::array<int64_t, 3> lo{};
  std::array<int64_t, 3> hi{};

  // floor division is required for nodes straddling the negative half-space.
  static int64_t block(int64_t coordinate, int64_t dimension) {
    return coordinate >= 0 ? coordinate / dimension
                           : (coordinate + 1) / dimension - 1;
  }
  long double nodes(int64_t dimension) const {
    long double count = 1;
    for (size_t axis = 0; axis < 3; ++axis)
      count *= static_cast<long double>(block(hi[axis], dimension)) -
               static_cast<long double>(block(lo[axis], dimension)) + 1;
    return count;
  }
};

void preflight(std::span<const Particle> particles,
               const ProcessingOptions& processing,
               const ParticleRasterizationOptions& options) {
  if (particles.empty())
    throw std::invalid_argument("particle array must not be empty");
  // The upstream partitioner stores indices and bucket counts as Index32.
  if (particles.size() > options.maxParticles ||
      particles.size() >= std::numeric_limits<openvdb::Index32>::max())
    throw std::invalid_argument("particle count exceeds the particle budget or OpenVDB index limit");
  if (static_cast<long double>(particles.size()) * sizeof(Particle) >
      static_cast<long double>(processing.maxInputBytes))
    throw std::invalid_argument("decoded particles exceed the input-memory budget");

  Bounds bounds;
  bounds.lo.fill(std::numeric_limits<int64_t>::max());
  bounds.hi.fill(std::numeric_limits<int64_t>::min());
  const double inverseVoxel = 1.0 / processing.voxelSize;
  // Upstream derives its band from the rounded FloatGrid background value.
  const double band = static_cast<float>(processing.voxelSize * processing.halfWidth) *
                      inverseVoxel;
  for (size_t index = 0; index < particles.size(); ++index) {
    const auto& particle = particles[index];
    const auto label = "particle " + std::to_string(index) + ": ";
    if (!std::isfinite(particle.radius) || particle.radius <= 0)
      throw std::invalid_argument(label + "radius must be finite and positive");
    const double radius = particle.radius * inverseVoxel;
    if (!std::isfinite(radius) || radius < 1.5 || radius > 100.0)
      throw std::invalid_argument(label +
          "radius must be in [1.5, 100] source voxels; OpenVDB otherwise ignores this particle");
    for (size_t axis = 0; axis < 3; ++axis) {
      if (!std::isfinite(particle.position[axis]))
        throw std::invalid_argument(label + "position must be finite");
      // Two spare voxels account for transform/band rounding. The upper limit
      // also leaves room for the upstream signed-coordinate loop increment.
      const double center = particle.position[axis] * inverseVoxel;
      const double low = std::floor(center - radius - band - 2.0);
      const double high = std::ceil(center + radius + band + 2.0);
      if (!std::isfinite(low) || !std::isfinite(high) ||
          low < static_cast<double>(std::numeric_limits<int32_t>::min()) + 8.0 ||
          high > static_cast<double>(std::numeric_limits<int32_t>::max()) - 8.0)
        throw std::invalid_argument(label + "sphere exceeds OpenVDB coordinate limits");
      bounds.lo[axis] = std::min(bounds.lo[axis], static_cast<int64_t>(low));
      bounds.hi[axis] = std::max(bounds.hi[axis], static_cast<int64_t>(high));
    }
  }
  if (bounds.nodes(1) > static_cast<long double>(processing.maxVoxelCount))
    throw std::invalid_argument(
        "particle bounds exceed the voxel budget; increase voxel size or --max-voxels");

  // Bound all nodes in the leaf-aligned aggregate box, including inactive
  // interior storage. sizeof(InternalNode) includes its fixed child table.
  // Reserve three tree-sized allocations for rasterization/finalization and
  // file statistics/compression, plus allocator overhead on every node. A
  // single-worker arena and zero raster grain size prevent per-worker trees.
  const long double treeBytes =
      bounds.nodes(Leaf::DIM) * (sizeof(Leaf) + Leaf::NUM_VALUES * sizeof(float) + 128.0L) +
      bounds.nodes(Lower::DIM) * (sizeof(Lower) + 128.0L) +
      bounds.nodes(Upper::DIM) * (sizeof(Upper) + 384.0L);
  // The PointPartitioner has point/offset arrays, map/deque bins and page
  // arrays. Its one-worker histogram is fixed-size (32^3 Index32 values).
  // 2 KiB per input conservatively covers even one separately allocated bin
  // per point; 4 MiB covers the histogram, stream and task bookkeeping.
  const long double workspace = treeBytes * 3.0L +
      static_cast<long double>(particles.size()) * (2048.0L + sizeof(Particle)) +
      4.0L * 1024.0L * 1024.0L;
  if (workspace > static_cast<long double>(processing.maxInputBytes))
    throw std::invalid_argument("particle rasterization workspace exceeds the input-memory budget");
}

struct ParticleList {
  using PosType = openvdb::Vec3R;
  std::span<const Particle> particles;
  size_t size() const { return particles.size(); }
  void getPos(size_t index, PosType& position) const {
    const auto& p = particles[index].position;
    position = PosType(p[0], p[1], p[2]);
  }
  void getPosRad(size_t index, PosType& position, openvdb::Real& radius) const {
    getPos(index, position);
    radius = particles[index].radius;
  }
};

// Prevent a concurrently enlarged input from escaping the file-size preflight.
// SAX's lexer still owns a token string; three encoded-file sizes below reserve
// its capacity growth and allocation replacement before parsing starts.
class BoundedInput : public std::streambuf {
 public:
  BoundedInput(std::ifstream& input, uintmax_t limit)
      : input_(input), remaining_(limit) {}
  uintmax_t remaining() const { return remaining_; }
  bool tokenExceeded() const { return tokenExceeded_; }
 protected:
  int_type underflow() override {
    if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
    if (remaining_ == 0) return traits_type::eof();
    const auto count = static_cast<std::streamsize>(
        std::min<uintmax_t>(remaining_, sizeof(buffer_)));
    input_.read(buffer_, count);
    const auto received = input_.gcount();
    remaining_ -= static_cast<uintmax_t>(received);
    if (received == 0) return traits_type::eof();
    // The schema needs two short keys and finite double numbers. Bound raw
    // tokens before handing a chunk to SAX, which otherwise allocates both
    // raw and decoded tokens (and copies malformed tokens into diagnostics).
    for (std::streamsize index = 0; index < received; ++index) {
      const char value = buffer_[index];
      if (inString_) {
        ++tokenBytes_;
        if (!escaped_ && value == '"') inString_ = false;
        if (!escaped_ && value == '\\') escaped_ = true;
        else escaped_ = false;
      } else if (value == '"') {
        inString_ = true;
        escaped_ = false;
        tokenBytes_ = 1;
      } else if (value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
                 value == '[' || value == ']' || value == '{' || value == '}' ||
                 value == ':' || value == ',') {
        tokenBytes_ = 0;
      } else {
        ++tokenBytes_;
      }
      if (tokenBytes_ > 128) {
        tokenExceeded_ = true;
        return traits_type::eof();
      }
    }
    setg(buffer_, buffer_, buffer_ + received);
    return traits_type::to_int_type(*gptr());
  }
 private:
  std::ifstream& input_;
  uintmax_t remaining_;
  size_t tokenBytes_{0};
  bool inString_{false}, escaped_{false}, tokenExceeded_{false};
  char buffer_[4096];
};

// SAX decoding avoids allocating a whole JSON DOM before particle limits can
// be checked. Only a validated, complete object is appended to the vector.
struct ParticleJson : nlohmann::json_sax<nlohmann::json> {
  enum class State { Start, Array, Object, Position, Done } state{State::Start};
  enum class Field { None, Position, Radius } field{Field::None};
  const ProcessingOptions& processing;
  const ParticleRasterizationOptions& options;
  long double encodedReserve;
  std::vector<Particle> particles;
  Particle current;
  size_t coordinates{0};
  bool gotPosition{false}, gotRadius{false};
  std::string error;

  ParticleJson(const ProcessingOptions& p, const ParticleRasterizationOptions& o,
               uintmax_t inputBytes)
      : processing(p), options(o), encodedReserve(3.0L * inputBytes) {}
  bool fail(const char* message) { error = message; return false; }
  bool null() override { return fail("particle values must be numeric"); }
  bool boolean(bool) override { return fail("particle values must be numeric"); }
  bool string(string_t&) override { return fail("particle values must be numeric"); }
  bool binary(binary_t&) override { return fail("binary particle values are unsupported"); }
  bool number_integer(number_integer_t n) override { return number(static_cast<double>(n)); }
  bool number_unsigned(number_unsigned_t n) override { return number(static_cast<double>(n)); }
  bool number_float(number_float_t n, const string_t&) override { return number(n); }
  bool number(double value) {
    if (!std::isfinite(value)) return fail("particle values must be finite");
    if (state == State::Position && coordinates < 3) {
      current.position[coordinates++] = value;
      return true;
    }
    if (state == State::Object && field == Field::Radius) {
      current.radius = value;
      gotRadius = true;
      field = Field::None;
      return true;
    }
    return fail("expected a particle object, position array or radius number");
  }
  bool start_object(std::size_t) override {
    if (state != State::Array) return fail("particle JSON must be an array of objects");
    if (particles.size() >= options.maxParticles ||
        particles.size() + 1 >= std::numeric_limits<openvdb::Index32>::max())
      return fail("particle count exceeds the particle budget or OpenVDB index limit");
    if (encodedReserve +
        (static_cast<long double>(particles.size()) + 1) * sizeof(Particle) * 3 >
        static_cast<long double>(processing.maxInputBytes))
      return fail("decoded particles exceed the input-memory budget");
    current = {};
    coordinates = 0;
    gotPosition = gotRadius = false;
    field = Field::None;
    state = State::Object;
    return true;
  }
  bool key(string_t& key) override {
    if (state != State::Object || field != Field::None)
      return fail("invalid particle object");
    if (key == "position" && !gotPosition) field = Field::Position;
    else if (key == "radius" && !gotRadius) field = Field::Radius;
    else return fail("unknown or duplicate particle field");
    return true;
  }
  bool end_object() override {
    if (state != State::Object || field != Field::None || !gotPosition || !gotRadius)
      return fail("particle requires position and radius");
    particles.push_back(current);
    state = State::Array;
    return true;
  }
  bool start_array(std::size_t) override {
    if (state == State::Start) { state = State::Array; return true; }
    if (state == State::Object && field == Field::Position) {
      state = State::Position;
      coordinates = 0;
      return true;
    }
    return fail("unexpected array in particle JSON");
  }
  bool end_array() override {
    if (state == State::Position && coordinates == 3) {
      gotPosition = true;
      field = Field::None;
      state = State::Object;
      return true;
    }
    if (state == State::Array) { state = State::Done; return true; }
    return fail("particle position must contain exactly three numbers");
  }
  bool parse_error(std::size_t position, const std::string&,
                   const nlohmann::detail::exception&) override {
    error = "invalid particle JSON at byte " + std::to_string(position);
    return false;
  }
};
#endif
}  // namespace

ProcessingResult WriteParticlesLevelSet(
    std::span<const Particle> particles, const std::filesystem::path& output,
    const ProcessingOptions& processing, const ParticleRasterizationOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(processing, options);
  validateOutput(output);
  preflight(particles, processing, options);
  static std::once_flag once;
  std::call_once(once, [] { openvdb::initialize(); });

  auto grid = openvdb::FloatGrid::create(
      static_cast<float>(processing.voxelSize * processing.halfWidth));
  grid->setTransform(openvdb::math::Transform::createLinearTransform(processing.voxelSize));
  grid->setGridClass(openvdb::GRID_LEVEL_SET);
  grid->setName(processing.gridName);
  tbb::task_arena arena(1);
  arena.execute([&] {
    openvdb::tools::ParticlesToLevelSet<openvdb::FloatGrid> raster(*grid);
    raster.setGrainSize(0);
    raster.rasterizeSpheres(ParticleList{particles});
    if (raster.ignoredParticles())
      throw std::runtime_error("OpenVDB unexpectedly ignored particles outside its radius cutoffs");
    raster.finalize(true);
  });
  if (grid->activeVoxelCount() == 0)
    throw std::runtime_error("particle rasterization produced an empty level set");
  if (grid->activeVoxelCount() > processing.maxVoxelCount ||
      grid->memUsage() > processing.maxInputBytes)
    throw std::runtime_error("particle rasterization exceeded the voxel or input-memory budget");
  const ProcessingResult result{grid->getName(), "level set",
      static_cast<uint64_t>(grid->activeVoxelCount()), grid->voxelSize().x()};
  try {
    arena.execute([&] {
      openvdb::io::File file(output.string());
      file.write(openvdb::GridPtrVec{grid});
      file.close();
    });
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    throw;
  }
  return result;
#else
  (void)particles; (void)output; (void)processing; (void)options;
  unavailable();
#endif
}

ProcessingResult WriteParticlesLevelSetFile(
    const std::filesystem::path& input, const std::filesystem::path& output,
    const ProcessingOptions& processing, const ParticleRasterizationOptions& options) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(processing, options);
  validateOutput(output);
  if (!std::filesystem::is_regular_file(input))
    throw std::invalid_argument("particle input is not a regular file");
  const auto inputBytes = std::filesystem::file_size(input);
  if (static_cast<long double>(inputBytes) * 3.0L >
      static_cast<long double>(processing.maxInputBytes))
    throw std::invalid_argument("particle input exceeds the file-size or JSON parsing workspace budget");
  std::ifstream stream(input, std::ios::binary);
  if (!stream) throw std::invalid_argument("could not open particle input");
  BoundedInput buffer(stream, inputBytes);
  std::istream boundedStream(&buffer);
  ParticleJson parser(processing, options, inputBytes);
  const bool parsed = nlohmann::json::sax_parse(boundedStream, &parser);
  if (buffer.tokenExceeded())
    throw std::invalid_argument("particle JSON token exceeds the 128-byte parsing limit");
  if (!parsed)
    throw std::invalid_argument(parser.error.empty() ? "invalid particle JSON" : parser.error);
  if (stream.bad()) throw std::runtime_error("failed while reading particle input");
  if (buffer.remaining() != 0 || stream.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("particle input changed size while being read");
  return WriteParticlesLevelSet(parser.particles, output, processing, options);
#else
  (void)input; (void)output; (void)processing; (void)options;
  unavailable();
#endif
}

}  // namespace container::geometry::vdb
