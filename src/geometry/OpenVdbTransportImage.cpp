#include "Container/geometry/OpenVdbTransportImage.h"
#include "OpenVdbTransportImageInternal.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <tinyexr.h>

namespace container::geometry::vdb {
TransportImageResult RenderTransportImageCpu(const TransportScene &scene,
                                             const TransportPinholeCamera &camera,
                                             const TransportImageOptions &options) {
  return image_detail::render(
      scene, camera, options, 0, 0, false,
      [&](const auto &rays, const auto &o) { return TraceTransportReference(scene, rays, o); });
}
namespace {
std::string metadata(const TransportImageResult &image,
                     const TransportImageExportResult &conversion) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::setprecision(17);
  const auto vector = [&](const TransportVector &v) { out << v[0] << ',' << v[1] << ',' << v[2]; };
  out << "schema=1; units=scene-linear radiance; uncertainty=Monte-Carlo standard error; "
      << "uncertainty_available=" << (image.samplesPerPixel >= 2 ? 1 : 0)
      << "; center_pixel_rays=1; "
      << "backend=" << (image.gpu ? "GPU Float32" : "CPU Double") << "; seed=" << image.seed
      << "; canonical_seed_block_pixels=64; spp=" << image.samplesPerPixel
      << "; max_scattering_events=" << image.maxScatteringEvents
      << "; finite_order_bias=1; width=" << image.width << "; height=" << image.height << "; eye=";
  vector(image.camera.eye);
  out << "; target=";
  vector(image.camera.target);
  out << "; up=";
  vector(image.camera.up);
  out << "; vertical_fov_degrees=" << image.camera.verticalFovDegrees
      << "; preparation_work=" << image.diagnostics.preparationWork
      << "; work=" << image.diagnostics.work << "; paths=" << image.diagnostics.paths
      << "; scattering_events=" << image.diagnostics.scatteringEvents
      << "; truncated_paths=" << image.diagnostics.truncatedPaths
      << "; rounded_float_values=" << conversion.roundedValues
      << "; max_absolute_conversion_error=" << conversion.maxAbsoluteError
      << "; max_relative_conversion_error=" << conversion.maxRelativeError;
  return out.str();
}
} // namespace
TransportImageExportResult WriteTransportImageExr(const std::filesystem::path &output,
                                                  const TransportImageResult &image,
                                                  uint64_t maxBytes) {
  using namespace image_detail;
  if (output.empty())
    throw std::invalid_argument("transport image output path must not be empty");
  if (std::filesystem::exists(output))
    throw std::invalid_argument("transport image output already exists");
  const uint64_t count = mul(image.width, image.height);
  if (!image.width || !image.height || image.width > 2048 || image.height > 2048 ||
      count > 262144 || image.pixels.size() != count || image.samplesPerPixel < 1 ||
      image.samplesPerPixel > 8192 || image.maxScatteringEvents > 64 || !image.maxImageBytes ||
      !maxBytes)
    throw std::invalid_argument("transport image export dimensions/sampling/budgets are invalid");
  const auto limit = std::min(maxBytes, image.maxImageBytes);
  const uint64_t input =
      add(add(image.retainedSceneBytes, mul(count, sizeof(TransportRayEstimate))), Fixed);
  // Uncompressed output: reserve input plus all planar channels, encoded
  // image, TinyEXR scanline/header duplication and temporary row buffers.
  budget(add(input, mul(count, 6 * 4 * 8)), limit,
         "transport image EXR/result/scene workspace exceeds byte budget");
  const Camera camera(image.camera, image.width, image.height);
  (void)camera;
  std::array<std::vector<float>, 6> channels;
  for (auto &c : channels)
    c.resize(size_t(count));
  TransportImageExportResult conversion;
  // EXR readers conventionally expect lexicographically sorted channel names.
  constexpr std::array<const char *, 6> names{"B", "G", "R", "SE.B", "SE.G", "SE.R"};
  constexpr std::array<int, 6> components{2, 1, 0, 2, 1, 0};
  for (size_t i = 0; i < count; ++i)
    for (size_t c = 0; c < 6; ++c) {
      const double value = c < 3 ? image.pixels[i].radiance[components[c]]
                                 : image.pixels[i].standardError[components[c]];
      const float f = static_cast<float>(value);
      if (!std::isfinite(value) || value < 0 || !std::isfinite(f) || (value != 0 && f == 0))
        throw std::invalid_argument("transport image EXR requires finite nonnegative "
                                    "Float32-representable values; conversion overflow/underflow");
      channels[c][i] = f;
      if (double(f) != value) {
        ++conversion.roundedValues;
        const double error = std::abs(double(f) - value);
        conversion.maxAbsoluteError = std::max(conversion.maxAbsoluteError, error);
        if (value != 0)
          conversion.maxRelativeError = std::max(conversion.maxRelativeError, error / value);
      }
    }
  const auto text = metadata(image, conversion);
  if (text.size() > 16 * 1024)
    throw std::invalid_argument("transport image EXR metadata limit exceeded");
  EXRImage native;
  InitEXRImage(&native);
  native.num_channels = 6;
  native.width = int(image.width);
  native.height = int(image.height);
  std::array<unsigned char *, 6> pointers;
  for (size_t i = 0; i < 6; ++i)
    pointers[i] = reinterpret_cast<unsigned char *>(channels[i].data());
  native.images = pointers.data();
  EXRHeader header;
  InitEXRHeader(&header);
  header.num_channels = 6;
  std::array<EXRChannelInfo, 6> info{};
  std::array<int, 6> types;
  types.fill(TINYEXR_PIXELTYPE_FLOAT);
  for (size_t i = 0; i < 6; ++i)
    std::strncpy(info[i].name, names[i], sizeof(info[i].name) - 1);
  header.channels = info.data();
  header.pixel_types = types.data();
  header.requested_pixel_types = types.data();
  header.compression_type = TINYEXR_COMPRESSIONTYPE_NONE;
  EXRAttribute attribute{};
  std::strcpy(attribute.name, "container.transport");
  std::strcpy(attribute.type, "string");
  attribute.value = reinterpret_cast<unsigned char *>(const_cast<char *>(text.data()));
  attribute.size = int(text.size());
  header.num_custom_attributes = 1;
  header.custom_attributes = &attribute;
  unsigned char *encoded = nullptr;
  const char *error = nullptr;
  const size_t size = SaveEXRImageToMemory(&native, &header, &encoded, &error);
  struct Cleanup {
    unsigned char *&data;
    const char *&error;
    ~Cleanup() {
      std::free(data);
      if (error)
        FreeEXRErrorMessage(error);
    }
  } cleanup{encoded, error};
  if (!size || !encoded)
    throw std::runtime_error(std::string("transport image EXR serialization failed: ") +
                             (error ? error : "unknown error"));
  budget(add(input, size), limit, "transport image encoded EXR exceeds byte budget");
  std::ofstream file;
  bool created = false;
  try {
    file.open(output, std::ios::binary | std::ios::out | std::ios::noreplace);
    created = file.is_open();
    if (!created)
      throw std::runtime_error("could not exclusively create transport image EXR");
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.write(reinterpret_cast<const char *>(encoded), std::streamsize(size));
    file.flush();
    file.close();
  } catch (...) {
    file.exceptions(std::ios::goodbit);
    if (file.is_open())
      file.close();
    if (created) {
      std::error_code ignored;
      std::filesystem::remove(output, ignored);
    }
    throw;
  }
  conversion.encodedBytes = size;
  return conversion;
}
} // namespace container::geometry::vdb
