// Include after the tool's value/number/count/path helpers and public
// OpenVdbFieldResampling.h include. Dispatch runFieldResamplingCommand before
// the legacy parser. Availability is runtime, never a private OpenVDB macro.
// Help:
//   --resample-field INPUT.vdb OUTPUT.vdb  Ordinary typed affine pull sampling
//   --field-target-transform M00 ... M33  Optional target index-to-world map
//   --field-world-transform M00 ... M33   World deformation, identity default
//   --field-interpolation nearest|linear|quadratic (default linear)
//   --field-activity nearest|any           Independent source activity policy
//   --field-vector-values semantic|raw    World vector deformation policy
//   --max-field-resampling-work/grids/metadata N
//   --grid/--input-grid NAME, --grid-name NAME, --max-voxels/input-bytes N
// Matrices: 16 row-major COLUMN entries, translation at3/7/11, final0/0/0/1.
std::optional<int> runFieldResamplingCommand(int argc, char **argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--resample-field")
    return {};
  if (argc < 4)
    throw std::invalid_argument(
        "--resample-field requires INPUT OUTPUT before options");
  for (int i = 2; i < 4; ++i)
    if (!argv[i][0] || std::string_view(argv[i]).starts_with("--") ||
        std::string_view(argv[i]) == "-h")
      throw std::invalid_argument(
          "--resample-field requires nonempty INPUT OUTPUT before options; "
          "use ./-h for a literal filename");
  vdb::FieldResamplingOptions options;
  vdb::ProcessingOptions processing;
  processing.gridName = "field";
  std::string selector;
  std::set<std::string> seen;
  const auto argument = [&](int &i, const std::string &option) {
    const auto text = value(argc, argv, i, option);
    if (text.empty() || std::string_view(text).starts_with("--") ||
        text == "-h")
      throw std::invalid_argument("missing nonempty value for " + option);
    return text;
  };
  const auto matrix = [&](int &i, const std::string &option) {
    std::array<double, 16> result;
    for (auto &entry : result)
      entry = number(argument(i, option), option);
    if (result[12] != 0 || result[13] != 0 || result[14] != 0 ||
        result[15] != 1)
      throw std::invalid_argument(
          "field transforms require16 row-major COLUMN affine entries; "
          "translation at3,7,11 and final row0,0,0,1");
    return result;
  };
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i],
                      key = arg == "--input-grid" ? "--grid" : arg;
    if (!seen.insert(key).second)
      throw std::invalid_argument("duplicate field resampling option: " + arg);
    if (arg == "--field-target-transform")
      options.targetIndexToWorld = matrix(i, arg);
    else if (arg == "--field-world-transform")
      options.worldFromWorld = matrix(i, arg);
    else if (arg == "--field-interpolation") {
      const auto mode = argument(i, arg);
      if (mode == "nearest")
        options.interpolation = vdb::FieldResamplingInterpolation::Nearest;
      else if (mode == "linear")
        options.interpolation = vdb::FieldResamplingInterpolation::Linear;
      else if (mode == "quadratic")
        options.interpolation = vdb::FieldResamplingInterpolation::Quadratic;
      else
        throw std::invalid_argument(
            "--field-interpolation requires nearest|linear|quadratic");
    } else if (arg == "--field-activity") {
      const auto policy = argument(i, arg);
      if (policy == "nearest")
        options.activity = vdb::FieldResamplingActivity::NearestSource;
      else if (policy == "any")
        options.activity = vdb::FieldResamplingActivity::AnyContributingStencil;
      else
        throw std::invalid_argument("--field-activity requires nearest|any");
    } else if (arg == "--field-vector-values") {
      const auto policy = argument(i, arg);
      if (policy == "semantic")
        options.vectorValues = vdb::FieldVectorTransform::Semantic;
      else if (policy == "raw")
        options.vectorValues =
            vdb::FieldVectorTransform::PreserveWorldComponents;
      else
        throw std::invalid_argument(
            "--field-vector-values requires semantic|raw");
    } else if (arg == "--max-field-resampling-work") {
      options.maxWorkItems = count(argument(i, arg), arg);
      if (options.maxWorkItems > 10'000'000'000ULL)
        throw std::invalid_argument("field resampling work cap exceeds1e10");
    } else if (arg == "--max-field-resampling-grids") {
      const auto n = count(argument(i, arg), arg);
      if (n > 4096)
        throw std::invalid_argument(
            "field resampling grid cap requires1..4096");
      options.maxGrids = size_t(n);
    } else if (arg == "--max-field-resampling-metadata")
      options.maxMetadataBytes = count(argument(i, arg), arg);
    else if (key == "--grid")
      selector = argument(i, arg);
    else if (arg == "--grid-name")
      processing.gridName = argument(i, arg);
    else if (arg == "--max-voxels")
      processing.maxVoxelCount = count(argument(i, arg), arg);
    else if (arg == "--max-input-bytes")
      processing.maxInputBytes = count(argument(i, arg), arg);
    else
      throw std::invalid_argument(
          "unknown/incompatible field resampling option: " + arg);
  }
  // Disabled mode must reach availability before filesystem conversion or
  // caller/report validation. The executable does not inherit the library's
  // private CONTAINER_HAS_OPENVDB definition.
  if (!vdb::IsAvailable())
    throw std::runtime_error("OpenVDB field resampling is unavailable; "
                             "configure with -DENABLE_OPENVDB=ON.");
  if (processing.gridName.empty() || processing.gridName.size() > 256)
    throw std::invalid_argument("field output name requires1..256 UTF-8 bytes");
  const auto inputBytes = std::string_view(argv[2]).size(),
             outputBytes = std::string_view(argv[3]).size();
  const long double callerBytes = inputBytes * 1.0L + outputBytes +
                                  selector.size() + processing.gridName.size();
  const long double reportReserve = 65536 + 16 * callerBytes;
  if (callerBytes > 256 * 1024 || reportReserve >= processing.maxInputBytes)
    throw std::invalid_argument(
        "field resampling caller/report text exceeds shared byte cap");
  // Fixed-size native names, diagnostics and bounds only enter the result.
  // Validate caller UTF-8 and reserve its bounded report before publication.
  nlohmann::json report{{"schemaVersion", 1},
                        {"operation", "resample-field"},
                        {"input", argv[2]},
                        {"output", argv[3]},
                        {"inputGrid", selector},
                        {"gridName", processing.gridName},
                        {"gridClass", ""},
                        {"valueType", ""},
                        {"activeVoxelCount", 0},
                        {"voxelSize", 0.},
                        {"inputActiveVoxelCount", 0},
                        {"inputStoredLeafCount", 0},
                        {"outputStoredLeafCount", 0},
                        {"sampledVoxelCount", 0},
                        {"inactiveSampleCount", 0},
                        {"workUpperBound", 0},
                        {"roundedFloatComponents", 0},
                        {"maximumFloatAbsoluteError", 0.},
                        {"exactCopy", false},
                        {"sourceSupportBounds", nullptr},
                        {"targetBounds", nullptr},
                        {"diagnostics", nlohmann::json::array()}};
  (void)report.dump();
  auto nativeProcessing = processing;
  nativeProcessing.maxInputBytes -= uintmax_t(reportReserve);
  const auto result = vdb::WriteResampledField(
      path(argv[2]), path(argv[3]), options, selector, nativeProcessing);
  report["gridName"] = result.grid.gridName;
  report["gridClass"] = result.grid.gridClass;
  report["valueType"] = result.valueType;
  report["activeVoxelCount"] = result.grid.activeVoxelCount;
  report["voxelSize"] = result.grid.voxelSize;
  report["inputActiveVoxelCount"] = result.inputActiveVoxelCount;
  report["inputStoredLeafCount"] = result.inputStoredLeafCount;
  report["outputStoredLeafCount"] = result.outputStoredLeafCount;
  report["sampledVoxelCount"] = result.sampledVoxelCount;
  report["inactiveSampleCount"] = result.inactiveSampleCount;
  report["workUpperBound"] = result.workUpperBound;
  report["roundedFloatComponents"] = result.roundedFloatComponents;
  report["maximumFloatAbsoluteError"] = result.maximumFloatAbsoluteError;
  report["exactCopy"] = result.exactCopy;
  report["sourceSupportBounds"] =
      result.sourceSupportBounds ? nlohmann::json(*result.sourceSupportBounds)
                                 : nlohmann::json(nullptr);
  report["targetBounds"] = result.targetBounds
                               ? nlohmann::json(*result.targetBounds)
                               : nlohmann::json(nullptr);
  report["diagnostics"] = result.diagnostics;
  std::cout << report.dump(2) << '\n';
  return EXIT_SUCCESS;
}
