#pragma once

#include "Container/geometry/OpenVdbFluidSimulation.h"

#include <functional>
#include <vector>

namespace container::geometry::vdb {

struct FluidSequenceOptions {
  uint32_t frameCount{24}; // [1,256]; each saved frame advances simulation.timeStep.
  uintmax_t maxOutputBytes{2ull * 1024 * 1024 * 1024}; // Aggregate encoded files and manifest.
  // Called synchronously between frames and once before publishing. Return true
  // to cancel. An in-progress library timestep completes before cancellation.
  std::function<bool()> stopRequested;
  // Called after each completed frame, on the calling thread. Exceptions abort.
  std::function<void(uint32_t completed, uint32_t total)> progress;
};

struct FluidSequenceFrame {
  std::filesystem::path path;
  double time{0};
  FluidSimulationResult simulation;
};

struct FluidSequenceResult {
  std::filesystem::path manifest;
  uintmax_t encodedBytes{0};
  std::vector<FluidSequenceFrame> frames;
};

// Advances through WriteFluidStep and restarts each following frame from the
// preceding five-grid state, retaining the configured selectors/output names.
// Frames are frame_0001.vdb .. frame_0256.vdb. sequence.json records units,
// selectors, exact times and numeric diagnostics. No initial frame is copied.
// The output DIRECTORY must not exist. Only a completely successful sequence
// is published into an exclusively created destination directory. Files are
// copied without overwrite, and owned published files are rolled back on error.
// Failures/cancellation remove owned temporary files and preserve all sources
// and existing destinations. Observers may see files during publication; this
// is not an atomic visibility or crash-durability guarantee. Encoded output is
// capped after each frame (staging/publication can temporarily use two copies).
// ProcessingOptions includes conservative retained manifest/path/result storage
// reserved before each frame solve. Caller callback storage/effects are excluded.
// Callbacks cannot interrupt a library solve.
// OFF fails before accessing paths or callbacks.
[[nodiscard]] FluidSequenceResult WriteFluidSequence(
    const FluidSimulationInputs& input, const std::filesystem::path& outputDirectory,
    const FluidSequenceOptions& sequence = {},
    const FluidSimulationOptions& simulation = {},
    const ProcessingOptions& processing = {});

} // namespace container::geometry::vdb
