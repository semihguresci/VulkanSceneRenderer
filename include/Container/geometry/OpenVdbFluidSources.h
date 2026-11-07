#pragma once

#include "Container/geometry/OpenVdbProcessing.h"

#include <array>
#include <vector>

namespace container::geometry::vdb {

struct FluidSourceSphere {
  std::array<double, 3> center{0, 0,
                               0}; // World coordinates, finite |x| <=1e12.
  double radius{1}; // World distance, [1e-6,1e6], inclusive cell-center test.
  double densityRate{0};               // Density units/sec, [0,1e6].
  double temperatureRate{0};           // Kelvin/sec, [0,1e6].
  double densityDecayRate{0};          // Inverse sec, [0,1e6].
  double temperatureRelaxationRate{0}; // Inverse sec, [0,1e6], toward ambient.
  bool clearDensity{false}; // Explicitly clear before applying combined rates.
  bool resetTemperature{false}; // Explicitly reset to ambient before rates.
};

struct FluidSourcesOptions {
  double timeStep{1.0 / 24.0};       // Seconds, [1e-6,3600].
  double ambientTemperature{293.15}; // Kelvin, [0,1e6].
  uint64_t maxFluidCells{100'000};   // [1,1000000], shared budgets also apply.
  std::vector<FluidSourceSphere> sources; // <=64, duplicates contribute twice.
  // Existing literal display names and output names, all distinct, 1..256B.
  std::string densityGridName{"density"}, temperatureGridName{"temperature"};
  std::string velocityGridName{"velocity"}, pressureGridName{"pressure"},
      domainGridName{"fluid"};
};

struct FluidSourcesResult {
  uint64_t fluidCellCount{0}, affectedCellCount{0}, changedCellCount{0};
  uint64_t clearedDensityCells{0}, resetTemperatureCells{0};
  // Integrals of stored fields; diagnostics below Double range round to zero.
  double densityMassBefore{0}, densityMassAfter{0};
  double temperatureIntegralBefore{0},
      temperatureIntegralAfter{0}; // Kelvin*volume.
  bool converged{false};
};

// Edit a canonical M05 five-grid state. Only TRUE fluid cell centers inside an
// inclusive world-space sphere are affected; solid/inactive/outside values are
// untouched. Cell-center and sphere precision is checked relative to
// min(radius, voxel size) before membership. Uniform orthogonal affine
// transforms (including rotation/reflection) are supported; all five grids must
// share the transform.
//
// Sources are canonically sorted; overlapping nonnegative rates are combined
// independently of caller ordering (each combined rate <=1e6). Explicit clear/
// reset flags apply first, then the exact constant-coefficient local ODE:
// dD/dt=Jd-Kd*D, dT/dt=Jt+Kt*(ambient-T). Rates have the units above. No
// velocity injection, domain expansion, projection, hidden mass cap or scalar
// upper clamp is applied. Final D/T outside [0,1e6] fails. Stiff decay is
// evaluated with scaled exponentials: a nonzero final term below Double range
// fails explicitly instead of silently erasing it; clear/reset flags provide
// intentional annihilation.
//
// Density/temperature are Double canonical scalar fields. Native Double MAC,
// Double pressure and Bool domain fields/topology/custom metadata are preserved
// exactly, as are custom density/temperature metadata. Shared encoded/resident/
// topology/metadata/writer budgets apply before expanded allocations. A
// no-source call still validates/copies the five selected loaded fields. Only
// success writes a new seekable output file. OFF fails before I/O/options.
// Processing generation/triangle options are ignored.
[[nodiscard]] FluidSourcesResult
WriteFluidSources(const std::filesystem::path &inputState,
                  const std::filesystem::path &output,
                  const FluidSourcesOptions &sources = {},
                  const ProcessingOptions &processing = {});

} // namespace container::geometry::vdb
