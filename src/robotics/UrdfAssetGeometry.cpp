#include "UrdfAssetGeometry.h"

#include "Container/geometry/Model.h"
#include "Container/utility/Platform.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>

namespace container::robotics::detail {
namespace {

namespace fs = std::filesystem;

constexpr size_t maxStlTriangles = 2'000'000;
constexpr uintmax_t maxStlBytes = 256ull * 1024 * 1024;
constexpr size_t binaryStlHeaderBytes = 84;
constexpr size_t binaryStlFacetBytes = 50;

float dimension(double value) {
  if (!std::isfinite(value) || value <= 0.0 || value > std::numeric_limits<float>::max() ||
      static_cast<float>(value) == 0.0f)
    throw std::invalid_argument("URDF shape dimensions must fit finite positive vertex coordinates");
  return static_cast<float>(value);
}

UrdfAssetGeometry scalePrimitive(const geometry::Model& model, const glm::dvec3& scale) {
  UrdfAssetGeometry result;
  result.indices = model.indices();
  result.positions.reserve(model.vertices().size());
  result.normals.reserve(model.vertices().size());
  for (const auto& vertex : model.vertices()) {
    result.positions.emplace_back(glm::dvec3(vertex.position) * scale);
    result.normals.push_back(vertex.normal);
  }
  return result;
}

UrdfAssetGeometry cylinderGeometry(const UrdfCylinder& cylinder) {
  constexpr uint32_t segments = 32;
  const auto radius = dimension(cylinder.radius);
  const auto halfHeight = dimension(cylinder.length * 0.5);
  UrdfAssetGeometry result;
  result.positions.reserve(4 * segments + 2);
  result.normals.reserve(4 * segments + 2);
  result.indices.reserve(12 * segments);
  std::array<glm::vec2, segments> directions;
  const auto vertex = [&](glm::vec3 position, glm::vec3 normal) {
    result.positions.push_back(position);
    result.normals.push_back(normal);
  };
  for (uint32_t i = 0; i < segments; ++i) {
    const double angle = (2.0 * std::numbers::pi) * i / segments;
    const glm::vec2 direction(std::cos(angle), std::sin(angle));
    directions[i] = direction;
    const glm::vec3 normal(direction, 0.0f);
    vertex({radius * direction, -halfHeight}, normal);
    vertex({radius * direction, halfHeight}, normal);
  }
  for (uint32_t i = 0; i < segments; ++i) {
    const auto low = 2u * i;
    const auto next = 2u * ((i + 1) % segments);
    result.indices.insert(result.indices.end(), {low, next, low + 1, low + 1, next, next + 1});
  }
  for (const bool top : {false, true}) {
    const float sign = top ? 1.0f : -1.0f;
    const glm::vec3 normal(0.0f, 0.0f, sign);
    const auto center = static_cast<uint32_t>(result.positions.size());
    vertex({0.0f, 0.0f, sign * halfHeight}, normal);
    for (const auto direction : directions)
      vertex({radius * direction, sign * halfHeight}, normal);
    for (uint32_t i = 0; i < segments; ++i) {
      const auto current = center + 1 + i;
      const auto next = center + 1 + ((i + 1) % segments);
      result.indices.insert(result.indices.end(), {center, top ? current : next, top ? next : current});
    }
  }
  return result;
}

bool containedBy(const fs::path& path, const fs::path& root) {
  const auto relative = path.lexically_relative(root);
  return !relative.empty() && !relative.has_root_path() && *relative.begin() != "..";
}

bool remotePath(std::string_view path) {
  // UNC/file device syntax can trigger network access or name a special file.
  return path.size() >= 2 && (path[0] == '/' || path[0] == '\\') &&
         (path[1] == '/' || path[1] == '\\');
}

fs::path resolvePackageMesh(std::string_view uri, std::span<const fs::path> packageRoots) {
  const auto separator = uri.find('/');
  const auto package = uri.substr(0, separator);
  if (separator == std::string_view::npos || package.empty() || separator + 1 == uri.size() ||
      !std::ranges::all_of(package, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
      }))
    throw std::invalid_argument("expected package://name/path");
  const auto paths = uri.substr(separator + 1);
  const auto relative = util::pathFromUtf8(paths);
  if (relative.has_root_path() || paths.find_first_of("\\:?#") != std::string_view::npos)
    throw std::invalid_argument("package mesh path must be relative to its package");

