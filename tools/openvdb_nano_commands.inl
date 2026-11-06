// Included in openvdb_process.cpp's private namespace. Keep native-file option
// validation independent of the ordinary VDB processing command matrix.
nlohmann::json nanoReport(const vdb::NanoVdbFileInfo &r) {
  nlohmann::json grids = nlohmann::json::array();
  for (const auto &g : r.grids)
    grids.push_back({{"index", g.index}, {"name", g.name},
      {"valueType", g.valueType}, {"gridClass", g.gridClass}, {"version", g.version},
      {"nativeBytes", g.nativeBytes}, {"encodedBytes", g.encodedBytes},
      {"activeVoxels", g.activeVoxels}, {"storedLeafVoxels", g.storedLeafVoxels},
      {"nodeCounts", g.nodeCounts}, {"containerMetadataBytes", g.containerMetadataBytes},
      {"codec", g.codec == vdb::NanoVdbCodec::None ? "none" : g.codec == vdb::NanoVdbCodec::Zip ? "zip" : "blosc"},
      {"checksum", !g.checksumModeKnown ? nlohmann::json(nullptr) :
          nlohmann::json(g.checksum == vdb::NanoVdbChecksum::Disabled ? "disabled" : g.checksum == vdb::NanoVdbChecksum::Header ? "header" : "full")},
      {"supported", g.supported}, {"structureValidated", g.structureValidated},
      {"checksumVerified", g.checksumVerified},
      {"checksumCoversAllData", g.structureValidated ? nlohmann::json(g.checksumCoversAllData) : nlohmann::json(nullptr)},
      {"containerMetadata", g.containerMetadata}});
  return {{"schemaVersion", 1}, {"encodedBytes", r.encodedBytes},
    {"nativeBytes", r.nativeBytes}, {"raw", r.raw}, {"work", r.work},
    {"grids", std::move(grids)}, {"diagnostics", r.diagnostics}};
}

