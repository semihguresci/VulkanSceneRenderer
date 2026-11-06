// Include in OpenVdbTool's private namespace after path(). The translation unit
// needs OpenVdbRayQueries.h, the existing runtime IsAvailable declaration and
// <charconv>/<set>/<optional>. Dispatch runRayQueriesCommand before legacy
// parse. No SDK feature macros: runtime OFF precedes path/report/source
// processing.
namespace ray_query_cli {
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
    throw std::invalid_argument("ray numeric literals are limited to128 bytes");
  if (text.starts_with('+')) {
    text.remove_prefix(1);
    if (text.starts_with('+') || text.starts_with('-'))
      throw std::invalid_argument("invalid ray decimal for " +
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
    throw std::invalid_argument("ray count literals are limited to128 bytes");
  uint64_t out = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), out);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() || (!out && !allowZero) ||
      out > maximum)
    throw std::invalid_argument(
        "unsigned ray count is outside its allowed range: " +
        std::string(option));
  return out;
}
vdb::WorldRay ray(int argc, char **argv, int &i) {
  vdb::WorldRay out;
  for (double &x : out.origin)
    x = finite(argument(argc, argv, i, "--ray"), "--ray");
  for (double &x : out.direction)
    x = finite(argument(argc, argv, i, "--ray"), "--ray");
  out.minimumDistance = finite(argument(argc, argv, i, "--ray"), "--ray");
  out.maximumDistance = finite(argument(argc, argv, i, "--ray"), "--ray");
  if ((out.direction[0] == 0 && out.direction[1] == 0 &&
       out.direction[2] == 0) ||
      out.minimumDistance < 0 || out.maximumDistance <= out.minimumDistance)
    throw std::invalid_argument(
        "--ray requires nonzero direction and 0<=MIN<MAX");
  return out;
}
struct Parsed {
  bool sdf{true};
  std::string_view input, selector;
  vdb::LevelSetRayOptions level;
  vdb::VolumeRayOptions volume;
  vdb::ProcessingOptions processing;
  size_t rayCount = 0;
  vdb::RayQueryLimits &limits() { return sdf ? level.limits : volume.limits; }
  const vdb::RayQueryLimits &limits() const {
    return sdf ? level.limits : volume.limits;
  }
};
Parsed parse(int argc, char **argv, bool sdf) {
  Parsed p;
  p.sdf = sdf;
  if (argc < 3 || missing(argv[2]))
    throw std::invalid_argument("ray query requires nonempty INPUT before "
                                "options; use ./-h for a literal filename");
  p.input = argv[2];
  std::set<std::string_view> seen;
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--ray") {
      (void)ray(argc, argv,
                i); // validate without allocating a caller batch yet
      if (++p.rayCount > 1'000'000)
        throw std::invalid_argument("ray batch exceeds1M queries");
      continue;
    }
    const auto key = arg == "--input-grid" ? std::string_view("--grid") : arg;
    if (!seen.insert(key).second)
      throw std::invalid_argument("duplicate ray-query option: " +
                                  std::string(arg));
    auto value = [&] { return argument(argc, argv, i, arg); };
    if (key == "--grid")
      p.selector = value();
    else if (arg == "--max-rays")
      p.limits().maxRays = uint32_t(integer(value(), arg, 1'000'000));
    else if (arg == "--max-ray-grids")
      p.limits().maxGrids = uint32_t(integer(value(), arg, 4096));
    else if (arg == "--max-ray-work")
      p.limits().maxWorkItems = integer(value(), arg, 10'000'000'000ull);
    else if (arg == "--max-ray-metadata")
      p.limits().maxMetadataBytes = integer(value(), arg, UINT64_MAX);
    else if (arg == "--max-voxels")
      p.processing.maxVoxelCount = integer(value(), arg, UINT64_MAX);
    else if (arg == "--max-input-bytes")
      p.processing.maxInputBytes = integer(value(), arg, UINT64_MAX);
    else if (arg == "--ray-isovalue" && sdf)
      p.level.isovalue = finite(value(), arg);
    else if (arg == "--ray-refinements" && sdf)
      p.level.refinementSteps = uint32_t(integer(value(), arg, 8, true));
    else if (arg == "--ray-normals" && sdf) {
      const auto setting = value();
      if (setting != "on" && setting != "off")
        throw std::invalid_argument("--ray-normals requires on|off");
      p.level.computeNormals = setting == "on";
    } else if (arg == "--max-ray-spans" && !sdf)
      p.volume.maxSpans = uint32_t(integer(value(), arg, 1'000'000));
    else
      throw std::invalid_argument("unknown/incompatible ray-query option: " +
                                  std::string(arg));
  }
  if (p.rayCount > p.limits().maxRays)
    throw std::invalid_argument("ray batch exceeds --max-rays");
  return p;
}
const char *kind(vdb::SdfRayHitKind value) noexcept {
  switch (value) {
  case vdb::SdfRayHitKind::Miss:
    return "miss";
  case vdb::SdfRayHitKind::SampledZero:
    return "sampled-zero";
  case vdb::SdfRayHitKind::BracketedCrossing:
    return "bracketed-crossing";
  }
  return "unknown";
}
const char *normal(vdb::RayNormalStatus value) noexcept {
  switch (value) {
  case vdb::RayNormalStatus::NotRequested:
    return "not-requested";
  case vdb::RayNormalStatus::NoHit:
    return "no-hit";
  case vdb::RayNormalStatus::Available:
    return "available";
  case vdb::RayNormalStatus::MissingActiveFootprint:
    return "missing-active-footprint";
  case vdb::RayNormalStatus::DegenerateGradient:
    return "degenerate-gradient";
  }
  return "unknown";
}

