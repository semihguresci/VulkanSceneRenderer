// Include in OpenVdbTool's private namespace after value/number/count/path.
// The translation unit needs OpenVdbDiagnostics.h, the existing IsAvailable
// declaration and <charconv>. Dispatch runDiagnosticsCommand before the legacy
// parser. Exit0 means a report was produced, not that its checks passed.
namespace diagnostic_cli {
constexpr size_t TextLimit = 4096;
bool utf8(std::string_view text) noexcept {
  for (size_t i = 0; i < text.size();) {
    const auto a = static_cast<unsigned char>(text[i++]);
    if (a < 0x80)
      continue;
    size_t remaining;
    unsigned char low = 0x80, high = 0xbf;
    if (a >= 0xc2 && a <= 0xdf)
      remaining = 1;
    else if (a >= 0xe0 && a <= 0xef) {
      remaining = 2;
      if (a == 0xe0)
        low = 0xa0;
      if (a == 0xed)
        high = 0x9f;
    } else if (a >= 0xf0 && a <= 0xf4) {
      remaining = 3;
      if (a == 0xf0)
        low = 0x90;
      if (a == 0xf4)
        high = 0x8f;
    } else
      return false;
    if (remaining > text.size() - i)
      return false;
    const auto first = static_cast<unsigned char>(text[i++]);
    if (first < low || first > high)
      return false;
    while (--remaining) {
      const auto c = static_cast<unsigned char>(text[i++]);
      if (c < 0x80 || c > 0xbf)
        return false;
    }
  }
  return true;
}
const char *profile(vdb::DiagnosticProfile p) noexcept {
  switch (p) {
  case vdb::DiagnosticProfile::StoredValues:
    return "stored";
  case vdb::DiagnosticProfile::SymmetricLevelSet:
    return "sdf";
  case vdb::DiagnosticProfile::NormalizedFog:
    return "fog";
  case vdb::DiagnosticProfile::PointData:
    return "points";
  }
  return "unknown";
}
const char *gradient(vdb::DiagnosticGradient p) noexcept {
  switch (p) {
  case vdb::DiagnosticGradient::None:
    return "none";
  case vdb::DiagnosticGradient::FirstOrder:
    return "first";
  case vdb::DiagnosticGradient::WenoInterface:
    return "weno";
  }
  return "unknown";
}
const char *status(vdb::DiagnosticStatus p) noexcept {
  switch (p) {
  case vdb::DiagnosticStatus::Pass:
    return "pass";
  case vdb::DiagnosticStatus::Fail:
    return "fail";
  case vdb::DiagnosticStatus::Unavailable:
    return "unavailable";
  case vdb::DiagnosticStatus::NotApplicable:
    return "not-applicable";
  }
  return "unknown";
}
void name(std::string_view text) {
  if (text.empty() || text.size() > 256 ||
      text.find('\0') != std::string_view::npos)
    throw std::invalid_argument(
        "diagnostic names must contain1..256 bytes without NUL");
}
double finiteNumber(const std::string &text, const std::string &option) {
  std::string_view digits = text;
  if (digits.starts_with('+')) {
    digits.remove_prefix(1);
    if (digits.starts_with('-'))
      throw std::invalid_argument("invalid Double decimal for " + option);
  }
  double result = 0;
  const auto parsed =
      std::from_chars(digits.data(), digits.data() + digits.size(), result,
                      std::chars_format::general);
  if (digits.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != digits.data() + digits.size() || !std::isfinite(result))
    throw std::invalid_argument(
        "finite representable Double decimal required for " + option);
  return result;
}
double rangeBound(const std::string &text, const std::string &option) {
  const double result = finiteNumber(text, option);
  // Bounds are Double. Do not accept an exact-looking int64 literal and then
  // silently change its endpoint by rounding that integer through Double.
  int64_t integer = 0;
  std::string_view digits = text;
  if (digits.starts_with('+'))
    digits.remove_prefix(1);
  const auto parsed =
      std::from_chars(digits.data(), digits.data() + digits.size(), integer);
  if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size()) {
    if (result >= 0x1p63 || result < -0x1p63 ||
        static_cast<int64_t>(result) != integer)
      throw std::invalid_argument(
          "diagnostic range bounds are Double; this integer endpoint is not "
          "exactly representable");
  }
  return result;
}

