// TU prerequisites: OpenVdbBoundaryFluidStep.h, runtime IsAvailable(), common
// value(argc,argv,i,flag)/path(UTF8), <charconv>, <set> and <type_traits>.
// Include once and dispatch runBoundaryFluidStepCommand before the legacy
// parser. Append boundary_fluid_cli::Help to the Tool's help text.
namespace boundary_fluid_cli {
constexpr std::string_view Help =
    R"(  --step-boundary-fluid STATE.vdb DENSITY.vdb OUTPUT.vdb
    --velocity-grid S --fluid-grid S --face-kind-grid S --face-fraction-grid S
    --face-pressure-grid S --wall-velocity-grid S --density-grid/--grid S
    --temperature FILE --temperature-grid S
    --initial-velocity stored|require-solenoidal
    --time-step SEC --substeps N --max-substeps N --max-step-attempts N --max-cfl X
    --gravity X Y Z --buoyancy-density X --buoyancy-temperature X
    --buoyancy-direction X Y Z --ambient-temperature K
    --reservoir-density X --reservoir-temperature K --tolerance X --iterations N
    --max-fluid-cells N --max-stored-leaf-voxels/--max-voxels N
    --max-fluid-nodes N --max-fluid-work N --max-grids N --max-metadata-bytes N
    --max-input-bytes N --max-output-bytes N
    --density-name S --temperature-name S --velocity-name S --pressure-name S
    --fluid-name S --face-kind-name S --face-fraction-name S --face-pressure-name S
    --wall-velocity-name S
  Fixed canonical geometry; stationary walls; pressure inflow uses reservoirs,
  outflow uses the adjacent scalar. Conservative first-order scalar flux and
  declared advective MAC transport; no scalar normalization/viscosity addon.
  Output is canonical7 plus Double density/temperature. A restart binds all six
  boundary members AND both scalars plus initial-velocity policy. Old pressure is ignored; nonuniform pD adds
  a fresh impulse per accepted actual dt. Endpoint-CFL retries discard trials.
  Default transports actual stored MAC without cleanup/preprojection; measured
  divergence status is explicit. require-solenoidal enforces entry AND output.
)";
bool utf8(std::string_view s) noexcept {
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80)
      continue;
    size_t n;
    unsigned char lo = 0x80, hi = 0xbf;
    if (c >= 0xc2 && c <= 0xdf)
      n = 1;
    else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      if (c == 0xe0)
        lo = 0xa0;
      if (c == 0xed)
        hi = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      if (c == 0xf0)
        lo = 0x90;
      if (c == 0xf4)
        hi = 0x8f;
    } else
      return false;
    if (n > s.size() - i)
      return false;
    const auto first = static_cast<unsigned char>(s[i++]);
    if (first < lo || first > hi)
      return false;
    while (--n) {
      const auto x = static_cast<unsigned char>(s[i++]);
      if (x < 0x80 || x > 0xbf)
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
    throw std::invalid_argument("boundary fluid count exceeds range: " + flag);
  return n;
}
double real(std::string_view s, const std::string &flag, double low,
            double high, bool exclusiveLow = false) {
  double n{};
  const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
  if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size() ||
      !std::isfinite(n))
    throw std::invalid_argument("finite Double required for " + flag);
  if (n < low || n > high || (exclusiveLow && n == low))
    throw std::invalid_argument("boundary fluid numeric value exceeds range: " +
                                flag);
  return n;
}
// Archive::write keeps distinct nonempty raw names as unique descriptor names.
// GridDescriptor::nameAsString changes only the first native record separator.
// Build these exact selectors before publication, without assuming a [0] suffix.
std::string descriptorSelector(std::string_view raw) {
  std::string result("descriptor:");
  const auto separator = raw.find('\x1e');
  if (separator == std::string_view::npos)
    result.append(raw);
  else {
    result.append(raw.substr(0, separator));
    result.push_back('[');
    result.append(raw.substr(separator + 1));
    result.push_back(']');
  }
  return result;
}
struct Report {
  std::string text;
  uintmax_t reservation{};
  uint64_t formattingWork{};
  void need(size_t n) const {
    if (n > text.capacity() - text.size())
      throw std::invalid_argument(
          "boundary fluid bounded report reservation exhausted");
  }
  void append(std::string_view s) {
    need(s.size());
    text.append(s);
  }
  void character(char c) {
    need(1);
    text.push_back(c);
  }
  template <class T> void numeric(T value) {
    std::array<char, 128> b;
    std::to_chars_result r;
    if constexpr (std::is_floating_point_v<T>)
      r = std::to_chars(b.data(), b.data() + b.size(), value,
                        std::chars_format::general,
                        std::numeric_limits<T>::max_digits10);
    else
      r = std::to_chars(b.data(), b.data() + b.size(), value);
    if (r.ec != std::errc{})
      throw std::invalid_argument("boundary fluid numeric formatting failed");
    append({b.data(), size_t(r.ptr - b.data())});
  }
  void string(std::string_view s) {
    constexpr char hex[] = "0123456789abcdef";
    character('"');
    for (unsigned char c : s) {
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
  template <class T> void field(const char *key, T value) {
    append(",\"");
    append(key);
    append("\":");
    if constexpr (std::is_same_v<T, bool>)
      append(value ? "true" : "false");
    else
      numeric(value);
  }
  void stringField(const char *key, std::string_view value) {
    append(",\"");
    append(key);
    append("\":");
    string(value);
  }
  template <class T, size_t N> void array(const std::array<T, N> &value) {
    character('[');
    for (size_t i = 0; i < N; ++i) {
      if (i)
        character(',');
      numeric(value[i]);
    }
    character(']');
  }
  void binding(const char *key, std::string_view file, std::string_view grid) {
    append(",\"");
    append(key);
    append("\":{\"file\":");
    string(file);
    stringField("grid", grid);
    character('}');
  }
  Report(char **argv, const vdb::BoundaryProjectionReplay &state,
         std::string_view densityGrid, std::string_view temperatureFile,
         std::string_view temperatureGrid,
         const vdb::BoundaryFluidStepOptions &o, long double callerBytes) {
    // 64 accepted rows, eight1024-byte diagnostics, nine256-byte names and all
    //  escaped caller text fit the explicit reserve. No report DOM is built.
    const long double wanted = 262144 + 32 * callerBytes;
    const long double expected = wanted + 1 + 65536 + 32 * callerBytes;
    if (expected >= o.maxInputBytes || 4 * expected >= o.maxWork ||
        wanted > std::numeric_limits<size_t>::max())
      throw std::invalid_argument("boundary fluid report/options reserve "
                                  "exceeds shared byte/work caps");
    text.reserve(size_t(wanted));
    const long double retained =
        text.capacity() + 1.0L + 65536 + 32 * callerBytes;
    if (retained >= o.maxInputBytes || 4 * retained >= o.maxWork)
      throw std::invalid_argument("boundary fluid actual report/options "
                                  "reserve exceeds shared byte/work caps");
    reservation = uintmax_t(retained);
    formattingWork = uint64_t(4 * retained);
    append("{\"schemaVersion\":1,\"operation\":\"step-boundary-fluid\","
           "\"output\":");
    string(argv[4]);
    append(",\"bindings\":{\"state\":");
    string(argv[2]);
    stringField("velocityGrid", state.velocityGrid);
    stringField("fluidGrid", state.fluidGrid);
    stringField("faceKindGrid", state.faceKindGrid);
    stringField("faceFractionGrid", state.faceFractionGrid);
    stringField("facePressureGrid", state.facePressureGrid);
    stringField("wallVelocityGrid", state.wallVelocityGrid);
    binding("density", argv[3], densityGrid);
    append(",\"temperature\":");
    if (temperatureFile.empty())
      append("null");
    else {
      append("{\"file\":");
      string(temperatureFile);
      stringField("grid", temperatureGrid);
      character('}');
    }
    character('}');
    append(",\"gridNames\":{\"density\":");
    string(o.names.density);
    stringField("temperature", o.names.temperature);
    stringField("velocity", o.names.velocity);
    stringField("pressure", o.names.pressure);
    stringField("fluid", o.names.fluid);
    stringField("faceKind", o.names.faceKind);
    stringField("faceFraction", o.names.faceFraction);
    stringField("facePressure", o.names.facePressure);
    stringField("wallVelocity", o.names.wallVelocity);
    character('}');
    append(",\"controls\":{\"timeStep\":");
    numeric(o.timeStep);
    stringField("initialVelocity",o.initialVelocity==vdb::BoundaryFluidInitialVelocity::RequireSolenoidal?"require-solenoidal":"stored");
    field("substeps", o.substeps);
    field("maxSubsteps", o.maxSubsteps);
    field("maxStepAttempts", o.maxStepAttempts);
    field("maxCfl", o.maxCfl);
    append(",\"gravityWorld\":");
    array(o.gravity);
    field("buoyancyDensity", o.buoyancyDensity);
    field("buoyancyTemperature", o.buoyancyTemperature);
    field("ambientTemperature", o.ambientTemperature);
    append(",\"buoyancyDirectionWorld\":");
    array(o.buoyancyDirection);
    field("reservoirDensity", o.reservoirDensity);
    field("effectiveReservoirTemperature",
          o.reservoirTemperature.value_or(o.ambientTemperature));
    field("tolerance", o.tolerance);
    field("maxIterationsPerComponent", o.maxIterations);
    character('}');
    append(",\"limits\":{\"maxFluidCells\":");
    numeric(o.maxFluidCells);
    field("maxStoredLeafVoxels", o.maxStoredLeafVoxels);
    field("maxNodes", o.maxNodes);
    field("maxWork", o.maxWork);
    field("maxGrids", o.maxGrids);
    field("maxMetadataBytes", o.maxMetadataBytes);
    field("maxInputBytes", o.maxInputBytes);
    field("maxOutputBytes", o.maxOutputBytes);
    character('}');
    // A restart is a nine-grid binding, not generic one-grid reuse. Emit its
    // complete selectors before the native publishing call.
    append(",\"restart\":{\"state\":");
    string(argv[4]);
    stringField("initialVelocity",o.initialVelocity==vdb::BoundaryFluidInitialVelocity::RequireSolenoidal?"require-solenoidal":"stored");
    stringField("velocityGrid", descriptorSelector(o.names.velocity));
    stringField("fluidGrid", descriptorSelector(o.names.fluid));
    stringField("faceKindGrid", descriptorSelector(o.names.faceKind));
    stringField("faceFractionGrid", descriptorSelector(o.names.faceFraction));
    stringField("facePressureGrid", descriptorSelector(o.names.facePressure));
    stringField("wallVelocityGrid", descriptorSelector(o.names.wallVelocity));
    binding("density", argv[4], descriptorSelector(o.names.density));
    binding("temperature", argv[4], descriptorSelector(o.names.temperature));
    character('}');
  }
  void write(const vdb::BoundaryFluidStepResult &r) {
#define F(x) field(#x, r.x)
    F(fluidCells);
    F(incidentFaces);
    F(pressureFaces);
    F(wallFaces);
    F(requestedSubsteps);
    F(actualSubsteps);
    F(attempts);
    F(rejectedAttempts);
    F(simulatedTime);
    F(timePartitionError);
    F(cellVolume);
    F(maxTransportCfl);
    F(maxEndpointCfl);
    F(maxConstantResponseDrift);
    F(maxDivergenceBefore);
    F(maxDivergenceAfter);
    F(divergenceRoundoffBound);
    F(pressureResidual);
    F(maxWorldSpeedBefore);
    F(maxWorldSpeedAfter);
    F(densityMassBefore);
    F(densityMassAfter);
    F(densityOutwardTransfer);
    F(temperatureVolumeBefore);
    F(temperatureVolumeAfter);
    F(temperatureOutwardTransfer);
    F(densityBalanceError);
    F(temperatureBalanceError);
    F(densityBalanceRoundoffBound);
    F(temperatureBalanceRoundoffBound);
    F(reservoirFaceApplications);
    F(outflowFaceApplications);
    F(roundedScalarRows);
    F(roundedMomentumRows);
    F(totalProjectionIterations);
    F(nodeUpperBound);
    F(roundoffLimited);
    F(diagnosticUnderflow);
    F(temperatureInitialized);
    F(initialDivergenceWithinTolerance);
    F(converged);
#undef F
    field("nativeWorkUpperBound", r.workUpperBound);
    field("formattingWorkUpperBound", formattingWork);
    field("workUpperBound", r.workUpperBound + formattingWork);
    field("nativeWorkspaceUpperBound", r.workspaceUpperBound);
    field("reportReservation", reservation);
    field("workspaceUpperBound", r.workspaceUpperBound + reservation);
    append(",\"steps\":[");
    for (size_t i = 0; i < r.steps.size(); ++i) {
      if (i)
        character(',');
      const auto &s = r.steps[i];
      append("{\"timeStep\":");
      numeric(s.timeStep);
      field("transportCfl", s.transportCfl);
      field("endpointCfl", s.endpointCfl);
      field("constantResponseDrift", s.constantResponseDrift);
      field("maxDivergence", s.maxDivergence);
      field("pressureResidual", s.pressureResidual);
      field("projectionIterations", s.projectionIterations);
      character('}');
    }
    character(']');
    stringField(
        "scalarPolicy",
        "First-order shared conservative donor flux; reservoir on pressure "
        "inflow, adjacent fluid donor on outflow; no normalization/clamp. "
        "Stored divergence can change constants; the entry criterion status "
        "and requested initial-velocity policy are explicit.");
    stringField("momentumPolicy",
                "First-order advective MAC; direct local fluid paths, "
                "missing-path continuation, existing incident wall-normal "
                "samples zero. No conservative momentum/energy/no-slip claim.");
    stringField(
        "restartPolicy",
        "Bind canonical six AND both scalars plus initial-velocity policy from output9. Old pressure "
        "ignored; nonuniform prescribed pressure adds a fresh impulse per "
        "accepted actual dt. Retry partitions can affect results.");
    stringField(
        "speedPolicy",
        "Maximum of cell-centered opposing-MAC-face averages; not a certified "
        "face/whole-field maximum. CFL uses separate outgoing/face rates.");
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
        string(
            "Native diagnostic contains invalid UTF-8; use native API bytes");
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
} // namespace boundary_fluid_cli

std::optional<int> runBoundaryFluidStepCommand(int argc, char **argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--step-boundary-fluid")
    return {};
  if (argc < 5)
    throw std::invalid_argument(
        "--step-boundary-fluid requires STATE DENSITY OUTPUT before options");
  for (int i = 2; i < 5; ++i)
    if (!argv[i][0] || std::string_view(argv[i]).starts_with("--") ||
        std::string_view(argv[i]) == "-h")
      throw std::invalid_argument(
          "boundary fluid requires nonempty STATE DENSITY OUTPUT; use ./-h for "
          "a literal filename");
  long double callerBytes = 0;
  for (int i = 1; i < argc; ++i)
    callerBytes += std::string_view(argv[i]).size();
  if (callerBytes > 256 * 1024)
    throw std::invalid_argument("boundary fluid caller text exceeds256KiB");
  vdb::BoundaryFluidStepOptions o;
  vdb::BoundaryProjectionReplay state;
  std::string densityGrid, temperatureFile, temperatureGrid;
  std::set<std::string> seen;
  const auto argument = [&](int &i, const std::string &flag) {
    const auto s = value(argc, argv, i, flag);
    if (s.empty() || std::string_view(s).starts_with("--") || s == "-h")
      throw std::invalid_argument("missing nonempty value for " + flag);
    return s;
  };
  const auto amount = [&](int &i, const std::string &flag, uint64_t cap) {
    return boundary_fluid_cli::count(argument(i, flag), flag, cap);
  };
  const auto number = [&](int &i, const std::string &flag, double lo, double hi,
                          bool exclusive = false) {
    return boundary_fluid_cli::real(argument(i, flag), flag, lo, hi, exclusive);
  };
  for (int i = 5; i < argc; ++i) {
    const std::string arg = argv[i];
    const std::string key = (arg == "--grid" || arg == "--input-grid")
                                ? "--density-grid"
                            : arg == "--acceleration"   ? "--gravity"
                            : arg == "--max-iterations" ? "--iterations"
                            : arg == "--max-voxels" ? "--max-stored-leaf-voxels"
                            : arg == "--max-work"   ? "--max-fluid-work"
                            : arg == "--max-nodes"  ? "--max-fluid-nodes"
                                                    : arg;
    if (!seen.insert(key).second)
      throw std::invalid_argument("duplicate boundary fluid option: " + arg);
    if (arg == "--initial-velocity") {
      const auto policy=argument(i,arg);
      if(policy=="stored")o.initialVelocity=vdb::BoundaryFluidInitialVelocity::TransportStored;
      else if(policy=="require-solenoidal")o.initialVelocity=vdb::BoundaryFluidInitialVelocity::RequireSolenoidal;
      else throw std::invalid_argument("--initial-velocity requires stored|require-solenoidal");
    } else if (key == "--density-grid")
      densityGrid = argument(i, arg);
    else if (arg == "--temperature")
      temperatureFile = argument(i, arg);
    else if (arg == "--temperature-grid")
      temperatureGrid = argument(i, arg);
    else if (arg == "--velocity-grid")
      state.velocityGrid = argument(i, arg);
    else if (arg == "--fluid-grid")
      state.fluidGrid = argument(i, arg);
    else if (arg == "--face-kind-grid")
      state.faceKindGrid = argument(i, arg);
    else if (arg == "--face-fraction-grid")
      state.faceFractionGrid = argument(i, arg);
    else if (arg == "--face-pressure-grid")
      state.facePressureGrid = argument(i, arg);
    else if (arg == "--wall-velocity-grid")
      state.wallVelocityGrid = argument(i, arg);
    else if (arg == "--time-step")
      o.timeStep = number(i, arg, 1e-6, 3600);
    else if (arg == "--substeps")
      o.substeps = uint32_t(amount(i, arg, 64));
    else if (arg == "--max-substeps")
      o.maxSubsteps = uint32_t(amount(i, arg, 64));
    else if (arg == "--max-step-attempts")
      o.maxStepAttempts = uint32_t(amount(i, arg, 128));
    else if (arg == "--max-cfl")
      o.maxCfl = number(i, arg, 0, .5, true);
    else if (key == "--gravity")
      for (double &x : o.gravity)
        x = number(i, arg, -1e12, 1e12);
    else if (arg == "--buoyancy-density")
      o.buoyancyDensity = number(i, arg, 0, 1e6);
    else if (arg == "--buoyancy-temperature")
      o.buoyancyTemperature = number(i, arg, 0, 1e6);
    else if (arg == "--ambient-temperature")
      o.ambientTemperature = number(i, arg, 0, 1e6);
    else if (arg == "--buoyancy-direction")
      for (double &x : o.buoyancyDirection)
        x = number(i, arg, -std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max());
    else if (arg == "--reservoir-density")
      o.reservoirDensity = number(i, arg, 0, 1e6);
    else if (arg == "--reservoir-temperature")
      o.reservoirTemperature = number(i, arg, 0, 1e6);
    else if (arg == "--tolerance")
      o.tolerance = number(i, arg, 1e-12, 1e-2);
    else if (key == "--iterations")
      o.maxIterations = uint32_t(amount(i, arg, 10000));
    else if (arg == "--max-fluid-cells")
      o.maxFluidCells = amount(i, arg, 1'000'000);
    else if (key == "--max-stored-leaf-voxels")
      o.maxStoredLeafVoxels = amount(i, arg, UINT64_MAX);
    else if (key == "--max-fluid-nodes")
      o.maxNodes = amount(i, arg, 1'000'000);
    else if (key == "--max-fluid-work")
      o.maxWork = amount(i, arg, 10'000'000'000ull);
    else if (arg == "--max-grids")
      o.maxGrids = uint32_t(amount(i, arg, 4096));
    else if (arg == "--max-metadata-bytes")
      o.maxMetadataBytes = amount(i, arg, 16ull << 20);
    else if (arg == "--max-input-bytes")
      o.maxInputBytes = amount(i, arg, 16ull << 30);
    else if (arg == "--max-output-bytes")
      o.maxOutputBytes = amount(i, arg, 16ull << 30);
    else if (arg == "--density-name")
      o.names.density = argument(i, arg);
    else if (arg == "--temperature-name")
      o.names.temperature = argument(i, arg);
    else if (arg == "--velocity-name")
      o.names.velocity = argument(i, arg);
    else if (arg == "--pressure-name")
      o.names.pressure = argument(i, arg);
    else if (arg == "--fluid-name")
      o.names.fluid = argument(i, arg);
    else if (arg == "--face-kind-name")
      o.names.faceKind = argument(i, arg);
    else if (arg == "--face-fraction-name")
      o.names.faceFraction = argument(i, arg);
    else if (arg == "--face-pressure-name")
      o.names.facePressure = argument(i, arg);
    else if (arg == "--wall-velocity-name")
      o.names.wallVelocity = argument(i, arg);
    else
      throw std::invalid_argument(
          "unknown/incompatible boundary fluid option: " + arg);
  }
  if (!temperatureGrid.empty() && temperatureFile.empty())
    throw std::invalid_argument(
        "--temperature-grid requires --temperature FILE");
  if (o.maxSubsteps < o.substeps || o.maxStepAttempts < o.substeps ||
      o.timeStep / o.substeps < 1e-6)
    throw std::invalid_argument(
        "boundary fluid substep/attempt counts or minimum dt are inconsistent");
  if (std::abs(std::hypot(o.buoyancyDirection[0], o.buoyancyDirection[1],
                          o.buoyancyDirection[2]) -
               1) > 1e-10)
    throw std::invalid_argument(
        "boundary fluid buoyancy direction requires unit WORLD length");
  std::set<std::string> names;
  for (const auto *name :
       {&o.names.density, &o.names.temperature, &o.names.velocity,
        &o.names.pressure, &o.names.fluid, &o.names.faceKind,
        &o.names.faceFraction, &o.names.facePressure, &o.names.wallVelocity})
    if (name->empty() || name->size() > 256 ||
        name->find('\0') != std::string::npos || !names.insert(*name).second)
      throw std::invalid_argument("boundary fluid nine output names must be "
                                  "distinct1..256 non-NUL bytes");
  // Syntax/applicability is strict in either build. Valid OFF commands reach
  // the actual native stub before paths, UTF-8 validation or report allocation.
  if (!vdb::IsAvailable()) {
    (void)vdb::WriteBoundaryFluidStep({}, {}, o);
    return EXIT_FAILURE;
  }
  for (int i = 1; i < argc; ++i)
    if (!boundary_fluid_cli::utf8(argv[i]))
      throw std::invalid_argument(
          "boundary fluid caller text must be valid UTF-8 before publication");
  boundary_fluid_cli::Report report(argv, state, densityGrid, temperatureFile,
                                    temperatureGrid, o, callerBytes);
  auto native = o;
  native.maxInputBytes -= report.reservation;
  native.maxWork -= report.formattingWork;
  vdb::BoundaryFluidInputs input;
  input.boundary = state;
  input.boundary.state = path(argv[2]);
  input.density = {path(argv[3]), densityGrid};
  if (!temperatureFile.empty())
    input.temperature =
        vdb::BoundaryProjectionField{path(temperatureFile), temperatureGrid};
  const auto result = vdb::WriteBoundaryFluidStep(input, path(argv[4]), native);
  report.write(result);
  return EXIT_SUCCESS;
}
