// Include in openvdb_process.cpp's private namespace after value/count/path.
// Requires OpenVdbNanoPrimitives.h, nlohmann/json.hpp and <charconv> at TU
// scope. Dispatch runNanoPrimitivesCommand before the generic parser. No SDK
// macros.
namespace nano_primitives_cli {
bool utf8(std::string_view s) noexcept {
  for (size_t i = 0; i < s.size();) {
    const auto a = static_cast<unsigned char>(s[i++]);
    if (a < 0x80)
      continue;
    size_t n;
    unsigned char low = 0x80, high = 0xbf;
    if (a >= 0xc2 && a <= 0xdf)
      n = 1;
    else if (a >= 0xe0 && a <= 0xef) {
      n = 2;
      if (a == 0xe0)
        low = 0xa0;
      if (a == 0xed)
        high = 0x9f;
    } else if (a >= 0xf0 && a <= 0xf4) {
      n = 3;
      if (a == 0xf0)
        low = 0x90;
      if (a == 0xf4)
        high = 0x8f;
    } else
      return false;
    if (n > s.size() - i)
      return false;
    const auto b = static_cast<unsigned char>(s[i++]);
    if (b < low || b > high)
      return false;
    while (--n) {
      const auto c = static_cast<unsigned char>(s[i++]);
      if (c < 0x80 || c > 0xbf)
        return false;
    }
  }
  return true;
}
double finite(std::string text, const std::string &option) {
  const bool positive = !text.empty() && text.front() == '+';
  if (positive)
    text.erase(text.begin());
  double result{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                      result, std::chars_format::general);
  if (text.empty() ||
      (positive && (text.front() == '+' || text.front() == '-')) ||
      parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      !std::isfinite(result))
    throw std::invalid_argument("finite representable decimal required for " +
                                option);
  return result;
}
const char *codec(vdb::NanoVdbCodec v) noexcept {
  return v == vdb::NanoVdbCodec::None  ? "none"
         : v == vdb::NanoVdbCodec::Zip ? "zip"
                                       : "blosc";
}
const char *checksum(vdb::NanoVdbChecksum v) noexcept {
  return v == vdb::NanoVdbChecksum::Disabled ? "disabled"
         : v == vdb::NanoVdbChecksum::Header ? "header"
                                             : "full";
}
nlohmann::json limitsJson(const vdb::NanoVdbLimits &l) {
  return {{"maxInputBytes", l.maxInputBytes},
          {"maxNativeBytes", l.maxNativeBytes},
          {"maxWorkspaceBytes", l.maxWorkspaceBytes},
          {"maxActiveVoxels", l.maxActiveVoxels},
          {"maxStoredLeafVoxels", l.maxStoredLeafVoxels},
          {"maxNodes", l.maxNodes},
          {"maxWork", l.maxWork},
          {"maxGrids", l.maxGrids},
          {"maxNameBytes", l.maxNameBytes},
          {"maxMetadataBytes", l.maxMetadataBytes}};
}
struct Report {
  std::string text, settings;
  uint64_t bytes{}, work{};
  bool good{true};
  Report(std::string_view destination, std::string_view shape,
         const vdb::NanoVdbPrimitiveOptions &o,
         const vdb::NanoVdbWriteOptions &w, const vdb::NanoVdbLimits &requested,
         vdb::NanoVdbLimits &native) {
    if (!utf8(destination) || !utf8(o.gridName))
      throw std::invalid_argument(
          "NanoVDB primitive CLI requires valid UTF-8 paths/names");
    const long double reserve =
        1048576.0L +
        64 * (static_cast<long double>(destination.size()) + o.gridName.size());
    if (reserve >= requested.maxWorkspaceBytes ||
        reserve * 4 >= requested.maxWork ||
        reserve > std::numeric_limits<size_t>::max())
      throw std::invalid_argument("NanoVDB primitive CLI report reservation "
                                  "exceeds workspace/work caps");
    bytes = uint64_t(reserve);
    work = bytes * 4;
    native.maxWorkspaceBytes -= bytes;
    native.maxWork -= work;
    // Every DOM/string allocation and caller-text escaping happens BEFORE the
    // primitive can publish. Final serialization uses only this fixed capacity.
    settings =
        nlohmann::json{
            {"output", destination},
            {"shape", shape},
            {"gridClass", o.gridClass == vdb::NanoVdbPrimitiveClass::Fog
                              ? "fog"
                              : "level-set"},
            {"storage", o.storage == vdb::NanoVdbPrimitiveStorage::Float
                            ? "float"
                            : "double"},
            {"voxelSize", o.voxelSize},
            {"halfWidth", o.halfWidth},
            {"center", o.center},
            {"origin", o.origin},
            {"radius", o.radius},
            {"boxDimensions", o.boxDimensions},
            {"majorRadius", o.majorRadius},
            {"minorRadius", o.minorRadius},
            {"gridName", o.gridName},
            {"nativeStatistics",
             o.statistics == vdb::NanoVdbStatisticsMode::Bounds   ? "bounds"
             : o.statistics == vdb::NanoVdbStatisticsMode::MinMax ? "minmax"
                                                                  : "all"},
            {"writer",
             {{"codec", codec(w.codec)},
              {"checksum", checksum(w.checksum)},
              {"raw", w.raw},
              {"allowFloatMapRounding", w.allowFloatMapRounding}}},
            {"requestedLimits", limitsJson(requested)},
            {"nativeLimits", limitsJson(native)}}
            .dump();
    text.reserve(size_t(bytes / 2));
  }
  void append(std::string_view s) {
    if (!good)
      return;
    if (s.size() > text.capacity() - text.size()) {
      good = false;
      return;
    }
    text.append(s);
  }
  void character(char c) { append(std::string_view(&c, 1)); }
  template <class T> void number(T n) {
    std::array<char, 128> buffer;
    std::to_chars_result result;
    if constexpr (std::is_floating_point_v<T>) {
      if (!std::isfinite(n)) {
        append("null");
        return;
      }
      result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), n,
                             std::chars_format::general,
                             std::numeric_limits<T>::max_digits10);
    } else
      result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), n);
    if (result.ec != std::errc{}) {
      good = false;
      return;
    }
    append({buffer.data(), size_t(result.ptr - buffer.data())});
  }
  void boolean(bool v) { append(v ? "true" : "false"); }
  void string(std::string_view s) {
    if (!utf8(s)) {
      append("null");
      return;
    }
    constexpr char hex[] = "0123456789abcdef";
    character('"');
    for (const unsigned char c : s) {
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
  template <class T, size_t N> void array(const std::array<T, N> &values) {
    character('[');
    for (size_t i = 0; i < N; ++i) {
      if (i)
        character(',');
      number(values[i]);
    }
    character(']');
  }
  void diagnostics(const std::vector<std::string> &values) {
    character('[');
    for (size_t i = 0; i < std::min<size_t>(values.size(), 8); ++i) {
      if (i)
        character(',');
      size_t n = std::min<size_t>(1024, values[i].size());
      while (n < values[i].size() && n &&
             (static_cast<unsigned char>(values[i][n]) & 0xc0) == 0x80)
        --n;
      string(std::string_view(values[i]).substr(0, n));
    }
    character(']');
  }
  static bool truncated(const std::vector<std::string> &values) {
    if (values.size() > 8)
      return true;
    for (const auto &v : values)
      if (v.size() > 1024 || !utf8(v))
        return true;
    return false;
  }
  void write(const vdb::NanoVdbPrimitiveResult &r, bool conversion) {
    append("{\"schemaVersion\":1,\"operation\":\"nano-primitive\","
           "\"reportProduced\":true,\"nativeIoAvailable\":true,"
           "\"openVdbConversionAvailable\":");
    boolean(conversion);
    append(",\"settings\":");
    append(settings);
    append(",\"reportReservationBytes\":");
    number(bytes);
    append(",\"reportReservationWork\":");
    number(work);
    append(",\"candidateMinimum\":");
    array(r.candidateMinimum);
    append(",\"candidateMaximum\":");
    array(r.candidateMaximum);
    append(",\"candidateVoxels\":");
    number(r.candidateVoxels);
    append(",\"storedLeafVoxelUpperBound\":");
    number(r.storedLeafVoxelUpperBound);
    append(",\"nodeUpperBound\":");
    number(r.nodeUpperBound);
    append(",\"nativeByteUpperBound\":");
    number(r.nativeByteUpperBound);
    append(",\"workspaceUpperBound\":");
    number(r.workspaceUpperBound);
    append(",\"workUpperBound\":");
    number(r.workUpperBound);
    append(",\"combinedWorkspaceUpperBound\":");
    number(r.workspaceUpperBound + bytes);
    append(",\"combinedWorkUpperBound\":");
    number(r.workUpperBound + work);
    append(",\"roundedFloatValues\":");
    number(r.roundedFloatValues);
    append(",\"maximumFloatAbsoluteError\":");
    number(r.maximumFloatAbsoluteError);
    append(",\"floatMapCachesRounded\":");
    boolean(r.floatMapCachesRounded);
    append(",\"maximumFloatMapCacheErrorVoxels\":");
    number(r.maximumFloatMapCacheErrorVoxels);
    append(",\"floatRoundingContract\":\"Float value rounding is measured "
           "separately; allow-float-map-rounding opts into cached Float "
           "forward/inverse map coefficients only, with a 1e-4 voxel "
           "discrepancy cap\"");
    append(",\"file\":{\"schemaVersion\":1,\"encodedBytes\":");
    number(r.file.encodedBytes);
    append(",\"nativeBytes\":");
    number(r.file.nativeBytes);
    append(",\"raw\":");
    boolean(r.file.raw);
    append(",\"work\":");
    number(r.file.work);
    append(",\"nativeGridCount\":");
    number(r.file.grids.size());
    append(",\"grids\":[");
    if (!r.file.grids.empty()) {
      const auto &g = r.file.grids.front();
      append("{\"index\":");
      number(g.index);
      append(",\"name\":");
      string(g.name);
      append(",\"valueType\":");
      string(g.valueType);
      append(",\"gridClass\":");
      string(g.gridClass);
      append(",\"version\":");
      string(g.version);
      append(",\"nativeBytes\":");
      number(g.nativeBytes);
      append(",\"encodedBytes\":");
      number(g.encodedBytes);
      append(",\"activeVoxels\":");
      number(g.activeVoxels);
      append(",\"storedLeafVoxels\":");
      number(g.storedLeafVoxels);
      append(",\"nodeCounts\":");
      array(g.nodeCounts);
      append(",\"containerMetadataBytes\":");
      number(g.containerMetadataBytes);
      append(",\"codec\":");
      string(codec(g.codec));
      append(",\"checksum\":");
      if (g.checksumModeKnown)
        string(checksum(g.checksum));
      else
        append("null");
      append(",\"supported\":");
      boolean(g.supported);
      append(",\"structureValidated\":");
      boolean(g.structureValidated);
      append(",\"checksumVerified\":");
      boolean(g.checksumVerified);
      append(",\"checksumCoversAllData\":");
      boolean(g.checksumCoversAllData);
      append(",\"containerMetadata\":");
      boolean(g.containerMetadata);
      character('}');
    }
    append("],\"diagnostics\":");
    diagnostics(r.file.diagnostics);
    append(",\"diagnosticsTruncated\":");
    boolean(r.file.grids.size() > 1 || truncated(r.file.diagnostics));
    character('}');
    append(",\"nativeDiagnosticCount\":");
    number(r.diagnostics.size());
    append(",\"diagnostics\":");
    diagnostics(r.diagnostics);
    append(",\"diagnosticsTruncated\":");
    boolean(truncated(r.diagnostics));
    character('}');
  }
};
} // namespace nano_primitives_cli