// Complete manual serialization into a fixed-capacity string. Every persistent
// caller/ray/path/report byte is reserved/subtracted before the native query;
// native strings are emitted completely as UTF8 text OR exact bytes hex, never
// truncated. Formatting after native work uses stack to_chars and no DOM.
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
        throw std::invalid_argument("ray CLI caller text must be valid UTF-8");
      caller += value.size();
    }
    const long double reportBytes =
        65536.0L + 16 * caller + 1024.0L * requested.rayCount +
        4.0L * requested.limits().maxMetadataBytes +
        // Eight finite Double values need <=26 chars each in
        // general/max_digits10 format; span keys/brackets/separators need <100
        // bytes:320 bounds one complete span. Native retained span memory is
        // budgeted by the API.
        (requested.sdf || !requested.rayCount
             ? 0
             : 320.0L * requested.volume.maxSpans);
    const long double extra =
        32768.0L + 16 * caller +
        sizeof(vdb::WorldRay) * static_cast<long double>(requested.rayCount);
    const auto check = [&](long double bytes) {
      if (!std::isfinite(bytes) ||
          bytes >= requested.processing.maxInputBytes ||
          bytes > std::numeric_limits<size_t>::max() ||
          bytes * 4 >= requested.limits().maxWorkItems)
        throw std::invalid_argument(
            "ray CLI full report/options/rays reservation exceeds shared "
            "--max-input-bytes or --max-ray-work");
    };
    check(reportBytes + extra);
    text.reserve(size_t(std::ceil(reportBytes)));
    rays.reserve(requested.rayCount);
    const long double actual =
        static_cast<long double>(text.capacity()) +
        rays.capacity() * static_cast<long double>(sizeof(vdb::WorldRay)) +
        32768 + 16 * caller;
    check(actual); // account actual retained capacities, not reserve() requests
    reservation = uint64_t(std::ceil(actual));
    workReservation = reservation * 4;
    limit = text.capacity();
    native.processing.maxInputBytes -= reservation;
    native.limits().maxWorkItems -= workReservation;
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
        append("null");
        return;
      }
    }
    std::array<char, 128> buffer{};
    std::to_chars_result result;
    if constexpr (std::is_floating_point_v<T>)
      result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                             value, std::chars_format::general,
                             std::numeric_limits<T>::max_digits10);
    else
      result =
          std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (result.ec != std::errc{}) {
      good = false;
      append("null");
      return;
    }
    append(std::string_view(buffer.data(), size_t(result.ptr - buffer.data())));
  }
  void quoted(std::string_view value) noexcept {
    constexpr char hex[] = "0123456789abcdef";
    character('"');
    for (unsigned char c : value) {
      if (c == '"' || c == '\\') {
        character('\\');
        character(char(c));
      } else if (c < 0x20) {
        append("\\u00");
        character(hex[c >> 4]);
        character(hex[c & 15]);
      } else
        character(char(c));
    }
    character('"');
  }
  void nativeText(std::string_view value) noexcept {
    append("{\"text\":");
    const bool valid = utf8(value);
    if (valid)
      quoted(value);
    else
      append("null");
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
  template <class T, size_t N>
  void array(const std::array<T, N> &values) noexcept {
    character('[');
    for (size_t i = 0; i < N; ++i) {
      if (i)
        character(',');
      numeric(values[i]);
    }
    character(']');
  }
  void limits(const Parsed &p) noexcept {
    append("{\"maxInputBytes\":");
    numeric(p.processing.maxInputBytes);
    append(",\"maxVoxelCount\":");
    numeric(p.processing.maxVoxelCount);
    append(",\"maxRays\":");
    numeric(p.limits().maxRays);
    append(",\"maxGrids\":");
    numeric(p.limits().maxGrids);
    append(",\"maxWorkItems\":");
    numeric(p.limits().maxWorkItems);
    append(",\"maxMetadataBytes\":");
    numeric(p.limits().maxMetadataBytes);
    append(",\"maxSpans\":");
    if (p.sdf)
      append("null");
    else
      numeric(p.volume.maxSpans);
    character('}');
  }
  void begin(const Parsed &p, const Parsed &native,
             const std::vector<vdb::WorldRay> &rays) noexcept {
    append("{\"schemaVersion\":1,\"reportProduced\":true,\"openvdbAvailable\":"
           "true,\"operation\":");
    quoted(p.sdf ? "ray-intersections" : "ray-volume-spans");
    append(",\"method\":");
    quoted(p.sdf ? "native-hdda-checked-chord"
                 : "native-coarse-active-leaf-tile-spans");
    append(",\"approximate\":");
    boolean(p.sdf);
    append(",\"coarseVolumeSpans\":");
    boolean(!p.sdf);
    append(",\"reportReservationBytes\":");
    numeric(reservation);
    append(",\"reportReservationWork\":");
    numeric(workReservation);
    append(",\"settings\":{\"input\":");
    quoted(p.input);
    append(",\"selectedGrid\":");
    quoted(p.selector);
    append(",\"rays\":[");
    for (size_t i = 0; i < rays.size(); ++i) {
      if (i)
        character(',');
      append("{\"origin\":");
      array(rays[i].origin);
      append(",\"direction\":");
      array(rays[i].direction);
      append(",\"minimumDistance\":");
      numeric(rays[i].minimumDistance);
      append(",\"maximumDistance\":");
      numeric(rays[i].maximumDistance);
      character('}');
    }
    append("],\"sdf\":");
    if (p.sdf) {
      append("{\"isovalue\":");
      numeric(p.level.isovalue);
      append(",\"refinementSteps\":");
      numeric(p.level.refinementSteps);
      append(",\"computeNormals\":");
      boolean(p.level.computeNormals);
      character('}');
    } else
      append("null");
    append(",\"requestedLimits\":");
    limits(p);
    append(",\"nativeLimits\":");
    limits(native);
    character('}');
  }
  void metadata(const std::vector<vdb::RayQueryMetadata> &values) noexcept {
    character('[');
    for (size_t i = 0; i < values.size(); ++i) {
      if (i)
        character(',');
      append("{\"name\":");
      nativeText(values[i].name);
      append(",\"type\":");
      nativeText(values[i].type);
      append(",\"value\":");
      nativeText(values[i].value);
      append(",\"nativeBytes\":");
      numeric(values[i].nativeBytes);
      append(",\"opaque\":");
      boolean(values[i].opaque);
      character('}');
    }
    character(']');
  }
  void source(const vdb::RayQuerySource &s) noexcept {
    append(",\"source\":{\"descriptorName\":");
    nativeText(s.descriptorName);
    append(",\"gridName\":");
    nativeText(s.gridName);
    append(",\"valueType\":");
    nativeText(s.valueType);
    append(",\"inputActiveVoxelCount\":");
    numeric(s.inputActiveVoxelCount);
    append(",\"inputActiveTileCount\":");
    numeric(s.inputActiveTileCount);
    append(",\"inputStoredLeafCount\":");
    numeric(s.inputStoredLeafCount);
    append(",\"empty\":");
    boolean(s.empty);
    append(",\"indexToWorld\":");
    array(s.indexToWorld);
    append(",\"workItems\":");
    numeric(s.workItems);
    append(",\"metadata\":");
    metadata(s.metadata);
    append(",\"fileMetadata\":");
    metadata(s.fileMetadata);
    character('}');
    append(",\"combinedWorkUpperBound\":");
    numeric(s.workItems + workReservation);
  }
  void finish(const std::vector<std::string> &diagnostics) noexcept {
    append(",\"diagnostics\":[");
    for (size_t i = 0; i < diagnostics.size(); ++i) {
      if (i)
        character(',');
      if (utf8(diagnostics[i]))
        quoted(diagnostics[i]);
      else {
        good = false;
        append("null");
      }
    }
    append("]}");
  }
  void write(const vdb::LevelSetRayBatch &batch) noexcept {
    source(batch.source);
    append(",\"rays\":[");
    for (size_t i = 0; i < batch.rays.size(); ++i) {
      if (i)
        character(',');
      const auto &r = batch.rays[i];
      const bool hit = r.kind != vdb::SdfRayHitKind::Miss;
      append("{\"kind\":");
      quoted(kind(r.kind));
      append(",\"hit\":");
      boolean(hit);
      append(",\"distance\":");
      if (hit)
        numeric(r.distance);
      else
        append("null");
      append(",\"position\":");
      if (hit)
        array(r.position);
      else
        append("null");
      append(",\"sampledResidual\":");
      if (hit)
        numeric(r.sampledResidual);
      else
        append("null");
      append(",\"normalStatus\":");
      quoted(normal(r.normalStatus));
      append(",\"normal\":");
      if (r.normalStatus == vdb::RayNormalStatus::Available)
        array(r.normal);
      else
        append("null");
      append(",\"normalizedDirection\":");
      array(r.normalizedDirection);
      append(",\"startSampleValid\":");
      boolean(r.startSampleValid);
      append(",\"startedInside\":");
      if (r.startSampleValid)
        boolean(r.startedInside);
      else
        append("null");
      character('}');
    }
    character(']');
    finish(batch.diagnostics);
  }
  void write(const vdb::VolumeRayBatch &batch) noexcept {
    source(batch.source);
    append(",\"totalSpanCount\":");
    numeric(batch.totalSpanCount);
    append(",\"rays\":[");
    for (size_t i = 0; i < batch.rays.size(); ++i) {
      if (i)
        character(',');
      append("{\"normalizedDirection\":");
      array(batch.rays[i].normalizedDirection);
      append(",\"spans\":[");
      for (size_t j = 0; j < batch.rays[i].spans.size(); ++j) {
        if (j)
          character(',');
        const auto &s = batch.rays[i].spans[j];
        append("{\"entryDistance\":");
        numeric(s.entryDistance);
        append(",\"exitDistance\":");
        numeric(s.exitDistance);
        append(",\"entryPosition\":");
        array(s.entryPosition);
        append(",\"exitPosition\":");
        array(s.exitPosition);
        character('}');
      }
      append("]}");
    }
    character(']');
    finish(batch.diagnostics);
  }
};
} // namespace ray_query_cli

