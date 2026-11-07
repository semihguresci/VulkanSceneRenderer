// Included in openvdb_process.cpp's private namespace after the common numeric,
// UTF-8 path and option helpers. Requires OpenVdbPotentialFlow.h and the
// existing OpenVdbLoader.h runtime availability declaration.
std::optional<int> runPotentialFlowCommand(int argc, char **argv) {
  if (argc < 2)
    return {};
  const std::string_view command = argv[1];
  const bool capabilities = command == "--potential-flow-capabilities";
  if (!capabilities && command != "--potential-flow")
    return {};
  // OpenVDB compile definitions are private to the processing library and do
  // not propagate to this executable. Query the linked runtime contract.
  const bool available = vdb::IsAvailable();
  using nlohmann::json;
  vdb::PotentialFlowOptions o;
  if (capabilities) {
    if (argc != 2)
      throw std::invalid_argument(
          "--potential-flow-capabilities accepts no paths or options");
    std::cout << json{{"schemaVersion", 1},
                      {"openvdbAvailable", available},
                      {"contracts",
                       {"Float/Double positive-exterior LEVEL_SET collider",
                        "Uniform orthogonal affine voxels; "
                        "rotations/reflections supported",
                        "Uniform world-space far-field and collider velocities",
                        "Fixed native 18-neighbor exterior shell, not general "
                        "pressure projection",
                        "Projected-normal Neumann collider cells; "
                        "zero-potential outer Dirichlet faces",
                        "Every component requires a Dirichlet face; "
                        "pure-Neumann cavities reject",
                        "Native boundary substitution drops tangential "
                        "potential derivative",
                        "Stored-potential seven-point residual; interior "
                        "divergence/curl are diagnostics",
                        "New output only; source and exact map retained"}},
                      {"limits",
                       {{"exteriorWidth", {1, 128}},
                        {"maxSolverVoxels", {1, 1'000'000}},
                        {"maxIterations", {1, 10'000}},
                        {"tolerance", {1e-12, 1e-2}},
                        {"maxWorkItems", {1ull, 1'000'000'000'000ull}},
                        {"maxGrids", {1, 4096}},
                        {"velocityComponentMagnitude", 1e12},
                        {"voxelAxisLength", {1e-6, 1e6}},
                        {"sourceBandHalfWidth", {3, 64}},
                        {"outputNameBytes", {1, 256}}}},
                      {"defaults",
                       {{"farFieldVelocity", o.farFieldVelocity},
                        {"colliderVelocity", o.colliderVelocity},
                        {"exteriorWidth", o.exteriorWidth},
                        {"maxSolverVoxels", o.maxSolverVoxels},
                        {"maxIterations", o.maxIterations},
                        {"tolerance", o.tolerance},
                        {"maxWorkItems", o.maxWorkItems},
                        {"maxGrids", o.maxGrids},
                        {"maxMetadataBytes", o.maxMetadataBytes},
                        {"velocityGridName", o.velocityGridName},
                        {"potentialGridName", o.potentialGridName},
                        {"domainGridName", o.domainGridName},
                        {"neumannGridName", o.neumannGridName}}}}
                     .dump(2)
              << '\n';
    return EXIT_SUCCESS;
  }
  const auto missingPath = [](std::string_view text) {
    return text.empty() || text == "-h" || text.starts_with("--");
  };
  if (argc < 4 || missingPath(argv[2]) || missingPath(argv[3]))
    throw std::invalid_argument(
        "--potential-flow requires INPUT OUTPUT before options");
  vdb::ProcessingOptions p;
  std::string grid;
  std::set<std::string> seen;
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i];
    if (!seen.insert(arg).second)
      throw std::invalid_argument("duplicate potential-flow option: " + arg);
    if (arg == "--far-field" || arg == "--collider-velocity") {
      auto &velocity =
          arg == "--far-field" ? o.farFieldVelocity : o.colliderVelocity;
      for (auto &x : velocity) {
        x = number(value(argc, argv, i, arg), arg);
        if (std::abs(x) > 1e12)
          throw std::invalid_argument(
              "potential-flow velocity component magnitude exceeds 1e12");
      }
    } else if (arg == "--exterior-width" || arg == "--max-solver-voxels" ||
               arg == "--max-iterations" || arg == "--max-flow-grids") {
      const auto n = count(value(argc, argv, i, arg), arg);
      const uint64_t maximum = arg == "--exterior-width"      ? 128
                               : arg == "--max-solver-voxels" ? 1'000'000
                               : arg == "--max-iterations"    ? 10'000
                                                              : 4096;
      if (n > maximum)
        throw std::invalid_argument(arg + " exceeds " +
                                    std::to_string(maximum));
      if (arg == "--exterior-width")
        o.exteriorWidth = uint32_t(n);
      else if (arg == "--max-solver-voxels")
        o.maxSolverVoxels = n;
      else if (arg == "--max-iterations")
        o.maxIterations = uint32_t(n);
      else
        o.maxGrids = uint32_t(n);
    } else if (arg == "--tolerance") {
      o.tolerance = number(value(argc, argv, i, arg), arg);
      if (o.tolerance < 1e-12 || o.tolerance > 1e-2)
        throw std::invalid_argument("--tolerance must be in [1e-12,1e-2]");
    } else if (arg == "--max-flow-work") {
      o.maxWorkItems = count(value(argc, argv, i, arg), arg);
      if (o.maxWorkItems > 1'000'000'000'000ull)
        throw std::invalid_argument("--max-flow-work exceeds 1000000000000");
    } else if (arg == "--max-flow-metadata")
      o.maxMetadataBytes = count(value(argc, argv, i, arg), arg);
    else if (arg == "--velocity-name")
      o.velocityGridName = value(argc, argv, i, arg);
    else if (arg == "--potential-name")
      o.potentialGridName = value(argc, argv, i, arg);
    else if (arg == "--domain-name")
      o.domainGridName = value(argc, argv, i, arg);
    else if (arg == "--neumann-name")
      o.neumannGridName = value(argc, argv, i, arg);
    else if (arg == "--grid")
      grid = value(argc, argv, i, arg);
    else if (arg == "--max-voxels")
      p.maxVoxelCount = count(value(argc, argv, i, arg), arg);
    else if (arg == "--max-input-bytes")
      p.maxInputBytes = count(value(argc, argv, i, arg), arg);
    else
      throw std::invalid_argument(
          "unknown/incompatible potential-flow option: " + arg);
  }
  const auto r =
      vdb::WritePotentialFlow(path(argv[2]), path(argv[3]), o, grid, p);
  json grids = json::array();
  for (const auto &g : r.grids)
    grids.push_back({{"name", g.gridName},
                     {"gridClass", g.gridClass},
                     {"activeVoxelCount", g.activeVoxelCount},
                     {"voxelSize", g.voxelSize}});
  const bool interiorAvailable = r.interiorDiagnosticCellCount != 0;
  std::cout
      << json{{"schemaVersion", 1},
              {"openvdbAvailable", available},
              {"converged", r.converged},
              {"iterationLimit", o.maxIterations},
              {"tolerance", o.tolerance},
              {"domainCellCount", r.domainCellCount},
              {"componentCount", r.componentCount},
              {"neumannCellCount", r.neumannCellCount},
              {"interiorDiagnosticCellCount", r.interiorDiagnosticCellCount},
              {"interiorDiagnosticsAvailable", interiorAvailable},
              {"workItems", r.workItems},
              {"iterations", r.iterations},
              {"residual", r.residual},
              {"maxBoundaryNormalError", r.maxBoundaryNormalError},
              {"maxInteriorDivergence", interiorAvailable
                                            ? json(r.maxInteriorDivergence)
                                            : json(nullptr)},
              {"maxInteriorCurl",
               interiorAvailable ? json(r.maxInteriorCurl) : json(nullptr)},
              {"grids", std::move(grids)},
              {"diagnostics", r.diagnostics}}
             .dump(2, ' ', false, json::error_handler_t::replace)
      << '\n';
  return EXIT_SUCCESS;
}
