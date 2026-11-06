#include "Container/geometry/OpenVdbFluidSequence.h"
#include "Container/geometry/OpenVdbLoader.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace container::geometry::vdb {
namespace {
struct StagingDirectory {
  std::filesystem::path path;
  std::vector<std::filesystem::path> owned;
  explicit StagingDirectory(const std::filesystem::path &parent) {
    static std::atomic_uint64_t counter{0};
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      auto candidate =
          parent /
          (".container-vdb-sequence-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++counter));
      if (std::filesystem::create_directory(candidate)) {
        path = std::move(candidate);
        return;
      }
    }
    throw std::runtime_error(
        "could not create exclusive OpenVDB sequence staging directory");
  }
  ~StagingDirectory() {
    if (path.empty())
      return;
    std::error_code ignored;
    for (const auto &file : owned)
      std::filesystem::remove(file, ignored);
    std::filesystem::remove(path, ignored);
  }
};
void cancel(const FluidSequenceOptions &options) {
  if (options.stopRequested && options.stopRequested())
    throw std::runtime_error("OpenVDB sequence cancelled before publication");
}
std::string name(uint32_t frame) {
  std::ostringstream text;
  text << "frame_" << std::setw(4) << std::setfill('0') << frame << ".vdb";
  return text.str();
}
} // namespace

