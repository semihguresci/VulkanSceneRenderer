#include "bim_robotics_twin_ingest.h"

#include "Container/geometry/IfcTessellatedLoader.h"
#include "Container/geometry/IfcxLoader.h"
#include "Container/robotics/RoboticsScene.h"
#include "Container/robotics/TwinIngest.h"
#include "Container/utility/AtomicFile.h"
#include "Container/utility/Platform.h"
#include "TwinRecordingJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace container::tools {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using robotics::TwinIngest;
using robotics::TwinIngestCode;
constexpr size_t maxStreamBytes = 64 * 1024 * 1024;
constexpr size_t maxStreamLines = 65'536;
constexpr size_t maxDiagnosticBytes = 512;

struct IngestOptions {
  uint64_t staleAfterNanoseconds{2'000'000'000};
  std::optional<double> metersPerRendererUnit;
};

IngestOptions parseOptions(std::span<const fs::path> options) {
  if (options.size() % 2 != 0)
    throw std::invalid_argument("Each BIM twin ingest flag requires a value");
  IngestOptions result;
  bool seenStaleness = false;
  for (size_t index = 0; index < options.size(); index += 2) {
    const auto& flag = options[index];
    const auto argument = util::pathToUtf8(options[index + 1]);
    const auto* end = argument.data() + argument.size();
    if (flag == "--stale-after-ns" && !seenStaleness) {
      const auto parsed = std::from_chars(argument.data(), end, result.staleAfterNanoseconds);
      if (parsed.ec != std::errc{} || parsed.ptr != end)
        throw std::invalid_argument("--stale-after-ns must be a nonnegative 64-bit integer");
      seenStaleness = true;
    } else if (flag == "--metres-per-renderer-unit" && !result.metersPerRendererUnit) {
      double scale{};
      const auto parsed = std::from_chars(argument.data(), end, scale);
      if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(scale) || scale <= 0)
        throw std::invalid_argument("--metres-per-renderer-unit must be finite and positive");
      result.metersPerRendererUnit = scale;
    } else {
      throw std::invalid_argument("Unknown or duplicate BIM twin ingest flag " + util::pathToUtf8(flag));
    }
  }
  return result;
}

geometry::dotbim::Model loadModel(const fs::path& path, std::vector<util::ContentFingerprint>& layers) {
  auto extension = util::pathToUtf8(path.extension());
  std::ranges::transform(extension, extension.begin(), [](char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
  });
  if (extension == ".ifc") {
    util::ContentFingerprint fingerprint;
    auto model = geometry::ifc::LoadFromFile(path, 1.f, {}, &fingerprint);
    layers.push_back(std::move(fingerprint));
    return model;
  }
  if (extension == ".ifcx") {
    const auto snapshot = geometry::ifcx::PrepareSource(path);
    layers.reserve(snapshot.layers.size());
    for (const auto& layer : snapshot.layers) layers.push_back(layer.fingerprint);
    return geometry::ifcx::LoadFromSnapshot(snapshot);
  }
  throw std::invalid_argument("BIM twin ingest requires an IFC STEP or IFCX source model");
}

// Drain an oversized row to its terminator. Neither it nor a file without
// line terminators can grow the in-memory row beyond the wire limit.
bool nextLine(std::istream& input, std::string& line, size_t& bytes, bool& oversized) {
  line.clear();
  oversized = false;
  char value{};
  while (input.get(value)) {
    if (++bytes > maxStreamBytes) throw std::runtime_error("BIM twin event stream exceeds 64 MiB");
    if (value == '\n') return true;
    if (line.size() < robotics::twin_json::maxIngestLineBytes)
      line.push_back(value);
    else
      oversized = true;
  }
  if (input.bad() || !input.eof()) throw std::runtime_error("Could not read the BIM twin event stream");
  return oversized || !line.empty();
}

// Diagnostics can include an excerpt from malformed JSON containing input
// bytes. Cap receipts without emitting invalid UTF-8 after truncation.
std::string boundedDiagnostic(std::string_view diagnostic) {
  auto result = std::string(diagnostic.substr(0, maxDiagnosticBytes));
  if (diagnostic.size() > maxDiagnosticBytes) result += "...";
  return result;
}

struct IngestBatch {
  Json receipts = Json::array();
  size_t accepted{};
  size_t acceptedOutOfOrder{};
  size_t duplicates{};
  size_t rejected{};

  void malformed(size_t line, std::string_view detail) {
    ++rejected;
    receipts.push_back({{"line", line}, {"disposition", "malformed-row"}, {"detail", boundedDiagnostic(detail)}});
  }