// The native result has bounded check/example cardinality. Every native string
// has a fixed display prefix and explicit original-byte count/truncation. All
// text storage is reserved/subtracted BEFORE the operation can publish a mask.
// No DOM, encoding allocation, stream formatting or capacity growth is used
// after publication. Caller text is validated before path conversion.
struct Report {
  std::string text;
  std::string_view inputPath, outputPath, selectedGrid;
  uintmax_t reservation = 0;
  uint64_t workReservation = 0;
  size_t limit = 0;
  bool overflow = false;
  Report(int argc, char **argv, const vdb::DiagnosticsOptions &o,
         const vdb::ProcessingOptions &p, std::string_view selector = {},
         bool writing = false) {
    if (argc > 2)
      inputPath = argv[2];
    if (writing && argc > 3)
      outputPath = argv[3];
    selectedGrid = selector;
    long double callerBytes = 0;
    for (int i = 0; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (!utf8(arg))
        throw std::invalid_argument("diagnostic CLI text must be valid UTF-8");
      callerBytes += arg.size();
    }
    const long double checks = 32.0L + o.pointAttributes.size();
    const long double bytes = 1024.0L * 1024 + 32 * callerBytes +
                              checks * (73728.0L + o.maxExamples * 33792.0L) +
                              o.pointAttributes.size() * 4096.0L;
    if (!std::isfinite(bytes) || bytes >= p.maxInputBytes ||
        bytes > std::numeric_limits<size_t>::max())
      throw std::invalid_argument(
          "diagnostic CLI report/options reserve exceeds --max-input-bytes");
    if (4 * std::ceil(bytes) >= o.maxWork)
      throw std::invalid_argument("diagnostic CLI report serialization reserve "
                                  "exceeds --max-diagnostic-work");
    reservation = uintmax_t(std::ceil(bytes));
    workReservation = 4 * uint64_t(reservation);
    limit = size_t(reservation);
    text.reserve(limit);
  }
  void append(std::string_view value) noexcept {
    if (value.size() > limit - text.size()) {
      overflow = true;
      return;
    }
    text.append(value.data(), value.size());
  }
  void character(char c) noexcept { append(std::string_view(&c, 1)); }
  void boolean(bool value) noexcept { append(value ? "true" : "false"); }
  template <class T> void numeric(T value) noexcept {
    std::array<char, 128> buffer{};
    std::to_chars_result r;
    if constexpr (std::is_floating_point_v<T>)
      r = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                        std::chars_format::general,
                        std::numeric_limits<T>::max_digits10);
    else
      r = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (r.ec != std::errc{}) {
      overflow = true;
      append("null");
      return;
    }
    append(std::string_view(buffer.data(), size_t(r.ptr - buffer.data())));
  }
  void quoted(std::string_view value) noexcept {
    constexpr char hex[] = "0123456789abcdef";
    character('"');
    for (const unsigned char c : value) {
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
  void key(std::string_view name) noexcept {
    quoted(name);
    character(':');
  }
  void hex(std::string_view value) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    character('"');
    for (const unsigned char c : value) {
      character(digits[c >> 4]);
      character(digits[c & 15]);
    }
    character('"');
  }
  void stringField(std::string_view name, std::string_view value) noexcept {
    key(name);
    if (value.size() <= TextLimit && utf8(value))
      quoted(value);
    else
      append("null");
    append(",\"");
    append(name);
    append("BytesHex\":");
    hex(value.substr(0, TextLimit));
    append(",\"");
    append(name);
    append("ByteCount\":");
    numeric(value.size());
    append(",\"");
    append(name);
    append("Truncated\":");
    boolean(value.size() > TextLimit);
  }
  template <class T> void numberField(std::string_view name, T value) noexcept {
    key(name);
    numeric(value);
  }
  void boolField(std::string_view name, bool value) noexcept {
    key(name);
    boolean(value);
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
  void strings(const std::vector<std::string> &values) noexcept {
    character('[');
    for (size_t i = 0; i < values.size(); ++i) {
      if (i)
        character(',');
      quoted(values[i]);
    }
    character(']');
  }
  void settings(const vdb::DiagnosticsOptions &o,
                const vdb::ProcessingOptions &p,
                const vdb::PointAttributeOptions &points,
                bool writing) noexcept {
    append("\"settings\":{\"input\":");
    quoted(inputPath);
    append(",\"output\":");
    if (writing)
      quoted(outputPath);
    else
      append("null");
    append(",\"selector\":");
    quoted(selectedGrid);
    append(",\"componentRange\":");
    if (o.componentRange)
      array(*o.componentRange);
    else
      append("null");
    append(",\"rangeScope\":");
    quoted(o.rangeScope == vdb::DiagnosticScope::Active ? "active" : "stored");
    append(",\"gradient\":");
    quoted(gradient(o.gradient));
    append(",\"gradientNormRange\":");
    array(o.gradientNormRange);
    append(",\"footprint\":");
    quoted(o.footprint == vdb::DiagnosticFootprint::RequireActive ? "active"
                                                                  : "stored");
    append(",\"magnitudeTolerance\":");
    numeric(o.magnitudeTolerance);
    append(",\"pointAttributes\":");
    strings(o.pointAttributes);
    append(",\"pointGroups\":{\"anyOf\":");
    strings(o.pointGroups.anyOf);
    append(",\"allOf\":");
    strings(o.pointGroups.allOf);
    append(",\"noneOf\":");
    strings(o.pointGroups.noneOf);
    append("},\"maskGridName\":");
    if (writing)
      quoted(o.maskGridName);
    else
      append("null");
    append(",\"caps\":{\"maxWork\":");
    numeric(o.maxWork);
    append(",\"nativeMaxWork\":");
    numeric(o.maxWork - workReservation);
    append(",\"reportReservedWork\":");
    numeric(workReservation);
    append(",\"maxGrids\":");
    numeric(o.maxGrids);
    append(",\"maxMetadataBytes\":");
    numeric(o.maxMetadataBytes);
    append(",\"maxExamples\":");
    numeric(o.maxExamples);
    append(",\"maxVoxelCount\":");
    numeric(p.maxVoxelCount);
    append(",\"maxInputBytes\":");
    numeric(p.maxInputBytes);
    append(",\"nativeMaxInputBytes\":");
    numeric(p.maxInputBytes - reservation);
    append(",\"reportReservedBytes\":");
    numeric(reservation);
    append(",\"maxPoints\":");
    numeric(points.maxPoints);
    append(",\"maxAttributes\":");
    numeric(points.maxAttributes);
    append(",\"maxGroups\":");
    numeric(points.maxGroups);
    append(",\"maxStringBytes\":");
    numeric(points.maxStringBytes);
    append("}}");
  }
  void write(const vdb::DiagnosticReport &r, const vdb::DiagnosticsOptions &o,
             const vdb::ProcessingOptions &p,
             const vdb::PointAttributeOptions &pointLimits,
             bool writing) noexcept {
    append("{\"schemaVersion\":1,\"operation\":");
    quoted(writing ? "diagnostic-mask" : "diagnose");
    append(",\"openvdbAvailable\":true,\"profile\":");
    quoted(profile(o.profile));
    character(',');
    settings(o, p, pointLimits, writing);
    append(",\"nativeStringPrefixBytes\":4096,\"reportProduced\":true,");
    stringField("descriptorName", r.descriptorName);
    character(',');
    stringField("gridName", r.gridName);
    character(',');
    stringField("valueType", r.valueType);
    character(',');
    stringField("gridClass", r.gridClass);
    character(',');
    stringField("vectorType", r.vectorType);
    append(",\"inWorldSpace\":");
    boolean(r.inWorldSpace);
    append(",\"affine\":");
    boolean(r.affine);
    append(",\"indexToWorld\":");
    if (r.indexToWorld)
      array(*r.indexToWorld);
    else
      append("null");
#define DIAGNOSTIC_NUMBER(n)                                                   \
  append(",\"" #n "\":");                                                      \
  numeric(r.n)
    DIAGNOSTIC_NUMBER(leafCount);
    DIAGNOSTIC_NUMBER(storedLeafSlots);
    DIAGNOSTIC_NUMBER(activeVoxelCount);
    DIAGNOSTIC_NUMBER(activeTileCount);
    DIAGNOSTIC_NUMBER(inactiveTileCount);
    DIAGNOSTIC_NUMBER(decodedBytes);
    DIAGNOSTIC_NUMBER(workUpperBound);
    DIAGNOSTIC_NUMBER(maskVoxelCount);
#undef DIAGNOSTIC_NUMBER
    append(",\"combinedWorkUpperBound\":");
    numeric(r.workUpperBound + workReservation);
    append(",\"allRequestedPassed\":");
    boolean(r.allRequestedPassed);
    append(",\"complete\":");
    boolean(r.complete);
    append(",\"unlocatedFailures\":");
    boolean(r.unlocatedFailures);
    append(",\"checks\":[");
    for (size_t i = 0; i < r.checks.size(); ++i) {
      if (i)
        character(',');
      const auto &c = r.checks[i];
      character('{');
      stringField("id", c.id);
      character(',');
      stringField("description", c.description);
      append(",\"status\":");
      quoted(status(c.status));
      append(",\"coverageComplete\":");
      boolean(c.coverageComplete);
#define DIAGNOSTIC_COUNT(n)                                                    \
  append(",\"" #n "\":");                                                      \
  numeric(c.n)
      DIAGNOSTIC_COUNT(examinedLeafVoxels);
      DIAGNOSTIC_COUNT(examinedTiles);
      DIAGNOSTIC_COUNT(examinedPoints);
      DIAGNOSTIC_COUNT(failedLeafVoxels);
      DIAGNOSTIC_COUNT(failedTiles);
      DIAGNOSTIC_COUNT(failedTileVoxelVolume);
      DIAGNOSTIC_COUNT(failedPoints);
      DIAGNOSTIC_COUNT(candidateCount);
      DIAGNOSTIC_COUNT(evaluatedCount);
      DIAGNOSTIC_COUNT(missingFootprintCount);
      DIAGNOSTIC_COUNT(nonfiniteFootprintCount);
      DIAGNOSTIC_COUNT(inactiveContinuationCount);
#undef DIAGNOSTIC_COUNT
      append(",\"backgroundInspected\":");
      boolean(c.backgroundInspected);
      append(",\"backgroundFailed\":");
      boolean(c.backgroundFailed);
      append(",\"examples\":[");
      for (size_t j = 0; j < c.examples.size(); ++j) {
        if (j)
          character(',');
        const auto &e = c.examples[j];
        append("{\"index\":");
        if (e.index)
          array(*e.index);
        else
          append("null");
        append(",\"pointOrdinal\":");
        if (e.pointOrdinal)
          numeric(*e.pointOrdinal);
        else
          append("null");
        character(',');
        stringField("detail", e.detail);
        character('}');
      }
      append("]}");
    }
    append("],\"points\":");
    if (!r.points)
      append("null");
    else {
      const auto &points = *r.points;
      append("{\"total\":");
      numeric(points.total);
      append(",\"selected\":");
      numeric(points.selected);
      append(",\"selectedActive\":");
      numeric(points.selectedActive);
      append(",\"selectedInactive\":");
      numeric(points.selectedInactive);
      append(",\"archivedSchema\":");
      boolean(points.archivedSchema);
      append(",\"countsValid\":");
      boolean(points.countsValid);
      append(",\"checkedAttributes\":");
      strings(points.checkedAttributes);
      character('}');
    }
    append(",\"mask\":");
    if (!r.mask)
      append("null");
    else {
      character('{');
      stringField("gridName", r.mask->gridName);
      character(',');
      stringField("gridClass", r.mask->gridClass);
      append(",\"valueType\":\"bool\",\"activeVoxelCount\":");
      numeric(r.mask->activeVoxelCount);
      append(",\"voxelSize\":");
      numeric(r.mask->voxelSize);
      character('}');
    }
    append(",\"diagnostics\":[");
    for (size_t i = 0; i < r.diagnostics.size(); ++i) {
      if (i)
        character(',');
      const auto &d = r.diagnostics[i];
      append("{\"text\":");
      if (d.size() <= TextLimit && utf8(d))
        quoted(d);
      else
        append("null");
      append(",\"bytesHex\":");
      hex(std::string_view(d).substr(0, TextLimit));
      append(",\"byteCount\":");
      numeric(d.size());
      append(",\"truncated\":");
      boolean(d.size() > TextLimit);
      character('}');
    }
    append("]}");
    if (overflow) {
      // This is an internal-version guard, not a source-dependent error path:
      // current native cardinalities are<=32+attributes and<=maxExamples.
      // Keep an already published mask, emit valid bounded JSON, never
      // allocate.
      std::cout << "{\"schemaVersion\":1,\"reportProduced\":false,"
                   "\"reportSerializationOverflow\":true,\"maskPublished\":"
                << (writing ? "true" : "false") << "}\n";
    } else {
      std::cout.write(text.data(), std::streamsize(text.size()));
      std::cout.put('\n');
    }
  }
};
} // namespace diagnostic_cli

std::optional<int> runDiagnosticsCommand(int argc, char **argv) {
  if (argc < 2)
    return {};
  const std::string_view command = argv[1];
  const bool writing = command == "--diagnostic-mask";
  if (command != "--diagnose" && !writing)
    return {};
  const auto missing = [](std::string_view text) {
    return text.empty() || text == "-h" || text.starts_with("--");
  };
  const int first = writing ? 4 : 3;
  if (argc < first || missing(argv[2]) || (writing && missing(argv[3])))
    throw std::invalid_argument(
        "diagnostics requires nonempty INPUT and mask OUTPUT before options; "
        "use ./-h for a literal filename");
  vdb::DiagnosticsOptions o;
  vdb::ProcessingOptions p;
  vdb::PointAttributeOptions pointLimits;
  std::string selector;
  std::set<std::string> seen, attributes;
  std::array<std::set<std::string>, 3> groups;
  const auto argument = [&](int &i, const std::string &option) {
    const auto result = value(argc, argv, i, option);
    if (missing(result))
      throw std::invalid_argument("missing nonempty value for " + option);
    return result;
  };
  const auto positive = [&](int &i, const std::string &option,
                            uint64_t maximum = UINT64_MAX) {
    const auto n = count(argument(i, option), option);
    if (n > maximum)
      throw std::invalid_argument("diagnostic option exceeds its range: " +
                                  option);
    return n;
  };
  for (int i = first; i < argc; ++i) {
    const std::string arg = argv[i];
    std::string key = arg == "--input-grid" ? "--grid" : arg;
    if (arg == "--any-group")
      key = "--point-any-group";
    else if (arg == "--all-group")
      key = "--point-all-group";
    else if (arg == "--exclude-group")
      key = "--point-none-group";
    if (key == "--diagnostic-attribute") {
      const auto name = argument(i, arg);
      diagnostic_cli::name(name);
      if (name == "P" || !attributes.insert(name).second ||
          attributes.size() > 1024)
        throw std::invalid_argument("diagnostic numeric attributes must be "
                                    "unique, exclude P, and count<=1024");
      o.pointAttributes.push_back(name);
      continue;
    }
    if (key == "--point-any-group" || key == "--point-all-group" ||
        key == "--point-none-group") {
      const size_t kind = key == "--point-any-group"   ? 0
                          : key == "--point-all-group" ? 1
                                                       : 2;
      const auto name = argument(i, arg);
      diagnostic_cli::name(name);
      if (!groups[kind].insert(name).second || groups[kind].size() > 4096)
        throw std::invalid_argument(
            "diagnostic group list contains duplicates or exceeds4096 terms");
      (kind == 0   ? o.pointGroups.anyOf
       : kind == 1 ? o.pointGroups.allOf
                   : o.pointGroups.noneOf)
          .push_back(name);
      continue;
    }
    if (!seen.insert(key).second)
      throw std::invalid_argument("duplicate diagnostic option: " + arg);
    if (arg == "--diagnostic-profile") {
      const auto text = argument(i, arg);
      if (text == "stored")
        o.profile = vdb::DiagnosticProfile::StoredValues;
      else if (text == "sdf")
        o.profile = vdb::DiagnosticProfile::SymmetricLevelSet;
      else if (text == "fog")
        o.profile = vdb::DiagnosticProfile::NormalizedFog;
      else if (text == "points")
        o.profile = vdb::DiagnosticProfile::PointData;
      else
        throw std::invalid_argument(
            "--diagnostic-profile requires stored, sdf, fog or points");
    } else if (arg == "--diagnostic-range") {
      const double low = diagnostic_cli::rangeBound(argument(i, arg), arg),
                   high = diagnostic_cli::rangeBound(argument(i, arg), arg);
      if (low > high)
        throw std::invalid_argument("diagnostic range bounds must be ordered");
      o.componentRange = std::array<double, 2>{low, high};
    } else if (arg == "--diagnostic-range-scope") {
      const auto text = argument(i, arg);
      if (text == "active")
        o.rangeScope = vdb::DiagnosticScope::Active;
      else if (text == "stored")
        o.rangeScope = vdb::DiagnosticScope::Stored;
      else
        throw std::invalid_argument(
            "--diagnostic-range-scope requires active or stored");
    } else if (arg == "--diagnostic-gradient") {
      const auto text = argument(i, arg);
      if (text == "none")
        o.gradient = vdb::DiagnosticGradient::None;
      else if (text == "first")
        o.gradient = vdb::DiagnosticGradient::FirstOrder;
      else if (text == "weno")
        o.gradient = vdb::DiagnosticGradient::WenoInterface;
      else
        throw std::invalid_argument(
            "--diagnostic-gradient requires none, first or weno");
    } else if (arg == "--diagnostic-norm-range") {
      o.gradientNormRange = {
          diagnostic_cli::finiteNumber(argument(i, arg), arg),
          diagnostic_cli::finiteNumber(argument(i, arg), arg)};
      if (o.gradientNormRange[0] < 0 ||
          o.gradientNormRange[0] > o.gradientNormRange[1])
        throw std::invalid_argument(
            "diagnostic norm bounds must be nonnegative and ordered");
      for (double n : o.gradientNormRange)
        if (!std::isfinite(n * n) || (n != 0 && n * n == 0))
          throw std::invalid_argument(
              "diagnostic squared norm bounds lose Double range");
    } else if (arg == "--diagnostic-footprint") {
      const auto text = argument(i, arg);
      if (text == "active")
        o.footprint = vdb::DiagnosticFootprint::RequireActive;
      else if (text == "stored")
        o.footprint = vdb::DiagnosticFootprint::NativeStoredContinuation;
      else
        throw std::invalid_argument(
            "--diagnostic-footprint requires active or stored");
    } else if (arg == "--diagnostic-tolerance") {
      o.magnitudeTolerance =
          diagnostic_cli::finiteNumber(argument(i, arg), arg);
      if (o.magnitudeTolerance < 0)
        throw std::invalid_argument("diagnostic tolerance must be nonnegative");
    } else if (arg == "--diagnostic-mask-name") {
      o.maskGridName = argument(i, arg);
      diagnostic_cli::name(o.maskGridName);
    } else if (arg == "--max-diagnostic-work")
      o.maxWork = positive(i, arg, 10'000'000'000ULL);
    else if (arg == "--max-diagnostic-grids")
      o.maxGrids = size_t(positive(i, arg, 4096));
    else if (arg == "--max-diagnostic-metadata")
      o.maxMetadataBytes =
          size_t(positive(i, arg, std::numeric_limits<size_t>::max()));
    else if (arg == "--max-diagnostic-examples") {
      const auto text = argument(i, arg);
      uint64_t n = 0;
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), n);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
          text.empty() ||
          text.find_first_not_of("0123456789") != std::string::npos || n > 128)
        throw std::invalid_argument(
            "--max-diagnostic-examples requires an unsigned count in[0,128]");
      o.maxExamples = size_t(n);
    } else if (arg == "--max-points")
      pointLimits.maxPoints = size_t(positive(
          i, arg,
          std::min<uint64_t>(INT64_MAX, std::numeric_limits<size_t>::max())));
    else if (arg == "--max-attributes")
      pointLimits.maxAttributes = size_t(positive(i, arg, 1024));
    else if (arg == "--max-groups")
      pointLimits.maxGroups = size_t(positive(i, arg, 4096));
    else if (arg == "--max-string-bytes")
      pointLimits.maxStringBytes = positive(i, arg);
    else if (key == "--grid")
      selector = argument(i, arg);
    else if (arg == "--max-voxels")
      p.maxVoxelCount = positive(i, arg);
    else if (arg == "--max-input-bytes")
      p.maxInputBytes = positive(i, arg);
    else
      throw std::invalid_argument("unknown/incompatible diagnostic option: " +
                                  arg);
  }
  const bool points = o.profile == vdb::DiagnosticProfile::PointData,
             sdf = o.profile == vdb::DiagnosticProfile::SymmetricLevelSet,
             fog = o.profile == vdb::DiagnosticProfile::NormalizedFog;
  if (!writing && seen.contains("--diagnostic-mask-name"))
    throw std::invalid_argument(
        "--diagnostic-mask-name requires --diagnostic-mask");
  if (seen.contains("--diagnostic-range-scope") &&
      (!o.componentRange || points))
    throw std::invalid_argument(
        "diagnostic range scope requires a field component range");
  if (seen.contains("--diagnostic-gradient") && !sdf)
    throw std::invalid_argument("diagnostic gradient requires the sdf profile");
  if ((seen.contains("--diagnostic-norm-range") ||
       seen.contains("--diagnostic-footprint")) &&
      o.gradient == vdb::DiagnosticGradient::None)
    throw std::invalid_argument(
        "diagnostic norm/footprint requires a non-none gradient");
  if (seen.contains("--diagnostic-tolerance") && !sdf && !fog)
    throw std::invalid_argument(
        "diagnostic tolerance requires sdf or fog profile");
  if (!points &&
      (!attributes.empty() || !groups[0].empty() || !groups[1].empty() ||
       !groups[2].empty() || seen.contains("--max-points") ||
       seen.contains("--max-attributes") || seen.contains("--max-groups") ||
       seen.contains("--max-string-bytes")))
    throw std::invalid_argument(
        "diagnostic point settings require the points profile");
  if (points && o.componentRange && attributes.empty())
    throw std::invalid_argument(
        "diagnostic point range requires selected numeric attributes");
  if (attributes.size() > pointLimits.maxAttributes ||
      groups[0].size() + groups[1].size() + groups[2].size() >
          3 * pointLimits.maxGroups)
    throw std::invalid_argument(
        "diagnostic selected attributes/groups exceed point caps");
  if (!vdb::IsAvailable()) {
    // Syntax is checked above; native availability precedes path conversion,
    // report reservation, filesystem access and source-dependent validation.
    (void)vdb::DiagnoseVdb({}, o, {}, p, pointLimits);
    return EXIT_FAILURE;
  }
  diagnostic_cli::Report report(argc, argv, o, p, selector, writing);
  auto native = p;
  native.maxInputBytes -= report.reservation;
  auto nativeOptions = o;
  nativeOptions.maxWork -= report.workReservation;
  const auto result = writing ? vdb::WriteVdbDiagnosticMask(
                                    path(argv[2]), path(argv[3]), nativeOptions,
                                    selector, native, pointLimits)
                              : vdb::DiagnoseVdb(path(argv[2]), nativeOptions,
                                                 selector, native, pointLimits);
  report.write(result, o, p, pointLimits, writing);
  return report.overflow ? EXIT_FAILURE : EXIT_SUCCESS;
}