FluidSequenceResult
WriteFluidSequence(const FluidSimulationInputs &input,
                   const std::filesystem::path &outputDirectory,
                   const FluidSequenceOptions &sequence,
                   const FluidSimulationOptions &simulation,
                   const ProcessingOptions &processing) {
  if (!IsAvailable())
    throw std::runtime_error("OpenVDB fluid sequences are unavailable; "
                             "configure with -DENABLE_OPENVDB=ON.");
  if (sequence.frameCount < 1 || sequence.frameCount > 256 ||
      !sequence.maxOutputBytes)
    throw std::invalid_argument("fluid sequence requires frameCount in [1,256] "
                                "and a positive output byte cap");
  if (!std::isfinite(simulation.timeStep) || simulation.timeStep < 1e-6 ||
      simulation.timeStep > 3600)
    throw std::invalid_argument(
        "fluid sequence frame time must be in [1e-6,3600] seconds");
  if (outputDirectory.empty() || std::filesystem::exists(outputDirectory))
    throw std::invalid_argument(
        "fluid sequence output directory must be new and nonempty");
  const auto destination =
      std::filesystem::absolute(outputDirectory).lexically_normal();
  const auto parent = destination.parent_path();
  if (!std::filesystem::is_directory(parent))
    throw std::invalid_argument(
        "fluid sequence parent directory does not exist");
  long double nameBytes = 0;
  for (const auto *value :
       {&simulation.densityGridName, &simulation.temperatureGridName,
        &simulation.velocityGridName, &simulation.pressureGridName,
        &simulation.domainGridName}) {
    if (value->empty() || value->size() > 256)
      throw std::invalid_argument(
          "fluid sequence output names must contain 1..256 bytes");
    nameBytes += value->size();
  }
  // Result paths, JSON object/string/node capacity, serialized manifest and
  // publication bookkeeping coexist with later frame solves. Reserve them
  // first. Borrowed caller callbacks and their external effects are excluded.
  const long double coordinator =
      1024.0L * 1024 +
      sequence.frameCount * (16.0L * 1024 +
                             8.0L * destination.native().size() *
                                 sizeof(std::filesystem::path::value_type) +
                             16 * nameBytes);
  if (!std::isfinite(coordinator) || coordinator >= processing.maxInputBytes)
    throw std::invalid_argument(
        "fluid sequence bookkeeping exceeds the shared input/workspace budget");
  auto frameProcessing = processing;
  frameProcessing.maxInputBytes -=
      static_cast<uintmax_t>(std::ceil(coordinator));
  cancel(sequence);
  StagingDirectory staging(parent);
  FluidSequenceResult result;
  result.manifest = destination / "sequence.json";
  result.frames.reserve(sequence.frameCount);
  FluidSimulationInputs current = input;
  nlohmann::json frames = nlohmann::json::array();
  for (uint32_t frame = 1; frame <= sequence.frameCount; ++frame) {
    cancel(sequence);
    const auto filename = name(frame);
    const auto temporary = staging.path / filename;
    staging.owned.push_back(temporary);
    const auto state =
        WriteFluidStep(current, temporary, simulation, frameProcessing);
    const auto bytes = std::filesystem::file_size(temporary);
    if (bytes > sequence.maxOutputBytes - result.encodedBytes)
      throw std::invalid_argument(
          "fluid sequence exceeds the aggregate encoded output byte cap");
    result.encodedBytes += bytes;
    const double time = frame * simulation.timeStep;
    result.frames.push_back({destination / filename, time, state});
    frames.push_back(
        {{"file", filename},
         {"timeSeconds", time},
         {"bytes", bytes},
         {"actualSubsteps", state.actualSubsteps},
         {"simulatedSeconds", state.simulatedTime},
         {"densityMassBefore", state.densityMassBefore},
         {"densityMassAfter", state.densityMassAfter},
         {"temperatureIntegralBefore", state.temperatureIntegralBefore},
         {"temperatureIntegralAfter", state.temperatureIntegralAfter},
         {"maxWorldSpeed", state.maxWorldSpeedAfter},
         {"maxDivergence", state.maxDivergenceAfter},
         {"pressureResidual", state.residual},
         {"divergenceRoundoffBound", state.divergenceRoundoffBound},
         {"roundoffLimited", state.roundoffLimited},
         {"viscositySubsteps", state.viscositySubsteps},
         {"sourceSubsteps", state.sourceSubsteps},
         {"maxDiffusionIterations", state.maxDiffusionIterations},
         {"maxDiffusionResidual", state.maxDiffusionResidual},
         {"viscosityEnergyDissipated", state.viscosityEnergyDissipated},
         {"sourceAffectedCellApplications",
          state.sourceAffectedCellApplications},
         {"sourceChangedCellApplications", state.sourceChangedCellApplications},
         {"sourceDensityMassChange", state.sourceDensityMassChange},
         {"sourceTemperatureIntegralChange",
          state.sourceTemperatureIntegralChange}});
    current = {.density = temporary,
               .velocity = temporary,
               .domain = temporary,
               .temperature = temporary,
               .densityGrid = "descriptor:" + simulation.densityGridName,
               .velocityGrid = "descriptor:" + simulation.velocityGridName,
               .domainGrid = "descriptor:" + simulation.domainGridName,
               .temperatureGrid =
                   "descriptor:" + simulation.temperatureGridName};
    if (sequence.progress)
      sequence.progress(frame, sequence.frameCount);
  }
  const nlohmann::json manifest{
      {"schemaVersion", 1},
      {"timeUnits", "seconds"},
      {"velocityUnits", "local voxels/second; negative-face MAC"},
      {"frameTimeSeconds", simulation.timeStep},
      {"initialFrameIncluded", false},
      {"grids",
       {{"density", simulation.densityGridName},
        {"temperature", simulation.temperatureGridName},
        {"velocity", simulation.velocityGridName},
        {"pressure", simulation.pressureGridName},
        {"domain", simulation.domainGridName}}},
      {"temperatureIntegralUnits", "Kelvin * world volume; not thermal energy"},
      {"operatorSplitting", "transport; forces; optional viscosity; pressure "
                            "projection; scalar sources last"},
      {"sourcesAffectBuoyancy", "next actual substep"},
      {"kinematicViscosity", simulation.kinematicViscosity},
      {"sourceSphereCount", simulation.sources.size()},
      {"frames", std::move(frames)}};
  const auto text = manifest.dump(2) + "\n";
  if (text.size() > sequence.maxOutputBytes - result.encodedBytes)
    throw std::invalid_argument("fluid sequence manifest exceeds the aggregate "
                                "encoded output byte cap");
  const auto manifestPath = staging.path / "sequence.json";
  staging.owned.push_back(manifestPath);
  std::ofstream stream(manifestPath,
                       std::ios::binary | std::ios::out | std::ios::noreplace);
  if (!stream)
    throw std::runtime_error(
        "could not exclusively create fluid sequence manifest");
  stream.exceptions(std::ios::badbit | std::ios::failbit);
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.close();
  result.encodedBytes += text.size();
  cancel(sequence);
  std::vector<std::filesystem::path> published;
  published.reserve(staging.owned.size());
  if (!std::filesystem::create_directory(destination))
    throw std::invalid_argument("fluid sequence output appeared during "
                                "processing; existing destination preserved");
  try {
    for (const auto &source : staging.owned) {
      const auto target = destination / source.filename();
      std::ifstream inputFile(source, std::ios::binary);
      if (!inputFile)
        throw std::runtime_error("could not read staged fluid sequence file");
      inputFile.exceptions(std::ios::badbit);
      published.push_back(target);
      std::ofstream outputFile(target, std::ios::binary | std::ios::out |
                                           std::ios::noreplace);
      if (!outputFile.is_open()) {
        published.pop_back(); // This path was never ours; preserve it.
        throw std::runtime_error(
            "could not exclusively create published fluid sequence file");
      }
      outputFile.exceptions(std::ios::badbit | std::ios::failbit);
      std::array<char, 64 * 1024> buffer;
      while (inputFile.read(buffer.data(),
                            static_cast<std::streamsize>(buffer.size())) ||
             inputFile.gcount())
        outputFile.write(buffer.data(), inputFile.gcount());
      if (!inputFile.eof())
        throw std::runtime_error(
            "could not read complete staged fluid sequence file");
      outputFile.flush();
      outputFile.close();
      inputFile.close();
    }
  } catch (...) {
    std::error_code ignored;
    for (const auto &file : published)
      std::filesystem::remove(file, ignored);
    std::filesystem::remove(destination, ignored);
    throw;
  }
  return result;
}
} // namespace container::geometry::vdb
