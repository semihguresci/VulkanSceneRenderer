// Include inside openvdb_process.cpp's private namespace, after its common
// value/count/path helpers. The tool needs OpenVdbSurfaceFields.h and the
// existing OpenVdbLoader.h runtime availability declaration. Dispatch
// runSurfaceFieldsCommand before the legacy parser; no private SDK macros.
std::optional<int> runSurfaceFieldsCommand(int argc, char **argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--surface-fields")
    return {};
  const auto missing = [](std::string_view text) {
    return text.empty() || text == "-h" || text.starts_with("--");
  };
  if (argc < 4 || missing(argv[2]) || missing(argv[3]))
    throw std::invalid_argument(
        "--surface-fields requires nonempty INPUT OUTPUT before options; "
        "use ./-h for a literal filename");

  using Operation = vdb::SurfaceFieldOperation;
  Operation operation = Operation::MeanCurvature;
  vdb::SurfaceFieldsOptions options;
  vdb::ProcessingOptions processing;
  std::string selector, operationName;
  std::set<std::string> seen;
  const auto argument = [&](int &index, const std::string &option) {
    const auto text = value(argc, argv, index, option);
    if (missing(text))
      throw std::invalid_argument("nonempty value required for " + option);
    return text;
  };
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i],
                      key = arg == "--input-grid" ? "--grid" : arg;
    if (!seen.insert(key).second)
      throw std::invalid_argument("duplicate surface-fields option: " + arg);
    if (arg == "--surface-field-operation") {
      operationName = argument(i, arg);
      if (operationName == "mean")
        operation = Operation::MeanCurvature;
      else if (operationName == "gaussian")
        operation = Operation::GaussianCurvature;
      else if (operationName == "normal")
        operation = Operation::Normal;
      else if (operationName == "closest-point")
        operation = Operation::ClosestPoint;
      else
        throw std::invalid_argument(
            "--surface-field-operation requires mean, gaussian, normal or "
            "closest-point");
    } else if (arg == "--surface-field-double")
      options.promoteToDouble = true;
    else if (arg == "--surface-validity-name")
      options.validityGridName = argument(i, arg);
    else if (arg == "--max-surface-field-work") {
      options.maxWorkItems = count(argument(i, arg), arg);
      if (options.maxWorkItems > 10'000'000'000ull)
        throw std::invalid_argument(
            "--max-surface-field-work must be in [1,10000000000]");
    } else if (arg == "--max-surface-field-grids") {
      const auto n = count(argument(i, arg), arg);
      if (n > 4096)
        throw std::invalid_argument(
            "--max-surface-field-grids must be in [1,4096]");
      options.maxGrids = uint32_t(n);
    } else if (arg == "--max-surface-field-metadata")
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
          "unknown/incompatible surface-fields option: " + arg);
  }
  if (operationName.empty())
    throw std::invalid_argument(
        "--surface-fields requires --surface-field-operation "
        "mean|gaussian|normal|closest-point");
  for (const auto *name : {&processing.gridName, &options.validityGridName})
    if (name->empty() || name->size() > 256 ||
        name->find('\0') != std::string::npos)
      throw std::invalid_argument(
          "surface-field output names must be 1..256 bytes");
  if (processing.gridName == options.validityGridName)
    throw std::invalid_argument(
        "surface-field value and validity names must differ");

  const auto result = vdb::WriteSurfaceFields(
      path(argv[2]), path(argv[3]), operation, options, selector, processing);
  using nlohmann::json;
  const auto grid = [](const vdb::ProcessingResult &g,
                       const std::string &type) {
    return json{{"gridName", g.gridName},
                {"gridClass", g.gridClass},
                {"valueType", type},
                {"activeVoxelCount", g.activeVoxelCount},
                {"voxelSize", g.voxelSize}};
  };
  std::cout << json{{"schemaVersion", 1},
                    {"openvdbAvailable", vdb::IsAvailable()},
                    {"operation", operationName},
                    {"promoteToDouble", options.promoteToDouble},
                    {"field", grid(result.field, result.valueType)},
                    {"validity", grid(result.validity, "bool")},
                    {"inputActiveVoxelCount", result.inputActiveVoxelCount},
                    {"validVoxelCount", result.validVoxelCount},
                    {"missingFootprintVoxelCount",
                     result.missingFootprintVoxelCount},
                    {"degenerateGradientVoxelCount",
                     result.degenerateGradientVoxelCount},
                    {"gradientSampleCount", result.gradientSampleCount},
                    {"maximumUnitGradientError",
                     result.gradientSampleCount
                         ? json(result.maximumUnitGradientError)
                         : json(nullptr)},
                    {"workItems", result.workItems},
                    {"diagnostics", result.diagnostics}}
                   .dump(2, ' ', false, json::error_handler_t::replace)
            << '\n';
  return EXIT_SUCCESS;
}
