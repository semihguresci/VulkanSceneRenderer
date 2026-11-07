// Include in the Tool's private namespace after path(), with
// OpenVdbFogRayIntegrals.h and the runtime IsAvailable() declaration. Standard
// array/charconv/cmath/limits/optional/set/string_view/type_traits are required.
// Copy Help into the earlier Tool help literal and dispatch before legacy parse.
namespace fog_integral_cli {
constexpr std::string_view Help =
    R"(  --integrate-fog-rays INPUT  Read-only nodal trilinear WORLD density integrals
    --ray OX OY OZ DX DY DZ MIN MAX  Repeatable; finite WORLD Euclidean distances
    --grid/--input-grid NAME --extinction S
    --transmission-policy reject-underflow|allow-opaque-underflow
    --max-rays N --max-ray-grids N --max-ray-work N --max-ray-metadata N
    --max-cell-segments-per-ray N --max-cell-segments N
    --max-voxels N --max-input-bytes N
  Inactive values and background participate. Activity-empty is not vacuum.
  Uniform orthogonal maps only; approximate native inverse/normalized direction.
  No scattering, emission, dense-preview parity or exact occupancy claim.
)";
bool missing(std::string_view value) noexcept {
  return value.empty() || value == "-h" || value.starts_with("--");
}
bool utf8(std::string_view text) noexcept {
  for (size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i++]);
    if (c < 0x80)
      continue;
    size_t n;
    unsigned char low = 0x80, high = 0xbf;
    if (c >= 0xc2 && c <= 0xdf)
      n = 1;
    else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      if (c == 0xe0) low = 0xa0;
      if (c == 0xed) high = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      if (c == 0xf0) low = 0x90;
      if (c == 0xf4) high = 0x8f;
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
double real(std::string_view text, std::string_view option) {
  if (text.size() > 128)
    throw std::invalid_argument("fog numeric literals are limited to128 bytes");
  if (text.starts_with('+')) {
    text.remove_prefix(1);
    if (text.starts_with('+') || text.starts_with('-'))
      throw std::invalid_argument("invalid fog decimal for " +
                                  std::string(option));
  }
  double value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                      value, std::chars_format::general);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() || !std::isfinite(value))
    throw std::invalid_argument("finite representable Double required for " +
                                std::string(option));
  return value;
}
uint64_t integer(std::string_view text, std::string_view option,
                 uint64_t maximum) {
  if (text.empty() || text.size() > 128 ||
      text.find_first_not_of("0123456789") != std::string_view::npos)
    throw std::invalid_argument("unsigned decimal required for " +
                                std::string(option));
  uint64_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      !value || value > maximum)
    throw std::invalid_argument("fog count is outside its allowed range: " +
                                std::string(option));
  return value;
}
vdb::WorldRay ray(int argc, char **argv, int &i) {
  vdb::WorldRay out;
  for (double &x : out.origin)
    x = real(argument(argc, argv, i, "--ray"), "--ray");
  for (double &x : out.direction)
    x = real(argument(argc, argv, i, "--ray"), "--ray");
  out.minimumDistance = real(argument(argc, argv, i, "--ray"), "--ray");
  out.maximumDistance = real(argument(argc, argv, i, "--ray"), "--ray");
  if ((out.direction[0] == 0 && out.direction[1] == 0 &&
       out.direction[2] == 0) ||
      out.minimumDistance < 0 || out.maximumDistance <= out.minimumDistance)
    throw std::invalid_argument("--ray requires nonzero direction and 0<=MIN<MAX");
  return out;
}
struct Parsed {
  std::string_view input, selector;
  vdb::FogRayIntegralOptions options;
  vdb::ProcessingOptions processing;
  size_t rayCount = 0;
};
const char *policy(vdb::FogTransmissionPolicy value) noexcept {
  return value == vdb::FogTransmissionPolicy::RejectUnderflow
             ? "reject-underflow" : "allow-opaque-underflow";
}
Parsed parse(int argc, char **argv) {
  Parsed p;
  if (argc < 3 || missing(argv[2]))
    throw std::invalid_argument("fog integral requires nonempty INPUT before "
                                "options; use ./-h for a literal filename");
  p.input = argv[2];
  std::set<std::string_view> seen;
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--ray") {
      (void)ray(argc, argv, i); // strict first pass, no caller batch allocation
      if (++p.rayCount > 1'000'000)
        throw std::invalid_argument("fog batch exceeds1M queries");
      continue;
    }
    const auto key = arg == "--input-grid" ? std::string_view("--grid") : arg;
    if (!seen.insert(key).second)
      throw std::invalid_argument("duplicate fog-integral option: " + std::string(arg));
    const auto value = [&] { return argument(argc, argv, i, arg); };
    if (key == "--grid")
      p.selector = value();
    else if (arg == "--extinction") {
      p.options.extinction = real(value(), arg);
      if (p.options.extinction < 0)
        throw std::invalid_argument("--extinction requires a nonnegative value");
    } else if (arg == "--transmission-policy") {
      const auto text = value();
      if (text == "reject-underflow")
        p.options.transmissionPolicy = vdb::FogTransmissionPolicy::RejectUnderflow;
      else if (text == "allow-opaque-underflow")
        p.options.transmissionPolicy = vdb::FogTransmissionPolicy::AllowOpaqueUnderflow;
      else
        throw std::invalid_argument("--transmission-policy requires "
                                    "reject-underflow|allow-opaque-underflow");
    } else if (arg == "--max-rays")
      p.options.limits.maxRays = uint32_t(integer(value(), arg, 1'000'000));
    else if (arg == "--max-ray-grids")
      p.options.limits.maxGrids = uint32_t(integer(value(), arg, 4096));
    else if (arg == "--max-ray-work")
      p.options.limits.maxWorkItems = integer(value(), arg, 10'000'000'000ull);
    else if (arg == "--max-ray-metadata")
      p.options.limits.maxMetadataBytes = integer(value(), arg, UINT64_MAX);
    else if (arg == "--max-cell-segments-per-ray")
      p.options.maxCellSegmentsPerRay = uint32_t(integer(value(), arg, 1'000'000));
    else if (arg == "--max-cell-segments")
      p.options.maxCellSegments = integer(value(), arg, 1'000'000);
    else if (arg == "--max-voxels")
      p.processing.maxVoxelCount = integer(value(), arg, UINT64_MAX);
    else if (arg == "--max-input-bytes")
      p.processing.maxInputBytes = integer(value(), arg, UINT64_MAX);
    else
      throw std::invalid_argument("unknown/incompatible fog-integral option: " +
                                  std::string(arg));
  }
  if (p.rayCount > p.options.limits.maxRays)
    throw std::invalid_argument("fog batch exceeds --max-rays");
  return p;
}

