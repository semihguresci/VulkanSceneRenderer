#include "Container/geometry/OpenVdbTransportRenderJob.h"
#include "../../geometry/OpenVdbTransportImageInternal.h"
#include "Container/renderer/effects/OpenVdbTransportImageGpu.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
namespace {
namespace vdb = container::geometry::vdb;
using Json = nlohmann::json;
struct JsonLimits final : nlohmann::json_sax<Json> {
  struct Frame {
    bool object;
    std::set<std::string> keys;
  };
  std::vector<Frame> frames;
  uint64_t nodes{}, strings{};
  std::string error;
  bool count() {
    if (++nodes > 65536) {
      error = "scene JSON exceeds65536 nodes";
      return false;
    }
    return true;
  }
  bool text(const std::string &s) {
    strings += s.size();
    if (s.size() > 65536 || strings > 1024 * 1024) {
      error = "scene JSON strings exceed limit";
      return false;
    }
    return true;
  }
  bool null() override { return count(); }
  bool boolean(bool) override { return count(); }
  bool number_integer(Json::number_integer_t) override { return count(); }
  bool number_unsigned(Json::number_unsigned_t) override { return count(); }
  bool number_float(Json::number_float_t v, const std::string &token) override {
    if (!std::isfinite(v)) {
      error = "scene JSON numbers must be finite";
      return false;
    }
    if (v == 0)
      for (char c : token) {
        if (c == 'e' || c == 'E')
          break;
        if (c >= '1' && c <= '9') {
          error = "scene JSON nonzero number underflows Double";
          return false;
        }
      }
    return count();
  }
  bool string(std::string &s) override { return count() && text(s); }
  bool binary(Json::binary_t &) override {
    error = "scene JSON binary values unsupported";
    return false;
  }
  bool start_object(std::size_t) override { return start(true); }
  bool key(std::string &s) override {
    if (!text(s) || frames.empty() || !frames.back().object)
      return false;
    if (!frames.back().keys.insert(s).second) {
      error = "duplicate scene JSON key";
      return false;
    }
    return count();
  }
  bool end_object() override {
    frames.pop_back();
    return true;
  }
  bool start_array(std::size_t) override { return start(false); }
  bool end_array() override {
    frames.pop_back();
    return true;
  }
  bool start(bool object) {
    if (frames.size() >= 12) {
      error = "scene JSON depth exceeds12";
      return false;
    }
    frames.push_back({object, {}});
    return count();
  }
  bool parse_error(std::size_t, const std::string &,
                   const nlohmann::detail::exception &e) override {
    error = e.what();
    return false;
  }
};
void object(const Json &j, std::initializer_list<const char *> allowed) {
  if (!j.is_object())
    throw std::invalid_argument("scene JSON expected object");
  for (auto it = j.begin(); it != j.end(); ++it) {
    bool found = false;
    for (const auto *k : allowed)
      found |= it.key() == k;
    if (!found)
      throw std::invalid_argument("unknown scene JSON field: " + it.key());
  }
}
double number(const Json &j) {
  if (!j.is_number())
    throw std::invalid_argument("scene JSON expected number");
  const double x = j.get<double>();
  if (!std::isfinite(x))
    throw std::invalid_argument("scene JSON number must be finite");
  return x;
}
uint32_t integer(const Json &j, uint32_t maximum) {
  if (!j.is_number_unsigned() && !j.is_number_integer())
    throw std::invalid_argument("scene JSON expected nonnegative integer");
  if (j.is_number_integer() && j.get<int64_t>() < 0)
    throw std::invalid_argument("scene JSON expected nonnegative integer");
  const auto n = j.get<uint64_t>();
  if (n > maximum)
    throw std::invalid_argument("scene JSON integer exceeds range");
  return uint32_t(n);
}
vdb::TransportVector vector(const Json &j) {
  if (!j.is_array() || j.size() != 3)
    throw std::invalid_argument("scene JSON expected3-vector");
  return {number(j[0]), number(j[1]), number(j[2])};
}
std::string string(const Json &j) {
  if (!j.is_string())
    throw std::invalid_argument("scene JSON expected string");
  auto s = j.get<std::string>();
  if (s.empty() || s.find('\0') != std::string::npos)
    throw std::invalid_argument("scene JSON path/type strings must be nonempty without NUL");
  return s;
}
struct Scene {
  vdb::TransportImageSceneDescription description;
  vdb::TransportPinholeCamera camera;
  vdb::TransportImageOptions options;
};
Scene read(const std::filesystem::path &path, uint64_t bytes, uint64_t maximum,
           const vdb::TransportImageOptions &defaults) {
  if (!std::filesystem::is_regular_file(path))
    throw std::invalid_argument("scene JSON input is not a regular file");
  const auto size = std::filesystem::file_size(path);
  if (size > maximum || size > bytes / 64)
    throw std::invalid_argument("scene JSON encoded/DOM workspace exceeds budget");
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::invalid_argument("cannot open scene JSON");
  std::string text(size_t(size), '\0');
  file.read(text.data(), std::streamsize(size));
  if (!file || file.peek() != std::char_traits<char>::eof())
    throw std::invalid_argument("scene JSON file changed size or could not be read");
  JsonLimits sax;
  if (!Json::sax_parse(text, &sax))
    throw std::invalid_argument("invalid bounded scene JSON: " + sax.error);
  const auto root = Json::parse(text);
  object(root, {"schemaVersion", "camera", "media", "lights", "occluders", "environment"});
  if (!root.contains("schemaVersion") || integer(root.at("schemaVersion"), 1) != 1)
    throw std::invalid_argument("scene JSON requires schemaVersion1");
  Scene out;
  out.options = defaults;
  out.description.maxInputBytes = bytes;
  out.options.maxImageBytes = bytes;
  if (root.contains("camera")) {
    const auto &c = root.at("camera");
    object(c, {"eye", "target", "up", "verticalFovDegrees", "width", "height"});
    if (c.contains("eye"))
      out.camera.eye = vector(c.at("eye"));
    if (c.contains("target"))
      out.camera.target = vector(c.at("target"));
    if (c.contains("up"))
      out.camera.up = vector(c.at("up"));
    if (c.contains("verticalFovDegrees"))
      out.camera.verticalFovDegrees = number(c.at("verticalFovDegrees"));
    if (c.contains("width"))
      out.options.width = integer(c.at("width"), 2048);
    if (c.contains("height"))
      out.options.height = integer(c.at("height"), 2048);
  }
  if (root.contains("environment"))
    out.description.environment = vector(root.at("environment"));
  if (root.contains("media")) {
    const auto &media = root.at("media");
    if (!media.is_array() || media.size() > 4)
      throw std::invalid_argument("scene JSON media must contain at most4 sources");
    for (const auto &m : media) {
      object(m, {"input", "gridName", "maxResolution", "importScale", "nativeSparse", "extinction",
                 "albedo", "anisotropy", "emission"});
      vdb::TransportImageVolumeSource source;
      source.input = std::filesystem::u8path(string(m.at("input")));
      if (source.input.is_relative())
        source.input = path.parent_path() / source.input;
      if (m.contains("gridName"))
        source.volume.gridName = string(m.at("gridName"));
      if (m.contains("maxResolution"))
        source.volume.maxResolution = integer(m.at("maxResolution"), 256);
      if (m.contains("importScale")) {
        const double x = number(m.at("importScale"));
        source.volume.importScale = float(x);
        if (!std::isfinite(source.volume.importScale) || (x != 0 && source.volume.importScale == 0))
          throw std::invalid_argument("scene importScale cannot convert toFloat32");
      }
      if (m.contains("nativeSparse")) {
        if (!m.at("nativeSparse").is_boolean())
          throw std::invalid_argument("nativeSparse must be boolean");
        source.volume.sparse = m.at("nativeSparse").get<bool>();
      }
      if (m.contains("extinction"))
        source.material.extinction = number(m.at("extinction"));
      if (m.contains("albedo"))
        source.material.albedo = vector(m.at("albedo"));
      if (m.contains("anisotropy"))
        source.material.anisotropy = number(m.at("anisotropy"));
      if (m.contains("emission"))
        source.material.emission = vector(m.at("emission"));
      out.description.volumes.push_back(std::move(source));
    }
  }
  if (root.contains("lights")) {
    const auto &lights = root.at("lights");
    if (!lights.is_array() || lights.size() > 32)
      throw std::invalid_argument("scene JSON lights must contain at most32 lights");
    for (const auto &l : lights) {
      object(l, {"type", "vector", "value"});
      vdb::TransportLight light;
      const auto type = string(l.at("type"));
      if (type == "directional")
        light.type = vdb::TransportLightType::Directional;
      else if (type == "point")
        light.type = vdb::TransportLightType::Point;
      else
        throw std::invalid_argument("scene JSON light type must be directional or point");
      light.vector = vector(l.at("vector"));
      light.value = vector(l.at("value"));
      out.description.lights.push_back(light);
    }
  }
  if (root.contains("occluders")) {
    const auto &triangles = root.at("occluders");
    if (!triangles.is_array() || triangles.size() > 4096)
      throw std::invalid_argument("scene JSON supports at most4096 black triangles");
    for (const auto &t : triangles) {
      if (!t.is_array() || t.size() != 3)
        throw std::invalid_argument("scene JSON triangle requires3 vertices");
      out.description.occluders.push_back({vector(t[0]), vector(t[1]), vector(t[2])});
    }
  }
  return out;
}
} // namespace