std::optional<int> runNanoPrimitivesCommand(int argc, char **argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--nano-primitive")
    return {};
  const auto missing = [](std::string_view v) {
    return v.empty() || v == "-h" || v.starts_with("--");
  };
  if (argc < 4 || missing(argv[2]) || missing(argv[3]))
    throw std::invalid_argument(
        "--nano-primitive requires sphere|box|torus OUTPUT before options; use "
        "./-h for a literal path");
  const std::string shapeName = argv[2];
  vdb::NanoVdbPrimitiveShape shape;
  if (shapeName == "sphere")
    shape = vdb::NanoVdbPrimitiveShape::Sphere;
  else if (shapeName == "box")
    shape = vdb::NanoVdbPrimitiveShape::Box;
  else if (shapeName == "torus")
    shape = vdb::NanoVdbPrimitiveShape::Torus;
  else
    throw std::invalid_argument(
        "--nano-primitive shape requires sphere|box|torus");
  vdb::NanoVdbPrimitiveOptions o;
  vdb::NanoVdbWriteOptions w;
  vdb::NanoVdbLimits l;
  std::set<std::string> seen;
  const auto argument = [&](int &i, const std::string &option) {
    auto s = value(argc, argv, i, option);
    if (missing(s))
      throw std::invalid_argument("nonempty value required for " + option);
    return s;
  };
  const auto real = [&](int &i, const std::string &option) {
    return nano_primitives_cli::finite(argument(i, option), option);
  };
  const auto u32 = [&](int &i, const std::string &option) {
    const auto n = count(argument(i, option), option);
    if (n > UINT32_MAX)
      throw std::invalid_argument(option + " exceeds uint32");
    return uint32_t(n);
  };
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i];
    if (!seen.insert(arg).second)
      throw std::invalid_argument("duplicate NanoVDB primitive option: " + arg);
    if (arg == "--nano-class") {
      const auto s = argument(i, arg);
      if (s == "level-set")
        o.gridClass = vdb::NanoVdbPrimitiveClass::LevelSet;
      else if (s == "fog")
        o.gridClass = vdb::NanoVdbPrimitiveClass::Fog;
      else
        throw std::invalid_argument("--nano-class requires level-set|fog");
    } else if (arg == "--nano-storage") {
      const auto s = argument(i, arg);
      if (s == "float")
        o.storage = vdb::NanoVdbPrimitiveStorage::Float;
      else if (s == "double")
        o.storage = vdb::NanoVdbPrimitiveStorage::Double;
      else
        throw std::invalid_argument("--nano-storage requires float|double");
    } else if (arg == "--voxel-size")
      o.voxelSize = real(i, arg);
    else if (arg == "--half-width")
      o.halfWidth = real(i, arg);
    else if (arg == "--center" || arg == "--origin") {
      auto &a = arg == "--center" ? o.center : o.origin;
      for (auto &x : a)
        x = real(i, arg);
    } else if (arg == "--radius" && shape == vdb::NanoVdbPrimitiveShape::Sphere)
      o.radius = real(i, arg);
    else if (arg == "--box-dimensions" &&
             shape == vdb::NanoVdbPrimitiveShape::Box)
      for (auto &x : o.boxDimensions)
        x = real(i, arg);
    else if (arg == "--major-radius" &&
             shape == vdb::NanoVdbPrimitiveShape::Torus)
      o.majorRadius = real(i, arg);
    else if (arg == "--minor-radius" &&
             shape == vdb::NanoVdbPrimitiveShape::Torus)
      o.minorRadius = real(i, arg);
    else if (arg == "--grid-name")
      o.gridName = argument(i, arg);
    else if (arg == "--native-statistics") {
      const auto s = argument(i, arg);
      if (s == "bounds")
        o.statistics = vdb::NanoVdbStatisticsMode::Bounds;
      else if (s == "minmax")
        o.statistics = vdb::NanoVdbStatisticsMode::MinMax;
      else if (s == "all")
        o.statistics = vdb::NanoVdbStatisticsMode::All;
      else
        throw std::invalid_argument(
            "--native-statistics requires bounds|minmax|all");
    } else if (arg == "--codec") {
      const auto s = argument(i, arg);
      if (s == "none")
        w.codec = vdb::NanoVdbCodec::None;
      else if (s == "zip")
        w.codec = vdb::NanoVdbCodec::Zip;
      else if (s == "blosc")
        w.codec = vdb::NanoVdbCodec::Blosc;
      else
        throw std::invalid_argument("--codec requires none|zip|blosc");
    } else if (arg == "--checksum") {
      const auto s = argument(i, arg);
      if (s == "disabled")
        w.checksum = vdb::NanoVdbChecksum::Disabled;
      else if (s == "header")
        w.checksum = vdb::NanoVdbChecksum::Header;
      else if (s == "full")
        w.checksum = vdb::NanoVdbChecksum::Full;
      else
        throw std::invalid_argument("--checksum requires disabled|header|full");
    } else if (arg == "--raw")
      w.raw = true;
    else if (arg == "--allow-float-map-rounding")
      w.allowFloatMapRounding = true;
    else if (arg == "--max-input-bytes")
      l.maxInputBytes = count(argument(i, arg), arg);
    else if (arg == "--max-native-bytes")
      l.maxNativeBytes = count(argument(i, arg), arg);
    else if (arg == "--max-workspace-bytes")
      l.maxWorkspaceBytes = count(argument(i, arg), arg);
    else if (arg == "--max-active-voxels")
      l.maxActiveVoxels = count(argument(i, arg), arg);
    else if (arg == "--max-stored-voxels")
      l.maxStoredLeafVoxels = count(argument(i, arg), arg);
    else if (arg == "--max-nodes")
      l.maxNodes = count(argument(i, arg), arg);
    else if (arg == "--max-nano-work")
      l.maxWork = count(argument(i, arg), arg);
    else if (arg == "--max-grids")
      l.maxGrids = u32(i, arg);
    else if (arg == "--max-name-bytes")
      l.maxNameBytes = u32(i, arg);
    else if (arg == "--max-metadata-bytes")
      l.maxMetadataBytes = u32(i, arg);
    else
      throw std::invalid_argument(
          "unknown/incompatible NanoVDB primitive option: " + arg);
  }
  if (w.raw && w.codec != vdb::NanoVdbCodec::None)
    throw std::invalid_argument(
        "raw NanoVDB primitive output requires --codec none");
  const auto capabilities = vdb::NanoVdbInterchangeCapabilities();
  if (!capabilities.nativeIo) {
    (void)vdb::WriteNanoVdbPrimitive(
        {}, shape, o, w, l); // Actual OFF stub before paths/report reserve.
    throw std::runtime_error("Native NanoVDB primitive authoring is "
                             "unavailable; enable ENABLE_NANOVDB_IO.");
  }
  auto native = l;
  nano_primitives_cli::Report report(argv[3], shapeName, o, w, l, native);
  const auto result =
      vdb::WriteNanoVdbPrimitive(path(argv[3]), shape, o, w, native);
  report.write(result, capabilities.openVdbConversion);
  if (!report.good) {
    std::cout << "{\"schemaVersion\":1,\"operation\":\"nano-primitive\","
                 "\"reportProduced\":false,\"reportSerializationOverflow\":"
                 "true,\"outputPublished\":true}\n";
    return EXIT_FAILURE;
  }
  std::cout.write(report.text.data(), std::streamsize(report.text.size()));
  std::cout.put('\n');
  return EXIT_SUCCESS;
}