  void observation(TwinIngest& ingester, size_t line, robotics::TwinIngestMessage message) {
    Json result{{"line", line}, {"clockId", message.clockId},
                {"receivedTimestampNanoseconds", message.receivedTimestampNanoseconds},
                {"timestampNanoseconds", message.event.timestampNanoseconds},
                {"inputEntityId", message.event.entityId}, {"source", message.event.source}};
    const auto receipt = ingester.ingest(std::move(message));
    result["disposition"] = robotics::twinIngestCodeName(receipt.code);
    if (!receipt.diagnostic.empty()) result["detail"] = boundedDiagnostic(receipt.diagnostic);
    if (receipt.entityId) result["resolvedEntityId"] = *receipt.entityId;
    if (receipt.ifcGuid) result["originalGuid"] = *receipt.ifcGuid;
    switch (receipt.code) {
    case TwinIngestCode::Accepted: ++accepted; break;
    case TwinIngestCode::AcceptedOutOfOrder: ++accepted; ++acceptedOutOfOrder; break;
    case TwinIngestCode::Duplicate: ++duplicates; break;
    default: ++rejected; break;
    }
    receipts.push_back(std::move(result));
  }
};

void ingestRows(TwinIngest& ingester, std::istream& input, IngestBatch& batch) {
  std::string line;
  size_t bytes = 0;
  bool oversized = false;
  while (nextLine(input, line, bytes, oversized)) {
    const auto lineNumber = batch.receipts.size() + 1;
    if (lineNumber > maxStreamLines) throw std::runtime_error("BIM twin event stream exceeds 65,536 rows");
    if (oversized) {
      batch.malformed(lineNumber, "BIM twin event row exceeds 512 KiB");
      continue;
    }
    std::optional<robotics::TwinIngestMessage> message;
    try {
      message = robotics::twin_json::parseIngestMessage(line);
    } catch (const std::invalid_argument& error) {
      batch.malformed(lineNumber, error.what());
    }
    if (message) batch.observation(ingester, lineNumber, std::move(*message));
  }
}

} // namespace

int runTwinIngest(std::span<const fs::path> arguments) {
  const IngestOptions options = parseOptions(arguments.subspan(5));
  const auto& output = arguments[2];
  if (!output.has_filename() || fs::exists(fs::symlink_status(output)))
    throw std::invalid_argument("Output must be a new BIM twin recording file: " + util::pathToUtf8(output));
  const auto sourceNamespace = util::pathToUtf8(arguments[3]);
  const auto clockId = util::pathToUtf8(arguments[4]);
  std::ifstream input(arguments[1], std::ios::binary);
  if (!input) throw std::runtime_error("Could not open BIM twin event stream " + util::pathToUtf8(arguments[1]));
  std::vector<util::ContentFingerprint> layers;
  const auto model = loadModel(arguments[0], layers);
  const auto scene = robotics::buildRoboticsScene(model, {sourceNamespace, options.metersPerRendererUnit});
  TwinIngest ingester(scene, std::move(layers), clockId, options.staleAfterNanoseconds);
  IngestBatch batch;
  bool published = false;
  std::string error;
  try {
    ingestRows(ingester, input, batch);
    if (batch.accepted == 0) {
      error = "No BIM twin observations were accepted";
    } else {
      const auto bytes = robotics::serializeTwinRecording(ingester.recording());
      if (!output.parent_path().empty()) fs::create_directories(output.parent_path());
      util::writeFileAtomically(output, bytes, util::AtomicFilePublication::CreateNew);
      published = true;
    }
  } catch (const std::exception& failure) {
    error = failure.what();
  }
  Json result{{"schema", "container.bim-twin-ingest-result"}, {"version", 1},
      {"sourceNamespace", sourceNamespace}, {"clockId", clockId},
      {"staleAfterNanoseconds", options.staleAfterNanoseconds}, {"lines", batch.receipts.size()},
      {"accepted", batch.accepted}, {"acceptedOutOfOrder", batch.acceptedOutOfOrder},
      {"duplicates", batch.duplicates}, {"rejected", batch.rejected},
      {"outputPublished", published}, {"receipts", std::move(batch.receipts)}};
  if (!error.empty()) result["error"] = boundedDiagnostic(error);
  if (!(std::cout << result.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n'))
    throw std::runtime_error("Could not write BIM twin ingest receipts");
  if (!error.empty()) {
    std::cerr << "BimRoboticsTwin: " << error << '\n';
    return 1;
  }
  return 0;
}

} // namespace container::tools