// One bounded buffer: native text is complete UTF8 or exact byte hex; integral
// results/counts are never truncated. Nothing reaches stdout before success.
struct Report {
  std::string text;
  uint64_t reservation = 0, workReservation = 0;
  size_t limit = 0;
  bool good = true;
  Report(int argc, char **argv, const Parsed &requested,
         std::vector<vdb::WorldRay> &rays, Parsed &native) {
    long double caller = 0;
    for (int i = 0; i < argc; ++i) {
      const std::string_view value = argv[i];
      if (!utf8(value))
        throw std::invalid_argument("fog CLI caller text must be valid UTF-8");
      caller += value.size();
    }
    // Native metadata's logical cap includes4*(name/type/payload)+512/item.
    // Four times that cap covers complete6-byte JSON escapes, exact hex,
    // scalar formatting and all per-entry keys.1500 bytes/ray covers submitted
    // eight Doubles plus the full typed result, at <=26 bytes/Double.
    const long double requestedReport =
        65536.0L + 16 * caller + 1500.0L * requested.rayCount +
        4.0L * requested.options.limits.maxMetadataBytes;
    const long double extra =
        32768.0L + 16 * caller +
        sizeof(vdb::WorldRay) * static_cast<long double>(requested.rayCount);
    const auto check = [&](long double n) {
      if (!std::isfinite(n) || n >= requested.processing.maxInputBytes ||
          n > std::numeric_limits<size_t>::max() ||
          n * 4 >= requested.options.limits.maxWorkItems)
        throw std::invalid_argument("fog CLI full report/options/rays reserve "
                                    "exceeds shared --max-input-bytes or --max-ray-work");
    };
    check(requestedReport + extra);
    text.reserve(size_t(std::ceil(requestedReport)));
    rays.reserve(requested.rayCount);
    const long double actual =
        text.capacity() + rays.capacity() * static_cast<long double>(sizeof(vdb::WorldRay)) +
        32768.0L + 16 * caller;
    check(actual);
    reservation = uint64_t(std::ceil(actual));
    workReservation = reservation * 4;
    limit = text.capacity();
    native.processing.maxInputBytes -= reservation;
    native.options.limits.maxWorkItems -= workReservation;
  }
  void append(std::string_view value) noexcept {
    if (value.size() > limit - text.size()) {
      good = false;
      return;
    }
    text.append(value.data(), value.size());
  }
  void character(char value) noexcept { append(std::string_view(&value, 1)); }
  void boolean(bool value) noexcept { append(value ? "true" : "false"); }
  template <class T> void numeric(T value) noexcept {
    if constexpr (std::is_floating_point_v<T>) {
      if (!std::isfinite(value)) {
        good = false;
        return;
      }
    }
    std::array<char, 128> buffer{};
    std::to_chars_result result;
    if constexpr (std::is_floating_point_v<T>)
      result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                             std::chars_format::general, std::numeric_limits<T>::max_digits10);
    else
      result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
      good = false;
      return;
    }
    append({buffer.data(), size_t(result.ptr - buffer.data())});
  }
  void quoted(std::string_view value) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    character('"');
    for (unsigned char c : value) {
      if (c == '"' || c == '\\') {
        character('\\');
        character(char(c));
      } else if (c < 0x20) {
        append("\\u00");
        character(digits[c >> 4]);
        character(digits[c & 15]);
      } else
        character(char(c));
    }
    character('"');
  }
  void nativeText(std::string_view value) noexcept {
    const bool valid = utf8(value);
    append("{\"text\":");
    if (valid) quoted(value); else append("null");
    append(",\"hex\":");
    if (valid)
      append("null");
    else {
      constexpr char digits[] = "0123456789abcdef";
      character('"');
      for (unsigned char c : value) {
        character(digits[c >> 4]);
        character(digits[c & 15]);
      }
      character('"');
    }
    append(",\"byteCount\":");
    numeric(value.size());
    character('}');
  }
  template <class T, size_t N> void array(const std::array<T, N> &values) noexcept {
    character('[');
    for (size_t i = 0; i < N; ++i) {
      if (i) character(',');
      numeric(values[i]);
    }
    character(']');
  }
  void limits(const Parsed &p) noexcept {
    append("{\"maxInputBytes\":"); numeric(p.processing.maxInputBytes);
    append(",\"maxVoxelCount\":"); numeric(p.processing.maxVoxelCount);
    append(",\"maxRays\":"); numeric(p.options.limits.maxRays);
    append(",\"maxGrids\":"); numeric(p.options.limits.maxGrids);
    append(",\"maxWorkItems\":"); numeric(p.options.limits.maxWorkItems);
    append(",\"maxMetadataBytes\":"); numeric(p.options.limits.maxMetadataBytes);
    append(",\"maxCellSegmentsPerRay\":"); numeric(p.options.maxCellSegmentsPerRay);
    append(",\"maxCellSegments\":"); numeric(p.options.maxCellSegments);
    character('}');
  }
  void begin(const Parsed &p, const Parsed &native,
             const std::vector<vdb::WorldRay> &rays) noexcept {
    append("{\"schemaVersion\":1,\"operation\":\"integrate-fog-rays\","
           "\"reportProduced\":true,\"openvdbAvailable\":true,\"approximate\":true,"
           "\"method\":\"bounded-rational-cell-simpson\","
           "\"fieldPolicy\":\"nodal trilinear values; inactive/background participate; activity is not density\","
           "\"mappingPolicy\":\"exact arithmetic on represented native Double inverse and normalized WORLD direction; no ideal-inverse/error certificate\","
           "\"supportPolicy\":\"stored non-background knots/tiles expanded one index unit; direct positive constant-background outer intervals\","
           "\"scope\":\"density/Beer query; no scattering, emission, exact occupancy or dense-preview parity\","
           "\"reportReservationBytes\":");
    numeric(reservation);
    append(",\"reportReservationWork\":"); numeric(workReservation);
    append(",\"settings\":{\"input\":"); quoted(p.input);
    append(",\"selectedGrid\":"); quoted(p.selector);
    append(",\"extinction\":"); numeric(p.options.extinction);
    append(",\"transmissionPolicy\":"); quoted(policy(p.options.transmissionPolicy));
    append(",\"rays\":[");
    for (size_t i = 0; i < rays.size(); ++i) {
      if (i) character(',');
      append("{\"origin\":"); array(rays[i].origin);
      append(",\"direction\":"); array(rays[i].direction);
      append(",\"minimumDistance\":"); numeric(rays[i].minimumDistance);
      append(",\"maximumDistance\":"); numeric(rays[i].maximumDistance);
      character('}');
    }
    append("],\"requestedLimits\":"); limits(p);
    append(",\"nativeLimits\":"); limits(native);
    character('}');
  }
  void metadata(const std::vector<vdb::RayQueryMetadata> &values) noexcept {
    character('[');
    for (size_t i = 0; i < values.size(); ++i) {
      if (i) character(',');
      append("{\"name\":"); nativeText(values[i].name);
      append(",\"type\":"); nativeText(values[i].type);
      append(",\"value\":"); nativeText(values[i].value);
      append(",\"nativeBytes\":"); numeric(values[i].nativeBytes);
      append(",\"opaque\":"); boolean(values[i].opaque);
      character('}');
    }
    character(']');
  }
  void write(const vdb::FogRayIntegralBatch &batch) noexcept {
    const auto &s = batch.source;
    append(",\"source\":{\"descriptorName\":"); nativeText(s.descriptorName);
    append(",\"gridName\":"); nativeText(s.gridName);
    append(",\"valueType\":"); nativeText(s.valueType);
    append(",\"inputActiveVoxelCount\":"); numeric(s.inputActiveVoxelCount);
    append(",\"inputActiveTileCount\":"); numeric(s.inputActiveTileCount);
    append(",\"inputStoredLeafCount\":"); numeric(s.inputStoredLeafCount);
    append(",\"empty\":"); boolean(s.empty);
    append(",\"indexToWorld\":"); array(s.indexToWorld);
    append(",\"workItems\":"); numeric(s.workItems);
    append(",\"metadata\":"); metadata(s.metadata);
    append(",\"fileMetadata\":"); metadata(s.fileMetadata);
    character('}');
    append(",\"backgroundDensity\":"); numeric(batch.backgroundDensity);
    append(",\"numericallyConstant\":"); boolean(batch.numericallyConstant);
    append(",\"cellSegments\":"); numeric(batch.cellSegments);
    append(",\"cellSegmentUpperBound\":"); numeric(batch.cellSegmentUpperBound);
    append(",\"nativeWorkItems\":"); numeric(batch.workItems);
    append(",\"combinedWorkUpperBound\":"); numeric(batch.workItems + workReservation);
    append(",\"rays\":[");
    for (size_t i = 0; i < batch.rays.size(); ++i) {
      if (i) character(',');
      const auto &r = batch.rays[i];
      append("{\"normalizedDirection\":"); array(r.normalizedDirection);
      append(",\"densityIntegral\":"); numeric(r.densityIntegral);
      append(",\"opticalDepth\":"); numeric(r.opticalDepth);
      append(",\"transmittance\":"); numeric(r.transmittance);
      append(",\"absorbedFraction\":"); numeric(r.absorbedFraction);
      append(",\"cellSegments\":"); numeric(r.cellSegments);
      append(",\"transmissionStatus\":");
      if (r.transmissionStatus == vdb::FogTransmissionStatus::Represented)
        quoted("represented");
      else if (r.transmissionStatus == vdb::FogTransmissionStatus::OpaqueUnderflow)
        quoted("opaque-underflow");
      else
        good = false;
      character('}');
    }
    append("],\"diagnostics\":[");
    for (size_t i = 0; i < batch.diagnostics.size(); ++i) {
      if (i) character(',');
      nativeText(batch.diagnostics[i]);
    }
    append("]}");
  }
};
} // namespace fog_integral_cli

std::optional<int> runFogRayIntegralsCommand(int argc, char **argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--integrate-fog-rays")
    return {};
  const auto requested = fog_integral_cli::parse(argc, argv);
  if (!vdb::IsAvailable()) {
    // Actual OFF API fails before paths/options/rays/report allocation.
    (void)vdb::IntegrateFogRays({}, {}, requested.options, {}, requested.processing);
    return EXIT_FAILURE;
  }
  auto native = requested;
  std::vector<vdb::WorldRay> rays;
  fog_integral_cli::Report report(argc, argv, requested, rays, native);
  for (int i = 3; i < argc; ++i)
    if (std::string_view(argv[i]) == "--ray")
      rays.push_back(fog_integral_cli::ray(argc, argv, i));
  report.begin(requested, native, rays);
  if (!report.good)
    throw std::runtime_error("fog CLI preflight report capacity invariant failed");
  const auto input = path(std::string(requested.input));
  const std::string selector(requested.selector);
  report.write(vdb::IntegrateFogRays(input, rays, native.options, selector, native.processing));
  if (!report.good)
    throw std::runtime_error("fog CLI complete report capacity invariant failed");
  std::cout << report.text << '\n';
  return EXIT_SUCCESS;
}
