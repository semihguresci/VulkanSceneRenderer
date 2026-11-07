// Include OpenVdbParticleGeometry.h and runtime IsAvailable(), plus common
// value(argc,argv,i,flag)/path(UTF8) helpers, <charconv>/<set>/<type_traits>.
// Dispatch runParticleGeometryCommand before the legacy parser; append Help to
// the Tool's --help text. Runtime availability is checked after strict parsing
// and before caller UTF-8/path conversion, report reservation or native I/O.
namespace particle_geometry_cli {
constexpr std::string_view Help =
    R"(  --particle-geometry INPUT.json OUTPUT.vdb  Sampled tapered sphere unions
    --particle-output sdf|mask|both --trail-time DT --trail-spacing X
    --particle-origin X Y Z --min-radius-voxels R --max-radius-voxels R
    --voxel-size H --half-width B --sdf-grid-name/--grid-name NAME
    --mask-grid-name NAME --max-particles N --max-trail-instances N
    --max-trail-length-voxels X --max-particle-nodes N
    --max-particle-work N --max-particle-metadata N
    --max-voxels N --max-input-bytes N --max-output-bytes N
  JSON v1 object: version=1, particles=[{position:[x,y,z],radius:r,velocity:[x,y,z]}].
  Velocity optional. Trails run OPPOSITE velocity*time; mask tests centers strictly
  inside sampled spheres, not cell coverage or SDF-band activity. No IDs/attributes.
)";
bool utf8(std::string_view s) noexcept {
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80)
      continue;
    size_t n;
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
    if (n > s.size() - i)
      return false;
    const auto first = static_cast<unsigned char>(s[i++]);
    if (first < low || first > high)
      return false;
    while (--n) {
      const auto d = static_cast<unsigned char>(s[i++]);
      if (d < 0x80 || d > 0xbf)
        return false;
    }
  }
  return true;
}
uint64_t count(std::string_view s, const std::string &flag, uint64_t maximum) {
  if (s.empty() || s.find_first_not_of("0123456789") != std::string_view::npos)
    throw std::invalid_argument("unsigned decimal required for " + flag);
  uint64_t n{};
  const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !n || n > maximum)
    throw std::invalid_argument("particle geometry count exceeds range: " +
                                flag);
  return n;
}
double real(std::string_view s, const std::string &flag) {
  double n{};
  const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
  if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size() ||
      !std::isfinite(n))
    throw std::invalid_argument("finite Double required for " + flag);
  return n;
}
struct Report {
  std::string text;
  uintmax_t reservation{};
  uint64_t formattingWork{};
  void need(size_t n) const {
    if (n > text.capacity() - text.size())
      throw std::invalid_argument(
          "particle geometry bounded report reservation exhausted; no partial "
          "JSON emitted");
  }
  void append(std::string_view s) {
    need(s.size());
    text.append(s);
  }
  void character(char c) {
    need(1);
    text.push_back(c);
  }
  template <class T> void numeric(T n) {
    std::array<char, 128> b;
    std::to_chars_result r;
    if constexpr (std::is_floating_point_v<T>)
      r = std::to_chars(b.data(), b.data() + b.size(), n,
                        std::chars_format::general,
                        std::numeric_limits<T>::max_digits10);
    else
      r = std::to_chars(b.data(), b.data() + b.size(), n);
    if (r.ec != std::errc{})
      throw std::invalid_argument(
          "particle geometry numeric formatting failed");
    append({b.data(), size_t(r.ptr - b.data())});
  }
  void string(std::string_view s) {
    constexpr char h[] = "0123456789abcdef";
    character('"');
    for (unsigned char c : s) {
      if (c == '"' || c == '\\') {
        character('\\');
        character(char(c));
      } else if (c < 0x20) {
        append("\\u00");
        character(h[c >> 4]);
        character(h[c & 15]);
      } else
        character(char(c));
    }
    character('"');
  }
  template <class T> void field(const char *key, T n) {
    append(",\"");
    append(key);
    append("\":");
    if constexpr (std::is_same_v<T, bool>)
      append(n ? "true" : "false");
    else
      numeric(n);
  }
  void stringField(const char *key, std::string_view s) {
    append(",\"");
    append(key);
    append("\":");
    string(s);
  }
  template <class T, size_t N> void array(const std::array<T, N> &a) {
    character('[');
    for (size_t i = 0; i < N; ++i) {
      if (i)
        character(',');
      numeric(a[i]);
    }
    character(']');
  }
  Report(int argc, char **argv, const vdb::ParticleGeometryOptions &o,
         const vdb::ProcessingOptions &p, long double callerBytes) {
    const long double wanted = 262144 + 32 * callerBytes;
    const long double expected = wanted + 1 + 65536 + 32 * callerBytes;
    if (expected >= p.maxInputBytes || 4 * expected >= o.maxWork ||
        wanted > std::numeric_limits<size_t>::max())
      throw std::invalid_argument("particle geometry report/options/format "
                                  "reserve exceeds shared byte/work caps");
    text.reserve(size_t(wanted));
    const long double retained =
        text.capacity() + 1.0L + 65536 + 32 * callerBytes;
    if (retained >= p.maxInputBytes || 4 * retained >= o.maxWork)
      throw std::invalid_argument(
          "particle geometry actual report/options/format reserve exceeds "
          "shared byte/work caps");
    reservation = uintmax_t(retained);
    formattingWork = uint64_t(4 * retained);
    append(
        "{\"schemaVersion\":1,\"operation\":\"particle-geometry\",\"input\":");
    string(argv[2]);
    stringField("output", argv[3]);
    stringField("outputMode",
                o.output == vdb::ParticleGeometryOutput::LevelSet ? "sdf"
                : o.output == vdb::ParticleGeometryOutput::Mask   ? "mask"
                                                                  : "both");
    append(",\"controls\":{\"timeStep\":");
    numeric(o.timeStep);
    field("spacing", o.spacing);
    field("minRadiusVoxels", o.minRadiusVoxels);
    field("maxRadiusVoxels", o.maxRadiusVoxels);
    field("voxelSize", p.voxelSize);
    append(",\"halfWidth\":");
    if (o.output == vdb::ParticleGeometryOutput::Mask)
      append("null");
    else
      numeric(p.halfWidth);
    append(",\"originWorld\":");
    if (o.originWorld)
      array(*o.originWorld);
    else
      append("null");
    character('}');
    append(",\"limits\":{\"maxParticles\":");
    numeric(o.maxParticles);
    field("maxTrailInstances", o.maxTrailInstances);
    field("maxTrailLengthVoxels", o.maxTrailLengthVoxels);
    field("maxNodes", o.maxNodes);
    field("maxWork", o.maxWork);
    field("maxMetadataBytes", o.maxMetadataBytes);
    field("maxOutputBytes", o.maxOutputBytes);
    field("maxInputBytes", p.maxInputBytes);
    field("maxVoxelCount", p.maxVoxelCount);
    character('}');
    (void)argc;
  }
  void grid(const std::optional<vdb::ProcessingResult> &g) {
    if (!g) {
      append("null");
      return;
    }
    append("{\"gridName\":");
    string(g->gridName);
    stringField("gridClass", g->gridClass);
    field("activeVoxelCount", g->activeVoxelCount);
    field("voxelSize", g->voxelSize);
    character('}');
  }
  void write(const vdb::ParticleGeometryResult &r) {
    append(",\"levelSet\":");
    grid(r.levelSet);
    append(",\"mask\":");
    grid(r.mask);
    append(",\"originWorld\":");
    array(r.originWorld);
    field("particleCount", r.particleCount);
    field("sphereParticleCount", r.sphereParticleCount);
    field("trailParticleCount", r.trailParticleCount);
    field("sphereInstances", r.sphereInstances);
    field("instanceUpperBound", r.instanceUpperBound);
    field("candidateVoxelUpperBound", r.candidateVoxelUpperBound);
    field("storedLeafVoxelUpperBound", r.storedLeafVoxelUpperBound);
    field("nodeUpperBound", r.nodeUpperBound);
    field("nativeWorkspaceUpperBound", r.workspaceUpperBound);
    field("reportReservation", reservation);
    field("workspaceUpperBound", r.workspaceUpperBound + reservation);
    field("nativeWorkUpperBound", r.workUpperBound);
    field("formattingWorkUpperBound", formattingWork);
    field("workUpperBound", r.workUpperBound + formattingWork);
    field("roundedDistanceValues", r.roundedDistanceValues);
    field("maximumDistanceAbsoluteError", r.maximumDistanceAbsoluteError);
    field("maximumPositionErrorVoxels", r.maximumPositionErrorVoxels);
    field("effectiveExteriorBand", r.effectiveExteriorBand);
    field("maximumTrailLengthWorld", r.maximumTrailLengthWorld);
    field("centerCandidateCount", r.centerCandidateCount);
    field("centerRepairPairUpperBound", r.centerRepairPairUpperBound);
    field("repairedNativeCenterValues", r.repairedNativeCenterValues);
    stringField(
        "trailPolicy",
        "Discrete tapered sphere union OPPOSITE velocity*timeStep; last stamp "
        "need not reach endpoint; no continuous capsule/mass/ODE claim.");
    stringField("maskPolicy",
                "Native voxel centers strictly inside sampled spheres, not "
                "cell intersections or SDF-band activity.");
    stringField("attributePolicy", "Source ordinal IDs and arbitrary particle "
                                   "attributes are not generated.");
    field("nativeDiagnosticCount", r.diagnostics.size());
    append(",\"diagnostics\":[");
    bool truncated = r.diagnostics.size() > 8;
    for (size_t i = 0; i < std::min<size_t>(8, r.diagnostics.size()); ++i) {
      if (i)
        character(',');
      const auto &s = r.diagnostics[i];
      size_t n = std::min<size_t>(1024, s.size());
      if (n < s.size())
        while (n && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80)
          --n;
      truncated |= n != s.size();
      const std::string_view prefix(s.data(), n);
      if (utf8(prefix))
        string(prefix);
      else {
        string("Native diagnostic has invalid UTF-8; inspect native API bytes");
        truncated = true;
      }
    }
    character(']');
    field("diagnosticsTruncated", truncated);
    character('}');
    std::cout.write(text.data(), std::streamsize(text.size()));
    std::cout.put('\n');
  }
};
} // namespace particle_geometry_cli

