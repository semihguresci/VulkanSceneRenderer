// Include in OpenVdbTool private namespace after path(); requires public
// OpenVdbSurfaceReference.h and IsAvailable declaration, plus <charconv>,<set>,
// <optional>,<array>,<cmath>. Dispatch runSurfaceReferenceCommand before legacy
// parsing. Runtime availability, never Tool-private SDK macros.
namespace surface_reference_cli {
bool missing(std::string_view text) noexcept {
  return text.empty() || text == "-h" || text.starts_with("--");
}
bool utf8(std::string_view text) noexcept {
  for (size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i++]);
    if (c < 0x80)
      continue;
    size_t n = 0;
    unsigned char low = 0x80, high = 0xbf;
    if (c >= 0xc2 && c <= 0xdf)
      n = 1;
    else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      if (c == 0xe0)
        low = 0xa0;
      if (c == 0xed)
        high = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      if (c == 0xf0)
        low = 0x90;
      if (c == 0xf4)
        high = 0x8f;
    } else
      return false;
    if (n > text.size() - i)
      return false;
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < low || first > high)
      return false;
    while (--n) {
      const auto next = static_cast<unsigned char>(text[i++]);
      if (next < 0x80 || next > 0xbf)
        return false;
    }
  }
  return true;
}
std::string_view argument(int argc, char **argv, int &i,
                          std::string_view option) {
  if (++i >= argc || !argv[i] || missing(argv[i]))
    throw std::invalid_argument("nonempty value required for " +
                                std::string(option));
  return argv[i];
}
double finite(std::string_view text, std::string_view option) {
  if (text.size() > 128)
    throw std::invalid_argument(
        "surface reference numeric literals are limited to128 bytes");
  if (text.starts_with('+')) {
    text.remove_prefix(1);
    if (text.starts_with('+') || text.starts_with('-'))
      throw std::invalid_argument("invalid surface reference decimal for " +
                                  std::string(option));
  }
  double out = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                      out, std::chars_format::general);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() || !std::isfinite(out))
    throw std::invalid_argument("finite representable Double required for " +
                                std::string(option));
  return out;
}
uint64_t integer(std::string_view text, std::string_view option,
                 uint64_t maximum, bool allowZero = false) {
  if (text.size() > 128)
    throw std::invalid_argument(
        "surface reference count literals are limited to128 bytes");
  uint64_t out = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), out);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() || (!out && !allowZero) ||
      out > maximum)
    throw std::invalid_argument(
        "unsigned surface reference count is outside its allowed range: " +
        std::string(option));
  return out;
}