std::optional<int> runRayQueriesCommand(int argc, char **argv) {
  if (argc < 2 || (std::string_view(argv[1]) != "--ray-intersections" &&
                   std::string_view(argv[1]) != "--ray-volume-spans"))
    return {};
  auto requested = ray_query_cli::parse(
      argc, argv, std::string_view(argv[1]) == "--ray-intersections");
  if (!vdb::IsAvailable()) {
    // Actual OFF API, with ignored paths, precedes UTF8/path conversion and all
    // local report/caller storage reservations, even with valid tiny caps.
    if (requested.sdf)
      (void)vdb::QueryLevelSetRays({}, {}, requested.level, {},
                                   requested.processing);
    else
      (void)vdb::QueryActiveVolumeRays({}, {}, requested.volume, {},
                                       requested.processing);
    return EXIT_FAILURE;
  }
  auto native = requested;
  std::vector<vdb::WorldRay> rays;
  ray_query_cli::Report report(argc, argv, requested, rays, native);
  for (int i = 3; i < argc; ++i)
    if (std::string_view(argv[i]) == "--ray")
      rays.push_back(ray_query_cli::ray(argc, argv, i));
  report.begin(requested, native, rays);
  if (!report.good)
    throw std::runtime_error(
        "ray CLI preflight report capacity invariant failed");
  const auto input = path(std::string(requested.input));
  const std::string selector(requested.selector);
  if (requested.sdf)
    report.write(vdb::QueryLevelSetRays(input, rays, native.level, selector,
                                        native.processing));
  else
    report.write(vdb::QueryActiveVolumeRays(input, rays, native.volume,
                                            selector, native.processing));
  if (!report.good) {
    std::cout << "{\"schemaVersion\":1,\"reportProduced\":false,"
                 "\"reportCapacityInvariantFailed\":true}\n";
    return EXIT_FAILURE;
  }
  std::cout << report.text << '\n';
  return EXIT_SUCCESS;
}
