// Include after the tool's value/number/count/path helpers and public
// OpenVdbPointMotion.h. Dispatch runPointMotionCommand before the legacy
// parser. Help: --move-native-points INPUT.vdb OUTPUT.vdb
//   --point-world-transform M00 ... M33   Row-major COLUMN world deformation
//   --point-target-transform M00 ... M33  Optional uniform orthogonal target
//   map
//   --point-position-error X             Target-voxel error (0,.25],
//   default1e-4
//   --allow-point-activity-merge         Opt into counted OR of merged activity
//   --any-group/--all-group/--exclude-group NAME (repeatable distinct names)
//   --max-point-motion-work/grids/metadata N
//   --max-points/attributes/groups/string-bytes, --max-voxels/input-bytes N
//   --grid/--input-grid NAME, --grid-name NAME
// Both matrices have translation at3,7,11 and affine final row0,0,0,1.
std::optional<int> runPointMotionCommand(int argc, char **argv) {
  if (argc < 2 || std::string_view(argv[1]) != "--move-native-points")
    return {};
  if (argc < 4)
    throw std::invalid_argument(
        "--move-native-points requires INPUT OUTPUT before options");
  for (int i = 2; i < 4; ++i)
    if (!argv[i][0] || std::string_view(argv[i]).starts_with("--") ||
        std::string_view(argv[i]) == "-h")
      throw std::invalid_argument(
          "--move-native-points requires nonempty INPUT OUTPUT before options; "
          "use ./-h for a literal filename");
  vdb::PointMotionOptions motion;
  vdb::ProcessingOptions processing;
  processing.gridName = "points";
  vdb::PointAttributeOptions attributes;
  std::string selector;
  std::set<std::string> seen;
  std::array<std::set<std::string>, 3> groupNames;
  const auto argument = [&](int &i, const std::string &option) {
    const auto text = value(argc, argv, i, option);
    if (text.empty() || std::string_view(text).starts_with("--") ||
        text == "-h")
      throw std::invalid_argument("missing nonempty value for " + option);
    return text;
  };
  const auto positive = [&](int &i, const std::string &option,
                            uint64_t maximum) {
    const auto n = count(argument(i, option), option);
    if (n > maximum)
      throw std::invalid_argument("point motion count exceeds range: " +
                                  option);
    return n;
  };
  const auto matrix = [&](int &i, const std::string &option) {
    std::array<double, 16> result;
    for (auto &x : result)
      x = number(argument(i, option), option);
    if (result[12] != 0 || result[13] != 0 || result[14] != 0 ||
        result[15] != 1)
      throw std::invalid_argument(
          "point transforms require16 row-major COLUMN affine entries; "
          "translation at3,7,11 and final row0,0,0,1");
    return result;
  };
  for (int i = 4; i < argc; ++i) {
    const std::string arg = argv[i],
                      key = arg == "--input-grid" ? "--grid" : arg;
    if (arg == "--any-group" || arg == "--all-group" ||
        arg == "--exclude-group") {
      const auto name = argument(i, arg);
      const size_t group = arg == "--any-group"   ? 0
                           : arg == "--all-group" ? 1
                                                  : 2;
      if (groupNames[group].size() >= 4096 ||
          !groupNames[group].insert(name).second)
        throw std::invalid_argument(
            "duplicate/excessive point-motion group predicate: " + arg);
      (group == 0   ? motion.groups.anyOf
       : group == 1 ? motion.groups.allOf
                    : motion.groups.noneOf)
          .push_back(name);
      continue;
    }
    if (!seen.insert(key).second)
      throw std::invalid_argument("duplicate point-motion option: " + arg);
    if (arg == "--point-world-transform")
      motion.worldFromWorld = matrix(i, arg);
    else if (arg == "--point-target-transform")
      motion.targetIndexToWorld = matrix(i, arg);
    else if (arg == "--point-position-error") {
      motion.maxPositionErrorVoxels = number(argument(i, arg), arg);
      if (!(motion.maxPositionErrorVoxels > 0) ||
          motion.maxPositionErrorVoxels > .25)
        throw std::invalid_argument(
            "--point-position-error requires target-voxel error in (0,.25]");
    } else if (arg == "--allow-point-activity-merge")
      motion.rejectActivityMerges = false;
    else if (arg == "--max-point-motion-work")
      motion.maxWork = positive(i, arg, 10'000'000'000ULL);
    else if (arg == "--max-point-motion-grids")
      motion.maxGrids = size_t(positive(i, arg, 4096));
    else if (arg == "--max-point-motion-metadata")
      motion.maxMetadataBytes = count(argument(i, arg), arg);
    else if (arg == "--max-points")
      attributes.maxPoints = size_t(positive(i, arg, uint64_t(INT64_MAX)));
    else if (arg == "--max-attributes")
      attributes.maxAttributes = size_t(positive(i, arg, 1024));
    else if (arg == "--max-groups")
      attributes.maxGroups = size_t(positive(i, arg, 4096));
    else if (arg == "--max-string-bytes")
      attributes.maxStringBytes = count(argument(i, arg), arg);
    else if (key == "--grid")
      selector = argument(i, arg);
    else if (arg == "--grid-name")
      processing.gridName = argument(i, arg);
    else if (arg == "--max-voxels")
      processing.maxVoxelCount = count(argument(i, arg), arg);
    else if (arg == "--max-input-bytes")
      processing.maxInputBytes = count(argument(i, arg), arg);
    else
      throw std::invalid_argument("unknown/incompatible point-motion option: " +
                                  arg);
  }
  // No arbitrary native attribute/group/codec names enter this summary. Full
  // native schema and columns have their own bounded, byte-aware export APIs.
  // Validate caller text and allocate the small report before publication.
  nlohmann::json report{
      {"schemaVersion", 1},
      {"operation", "move-native-points"},
      {"output", argv[3]},
      {"gridName", processing.gridName},
      {"inputCount", 0},
      {"selectedCount", 0},
      {"outputCount", 0},
      {"inputActiveCount", 0},
      {"outputActiveCount", 0},
      {"inactiveRecordsActivated", 0},
      {"outputVoxelCount", 0},
      {"workUpperBound", 0},
      {"maxPositionErrorVoxels", 0.},
      {"maxPositionErrorWorld", 0.},
      {"maxUnselectedPositionErrorVoxels", 0.},
      {"noOp", false},
      {"archivedSchema", false},
      {"schema",
       {{"pointCount", 0},
        {"leafCount", 0},
        {"attributeCount", 0},
        {"groupCount", 0}}},
      {"payloadSemantics",
       "Only P is deformed; v, pscale and all non-P storage are preserved"},
      {"recordOrder",
       "Native order may change; no public original-reference column is added"},
      {"diagnostics", nlohmann::json::array()}};
  (void)report.dump(); // Strict UTF-8 validation before creating a VDB output.
  const auto r = vdb::WriteMovedPoints(path(argv[2]), path(argv[3]), motion,
                                       selector, processing, attributes);
  report["inputCount"] = r.inputCount;
  report["selectedCount"] = r.selectedCount;
  report["outputCount"] = r.outputCount;
  report["inputActiveCount"] = r.inputActiveCount;
  report["outputActiveCount"] = r.outputActiveCount;
  report["inactiveRecordsActivated"] = r.inactiveRecordsActivated;
  report["outputVoxelCount"] = r.outputVoxelCount;
  report["workUpperBound"] = r.workUpperBound;
  report["maxPositionErrorVoxels"] = r.maxPositionErrorVoxels;
  report["maxPositionErrorWorld"] = r.maxPositionErrorWorld;
  report["maxUnselectedPositionErrorVoxels"] =
      r.maxUnselectedPositionErrorVoxels;
  report["noOp"] = r.noOp;
  report["archivedSchema"] = r.archivedSchema;
  auto &schema = report["schema"];
  schema["pointCount"] = r.schema.pointCount;
  schema["leafCount"] = r.schema.leafCount;
  schema["attributeCount"] = r.schema.attributes.size();
  schema["groupCount"] = r.schema.groups.size();
  report["diagnostics"] = r.diagnostics;
  std::cout << report.dump(2) << '\n';
  return EXIT_SUCCESS;
}