  const auto packageName = util::pathFromUtf8(package);
  for (const auto& root : packageRoots) {
    std::error_code error;
    const auto search = fs::canonical(root, error);
    if (error) continue;
    const auto directory = fs::canonical(search / packageName, error);
    if (error) continue;
    if (!containedBy(directory, search))
      throw std::invalid_argument("resolved package escapes its package root");
    // Check lexical escapes before resolving symlinks, then verify the target's
    // canonical path too so an internal symlink cannot leave the package.
    const auto candidate = directory / relative;
    if (!containedBy(candidate.lexically_normal(), directory))
      throw std::invalid_argument("mesh path escapes its package");
    const auto resolved = fs::canonical(candidate, error);
    if (error) continue;
    if (!containedBy(resolved, directory))
      throw std::invalid_argument("resolved mesh escapes its package");
    if (fs::is_regular_file(resolved, error)) return resolved;
  }
  throw std::runtime_error("package mesh was not found in the configured package roots");
}

fs::path resolveMesh(std::string_view uri, const fs::path& urdfPath,
                     std::span<const fs::path> packageRoots) {
  constexpr std::string_view packagePrefix = "package://";
  if (uri.starts_with(packagePrefix))
    return resolvePackageMesh(uri.substr(packagePrefix.size()), packageRoots);
  if (uri.find("://") != std::string_view::npos || remotePath(uri))
    throw std::invalid_argument("supported mesh sources are local paths and package:// URIs");
  const auto path = util::pathFromUtf8(uri);
  const auto candidate = path.is_absolute() ? path : urdfPath.parent_path() / path;
  if (!fs::is_regular_file(candidate))
    throw std::runtime_error("mesh file was not found");
  return candidate;
}

uint32_t littleU32(const unsigned char* bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
         (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

void appendFacet(UrdfAssetGeometry& result, const std::array<glm::vec3, 3>& points) {
  if (result.indices.size() / 3 >= maxStlTriangles)
    throw std::length_error("STL exceeds the triangle count limit");
  for (const auto& point : points)
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
      throw std::invalid_argument("STL contains a nonfinite vertex");

  const auto a = glm::dvec3(points[0]);
  const auto b = glm::dvec3(points[1]);
  const auto c = glm::dvec3(points[2]);
  const auto cross = glm::cross(b - a, c - a);
  const auto length = glm::length(cross);
  if (length == 0.0)
    throw std::invalid_argument("STL contains a degenerate triangle");
  // Some exporters write zero or stale facet normals. Winding controls both
  // the visual surface orientation and the generated normal.
  const auto normal = glm::vec3(cross / length);
  for (const auto& point : points) {
    result.indices.push_back(static_cast<uint32_t>(result.positions.size()));
    result.positions.push_back(point);
    result.normals.push_back(normal);
  }
}

UrdfAssetGeometry parseBinaryStl(std::istream& input, size_t count) {
  if (count == 0)
    throw std::invalid_argument("STL contains no triangles");
  if (count > maxStlTriangles)
    throw std::length_error("STL exceeds the triangle count limit");
  UrdfAssetGeometry result;
  result.positions.reserve(count * 3);
  result.normals.reserve(count * 3);
  result.indices.reserve(count * 3);
  std::array<unsigned char, binaryStlFacetBytes> record;
  for (size_t i = 0; i < count; ++i) {
    if (!input.read(reinterpret_cast<char*>(record.data()), record.size()))
      throw std::invalid_argument("binary STL ended in a triangle record");
    std::array<glm::vec3, 3> positions;
    for (size_t vertex = 0; vertex < positions.size(); ++vertex)
      for (glm::length_t axis = 0; axis < 3; ++axis)
        positions[vertex][axis] = std::bit_cast<float>(littleU32(record.data() + 12 + 12 * vertex + 4 * axis));
    appendFacet(result, positions);
  }
  if (input.peek() != std::char_traits<char>::eof())
    throw std::invalid_argument("binary STL contains trailing data");
  return result;
}

bool asciiSpace(int value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\f' || value == '\v';
}

std::string asciiToken(std::istream& input) {
  std::string token;
  input >> std::setw(80) >> token;
  const auto next = input.peek();
  if (next != std::char_traits<char>::eof() && !asciiSpace(next))
    throw std::invalid_argument("ASCII STL contains an overlong token");
  return token;
}

void expectAscii(std::istream& input, std::string_view expected) {
  if (asciiToken(input) != expected)
    throw std::invalid_argument("ASCII STL expected " + std::string(expected));
}

float asciiNumber(std::istream& input) {
  const auto token = asciiToken(input);
  const std::string_view value = token;
  const auto numeric = value.starts_with('+') ? value.substr(1) : value;
  float result{};
  const auto end = numeric.data() + numeric.size();
  const auto parsed = std::from_chars(numeric.data(), end, result);
  if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(result) ||
      (value.starts_with('+') && numeric.starts_with('-')))
    throw std::invalid_argument("ASCII STL expected a finite vertex or normal coordinate");
  return result;
}

UrdfAssetGeometry parseAsciiStl(std::istream& input) {
  input.imbue(std::locale::classic());
  UrdfAssetGeometry result;
  auto token = asciiToken(input);
  if (token.starts_with("\xef\xbb\xbf")) token.erase(0, 3);
  if (token != "solid")
    throw std::invalid_argument("invalid STL header or truncated binary STL");
  do {
    // The name after solid/endsolid is optional and does not affect geometry.
    input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    token = asciiToken(input);
    while (token == "facet") {
      expectAscii(input, "normal");
      for (int axis = 0; axis < 3; ++axis) (void)asciiNumber(input);
      expectAscii(input, "outer");
      expectAscii(input, "loop");
      std::array<glm::vec3, 3> positions;
      for (auto& position : positions) {
        expectAscii(input, "vertex");
        for (glm::length_t axis = 0; axis < 3; ++axis) position[axis] = asciiNumber(input);
      }
      expectAscii(input, "endloop");
      expectAscii(input, "endfacet");
      appendFacet(result, positions);
      token = asciiToken(input);
    }
    if (token != "endsolid")
      throw std::invalid_argument("ASCII STL expected facet or endsolid");
    input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    token = asciiToken(input);
  } while (token == "solid");
  if (!token.empty() || input.bad())
    throw std::invalid_argument("ASCII STL contains trailing data or could not be read");
  if (result.indices.empty())
    throw std::invalid_argument("STL contains no triangles");
  return result;
}

UrdfAssetGeometry loadStl(const fs::path& path) {
  auto extension = util::pathToUtf8(path.extension());
  std::ranges::transform(extension, extension.begin(), [](char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
  });
  if (extension != ".stl")
    throw std::invalid_argument("unsupported URDF mesh format; expected a binary or ASCII .stl file");

  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    throw std::runtime_error("could not open STL file");
  const auto end = input.tellg();
  if (end < 0 || static_cast<uintmax_t>(end) > maxStlBytes)
    throw std::length_error("STL exceeds the supported input size or could not be inspected");
  input.seekg(0);
  std::array<unsigned char, binaryStlHeaderBytes> header;
  if (input.read(reinterpret_cast<char*>(header.data()), header.size())) {
    // A legal binary header may itself begin with 'solid'. The exact byte
    // count disambiguates the two encodings without relying on that word.
    const auto count = littleU32(header.data() + 80);
    if (binaryStlHeaderBytes + uint64_t(count) * binaryStlFacetBytes == static_cast<uintmax_t>(end))
      return parseBinaryStl(input, count);
  }
  input.clear();
  input.seekg(0);
  return parseAsciiStl(input);
}

} // namespace

UrdfAssetGeometry loadUrdfAssetGeometry(const UrdfGeometry& shape, const fs::path& urdfPath,
                                       std::span<const fs::path> packageRoots) {
  if (const auto* box = std::get_if<UrdfBox>(&shape)) {
    // The reusable cube spans [-0.5, 0.5]. Only its half-extents must fit the
    // float vertex stream, so scale the vertices before narrowing to float.
    for (glm::length_t axis = 0; axis < 3; ++axis) (void)dimension(box->size[axis] * 0.5);
    return scalePrimitive(geometry::Model::MakeCube(), box->size);
  }
  if (const auto* sphere = std::get_if<UrdfSphere>(&shape))
    return scalePrimitive(geometry::Model::MakeSphere(16, 32, 1.0f), glm::dvec3(dimension(sphere->radius)));
  if (const auto* cylinder = std::get_if<UrdfCylinder>(&shape))
    return cylinderGeometry(*cylinder);
  const auto& mesh = std::get<UrdfMesh>(shape);
  try {
    return loadStl(resolveMesh(mesh.uri, urdfPath, packageRoots));
  } catch (const std::exception& error) {
    throw std::runtime_error("URDF mesh '" + mesh.uri + "': " + error.what());
  }
}

} // namespace container::robotics::detail