struct Parsed {
  std::string_view input, output, grid;
  vdb::SurfaceReferenceCamera camera;
  vdb::SurfaceReferenceOptions image;
  vdb::ProcessingOptions processing;
  uint64_t maxExportBytes{512ull * 1024 * 1024};
};
Parsed parse(int argc, char **argv) {
  if (argc < 4 || missing(argv[2]) || missing(argv[3]))
    throw std::invalid_argument("--surface-reference requires nonempty INPUT "
                                "and OUTPUT.exr before options");
  Parsed p;
  p.input = argv[2];
  p.output = argv[3];
  std::set<std::string_view> seen;
  for (int i = 4; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto key = arg == "--input-grid"     ? std::string_view("--grid")
                     : arg == "--vertical-fov" ? std::string_view("--fov")
                     : arg == "--orthographic-height"
                         ? std::string_view("--ortho-height")
                         : arg;
    if (!seen.insert(key).second)
      throw std::invalid_argument("duplicate surface-reference option: " +
                                  std::string(key));
    const auto value = [&] { return argument(argc, argv, i, arg); };
    const auto real = [&] { return finite(value(), arg); };
    const auto count = [&](uint64_t max, bool zero = false) {
      return integer(value(), arg, max, zero);
    };
    if (key == "--grid")
      p.grid = value();
    else if (arg == "--projection") {
      const auto s = value();
      if (s == "perspective")
        p.camera.projection = vdb::SurfaceProjection::Perspective;
      else if (s == "orthographic")
        p.camera.projection = vdb::SurfaceProjection::Orthographic;
      else
        throw std::invalid_argument(
            "--projection requires perspective|orthographic");
    } else if (arg == "--eye" || arg == "--target" || arg == "--up") {
      auto &v = arg == "--eye"      ? p.camera.eye
                : arg == "--target" ? p.camera.target
                                    : p.camera.up;
      for (auto &x : v)
        x = real();
    } else if (key == "--fov")
      p.camera.verticalFovDegrees = real();
    else if (key == "--ortho-height")
      p.camera.orthographicHeight = real();
    else if (arg == "--ray-min")
      p.camera.minimumDistance = real();
    else if (arg == "--ray-max")
      p.camera.maximumDistance = real();
    else if (arg == "--width")
      p.image.width = uint32_t(count(2048));
    else if (arg == "--height")
      p.image.height = uint32_t(count(2048));
    else if (arg == "--max-pixels")
      p.image.maxPixels = count(262144);
    else if (arg == "--surface-shader") {
      const auto s = value();
      if (s == "matte")
        p.image.shading = vdb::SurfaceShading::Matte;
      else if (s == "normal")
        p.image.shading = vdb::SurfaceShading::WorldNormal;
      else if (s == "headlight")
        p.image.shading = vdb::SurfaceShading::TwoSidedHeadlight;
      else
        throw std::invalid_argument(
            "--surface-shader requires matte|normal|headlight");
    } else if (arg == "--color" || arg == "--background" ||
               arg == "--invalid-normal-color") {
      auto &v = arg == "--color"        ? p.image.color
                : arg == "--background" ? p.image.background
                                        : p.image.invalidNormalColor;
      for (auto &x : v) {
        x = real();
        if (x < 0 || x > 1)
          throw std::invalid_argument(
              "surface-reference colors require values within[0,1]");
      }
    } else if (arg == "--ray-isovalue")
      p.image.rays.isovalue = real();
    else if (arg == "--ray-refinements")
      p.image.rays.refinementSteps = uint32_t(count(8, true));
    else if (arg == "--ray-normals") {
      const auto s = value();
      if (s != "on" && s != "off")
        throw std::invalid_argument("--ray-normals requires on|off");
      p.image.rays.computeNormals = s == "on";
    } else if (arg == "--max-rays")
      p.image.rays.limits.maxRays = uint32_t(count(1'000'000));
    else if (arg == "--max-ray-grids")
      p.image.rays.limits.maxGrids = uint32_t(count(4096));
    else if (arg == "--max-ray-work")
      p.image.rays.limits.maxWorkItems = count(10'000'000'000ull);
    else if (arg == "--max-ray-metadata")
      p.image.rays.limits.maxMetadataBytes = count(UINT64_MAX);
    else if (arg == "--max-voxels")
      p.processing.maxVoxelCount = count(UINT64_MAX);
    else if (arg == "--max-input-bytes")
      p.processing.maxInputBytes = count(16ull << 30);
    else if (arg == "--max-export-bytes")
      p.maxExportBytes = count(16ull << 30);
    else
      throw std::invalid_argument(
          "unknown/incompatible surface-reference option");
  }
  if (p.camera.projection == vdb::SurfaceProjection::Perspective &&
      seen.contains("--ortho-height"))
    throw std::invalid_argument(
        "--ortho-height requires orthographic projection");
  if (p.camera.projection == vdb::SurfaceProjection::Orthographic &&
      seen.contains("--fov"))
    throw std::invalid_argument("--fov requires perspective projection");
  if (p.image.shading == vdb::SurfaceShading::Matte &&
      seen.contains("--invalid-normal-color"))
    throw std::invalid_argument(
        "--invalid-normal-color is incompatible with matte shading");
  if (p.camera.verticalFovDegrees < .1 || p.camera.verticalFovDegrees > 150 ||
      p.camera.orthographicHeight <= 0 || p.camera.minimumDistance < 0 ||
      p.camera.maximumDistance <= p.camera.minimumDistance)
    throw std::invalid_argument(
        "surface-reference camera FOV/height/radial range is invalid");
  const uint64_t pixels = uint64_t(p.image.width) * p.image.height;
  if (pixels > p.image.maxPixels || pixels > p.image.rays.limits.maxRays)
    throw std::invalid_argument(
        "surface-reference resolution exceeds pixel/ray cap");
  return p;
}

const char *projection(vdb::SurfaceProjection p) noexcept {
  return p == vdb::SurfaceProjection::Perspective ? "perspective"
                                                  : "orthographic";
}
const char *shader(vdb::SurfaceShading s) noexcept {
  return s == vdb::SurfaceShading::Matte         ? "matte"
         : s == vdb::SurfaceShading::WorldNormal ? "normal"
                                                 : "headlight";
}
struct Report {
  std::string text;
  uint64_t bytes{}, work{};
  bool good{true};
  Report(int argc, char **argv, const Parsed &requested, Parsed &native) {
    long double caller = 0;
    for (int i = 0; i < argc; ++i) {
      const std::string_view s = argv[i];
      if (!utf8(s))
        throw std::invalid_argument(
            "surface-reference CLI paths/options must be valid UTF-8");
      caller += s.size();
    }
    if (caller > 1024 * 1024)
      throw std::invalid_argument("surface-reference caller text exceeds1MiB");
    const long double wanted =
        65536.0L + 16 * caller +
        8.0L * requested.image.rays.limits.maxMetadataBytes;
    const uint64_t effective = std::min<uint64_t>(
        requested.processing.maxInputBytes, requested.maxExportBytes);
    const uint64_t pixels =
        uint64_t(requested.image.width) * requested.image.height;
    const long double provisional = wanted + 65536 + 16 * caller;
    const long double provisionalWork = 4 * provisional + 32.0L * pixels;
    if (provisional >= effective ||
        provisionalWork >= requested.image.rays.limits.maxWorkItems ||
        wanted > std::numeric_limits<size_t>::max())
      throw std::invalid_argument("surface-reference report reservation "
                                  "exceeds shared byte/work/export caps");
    text.reserve(size_t(std::ceil(wanted)));
    const long double retained = text.capacity() + 65536.0L + 16 * caller;
    const long double actualWork = 4 * retained + 32.0L * pixels;
    if (retained >= effective ||
        actualWork >= requested.image.rays.limits.maxWorkItems)
      throw std::invalid_argument(
          "surface-reference actual report capacity exceeds joint limits");
    bytes = uint64_t(std::ceil(retained));
    work = uint64_t(std::ceil(actualWork));
    native.processing.maxInputBytes -= bytes;
    native.maxExportBytes = effective - bytes;
    native.image.rays.limits.maxWorkItems -= work;
  }
  void append(std::string_view s) noexcept {
    if (!good)
      return;
    if (s.size() > text.capacity() - text.size()) {
      good = false;
      return;
    }
    text.append(s);
  }
  void character(char c) noexcept { append({&c, 1}); }
  template <class T> void number(T v) noexcept {
    char b[128];
    std::to_chars_result r;
    if constexpr (std::is_floating_point_v<T>)
      r = std::to_chars(b, b + sizeof(b), v, std::chars_format::general,
                        std::numeric_limits<T>::max_digits10);
    else
      r = std::to_chars(b, b + sizeof(b), v);
    if (r.ec != std::errc{}) {
      good = false;
      return;
    }
    append({b, size_t(r.ptr - b)});
  }
  void boolean(bool b) noexcept { append(b ? "true" : "false"); }
  void quoted(std::string_view s) noexcept {
    constexpr char hex[] = "0123456789abcdef";
    character('"');
    for (unsigned char c : s) {
      if (c == '"' || c == '\\') {
        character('\\');
        character(char(c));
      } else if (c < 32) {
        char x[]{'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
        append({x, 6});
      } else
        character(char(c));
    }
    character('"');
  }
  void nativeText(std::string_view s) noexcept {
    constexpr char hex[] = "0123456789abcdef";
    append("{\"text\":");
    if (utf8(s)) {
      quoted(s);
      append(",\"hex\":null");
    } else {
      append("null,\"hex\":\"");
      for (unsigned char c : s) {
        char x[]{hex[c >> 4], hex[c & 15]};
        append({x, 2});
      }
      character('"');
    }
    append(",\"byteCount\":");
    number(s.size());
    character('}');
  }
  template <class T, size_t N> void array(const std::array<T, N> &a) noexcept {
    character('[');
    for (size_t i = 0; i < N; ++i) {
      if (i)
        character(',');
      number(a[i]);
    }
    character(']');
  }
  void begin(const Parsed &requested, const Parsed &native) noexcept {
    append("{\"schemaVersion\":1,\"operation\":\"surface-reference\","
           "\"reportProduced\":true,\"openvdbAvailable\":true,"
           "\"outputPublished\":true,\"method\":\"native-camera-checked-hdda-"
           "constant-shader\",\"approximate\":true,\"output\":");
    quoted(requested.output);
    append(",\"dimensions\":{\"width\":");
    number(requested.image.width);
    append(",\"height\":");
    number(requested.image.height);
    append(",\"pixelCount\":");
    number(uint64_t(requested.image.width) * requested.image.height);
    append("},\"camera\":{\"projection\":");
    quoted(projection(requested.camera.projection));
    append(",\"eye\":");
    array(requested.camera.eye);
    append(",\"target\":");
    array(requested.camera.target);
    append(",\"up\":");
    array(requested.camera.up);
    append(",\"verticalFovDegrees\":");
    if (requested.camera.projection == vdb::SurfaceProjection::Perspective)
      number(requested.camera.verticalFovDegrees);
    else
      append("null");
    append(",\"orthographicHeight\":");
    if (requested.camera.projection == vdb::SurfaceProjection::Orthographic)
      number(requested.camera.orthographicHeight);
    else
      append("null");
    append(",\"minimumDistance\":");
    number(requested.camera.minimumDistance);
    append(",\"maximumDistance\":");
    number(requested.camera.maximumDistance);
    append(",\"distanceUnits\":\"euclidean-world\"},\"shading\":{\"mode\":");
    quoted(shader(requested.image.shading));
    append(",\"color\":");
    array(requested.image.color);
    append(",\"background\":");
    array(requested.image.background);
    append(",\"invalidNormalColor\":");
    array(requested.image.invalidNormalColor);
    append("},\"rays\":{\"isovalue\":");
    number(requested.image.rays.isovalue);
    append(",\"refinementSteps\":");
    number(requested.image.rays.refinementSteps);
    append(",\"computeNormals\":");
    boolean(requested.image.rays.computeNormals);
    append("},\"budgets\":{\"requestedInputBytes\":");
    number(requested.processing.maxInputBytes);
    append(",\"nativeInputBytes\":");
    number(native.processing.maxInputBytes);
    append(",\"requestedExportBytes\":");
    number(requested.maxExportBytes);
    append(",\"nativeExportBytes\":");
    number(native.maxExportBytes);
    append(",\"requestedMaxWork\":");
    number(requested.image.rays.limits.maxWorkItems);
    append(",\"nativeMaxWork\":");
    number(native.image.rays.limits.maxWorkItems);
    append(",\"reportReservationBytes\":");
    number(bytes);
    append(",\"maxPixels\":");
    number(requested.image.maxPixels);
    append(",\"maxRays\":");
    number(requested.image.rays.limits.maxRays);
    append(",\"maxRayGrids\":");
    number(requested.image.rays.limits.maxGrids);
    append(",\"maxRayMetadataBytes\":");
    number(requested.image.rays.limits.maxMetadataBytes);
    append(",\"maxVoxels\":");
    number(requested.processing.maxVoxelCount);
    character('}');
  }
  void image(const vdb::SurfaceReferenceImage &result) noexcept {
    const auto &s = result.geometry.source;
    append(",\"source\":{\"descriptorName\":");
    nativeText(s.descriptorName);
    append(",\"gridName\":");
    nativeText(s.gridName);
    append(",\"valueType\":");
    nativeText(s.valueType);
    append(",\"indexToWorld\":");
    array(s.indexToWorld);
    append(",\"inputActiveVoxelCount\":");
    number(s.inputActiveVoxelCount);
    append(",\"inputActiveTileCount\":");
    number(s.inputActiveTileCount);
    append(",\"inputStoredLeafCount\":");
    number(s.inputStoredLeafCount);
    append(",\"gridMetadataRecords\":");
    number(s.metadata.size());
    append(",\"fileMetadataRecords\":");
    number(s.fileMetadata.size());
    append(",\"empty\":");
    boolean(s.empty);
    character('}');
    std::array<uint64_t, 5> normals{};
    std::array<uint64_t, 3> kinds{};
    for (const auto &hit : result.geometry.rays) {
      const auto n = unsigned(hit.normalStatus), k = unsigned(hit.kind);
      if (n >= normals.size() || k >= kinds.size()) {
        good = false;
        return;
      }
      ++normals[n];
      ++kinds[k];
    }
    append(",\"results\":{\"hitCount\":");
    number(result.hitCount);
    append(",\"missCount\":");
    number(result.geometry.rays.size() - result.hitCount);
    append(",\"unavailableNormalCount\":");
    number(result.unavailableNormalCount);
    append(",\"normalStatusCounts\":{\"notRequested\":");
    number(normals[0]);
    append(",\"noHit\":");
    number(normals[1]);
    append(",\"available\":");
    number(normals[2]);
    append(",\"missingActiveFootprint\":");
    number(normals[3]);
    append(",\"degenerateGradient\":");
    number(normals[4]);
    append("},\"kindCounts\":{\"miss\":");
    number(kinds[0]);
    append(",\"sampledZero\":");
    number(kinds[1]);
    append(",\"bracketedCrossing\":");
    number(kinds[2]);
    append("}},\"work\":{\"nativeAndImage\":");
    number(result.work);
    append(",\"reportReservation\":");
    number(work);
    append(",\"combinedUpperBound\":");
    number(result.work + work);
    append(
        "},\"format\":{\"channels\":[\"A\",\"B\",\"G\",\"HIT.KIND\",\"N."
        "STATUS\",\"N.VALID\",\"N.X\",\"N.Y\",\"N.Z\",\"R\",\"Z\"],"
        "\"compression\":\"none\",\"storage\":\"float32\",\"linear\":true,"
        "\"depthUnits\":\"world\",\"normalFrame\":\"world\",\"alpha\":\"center-"
        "sample-hit-coverage\",\"normalStatusCodes\":[\"not-requested\",\"no-"
        "hit\",\"available\",\"missing-active-footprint\",\"degenerate-"
        "gradient\"],\"hitKindCodes\":[\"miss\",\"sampled-zero\",\"bracketed-"
        "crossing\"]},\"limitations\":[\"approximate native HDDA/chord; "
        "tangencies or multiple roots may be missed\",\"sampled zero includes "
        "signed zero and plateaus, not an isolated surface certificate\",\"one "
        "deterministic center ray; no anti-aliasing or Monte-Carlo "
        "uncertainty\",\"native Float constant shader response, not physical "
        "radiance; no shadows or fog transport\",\"zero depth/normal "
        "placeholders require masks/status\",\"source custom metadata retained "
        "in memory; EXR records bounded source identification and "
        "map\"],\"conversion\":{\"shaderRoundedValues\":");
    number(result.roundedColorValues);
    append(",\"maxShaderAbsoluteError\":");
    number(result.maximumColorAbsoluteError);
  }
  void end(const vdb::SurfaceReferenceExportResult &result) noexcept {
    append(",\"exportRoundedValues\":");
    number(result.roundedValues);
    append(",\"maxExportAbsoluteError\":");
    number(result.maximumAbsoluteError);
    append(",\"maxExportRelativeError\":");
    number(result.maximumRelativeError);
    append(",\"encodedBytes\":");
    number(result.encodedBytes);
    append("}}");
  }
};
} // namespace surface_reference_cli

std::optional<int> runSurfaceReferenceCommand(int argc, char **argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--surface-reference")
    return {};
  const auto requested = surface_reference_cli::parse(argc, argv);
  if (!vdb::IsAvailable()) {
    (void)vdb::RenderSurfaceReference({}, requested.camera, requested.image, {},
                                      requested.processing);
    return EXIT_FAILURE; // Actual OFF API precedes all paths/report
                         // reservations.
  }
  auto native = requested;
  surface_reference_cli::Report report(argc, argv, requested, native);
  report.begin(requested, native);
  if (!report.good)
    throw std::runtime_error(
        "surface-reference preflight report capacity invariant failed");
  const auto input = path(std::string(requested.input)),
             output = path(std::string(requested.output));
  const auto result =
      vdb::RenderSurfaceReference(input, native.camera, native.image,
                                  std::string(native.grid), native.processing);
  report.image(result);
  // Every variable-size native field and pixel-status summary is formatted
  // BEFORE publication. The fixed export tail needs <4096 bytes and no heap.
  if (!report.good || report.text.capacity() - report.text.size() < 4096)
    throw std::runtime_error("surface-reference native report capacity "
                             "invariant failed before publication");
  const auto exported =
      vdb::WriteSurfaceReferenceExr(output, result, native.maxExportBytes);
  report.end(exported);
  if (!report.good) {
    std::cout << "{\"schemaVersion\":1,\"operation\":\"surface-reference\","
                 "\"reportProduced\":false,\"outputPublished\":true,"
                 "\"reportCapacityInvariantFailed\":true}\n";
    return EXIT_FAILURE;
  }
  std::cout.write(report.text.data(), std::streamsize(report.text.size()));
  std::cout.put('\n');
  return EXIT_SUCCESS;
}