std::optional<int> runParticleGeometryCommand(int argc, char **argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--particle-geometry")
    return {};
  if (argc < 4)
    throw std::invalid_argument(
        "--particle-geometry requires INPUT OUTPUT before options");
  for (int i = 2; i < 4; ++i)
    if (!argv[i][0] || std::string_view(argv[i]).starts_with("--") ||
        std::string_view(argv[i]) == "-h")
      throw std::invalid_argument("--particle-geometry requires nonempty INPUT "
                                  "OUTPUT; use ./-h for a literal filename");
  long double callerBytes = 0;
  for (int i = 1; i < argc; ++i)
    callerBytes += std::string_view(argv[i]).size();
  if (callerBytes > 256 * 1024)
    throw std::invalid_argument("particle geometry caller text exceeds256KiB");
  vdb::ParticleGeometryOptions options;
  vdb::ProcessingOptions processing;
  std::set<std::string> seen;
  const auto argument = [&](int &i, const std::string &flag) {
    const auto s = value(argc, argv, i, flag);
    if (s.empty() || std::string_view(s).starts_with("--") || s == "-h")
      throw std::invalid_argument("missing nonempty value for " + flag);
    return s;
  };
  const auto amount = [&](int &i, const std::string &flag, uint64_t maximum) {
    return particle_geometry_cli::count(argument(i, flag), flag, maximum);
  };
  const auto number = [&](int &i, const std::string &flag) {
    return particle_geometry_cli::real(argument(i, flag), flag);
  };
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i],
                      key = arg == "--grid-name"   ? "--sdf-grid-name"
                            : arg == "--time-step" ? "--trail-time"
                                                   : arg;
    if (!seen.insert(key).second)
      throw std::invalid_argument("duplicate particle geometry option: " + arg);
    if (arg == "--particle-output") {
      const auto mode = argument(i, arg);
      if (mode == "sdf")
        options.output = vdb::ParticleGeometryOutput::LevelSet;
      else if (mode == "mask")
        options.output = vdb::ParticleGeometryOutput::Mask;
      else if (mode == "both")
        options.output = vdb::ParticleGeometryOutput::Both;
      else
        throw std::invalid_argument("--particle-output requires sdf|mask|both");
    } else if (key == "--trail-time")
      options.timeStep = number(i, arg);
    else if (arg == "--trail-spacing")
      options.spacing = number(i, arg);
    else if (arg == "--particle-origin") {
      std::array<double, 3> origin;
      for (auto &n : origin)
        n = number(i, arg);
      options.originWorld = origin;
    } else if (arg == "--min-radius-voxels")
      options.minRadiusVoxels = number(i, arg);
    else if (arg == "--max-radius-voxels")
      options.maxRadiusVoxels = number(i, arg);
    else if (arg == "--voxel-size")
      processing.voxelSize = number(i, arg);
    else if (arg == "--half-width")
      processing.halfWidth = number(i, arg);
    else if (key == "--sdf-grid-name")
      processing.gridName = argument(i, arg);
    else if (arg == "--mask-grid-name")
      options.maskGridName = argument(i, arg);
    else if (arg == "--max-particles")
      options.maxParticles = amount(i, arg, UINT32_MAX - 1);
    else if (arg == "--max-trail-instances")
      options.maxTrailInstances = amount(i, arg, 16'000'000);
    else if (arg == "--max-trail-length-voxels")
      options.maxTrailLengthVoxels = number(i, arg);
    else if (arg == "--max-particle-nodes")
      options.maxNodes = amount(i, arg, 1'000'000);
    else if (arg == "--max-particle-work")
      options.maxWork = amount(i, arg, 10'000'000'000ull);
    else if (arg == "--max-particle-metadata")
      options.maxMetadataBytes = amount(i, arg, 16ull << 20);
    else if (arg == "--max-voxels")
      processing.maxVoxelCount = amount(i, arg, UINT64_MAX);
    else if (arg == "--max-input-bytes")
      processing.maxInputBytes = amount(i, arg, 16ull << 30);
    else if (arg == "--max-output-bytes")
      options.maxOutputBytes = amount(i, arg, 16ull << 30);
    else
      throw std::invalid_argument(
          "unknown/incompatible particle geometry option: " + arg);
  }
  if (options.output == vdb::ParticleGeometryOutput::LevelSet &&
      seen.contains("--mask-grid-name"))
    throw std::invalid_argument(
        "--mask-grid-name requires mask or both output");
  if (options.output == vdb::ParticleGeometryOutput::Mask &&
      (seen.contains("--sdf-grid-name") || seen.contains("--half-width")))
    throw std::invalid_argument(
        "SDF grid-name/half-width controls require sdf or both output");
  if (!vdb::IsAvailable())
    throw std::runtime_error("OpenVDB particle geometry is unavailable; "
                             "configure with -DENABLE_OPENVDB=ON.");
  for (int i = 1; i < argc; ++i)
    if (!particle_geometry_cli::utf8(argv[i]))
      throw std::invalid_argument("particle geometry caller text must be valid "
                                  "UTF-8 before publication");
  particle_geometry_cli::Report report(argc, argv, options, processing,
                                       callerBytes);
  auto nativeProcessing = processing;
  nativeProcessing.maxInputBytes -= report.reservation;
  auto nativeOptions = options;
  nativeOptions.maxWork -= report.formattingWork;
  const auto result = vdb::WriteParticleGeometryFile(
      path(argv[2]), path(argv[3]), nativeOptions, nativeProcessing);
  report.write(result);
  return EXIT_SUCCESS;
}