namespace container::geometry::vdb {
namespace {
uint64_t pathBytes(const std::filesystem::path &path) {
  return image_detail::mul(path.native().size(), sizeof(std::filesystem::path::value_type));
}
uint64_t descriptionBytes(const TransportImageSceneDescription &desc) {
  using namespace image_detail;
  uint64_t bytes = add(sizeof(desc), add(mul(desc.lights.size(), sizeof(TransportLight)),
                                         mul(desc.occluders.size(), sizeof(TransportTriangle))));
  for (const auto &v : desc.volumes)
    bytes = add(bytes,
                add(sizeof(v), add(pathBytes(v.input), add(v.volume.gridName.size(),
                                                           v.volume.temperatureGridName.size()))));
  return bytes;
}
void validate(const TransportRenderJobOptions &o) {
  const auto &p = o.image.transport;
  if (o.backend != TransportRenderBackend::Cpu && o.backend != TransportRenderBackend::Gpu)
    throw std::invalid_argument("transport render backend is invalid");
  if (!o.maxSpecBytes || o.maxSpecBytes > 8 * 1024 * 1024 || !o.maxSceneBytes ||
      o.maxSceneBytes > 4ull * 1024 * 1024 * 1024 || !o.maxDensityValues ||
      o.maxDensityValues > 16'000'000 || o.maxWorkPerPath < 1 || o.maxWorkPerPath > 65536 ||
      !o.image.maxImageBytes || o.image.maxImageBytes > 4ull * 1024 * 1024 * 1024 ||
      o.image.width < 1 || o.image.width > 2048 || o.image.height < 1 || o.image.height > 2048 ||
      !o.image.maxPixels || o.image.maxPixels > 262144 || !o.image.maxPaths ||
      o.image.maxPaths > 16'000'000 || p.samplesPerRay < 1 || p.samplesPerRay > 8192 ||
      p.maxScatteringEvents > (o.backend == TransportRenderBackend::Gpu ? 16u : 64u) ||
      p.rouletteStart < 1 || p.rouletteStart > 65 || !p.maxWork || p.maxWork > 1'000'000'000 ||
      !p.maxInputBytes || !p.maxDensityValues || !std::isfinite(p.emissionSamplingRate) ||
      p.emissionSamplingRate < 1e-6 || p.emissionSamplingRate > 1e6)
    throw std::invalid_argument("transport render job sampling/work/payload caps are invalid");
  if (o.sceneSpec.empty() || o.output.empty() ||
      (o.backend == TransportRenderBackend::Gpu && o.spirv.empty()))
    throw std::invalid_argument("transport render job requires spec/output and GPU shader paths");
}
} // namespace
TransportRenderJobResult RunTransportRenderJob(const TransportRenderJobOptions &o) {
  using namespace image_detail;
  validate(o); // Authoritative external caps precede ANY path/file access.
  uint64_t metadata = add(Fixed, add(sizeof(o), sizeof(TransportRenderJobResult)));
  metadata = add(metadata,
                 mul(add(add(pathBytes(o.sceneSpec), pathBytes(o.output)), pathBytes(o.spirv)), 2));
  budget(metadata, o.image.maxImageBytes,
         "transport render job path/options workspace exceeds image cap");
  const auto specPath = std::filesystem::absolute(o.sceneSpec);
  const auto outputPath = std::filesystem::absolute(o.output);
  const auto shaderPath =
      o.spirv.empty() ? std::filesystem::path{} : std::filesystem::absolute(o.spirv);
  if (std::filesystem::exists(outputPath))
    throw std::invalid_argument("transport image output already exists");
  const auto parent = outputPath.parent_path();
  if (!std::filesystem::is_directory(parent))
    throw std::invalid_argument("transport image output parent is not a directory");
  auto spec = read(specPath, o.image.maxImageBytes - metadata, o.maxSpecBytes, o.image);
  metadata = add(metadata, descriptionBytes(spec.description));
  budget(metadata, o.image.maxImageBytes,
         "transport render job spec/path payload exceeds image cap");
  const uint64_t count = mul(spec.options.width, spec.options.height);
  if (!spec.options.width || !spec.options.height || count > o.image.maxPixels ||
      mul(count, o.image.transport.samplesPerRay) > o.image.maxPaths)
    throw std::invalid_argument(
        "transport render job spec dimensions exceed external pixel/path caps");
  const Camera check(spec.camera, spec.options.width, spec.options.height);
  (void)check;
  spec.description.maxInputBytes = std::min(o.maxSceneBytes, o.image.maxImageBytes - metadata);
  spec.description.maxDensityValues =
      std::min(o.maxDensityValues, o.image.transport.maxDensityValues);
  TransportRenderJobResult result;
  result.output = outputPath;
  std::shared_ptr<const TransportImageScene> loaded;
  TransportScene direct;
  const TransportScene *scene = nullptr;
  std::array<std::shared_ptr<const renderer::TransportGpuNativeSource>, 4> sources{};
  if (spec.description.volumes.empty()) {
    const uint64_t scenePayload =
        add(sizeof(TransportScene),
            add(mul(spec.description.lights.size(), sizeof(TransportLight)),
                mul(spec.description.occluders.size(), sizeof(TransportTriangle))));
    budget(add(scenePayload, Fixed), spec.description.maxInputBytes,
           "transport render direct scene exceeds external scene byte cap");
    direct.lights = std::move(spec.description.lights);
    direct.occluders = std::move(spec.description.occluders);
    direct.environment = spec.description.environment;
    scene = &direct;
  } else {
    if (!openVdbVolumeAvailable())
      throw std::runtime_error(
          "OpenVDB render job source loading is unavailable; configure with -DENABLE_OPENVDB=ON.");
    if (o.backend == TransportRenderBackend::Cpu)
      for (const auto &v : spec.description.volumes)
        if (v.volume.sparse)
          throw std::invalid_argument(
              "CPU image rendering requires dense sources; nativeSparse is GPU-only");
    loaded = LoadTransportImageScene(spec.description);
    scene = &loaded->scene();
    sources = loaded->nativeSources();
    uint64_t info = 0;
    for (const auto &s : loaded->sources())
      info = add(info, add(sizeof(s), add(pathBytes(s.input), s.gridName.size())));
    metadata = add(metadata, mul(info, 2));
    budget(metadata, o.image.maxImageBytes,
           "transport render source diagnostics exceed image byte cap");
    uint64_t resident = sceneBytes(*scene);
    for (const auto &source : sources)
      if (source) {
        const auto &v = source->volume();
        resident =
            add(resident, add(sizeof(v), add(v.gridName.size(), v.temperatureGridName.size())));
        resident = add(resident, mul(add(add(v.density.size(), v.temperature.size()),
                                         add(v.sparseDensity.size(), v.sparseTemperature.size())),
                                     4));
      }
    budget(add(metadata, resident), o.image.maxImageBytes,
           "transport render retained source/diagnostic snapshot exceeds shared byte cap");
    result.sources = loaded->sources();
  }
  spec.description = {}; // Release descriptor/path duplicates before transport.
  spec.options.maxImageBytes = o.image.maxImageBytes - metadata;
  spec.options.transport.maxInputBytes =
      std::min(o.image.transport.maxInputBytes, spec.options.maxImageBytes);
  spec.options.transport.maxDensityValues =
      std::min(o.maxDensityValues, o.image.transport.maxDensityValues);
  if (o.backend == TransportRenderBackend::Cpu)
    result.image = RenderTransportImageCpu(*scene, spec.camera, spec.options);
  else {
    renderer::TransportImageGpuSession session(shaderPath, o.validation);
    result.image = session.render(*scene, spec.camera, spec.options, sources, o.maxWorkPerPath);
  }
  result.exported = WriteTransportImageExr(outputPath, result.image, spec.options.maxImageBytes);
  return result;
}
} // namespace container::geometry::vdb