std::optional<int> runNanoCommand(int argc, char **argv) {
  if (argc < 2)
    return {};
  const std::string command = argv[1];
  const bool capabilities = command == "--nano-capabilities";
  const bool inspect = command == "--inspect-nano";
  const bool validate = command == "--validate-nano";
  const bool copy = command == "--copy-nano";
  const bool rewrite = command == "--checksum-nano";
  const bool forward = command == "--to-nano";
  const bool reverse = command == "--from-nano";
  const bool statistics = command == "--stats-nano";
  if (!(capabilities || inspect || validate || copy || rewrite || forward || reverse || statistics))
    return {};
  if (capabilities) {
    if (argc != 2)
      throw std::invalid_argument("--nano-capabilities accepts no paths or options");
    const auto c = vdb::NanoVdbInterchangeCapabilities();
    std::cout << nlohmann::json{{"schemaVersion", 1}, {"nativeIo", c.nativeIo},
      {"openVdbConversion", c.openVdbConversion}, {"zip", c.zip}, {"blosc", c.blosc},
      {"supportedAbi", {"32.6", "32.7"}},
      {"types", {"float", "double", "int32", "int64", "bool", "mask", "vec3f", "vec3d"}}}.dump(2) << '\n';
    return EXIT_SUCCESS;
  }
  const int positional = inspect || validate ? 1 : 2;
  if (argc < positional + 2)
    throw std::invalid_argument("NanoVDB command is missing its input/output path");
  for (int i = 2; i < 2 + positional; ++i)
    if (std::string_view(argv[i]).starts_with("--"))
      throw std::invalid_argument("NanoVDB paths must precede options");
  vdb::NanoVdbLimits limits;
  vdb::NanoVdbReadOptions read;
  vdb::NanoVdbWriteOptions write;
  auto statisticsMode=vdb::NanoVdbStatisticsMode::All;
  std::string selection;
  bool allowLoss = false;
  std::set<std::string> seen;
  const auto uint32 = [&](const std::string &text, const std::string &arg) {
    const auto n = count(text, arg);
    if (n > UINT32_MAX)
      throw std::invalid_argument(arg + " exceeds uint32");
    return uint32_t(n);
  };
  for (int i = 2 + positional; i < argc; ++i) {
    const std::string arg = argv[i];
    if (!seen.insert(arg).second)
      throw std::invalid_argument("duplicate NanoVDB option: " + arg);
    if (arg == "--grid" && (forward || reverse || copy || validate || statistics))
      selection = value(argc, argv, i, arg);
    else if (arg == "--codec" && (forward || copy || statistics)) {
      const auto text = value(argc, argv, i, arg);
      if (text == "none") write.codec = vdb::NanoVdbCodec::None;
      else if (text == "zip") write.codec = vdb::NanoVdbCodec::Zip;
      else if (text == "blosc") write.codec = vdb::NanoVdbCodec::Blosc;
      else throw std::invalid_argument("--codec requires none|zip|blosc");
    } else if (arg == "--checksum" && (forward || copy || rewrite || statistics)) {
      const auto text = value(argc, argv, i, arg);
      if (text == "disabled") write.checksum = vdb::NanoVdbChecksum::Disabled;
      else if (text == "header") write.checksum = vdb::NanoVdbChecksum::Header;
      else if (text == "full") write.checksum = vdb::NanoVdbChecksum::Full;
      else throw std::invalid_argument("--checksum requires disabled|header|full");
    } else if (arg == "--native-statistics" && statistics) {
      const auto text=value(argc,argv,i,arg);
      if (text=="bounds") statisticsMode=vdb::NanoVdbStatisticsMode::Bounds;
      else if (text=="minmax") statisticsMode=vdb::NanoVdbStatisticsMode::MinMax;
      else if (text=="all") statisticsMode=vdb::NanoVdbStatisticsMode::All;
      else throw std::invalid_argument("--native-statistics requires bounds|minmax|all");
    } else if (arg == "--raw" && (forward || copy || statistics)) write.raw = true;
    else if (arg == "--allow-float-map-rounding" && forward) write.allowFloatMapRounding = true;
    else if (arg == "--allow-metadata-loss" && reverse) allowLoss = true;
    else if (arg == "--ignore-checksum" && (validate || copy || reverse || statistics)) read.verifyChecksums = false;
    else if (arg == "--require-full-checksum" && (validate || copy || reverse || statistics)) read.requireFullChecksum = true;
    else if (arg == "--max-input-bytes") limits.maxInputBytes = count(value(argc, argv, i, arg), arg);
    else if (arg == "--max-native-bytes") limits.maxNativeBytes = count(value(argc, argv, i, arg), arg);
    else if (arg == "--max-workspace-bytes") limits.maxWorkspaceBytes = count(value(argc, argv, i, arg), arg);
    else if (arg == "--max-active-voxels") limits.maxActiveVoxels = count(value(argc, argv, i, arg), arg);
    else if (arg == "--max-stored-voxels") limits.maxStoredLeafVoxels = count(value(argc, argv, i, arg), arg);
    else if (arg == "--max-nodes") limits.maxNodes = count(value(argc, argv, i, arg), arg);
    else if (arg == "--max-nano-work") limits.maxWork = count(value(argc, argv, i, arg), arg);
    else if (arg == "--max-grids") limits.maxGrids = uint32(value(argc, argv, i, arg), arg);
    else if (arg == "--max-name-bytes") limits.maxNameBytes = uint32(value(argc, argv, i, arg), arg);
    else if (arg == "--max-metadata-bytes") limits.maxMetadataBytes = uint32(value(argc, argv, i, arg), arg);
    else throw std::invalid_argument("unrecognized or incompatible NanoVDB option: " + arg);
  }
  read.inputGrid = selection;
  const auto input = path(argv[2]);
  const auto output = positional == 2 ? path(argv[3]) : std::filesystem::path{};
  if (statistics) {
    const auto result=vdb::WriteNanoVdbStatistics(input,output,statisticsMode,read,write,limits);
    auto report=nanoReport(result.file);
    report["work"]=result.work; report["statisticsDiagnostics"]=result.diagnostics;
    auto summaries=nlohmann::json::array();
    const auto numeric=[](const auto& value)->nlohmann::json {
      if (!value) return nullptr;
      return std::visit([](const auto& v) { return nlohmann::json(v); },*value);
    };
    for (const auto& s:result.summaries)
      summaries.push_back({{"sourceIndex",s.sourceIndex},{"name",s.name},{"valueType",s.valueType},
          {"activeVoxels",s.activeVoxels},
          {"indexMinimum",s.indexMinimum ? nlohmann::json(*s.indexMinimum) : nlohmann::json(nullptr)},
          {"indexMaximum",s.indexMaximum ? nlohmann::json(*s.indexMaximum) : nlohmann::json(nullptr)},
          {"minimum",numeric(s.minimum)},{"maximum",numeric(s.maximum)},
          {"average",s.average ? nlohmann::json(*s.average) : nlohmann::json(nullptr)},
          {"standardDeviation",s.standardDeviation ? nlohmann::json(*s.standardDeviation) : nlohmann::json(nullptr)}});
    report["summaries"]=std::move(summaries);
    std::cout << report.dump(2,' ',false,nlohmann::json::error_handler_t::replace) << '\n';
    return EXIT_SUCCESS;
  }
  vdb::NanoVdbFileInfo report;
  if (inspect) report = vdb::InspectNanoVdbFile(input, limits);
  else if (validate) report = vdb::ValidateNanoVdbFile(input, read, limits);
  else if (rewrite) report = vdb::RewriteNanoVdbChecksums(input, output, write.checksum, limits);
  else if (forward) report = vdb::WriteNanoVdbFromVdb(input, output, selection, write, limits);
  else if (reverse) report = vdb::WriteVdbFromNanoVdb(input, output, read, allowLoss, limits);
  else {
    const auto archive = vdb::ReadNanoVdbFile(input, read, limits);
    const auto previous = archive.info().work;
    if (previous >= limits.maxWork)
      throw std::invalid_argument("NanoVDB copy exhausted its aggregate work budget during reading");
    limits.maxWork -= previous;
    report = vdb::WriteNanoVdbFile(archive, output, write, limits);
    report.work += previous;
  }
  std::cout << nanoReport(report).dump(2, ' ', false,
      nlohmann::json::error_handler_t::replace) << '\n';
  return EXIT_SUCCESS;
}
