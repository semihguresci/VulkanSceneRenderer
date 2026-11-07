#include "Container/geometry/OpenVdbProcessing.h"
#include "Container/geometry/OpenVdbParticles.h"
#include "Container/geometry/OpenVdbAdvection.h"
#include "Container/geometry/OpenVdbSegmentation.h"
#include "Container/geometry/OpenVdbMeshExport.h"
#include "Container/geometry/OpenVdbInspect.h"
#include "Container/geometry/OpenVdbPoints.h"
#include "Container/geometry/OpenVdbVectorProcessing.h"
#include "Container/geometry/OpenVdbPoisson.h"
#include "Container/geometry/OpenVdbPointProcessing.h"
#include "Container/geometry/OpenVdbFluidProjection.h"
#include "Container/geometry/OpenVdbPrimitives.h"
#include "Container/geometry/OpenVdbFluidSimulation.h"
#include "Container/geometry/OpenVdbMeasure.h"
#include "Container/geometry/OpenVdbFluidSequence.h"
#include "Container/geometry/OpenVdbMeshConversion.h"
#include "Container/geometry/OpenVdbPointRasterization.h"
#include "Container/geometry/OpenVdbFluidViscosity.h"
#include "Container/geometry/OpenVdbLevelSetTransport.h"
#include "Container/geometry/OpenVdbFluidSources.h"
#include "Container/geometry/OpenVdbPointAttributesJson.h"
#include "Container/geometry/OpenVdbDenseInterchange.h"
#include "Container/geometry/OpenVdbMultiResolution.h"
#include "Container/geometry/OpenVdbPointSelection.h"
#include "Container/geometry/OpenVdbFileAuthoringSpec.h"
#include "Container/geometry/OpenVdbFieldFilter.h"
#include "Container/geometry/OpenVdbFieldComposite.h"
#include "Container/geometry/OpenVdbPointMask.h"
#include "Container/geometry/OpenVdbTopology.h"
#include "Container/geometry/OpenVdbMaskedValues.h"
#include "Container/geometry/OpenVdbSdfBackground.h"
#include "Container/geometry/OpenVdbSdfFilters.h"
#include "Container/geometry/OpenVdbVolumeSpheres.h"
#include "Container/geometry/OpenVdbFastSweeping.h"
#include "Container/geometry/OpenVdbRegions.h"
#include "Container/geometry/OpenVdbRegionQueries.h"
#include "Container/geometry/OpenVdbRayQueries.h"
#include "Container/geometry/OpenVdbFogRayIntegrals.h"
#include "Container/geometry/OpenVdbValueComponents.h"
#include "Container/geometry/OpenVdbSurfaceReference.h"
#include "Container/geometry/OpenVdbParticleGeometry.h"
#include "Container/geometry/OpenVdbSurfaceQueries.h"
#include "Container/geometry/OpenVdbFracture.h"
#include "Container/geometry/OpenVdbFieldAdvection.h"
#include "Container/geometry/OpenVdbPotentialFlow.h"
#include "Container/geometry/OpenVdbPointNeighbors.h"
#include "Container/geometry/OpenVdbLevelSetRebuild.h"
#include "Container/geometry/OpenVdbSurfaceFields.h"
#include "Container/geometry/OpenVdbPointMotion.h"
#include "Container/geometry/OpenVdbNativePointAdvection.h"
#include "Container/geometry/OpenVdbPointTransfer.h"
#include "Container/geometry/OpenVdbFieldResampling.h"
#include "Container/geometry/OpenVdbDiagnostics.h"
#include "Container/geometry/OpenVdbBoundaryProjection.h"
#include "Container/geometry/OpenVdbBoundaryFluidStep.h"
#include "Container/geometry/OpenVdbPointFieldSampling.h"
#include "Container/geometry/OpenVdbNanoInterchange.h"
#include "Container/geometry/OpenVdbNanoStatistics.h"
#include "Container/geometry/OpenVdbNanoPrimitives.h"

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <type_traits>
#include <vector>

namespace {
namespace vdb = container::geometry::vdb;
#ifndef CONTAINER_OPENVDB_BUILD_VERSION
#define CONTAINER_OPENVDB_BUILD_VERSION ""
#endif

void help() {
  std::cout << R"(Usage: OpenVdbTool OPERATION [options]
  --capabilities                   Print versioned read-only protocol/runtime discovery
  --nano-capabilities              Print native I/O, conversion and codec availability
  --inspect-nano INPUT.nvdb        Inspect bounded native-file catalog (no payload validity claim)
  --validate-nano INPUT.nvdb       Check native structure and available CRC32 integrity
  --to-nano INPUT.vdb OUTPUT.nvdb  Convert exact native types with VDB metadata
  --from-nano INPUT.nvdb OUTPUT.vdb  Restore VDB values, states, maps and metadata
  --copy-nano INPUT.nvdb OUTPUT.nvdb  Select/re-encode native grids
  --checksum-nano INPUT OUTPUT.nvdb  Explicit structurally checked CRC repair/rewrite
  --stats-nano INPUT OUTPUT.nvdb   Recompute native active bounds/extrema/moment caches
  --nano-primitive sphere|box|torus OUTPUT.nvdb  Author a bounded CPU native primitive
    --nano-class level-set|fog --nano-storage float|double
    --voxel-size H --half-width B --center X Y Z --origin X Y Z
    --radius R                         Sphere only
    --box-dimensions W H D              Box only, full WORLD widths
    --major-radius R --minor-radius R   WORLD-Y torus only
    --grid-name NAME --native-statistics bounds|minmax|all
    --raw --codec none|zip|blosc --checksum disabled|header|full
    --allow-float-map-rounding          Bounded cached Float map coefficients
    --max-input-bytes N --max-native-bytes N --max-workspace-bytes N
    --max-active-voxels N --max-stored-voxels N --max-nodes N --max-nano-work N
    --max-grids N --max-name-bytes N --max-metadata-bytes N
  --advect-native-points POINTS VELOCITY OUTPUT.vdb  Preserve arbitrary fixed native point payloads
    --point-integration rk2|rk4 --time-step DT --steps N
    --velocity-layout auto|world|native-mac --velocity-grid NAME
    --point-grid NAME --grid-name NAME --point-target-transform M00 ... M33
    --point-position-error X --allow-point-activity-merge
    --max-point-step-travel X --max-point-travel X  L2 VELOCITY-index voxels
    --any-group NAME --all-group NAME --exclude-group NAME
    --max-point-advection-work N --max-point-advection-grids N --max-point-advection-metadata N
    --max-points N --max-attributes N --max-groups N --max-string-bytes N
    --max-voxels N --max-input-bytes N
  --author-file SPEC.json OUTPUT.vdb  Select/copy/rename/merge grids and edit typed metadata
  --sphere OUTPUT.vdb              Create a narrow-band sphere SDF
  --platonic OUTPUT.vdb            Create a tetrahedron, cube, octahedron, dodecahedron or icosahedron
  --mesh INPUT OUTPUT.vdb          Voxelize a closed model surface
  --mesh-distance INPUT OUTPUT.vdb  Unsigned open/closed mesh distance + closest triangle IDs
  --union A.vdb B.vdb OUTPUT.vdb    SDF union
  --intersection A B OUTPUT.vdb    SDF intersection
  --difference A B OUTPUT.vdb      SDF difference (A minus B)
  --fog INPUT.vdb OUTPUT.vdb       Convert an SDF into fog density [0,1]
  --resample INPUT.vdb OUTPUT.vdb  Rebuild SDF or resample fog at a new voxel size
  --resample-field INPUT OUTPUT.vdb  Affine ordinary-field sampling and world deformation
  --offset INPUT.vdb OUTPUT.vdb    Dilate/erode an SDF by --distance world units
  --smooth INPUT.vdb OUTPUT.vdb    Smooth an SDF using an interface-tracked filter
  --filter-field INPUT.vdb OUTPUT.vdb  Filter ordinary scalar/collocated world-vector values
  --composite-fields A B OUTPUT.vdb  Pointwise ordinary field arithmetic or active-value merge
  --transform-field INPUT OUTPUT.vdb  Copy or transform native scalar/vector values
  --topology-mask INPUT OUTPUT.vdb  Extract active states or edit mask morphology
  --topology-boolean A B OUTPUT.vdb  Union/intersect/subtract active-state masks
  --edit-topology INPUT OUTPUT.vdb  Edit native values/activity without mask conversion
  --edit-topology-mask A MASK OUTPUT.vdb  Edit/clip native values using mask active states
  --sdf-background INPUT OUTPUT.vdb  Set signed SDF backgrounds or propagate band signs
  --sdf-filter INPUT OUTPUT.vdb    Native Float/Double SDF filtering and tracking
  --volume-spheres INPUT OUTPUT.json  Approximate native interior sphere fitting
  --fast-sweep INPUT OUTPUT.vdb    Bounded native distance/topology sweeping
  --fast-extend INPUT FIELD OUTPUT.vdb  Extend scalar/vector fields from an interface
  --query-active-regions INPUT.vdb  Exact typed active-value records in INDEX boxes
    --region MINX MINY MINZ MAXX MAXY MAXZ  Repeatable inclusive Int32 INDEX box
    --grid/--input-grid NAME --counts-only
    --max-region-records N --max-region-total-records N  Complete counts, bounded records
    --max-region-nodes N --max-region-active-voxels N --max-region-work N
    --max-region-grids N --max-region-queries N --max-region-metadata N
    --region-report-bytes N --max-voxels N --max-input-bytes N
  --ray-intersections INPUT.vdb    Approximate native SDF first-hit report
  --ray-volume-spans INPUT.vdb     COARSE active-leaf/active-tile span report
    --ray OX OY OZ DX DY DZ MIN MAX  Repeatable WORLD origin/direction/distances
    --grid/--input-grid NAME --max-rays N --max-ray-grids N --max-ray-work N
    --max-ray-metadata N --max-voxels N --max-input-bytes N
    --ray-isovalue X --ray-refinements N --ray-normals on|off  SDF only
    --max-ray-spans N              Volume only; coarse full leaf blocks and tiles
  --particle-geometry INPUT.json OUTPUT.vdb  Sampled tapered sphere unions
    --particle-output sdf|mask|both --trail-time DT --trail-spacing X
    --particle-origin X Y Z --min-radius-voxels R --max-radius-voxels R
    --voxel-size H --half-width B --sdf-grid-name/--grid-name NAME
    --mask-grid-name NAME --max-particles N --max-trail-instances N
    --max-trail-length-voxels X --max-particle-nodes N
    --max-particle-work N --max-particle-metadata N
    --max-voxels N --max-input-bytes N --max-output-bytes N
  JSON v1 object: version=1, particles=[{position:[x,y,z],radius:r,velocity:[x,y,z]}].
  Velocity optional. Trails run OPPOSITE velocity*time; mask tests centers strictly
  inside sampled spheres, not cell coverage or SDF-band activity. No IDs/attributes.
  --integrate-fog-rays INPUT  Read-only nodal trilinear WORLD density integrals
    --ray OX OY OZ DX DY DZ MIN MAX  Repeatable; finite WORLD Euclidean distances
    --grid/--input-grid NAME --extinction S
    --transmission-policy reject-underflow|allow-opaque-underflow
    --max-rays N --max-ray-grids N --max-ray-work N --max-ray-metadata N
    --max-cell-segments-per-ray N --max-cell-segments N
    --max-voxels N --max-input-bytes N
  Inactive values and background participate. Activity-empty is not vacuum.
  Uniform orthogonal maps only; approximate native inverse/normalized direction.
  No scattering, emission, dense-preview parity or exact occupancy claim.
  --surface-reference INPUT OUTPUT.exr  Native CPU SDF center-ray surface image
    --projection perspective|orthographic --eye X Y Z --target X Y Z --up X Y Z
    --fov DEGREES (perspective) --ortho-height WORLD (orthographic)
    --ray-min WORLD --ray-max WORLD --width N --height N --max-pixels N
    --surface-shader matte|normal|headlight --color R G B --background R G B
    --invalid-normal-color R G B (normal/headlight) --grid/--input-grid NAME
    --ray-isovalue WORLD --ray-refinements N --ray-normals on|off
    --max-rays N --max-ray-grids N --max-ray-work N --max-ray-metadata N
    --max-voxels N --max-input-bytes N --max-export-bytes N
  --value-components INPUT OUTPUT.vdb  Source FIELD with six-connected component ACTIVITY
    --grid/--input-grid NAME --component-prefix/--grid-name NAME
    --max-components N --max-component-active-voxels N --max-voxels N
    --max-component-output-voxels N --max-component-nodes N
    --max-component-work N --max-component-grids N --max-component-metadata N
    --max-input-bytes N --max-output-bytes N
  --regions INPUT OUTPUT.vdb       Components, signed interior, enclosure and cavity masks
  --surface-query INPUT            Approximate surface point-cloud distance/local geometry
  --fracture INPUT CUTTER OUTPUT.vdb  Bounded native CSG fracture and rigid instancing
  --advect-field INPUT VELOCITY OUTPUT.vdb  Six-scheme passive scalar/vector transport
  --potential-flow INPUT OUTPUT.vdb  Boundary-driven irrotational exterior flow
  --potential-flow-capabilities    Print potential-flow contracts/defaults/limits
  --point-neighbors INPUT.vdb      Inclusive native center/particle radius queries
  --indexed-point-mask INPUT OUTPUT.vdb  Rebucket selected centers on an explicit map
  --rebuild-level-set INPUT OUTPUT.vdb  Typed closed-contour distance rebuild
  --surface-fields INPUT OUTPUT.vdb  Curvature, normals or local closest-point field + validity
  --move-native-points INPUT OUTPUT.vdb  Deform/repartition P, preserving native payloads
  --transfer-point-attribute INPUT ATTRIBUTE OUTPUT.vdb  Numeric CIC field + weight/density
  --sample-point-attribute POINTS FIELD OUTPUT.vdb  Sample into a native typed point column
  --particles INPUT.json OUTPUT.vdb  Rasterize world-space particle spheres as an SDF
  --velocity OUTPUT.vdb            Create a constant world-space velocity grid
  --advect INPUT.vdb VELOCITY.vdb OUTPUT.vdb  Transport fog through a velocity grid
  --advect-sdf INPUT.vdb VELOCITY.vdb OUTPUT.vdb  Track a moving signed-distance front
  --morph-sdf INPUT.vdb TARGET.vdb OUTPUT.vdb  Relax a front toward a target level set
  --segment INPUT.vdb OUTPUT.vdb   Separate connected SDF zero surfaces into grids
  --export-mesh INPUT.vdb OUTPUT.obj  Export scalar isosurfaces with OBJ normals
  --inspect INPUT.vdb              Print bounded grid metadata as JSON
  --diagnose INPUT.vdb             Report native checks; exit0 means report produced
  --diagnostic-mask INPUT OUTPUT.vdb  Report checks and write Bool union failure mask
  --measure INPUT.vdb              Print level-set area/volume/curvature estimates as JSON
  --points INPUT.json OUTPUT.vdb   Create a PointDataGrid with P/v/pscale attributes
  --export-points INPUT.vdb OUTPUT.json  Export standard point records in world units
  --edit-point-attributes INPUT.vdb EDITS.json OUTPUT.vdb  Edit native typed attributes/groups
  --point-schema INPUT.vdb OUTPUT.json  Export native point schema and traversal contract
  --point-attribute INPUT.vdb NAME OUTPUT.json  Export an exact typed native point column
  --point-group INPUT.vdb NAME OUTPUT.json  Export native point group membership
  --select-points INPUT.vdb OUTPUT.vdb  Keep matching native points, preserving attributes
  --delete-points INPUT.vdb OUTPUT.vdb  Delete matching native points
  --count-points INPUT.vdb         Print native selected/active/inactive counts
  --point-mask INPUT.vdb OUTPUT.vdb  Mark original selected-point voxels, including inactive points
  --point-statistics INPUT.vdb NAME  Print exact native extrema and stable numeric moments
  --dense-export INPUT.vdb OUTPUT.cvarr  Export a native dense region with its activity mask
  --dense-import INPUT.cvarr OUTPUT.vdb  Import the versioned native array format
  --multires INPUT.vdb OUTPUT.vdb  Build full-weighting levels (all, or --level N)
  --multires-info INPUT.vdb        Print constructed level metadata as JSON
  --multires-sample INPUT.vdb X Y Z LOD  Sample a world position and fractional level
  --scatter FOG.vdb OUTPUT.vdb     Scatter density-weighted standard points
  --advect-points POINTS.vdb VELOCITY.vdb OUTPUT.vdb  Integrate point trajectories
  --sample-point-velocity POINTS.vdb FIELD.vdb OUTPUT.vdb  Sample field into v
  --sample-point-radius POINTS.vdb FIELD.vdb OUTPUT.vdb  Sample positive field into pscale
  --points-sdf POINTS.vdb OUTPUT.vdb  Rasterize native point radii as a sphere-union SDF
  --splat-points-density POINTS.vdb OUTPUT.vdb  Deposit point mass with trilinear weights
  --splat-points-velocity POINTS.vdb OUTPUT.vdb  Deposit normalized velocity + density
  --splat-points-radius POINTS.vdb OUTPUT.vdb  Deposit normalized radius + density
  --project VELOCITY.vdb DOMAIN.vdb OUTPUT.vdb  Closed-domain MAC pressure projection
  --step-boundary-fluid STATE.vdb DENSITY.vdb OUTPUT.vdb
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
  --project-boundaries VELOCITY FLUID OUTPUT.vdb  MAC projection with explicit pressure/wall boundaries
  --reproject-boundaries STATE OUTPUT.vdb  Replay a validated canonical boundary face table
  --simulate DENSITY.vdb VELOCITY.vdb DOMAIN.vdb OUTPUT.vdb  Coupled closed smoke timestep
  --simulate-sequence DENSITY VELOCITY DOMAIN OUTPUT_DIR  Save coupled smoke frames
  --viscosity STATE.vdb OUTPUT.vdb  Implicit MAC viscosity followed by pressure projection
  --fluid-sources STATE.vdb OUTPUT.vdb  Exact local density/heat source and sink update
  --gradient INPUT.vdb OUTPUT.vdb  Scalar gradient in world units
  --divergence INPUT.vdb OUTPUT.vdb  World-vector divergence
  --curl INPUT.vdb OUTPUT.vdb      World-vector curl
  --laplacian INPUT.vdb OUTPUT.vdb  Scalar world-space Laplacian
  --magnitude INPUT.vdb OUTPUT.vdb  Vector magnitude
  --normalize INPUT.vdb OUTPUT.vdb  Unit vector field (zero stays zero)
  --poisson INPUT.vdb OUTPUT.vdb   Solve Laplacian(u)=input with zero exterior
  --sample INPUT.vdb X Y Z         Print a nearest-voxel world-space scalar
  --sample-vector INPUT.vdb X Y Z  Print a trilinear world-vector sample as JSON

  --radius R                      Sphere radius (default 1)
  --codec none|zip|blosc           Native writer codec (default none)
  --checksum disabled|header|full Native writer/repair CRC32 mode (default full)
  --raw                           Write raw uncompressed native buffers
  --allow-float-map-rounding       Permit derived Float map caches; retain Double affine map
  --allow-metadata-loss            Permit named reverse-conversion omissions/defaults
  --ignore-checksum               Explicitly accept CRC mismatch after structural checks
  --require-full-checksum         Require full native CRC coverage
  --max-input-bytes N --max-native-bytes N --max-workspace-bytes N  Native shared byte caps
  --max-active-voxels N --max-stored-voxels N --max-nodes N --max-nano-work N  Native topology/work caps
  --max-name-bytes N               Native grid/metadata name byte cap
  --native-statistics bounds|minmax|all  Native cache scope; vector moments use squared magnitudes
  --center X Y Z                  Sphere center (default 0 0 0)
  --faces N                       Platonic face count: 4,6,8,12,20 (default 6)
  --scale S                       Platonic canonical world scale (cube side length; default 1)
  --voxel-size S                  Generation/resampling voxel size (default 0.1)
  --half-width N                  SDF band half width in voxels (default 3)
  --import-scale S                Mesh import scale (default 1)
  --triangle-id-name NAME          Unsigned mesh closest-ID grid name (default triangle_id)
  --no-triangle-ids                Omit the unsigned mesh closest-ID grid
  --distance D                    Offset: positive expands, negative contracts
  --filter gaussian|mean|median    Smoothing filter (default gaussian)
  --width N                       Smoothing kernel radius, 1..4 voxels (default 1)
  --iterations N                  Smoothing iterations, 1..32 (default 1)
  --field-filter mean|gaussian|median|offset  Ordinary field filter (default mean)
  --field-offset X Y Z            Stored-component offset per iteration; scalar Y/Z must be zero
  --filter-mask INPUT.vdb         Float/Double alpha mask with exactly matching transform
  --filter-mask-grid NAME         Optional mask grid selector
  --mask-min V --mask-max V        Alpha range; SmoothStep per axis (default 0..1)
  --invert-mask                   Invert the normalized alpha weights
  --voxelize-tiles                Filter active tile cells; otherwise tiles remain unchanged
  --max-filter-work N             Ordinary filter kernel work cap (default 200000000)
  --field-operation sum|product|divide|min|max|blend|merge-active  Composite operation (default sum)
  --field-activity union|intersection|left|right  Composite active flags (default union)
  --blend-weight W                Weight of B in blend, [0,1] (default 0.5)
  --merge-preference left|right    Preferred active operand (default left)
  --vector-extrema componentwise|magnitude  Min/max vector comparison (default componentwise)
  --value-operation copy|affine|absolute|clamp  Unary value operation (default copy)
  --value-scope active|all         Numeric unary scope (default active); Copy always copies all bits
  --value-scale X Y Z             Affine stored-component scale (default 1 1 1)
  --value-offset X Y Z            Affine stored-component offset (default 0 0 0)
  --value-min X Y Z --value-max X Y Z  Clamp bounds (scalar Y/Z must remain native defaults)
  --max-field-work N              Composite/value-transform sparse work cap (default 200000000)
  --point-counts                  Also write exact Int64 selected-point counts per original voxel
  --point-count-name NAME          Counts grid name (default point_count); requires --point-counts
  --max-particles N               Particle count budget (default 1000000)
  --vector X Y Z                  Constant velocity in world units per second
  --time DT                       Signed advection time in seconds, [-3600,3600]
  --steps N                       Advection substeps, 1..64 (default 1)
  --velocity-grid NAME            Select the advection velocity grid
  --target-grid NAME              Select the morph target level set
  --normalization-iterations N    SDF tracker passes, 1..8 (default 3)
  --max-cfl-steps N               SDF transport CFL cap, 1..4096 (default 512)
  --max-transport-work N          SDF leaf-slot/sample work cap (default 500000000)
  --max-components N              Segmentation component cap, 1..1024 (default 64)
  --iso-value V                   Mesh export isovalue (level set 0; otherwise 0.5)
  --adaptivity A                  Mesh export adaptivity, 0..1 (default 0)
  --stats                         Inspect loaded Float/Double active-value statistics
  --histogram-bins N              Active-value histogram, 1..4096 bins (requires --stats)
  --histogram-range MIN MAX       Optional finite increasing histogram range
  --max-grids N                   Inspection descriptor cap, 1..4096 (default 256)
  --max-metadata-bytes N          Inspection metadata text budget (default 1048576)
  --max-spec-bytes N              File authoring JSON byte cap, 1..67108864 (default 1048576)
  --max-spec-depth N              File authoring JSON depth cap, 1..64 (default 16)
  --max-points N                  Point count cap (default 1000000)
  --max-attributes N              Native attribute cap, 1..1024 (default 64)
  --max-groups N                  Native point group cap, 1..4096 (default 256)
  --max-edits N                   Native attribute/group edit cap, 1..1024 (default 64)
  --max-string-bytes N            Native dictionary/edit/decoded string cap (default 1048576)
  --any-group NAME                Require any named group (repeatable)
  --all-group NAME                Require all named groups (repeatable)
  --exclude-group NAME            Exclude any named group (repeatable)
  --dense-origin I J K            Required dense export absolute integer origin
  --dense-dimensions NX NY NZ      Required positive dense export dimensions, x fastest
  --levels N                      Multiresolution retained levels, 1..8 (default 3)
  --level N                       Write one integer multiresolution level, starting at 0
  --max-multires-work N           Conservative voxel/filter work cap (default 1000000000)
  --point-count N                 Scatter count, including zero (default 1000)
  --seed N                        Scatter unsigned random seed (default 0)
  --point-radius R                Scatter point radius in world units (default 1)
  --integration rk2|rk4           Point trajectory integrator (default rk2)
  --keep-velocity                 Preserve point v during trajectory integration
  --field-grid NAME               Point attribute sampling field selector
  --topology-operation OP          active-mask, dilate, erode, open or close
  --topology-boolean-operation OP  union, intersection or difference
  --topology-iterations N          Lattice steps per morphology phase [1,32]
  --topology-neighbors N           6, 18 or 26 lattice neighbors
  --topology-tiles POLICY          preserve, expand or ignore active tiles
  --max-topology-work N            Shared topology operation work cap
  --edit-operation OP             activate-value, deactivate-value, prune-exact,
                                  fill-box, clip-box or replace-background
  --native-value VALUE            One native decimal or true/false (exact Int64)
  --native-vector X Y Z           Three native decimal components
  --index-box X0 Y0 Z0 X1 Y1 Z1   Inclusive index box required for fill/clip
  --fill-inactive                 Fill with inactive values (default active)
  --mask-operation OP             activate, deactivate, intersect-activity,
                                  clip-inside or clip-outside
  --mask-grid NAME                Native mask grid selector for --edit-topology-mask
  --sdf-mode MODE                 background or flood-fill (closed signed field)
  --sdf-filter-operation OP       gaussian|mean|median|mean-curvature|laplacian|fillet|offset|
                                 normalize|track|prune|resize-band|dilate-band|erode-band
  --sdf-width N                   Kernel radius, 1..4 (kernel operations only)
  --sdf-iterations N              Repetitions, 1..32
  --sdf-normalizations N          Tracker normalization count, 0..64 (default 3)
  --sdf-spatial SCHEME            first|second|third|weno5|hjweno5 (default hjweno5)
  --sdf-temporal SCHEME           rk1|rk2|rk3 (default rk1)
  --sdf-trim MODE                 none|interior|exterior|all (default all)
  --sdf-distance DISTANCE         Outward world distance per offset iteration (required)
  --sdf-band-width N              Resize target half width, 1..64 source voxels
  --sdf-band-layers N             Dilation/erosion layers per iteration, 1..64
  --sdf-mask INPUT.vdb            Optional matching-map Float/Double alpha field
  --sdf-mask-grid NAME            Alpha field selector
  --sdf-normalization-mask FILE   Optional native activity mask for normalize
  --sdf-normalization-grid NAME   Normalization activity grid selector
  --sphere-min-count N            Required minimum after radius filtering (default 0)
  --sphere-max-count N            Maximum fitted spheres, 1..4096 (default 50)
  --sphere-candidates N           Scatter candidates, 1..100000 (default 2048)
  --sphere-min-radius WORLD       Discard smaller fitted spheres (default 0)
  --sphere-max-radius WORLD       Shrink fitted spheres to this radius (default 1e6)
  --sphere-overlap                Permit sphere overlap
  --max-sphere-work N             Sphere fitting work cap (default 8000000000)
  --max-sphere-grids N            Sphere fitting input catalog cap (default 256)
  --max-sphere-metadata BYTES     Sphere fitting metadata cap (default 1048576)
  --sweep-operation OP            fog|reinitialize|dilate|mask|topology (required for --fast-sweep)
  --sweep-fog                     Field extension source is fog (default SDF)
  --sweep-isovalue VALUE          Source isovalue (default fog .5 / SDF 0)
  --sweep-iterations N            Eight-direction iterations, 1..32 (default 1)
  --sweep-dilation N              Active band layers, 1..64 (default 1)
  --sweep-neighbors MODE          face|face-edge|all for band dilation
  --sweep-side MODE               all|greater|less for dilation/field extension
  --sweep-mask INPUT.vdb          Native activity domain; required for mask operation
  --sweep-mask-grid NAME          Sweeping mask selector
  --sweep-voxelize-tiles          Permit active fog/topology tiles
  --topology-half-width N         Topology distance band, 3..16 (default 3)
  --topology-closing N            Topology closing steps, 0..8
  --topology-dilation N           Topology dilation steps, 0..8
  --topology-smoothing N          Topology smoothing steps, 0..8
  --max-sweep-work N              Sweep work cap, at most 10000000000
  --region-operation MODE         components|interior|enclosed|cavities (required)
  --region-isovalue VALUE         Strict signed threshold (default 0)
  --max-region-components N       Component cap, 1..1024 (default 64)
  --max-region-grids N            Region input descriptor cap (default 256)
  --max-region-metadata BYTES     Aggregate region metadata cap (default 1048576)
  --max-region-work N             Region work cap (default 200000000)
  --position X Y Z                Repeat for surface queries; no positions returns metadata
  --max-surface-queries N         Query cap, 1..1000000 (default 10000)
  --max-surface-work N            Surface point-cloud/stencil work (default 1000000000)
  --max-surface-grids N           Surface query descriptor cap (default 256)
  --max-surface-metadata BYTES    Surface query metadata cap (default 1048576)
  --fracture-instance TX TY TZ RX RY RZ  Repeat up to 32; rotations in world radians
  --fracture-pivot X Y Z          Pivot for preceding instance (default source map origin)
  --fracture-disjoint-cutters     Later cutters only cut residuals
  --fracture-segment              Split zero surfaces; negative-exterior cavities reject
  --cutter-grid NAME              Cutter selector for fracture
  --max-fracture-pieces N         Aggregate piece cap, 1..256 (default 64)
  --max-fracture-work N           Fracture work cap (default 200000000)
  --advection-scheme MODE         semi|midpoint|rk3|rk4|maccormack|bfecc (default midpoint)
  --advection-limiter MODE        none|clamp|revert, correction schemes only (default clamp)
  --velocity-layout MODE          auto|world|mac-index for field advection
  --advection-cfl VALUE           Source-voxel travel/step in (0,16], default 4
  --advection-mask INPUT          Optional matching-map final activity write selector
  --advection-mask-grid NAME      Advection mask selector
  --advection-voxelize-tiles       Permit active source tile voxelization
  --max-advection-work N          Field transport work cap (default 200000000)
  --far-field X Y Z               Potential-flow WORLD velocity (default 1 0 0)
  --collider-velocity X Y Z        Uniform WORLD collider velocity (default 0 0 0)
  --exterior-width N              Native 18-neighbor shell steps, 1..128 (default 6)
  --max-flow-work N               Potential flow work cap (default 8000000000)
  --max-flow-grids N              Flow input descriptor cap (default 256)
  --max-flow-metadata N           Flow metadata byte cap (default 1048576)
  --potential-name NAME           Physical perturbation-potential grid (default potential)
  --neumann-name NAME             Projected-normal grid (default neumann)
  --neighbor-query X Y Z R        Repeatable inclusive world query, R in [0,1e6]
  --neighbor-mode MODE            centers|particles (default centers)
  --neighbor-index-voxel-size S   World index spacing [1e-6,1e6] (default .1)
  --neighbor-buckets              Include native 8^3 partitions; permits index-only query
  --neighbor-atlas-levels N       Particle subdivision cap, 1..32 (default 16)
  --neighbor-radius-attribute NAME  Native particle radius column
  --neighbor-radius R            Constant particle world radius; exclusive with attribute
  --max-neighbor-queries N        Query batch cap (default 10000)
  --max-neighbors N               Aggregate returned hit cap (default 1000000)
  --indexed-voxel-size S          Indexed mask world voxel size (default .1)
  --indexed-origin X Y Z          Indexed mask world origin (default 0 0 0)
  --indexed-counts                Include an Int64 count grid in the mask output
  --indexed-count-name NAME       Count grid name; requires --indexed-counts
  --max-point-index-work N        Point index work cap (default 200000000)
  --max-point-index-grids N       Point index descriptor cap (default 256)
  --max-point-index-metadata N    Point index metadata byte cap (default 1048576)
  --rebuild-isovalue VALUE        Finite source contour threshold (default 0)
  --rebuild-exterior-width N      Exterior band in TARGET voxels, 1..64 (default 3)
  --rebuild-interior-width N      Interior band in TARGET voxels, 1..64 (default 3)
  --rebuild-target-transform M00..M33  16 row-major COLUMN-vector coefficients
  --max-rebuild-work N            Rebuild work upper-bound cap (default 1000000000)
  --max-rebuild-grids N           Rebuild descriptor cap (default 256)
  --max-rebuild-metadata N        Rebuild aggregate metadata cap (default 1048576)
  --surface-field-operation OP   Required: mean|gaussian|normal|closest-point
  --surface-field-double         Promote surface values to Double/Vec3D
  --surface-validity-name NAME   Bool field validity grid name (default surface_valid)
  --max-surface-field-work N     Surface field work cap (default 200000000)
  --max-surface-field-grids N    Surface field descriptor cap (default 256)
  --max-surface-field-metadata N Surface field aggregate metadata cap (default 1048576)
  --point-world-transform M00..M33  16 row-major COLUMN-vector world deformation entries
  --point-target-transform M00..M33 Optional uniform orthogonal destination map
  --point-position-error X       Max target-voxel position error (0,.25], default 1e-4
  --allow-point-activity-merge    Permit counted active/inactive destination activity OR
  --max-point-motion-work N      Native move work cap (default 200000000)
  --max-point-motion-grids N     Native move descriptor cap (default 256)
  --max-point-motion-metadata N  Native move aggregate metadata cap (default 1048576)
  --transfer-mode mean|sum        Native column weighted mean (default) or sum
  --transfer-storage double|float Field precision; companion always Double
  --transfer-companion weight|density Weight sum (default) or mass/world-volume
  --transfer-target-transform M00..M33 16 row-major COLUMN-vector destination map entries
  --transfer-companion-name NAME Default field name plus _weight or _density
  --transfer-weight-attribute NAME Positive scalar mass column; excludes --transfer-mass
  --transfer-mass X               Positive constant mass per selected point (default 1)
  --transfer-integers exact|allow-rounded Explicit Int64 conversion policy
  --max-point-transfer-work N     Native transfer work cap (default 2000000000)
  --max-point-transfer-grids N    Native transfer descriptor cap (default 256)
  --max-point-transfer-metadata N Native transfer aggregate metadata cap (default 1048576)
  --field-target-transform M00..M33 16 row-major COLUMN-vector target map entries
  --field-world-transform M00..M33 16 row-major COLUMN-vector world deformation entries
  --field-interpolation nearest|linear|quadratic Default linear; discrete types require nearest
  --field-activity nearest|any    Sampled activity policy (default any stencil contributor)
  --field-vector-values semantic|raw WORLD vector handling under deformation (default semantic)
  --max-field-resampling-work N   Native sampling work cap (default 1000000000)
  --max-field-resampling-grids N  Native descriptor cap (default 256)
  --max-field-resampling-metadata N Aggregate metadata cap (default 1048576)
  --diagnostic-profile stored|sdf|fog|points Native diagnostic profile (default stored)
  --diagnostic-range MIN MAX      Inclusive raw component Double bounds
  --diagnostic-range-scope active|stored Field range scope (default active)
  --diagnostic-gradient none|first|weno SDF gradient choice (default none)
  --diagnostic-norm-range MIN MAX Unsquared world gradient norm bounds
  --diagnostic-footprint active|stored Required active samples or native continuation
  --diagnostic-tolerance X        Absolute inactive/background tolerance for sdf/fog
  --diagnostic-attribute NAME     Repeat chosen numeric PointData columns
  --diagnostic-mask-name NAME     Bool failure mask name (writer only)
  --max-diagnostic-work N         Shared native/report work cap (default 1000000000)
  --max-diagnostic-grids N        Catalog cap (default 256)
  --max-diagnostic-metadata N     Aggregate metadata cap (default 1048576)
  --max-diagnostic-examples N     Examples per check, 0..128 (default 8)
  --velocity-grid NAME            Boundary velocity selector; --grid/--input-grid aliases
  --fluid-grid NAME               Boundary fluid-mask selector
  --boundary-solid FILE          Optional solid-mask binding
  --boundary-open FILE           Optional open-face pressure region
  --boundary-air FILE            Optional air-center/interface pressure region
  --boundary-phi FILE            Scalar zero crossing for free-surface fractions
  --boundary-wall FILE           Optional world/MAC wall velocity field
  --boundary-pressure FILE       Optional world scalar prescribed pressure field
  --boundary-ROLE-grid NAME       Optional selector for its boundary role file
  --missing-boundary wall|open|air|surface Missing-neighbor policy (default wall)
  --air-pressure center|surface   Explicit-air pressure location (default center)
  --minimum-interface-fraction X  Effective free-surface clamp, 1e-6..1 (default .01)
  --wall-velocity X Y Z           Constant world wall velocity; excludes wall field
  --ambient-pressure X           Constant pressure; excludes pressure field
  --velocity-name NAME           Boundary velocity output name
  --pressure-name NAME           Physical pressure output name
  --fluid-name NAME              Fluid-mask output name
  --face-kind-name NAME          Canonical packed face-kind output name
  --face-fraction-name NAME      Canonical packed effective-fraction output name
  --face-pressure-name NAME      Canonical packed prescribed-pressure output name
  --wall-velocity-name NAME      Canonical MAC wall-normal output name
  --no-boundary-state             Omit packed canonical face channels
  --no-domain                     Omit fluid output; requires --no-boundary-state
  --face-kind-grid NAME          Canonical replay face-kind selector
  --face-fraction-grid NAME      Canonical replay effective-fraction selector
  --face-pressure-grid NAME      Canonical replay prescribed-pressure selector
  --wall-velocity-grid NAME      Canonical replay MAC wall-normal selector
  --max-boundary-work N           Shared native solver work cap (default 2000000000)
  --max-boundary-grids N          Catalog cap (default 256)
  --max-boundary-metadata N       Aggregate metadata cap (default 1048576)
  Boundary WORLD tags use raw physical XYZ velocity components. Nonuniform
  prescribed pressure adds a fresh impulse on every replay, even at zero divergence.
  Diagnostic allRequestedPassed/complete are separate from report/job success;
  an empty failure mask does not imply passing checks. No implicit repair occurs.
  --sdf-alpha-range MIN MAX       Native alpha interval (default 0 1)
  --sdf-invert-alpha              Invert alpha weights
  --max-sdf-work N                Shared bounded sample work, 1..10000000000
  --outside-width DISTANCE        Positive world-distance exterior background
  --inside-width DISTANCE         Positive interior magnitude (default symmetric)
  --attribute-name NAME           Sampled native column name (sampled)
  --attribute-operation OP        add or set (add)
  --point-interpolation MODE      nearest, linear or quadratic (linear)
  --point-field-precision MODE    promote or source (promote)
  --max-point-field-work N        Native point sampling work cap
  --max-point-field-grids N       Source/field catalog cap [1,4096]
  --max-point-field-metadata N    Shared sampling metadata byte cap
  --point-mass M                  Positive mass per point for splatting (default 1)
  --splat-density-name NAME        Companion density for velocity/radius splats (point_density)
  --kinematic-viscosity NU         World distance squared/sec (viscosity default .01; smoke default 0)
  --source X Y Z R                Append a world sphere; subsequent rate flags edit it
  --density-rate J                Sphere density injection per second (default 0)
  --heat-rate J                   Sphere Kelvin injection per second (default 0)
  --density-decay K               Sphere density decay per second (default 0)
  --temperature-relaxation K      Sphere relaxation toward ambient per second (default 0)
  --clear-density                 Explicitly clear sphere density before its rates
  --reset-temperature             Explicitly reset sphere Kelvin to ambient before rates
  --domain-grid NAME              Projection domain selector (--input-grid selects velocity)
  --acceleration X Y Z            Projection world acceleration (default 0 0 0)
  --max-fluid-cells N             Projection domain cap, at most 1000000 (default 100000)
  --velocity-name NAME            Velocity output name (projection velocity; flow flow)
  --pressure-name NAME            Pressure output name (default pressure)
  --domain-name NAME              Domain output name (simulation fluid; flow flow_domain)
  --no-domain                    Omit saved projection domain grid
  --temperature-input PATH        Optional smoke Kelvin field (otherwise ambient)
  --temperature-grid NAME         Smoke temperature selector
  --gravity X Y Z                 Smoke world acceleration (default 0 0 0)
  --buoyancy-density A            Smoke downward density coefficient (default 0)
  --buoyancy-temperature B        Smoke upward Kelvin coefficient (default 0)
  --buoyancy-direction X Y Z      Unit world direction (default 0 1 0)
  --ambient-temperature K         Smoke ambient Kelvin (default 293.15)
  --cfl C                        Smoke component travel limit, (0,0.5] (default 0.5)
  --max-substeps N                Adaptive smoke step cap, 1..64 (default 64)
  --frames N                      Simulation sequence frame count, 1..256 (default 24)
  --max-output-bytes N            Aggregate sequence files/manifest cap (default 2147483648)
  --density-name NAME             Smoke output density name (default density)
  --temperature-name NAME         Smoke output Kelvin name (default temperature)
  --max-iterations N              Solver iteration cap, 1..10000 (Poisson 200; projection/flow 1000)
  --tolerance T                   Solver relative residual/divergence tolerance (default 1e-6)
  --max-solver-voxels N            Solver domain cap <=1000000 (Poisson 1000000; flow 100000)
  --grid-name NAME                Output name (surface; field/point_mask/points/velocity/solution by operation)
  --input-grid NAME               Select input for processing, export or inspection
  --grid-a NAME --grid-b NAME      Select input grids for CSG or field compositing
  --max-voxels N                  Conservative bounds budget (default 16000000)
  --max-triangles N               Mesh triangle budget (default 1000000)
  --max-input-bytes N             Input/filter workspace budget (default 536870912)

Mesh inputs: .bim, .ifc, .ifcx, .usd, .usda, .usdc, .usdz, .gltf, .glb.
Mesh surfaces should be closed. CSG inputs must have matching transforms and
SDF band widths; resample both to matching --voxel-size and --half-width first.
Input grid names accept descriptor:<exact descriptor name> for duplicate names.
Resampling rebuilds SDF world distances; OpenVDB rebuilds can remove interior
bubbles. glTF uses static active-scene world instances; bake skins and morphs.
Particle JSON is an array of {"position":[x,y,z],"radius":r}; radii span 1.5..100
source voxels. Advection requires FloatGrid fog and collocated world-space
Vec3S/Vec3D velocity. It preserves the fog transform; generation settings do not
resample it. Total displacement is at most 256 source voxels and each substep
at most 16; increase --steps when needed. Semi-Lagrangian transport can diffuse
density and does not simulate pressure, forces or incompressibility.
Segmentation labels connected zero surfaces rather than solid-volume objects.
Negative-exterior cavity components are rejected to preserve distance signs.
Components share one VDB output; the output name base is at most 256 bytes.
Inspection reads metadata without voxel data unless --stats is provided. Scalar
statistics count active tiles by voxel multiplicity; empty min/max are null.
Mesh export reuses surface import, preserving world coordinates and winding.
Triangle caps are checked after meshing; input limits also bound extraction.
Point JSON uses position:[x,y,z], optional velocity:[x,y,z] and radius:r.
Points use the standard P/v/pscale layout; unsupported attributes/groups fail.
Native point SDF uses pscale as sphere radius (1.5..100 target voxels). Splats
use a fixed eight-cell kernel independent of radius. Density is mass per world
volume; velocity/radius are weighted means with a companion density grid.
Point scattering weights active fog cells by density and world volume. Point
advection uses RK2/RK4 and optionally updates v from the final field sample;
point processing repartitions into origin-aligned --voxel-size cells.
Projection uses active positive scalar/true Bool domain cells and closed walls.
It writes local index-space staggered velocity in voxels/second, physical
unit-density pressure, and optionally the domain. It adds force and projects;
it does not advect velocity or perform a complete fluid simulation timestep.
--simulate couples MAC self-advection, density/Kelvin transport, gravity/buoyancy
and projection at each adaptive CFL step. It requires native MAC from --project
and matching domain/scalar transforms. The five-grid output is a restart state.
Semi-Lagrangian transport can diffuse density; mass/integral drift is reported.
Optional viscosity and source spheres run at each actual smoke substep. Sources
run last, so their density/heat drives buoyancy on the next actual substep.
Vorticity and free surfaces remain separate workflows.
--viscosity takes a canonical five-grid restart state. Its zero exterior
face-lattice stencil approximates no-slip stair-step walls. Scalar fields are
preserved; --time and solver/state-name options apply. Zero viscosity validates
and copies the selected state without another solve.
Calculus operates on active topology with inactive values in derivative stencils.
Poisson solves the active domain with zero Dirichlet exterior; it is one solver
operation, not a complete fluid simulation. Nonconvergence leaves no output.
Editing preserves the source transform and band; uniform voxels and a 3..64
voxel band half width are required. Float32 tracking supports voxel sizes from
1e-6 to 1e6 world units. Nonzero offsets span 0.001..256 source voxels.
Incomplete imports fail. Output paths must not exist. Build with ENABLE_OPENVDB=ON.
Field compositing requires identical native types, semantic tags and EXACT affine
maps. Arithmetic includes inactive values and backgrounds; activity is selected
separately. Divide requires nonzero denominators everywhere, including background.
Merge-active copies active values and otherwise retains A's inactive/background bits.
Scalar affine parameters use X; leave Y/Z at their defaults. Clamp bound vectors
for scalar fields must keep Y/Z at +/-1.7976931348623157e308 respectively.
)";
}

std::string value(int argc, char** argv, int& index, std::string_view option) {
  if (++index >= argc || argv[index] == nullptr)
    throw std::invalid_argument("missing value for " + std::string(option));
  return argv[index];
}

double number(const std::string& text, std::string_view option) {
  size_t end = 0;
  double result;
  try { result = std::stod(text, &end); }
  catch (...) { throw std::invalid_argument("invalid number for " + std::string(option)); }
  if (end != text.size() || !std::isfinite(result))
    throw std::invalid_argument("finite number required for " + std::string(option));
  return result;
}

uint64_t unsignedCount(const std::string& text, std::string_view option, bool allowZero) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    throw std::invalid_argument("unsigned decimal count required for " + std::string(option));
  size_t end = 0;
  uint64_t result;
  try { result = std::stoull(text, &end); }
  catch (...) { throw std::invalid_argument("invalid count for " + std::string(option)); }
  if (end != text.size() || (!allowZero && result == 0))
    throw std::invalid_argument("positive count required for " + std::string(option));
  return result;
}

uint64_t count(const std::string& text, std::string_view option) {
  return unsignedCount(text, option, false);
}

int32_t indexCoordinate(const std::string& text,std::string_view option) {
  const auto digits=text.starts_with('-') ? std::string_view(text).substr(1) : std::string_view(text);
  if (digits.empty() || digits.find_first_not_of("0123456789")!=std::string_view::npos)
    throw std::invalid_argument("signed integer required for "+std::string(option));
  size_t end=0; int64_t result;
  try { result=std::stoll(text,&end); }
  catch (...) { throw std::invalid_argument("invalid integer for "+std::string(option)); }
  if (end!=text.size() || result<INT32_MIN || result>INT32_MAX)
    throw std::invalid_argument("int32 coordinate required for "+std::string(option));
  return static_cast<int32_t>(result);
}

std::filesystem::path path(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::optional<vdb::DifferentialOperation> differentialOperation(std::string_view command) {
  using Operation = vdb::DifferentialOperation;
  if (command == "--gradient") return Operation::Gradient;
  if (command == "--divergence") return Operation::Divergence;
  if (command == "--curl") return Operation::Curl;
  if (command == "--laplacian") return Operation::Laplacian;
  if (command == "--magnitude") return Operation::Magnitude;
  if (command == "--normalize") return Operation::Normalize;
  return {};
}

template <typename T>
nlohmann::json optionalJson(const std::optional<T>& value) {
  return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

template <typename Bounds>
nlohmann::json boundsJson(const std::optional<Bounds>& bounds) {
  if (!bounds) return nullptr;
  return {{"minimum", bounds->minimum}, {"maximum", bounds->maximum}};
}

void printInspection(const vdb::FileInspection& inspection) {
  using nlohmann::json;
  json grids = json::array();
  for (const auto& grid : inspection.grids) {
    json metadata = json::array();
    for (const auto& field : grid.metadata)
      metadata.push_back({{"name", field.name}, {"type", field.type}, {"value", field.value}});
    json statistics = nullptr;
    if (grid.statistics) {
      const auto& values = *grid.statistics;
      json histogram = nullptr;
      if (values.histogram) {
        const auto& h = *values.histogram;
        histogram = {{"minimum", optionalJson(h.minimum)}, {"maximum", optionalJson(h.maximum)},
                     {"bins", h.bins}, {"belowRange", h.belowRange}, {"aboveRange", h.aboveRange}};
      }
      statistics = {{"background", values.background}, {"count", values.count},
                    {"minimum", optionalJson(values.minimum)},
                    {"maximum", optionalJson(values.maximum)}, {"sum", values.sum},
                    {"mean", optionalJson(values.mean)},
                    {"populationStdDev", optionalJson(values.populationStdDev)},
                    {"rootMeanSquare", optionalJson(values.rootMeanSquare)},
                    {"histogram", std::move(histogram)},
                    {"actualMemoryBytes", values.actualMemoryBytes},
                    {"indexBounds", boundsJson(values.indexBounds)},
                    {"worldBounds", boundsJson(values.worldBounds)}};
    }
    grids.push_back({{"descriptorName", grid.descriptorName}, {"name", grid.name},
                     {"valueType", grid.valueType}, {"gridClass", grid.gridClass},
                     {"vectorType", grid.vectorType}, {"inWorldSpace", grid.inWorldSpace},
                     {"voxelSize", grid.voxelSize}, {"affine", grid.affine},
                     {"indexToWorld", optionalJson(grid.indexToWorld)},
                     {"transformDiagnostic", grid.transformDiagnostic},
                     {"recorded", {{"activeVoxelCount", optionalJson(grid.activeVoxelCount)},
                                   {"memoryBytes", optionalJson(grid.memoryBytes)},
                                   {"indexBounds", boundsJson(grid.indexBounds)},
                                   {"worldBounds", boundsJson(grid.worldBounds)}}},
                     {"metadata", std::move(metadata)}, {"statistics", std::move(statistics)},
                     {"statisticsUnavailableReason", grid.statisticsUnavailableReason}});
  }
  const json report{{"schemaVersion", 1}, {"fileBytes", inspection.fileBytes},
                    {"grids", std::move(grids)}};
  // Invalid source string bytes are replaced so the diagnostic report remains
  // valid UTF-8 JSON even when a third-party VDB contains non-UTF-8 metadata.
  std::cout << report.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
}
#include "openvdb_nano_commands.inl"
#include "openvdb_nano_primitives_commands.inl"
#include "openvdb_value_topology_commands.inl"
#include "openvdb_masked_values_commands.inl"
#include "openvdb_sdf_background_commands.inl"
#include "openvdb_sdf_filter_commands.inl"
#include "openvdb_volume_spheres_commands.inl"
#include "openvdb_fast_sweeping_commands.inl"
#include "openvdb_regions_commands.inl"
#include "openvdb_region_queries_commands.inl"
#include "openvdb_ray_queries_commands.inl"
#include "openvdb_fog_ray_integrals_commands.inl"
#include "openvdb_value_components_commands.inl"
#include "openvdb_surface_reference_commands.inl"
#include "openvdb_particle_geometry_commands.inl"
#include "openvdb_field_advection_commands.inl"
#include "openvdb_potential_flow_commands.inl"
#include "openvdb_point_neighbors_commands.inl"
#include "openvdb_level_set_rebuild_commands.inl"
#include "openvdb_surface_fields_commands.inl"
#include "openvdb_point_motion_commands.inl"
#include "openvdb_native_point_advection_commands.inl"
#include "openvdb_point_transfer_commands.inl"
#include "openvdb_field_resampling_commands.inl"
#include "openvdb_diagnostics_commands.inl"
#include "openvdb_boundary_projection_commands.inl"
#include "openvdb_boundary_fluid_step_commands.inl"
#include "openvdb_surface_query_commands.inl"
#include "openvdb_fracture_commands.inl"
}  // namespace

int runOpenVdbTool(int argc, char** argv) {
  try {
    if (const auto regions = runRegionQueriesCommand(argc, argv)) return *regions;
    if (const auto rays = runRayQueriesCommand(argc, argv)) return *rays;
    if (const auto fog = runFogRayIntegralsCommand(argc, argv)) return *fog;
    if (const auto components = runValueComponentsCommand(argc, argv)) return *components;
    if (const auto surface = runSurfaceReferenceCommand(argc, argv)) return *surface;
    if (const auto particles = runParticleGeometryCommand(argc, argv)) return *particles;
    if (const auto primitive = runNanoPrimitivesCommand(argc, argv)) return *primitive;
    if (const auto nativePoints = runNativePointAdvectionCommand(argc, argv)) return *nativePoints;
    if (const auto native = runNanoCommand(argc, argv)) return *native;
    if (const auto edit = runValueTopologyCommand(argc, argv)) return *edit;
    if (const auto mask = runMaskedValuesCommand(argc, argv)) return *mask;
    if (const auto sdf = runSdfBackgroundCommand(argc, argv)) return *sdf;
    if (const auto filter = runSdfFilterCommand(argc, argv)) return *filter;
    if (const auto spheres = runVolumeSpheresCommand(argc, argv)) return *spheres;
    if (const auto swept = runFastSweepingCommand(argc, argv)) return *swept;
    if (const auto regions = runRegionsCommand(argc, argv)) return *regions;
    if (const auto query = runSurfaceQueryCommand(argc, argv)) return *query;
    if (const auto fracture = runFractureCommand(argc, argv)) return *fracture;
    if (const auto advected = runFieldAdvectionCommand(argc, argv)) return *advected;
    if (const auto flow = runPotentialFlowCommand(argc, argv)) return *flow;
    if (const auto indexed = runPointNeighborsCommand(argc, argv)) return *indexed;
    if (const auto rebuild = runLevelSetRebuildCommand(argc, argv)) return *rebuild;
    if (const auto surface = runSurfaceFieldsCommand(argc, argv)) return *surface;
    if (const auto motion = runPointMotionCommand(argc, argv)) return *motion;
    if (const auto transferred = runPointTransferCommand(argc, argv)) return *transferred;
    if (const auto resampled = runFieldResamplingCommand(argc, argv)) return *resampled;
    if (const auto diagnosed = runDiagnosticsCommand(argc, argv)) return *diagnosed;
    if (const auto projected = runBoundaryProjectionCommand(argc, argv)) return *projected;
    if (const auto stepped = runBoundaryFluidStepCommand(argc, argv)) return *stepped;
    if (argc==2 && std::string_view(argv[1])=="--capabilities") {
      const std::string version=CONTAINER_OPENVDB_BUILD_VERSION;
      std::cout << nlohmann::json{{"schemaVersion",1},{"tool","OpenVdbTool"},
          {"openvdbAvailable",vdb::IsAvailable()},
          {"openvdbVersion",vdb::IsAvailable() && !version.empty() ? nlohmann::json(version) : nlohmann::json(nullptr)},
          {"protocols",{{"denseArray",1},{"pointAttributes",1},{"pointSelection",1},{"fileAuthoringSpec",1}}},
          {"pointTypes",{"bool","int32","int64","float","double","vec3f","vec3d","string"}},
          {"pointTraversal","leaf_origin_xyz_then_local_index"}}.dump(2) << '\n';
      return EXIT_SUCCESS;
    }
    if (argc == 1) { help(); return EXIT_FAILURE; }
    vdb::ProcessingOptions options;
    vdb::SphereOptions sphere;
    vdb::SmoothingOptions smoothing;
    vdb::ParticleRasterizationOptions particles;
    vdb::AdvectionOptions advection;
    vdb::SegmentationOptions segmentation;
    vdb::ImportOptions meshExport;
    vdb::InspectionOptions inspection;
    vdb::PointOptions points;
    vdb::PoissonOptions poisson;
    vdb::ScatterOptions scatter;
    vdb::PointAdvectionOptions pointAdvection;
    vdb::FluidProjectionOptions projection;
    vdb::PlatonicOptions platonic;
    vdb::FluidSimulationOptions simulation;
    vdb::FluidSequenceOptions sequence;
    vdb::MeshConversionOptions meshConversion;
    vdb::PointSplatOptions splat;
    vdb::FluidViscosityOptions viscosity;
    vdb::LevelSetTransportOptions levelSetTransport;
    vdb::FluidSourcesOptions sources;
    vdb::PointAttributeOptions pointAttributes;
    vdb::DenseRegion denseRegion;
    vdb::MultiResolutionOptions multiresolution;
    std::optional<uint32_t> selectedLevel;
    double sampleLevel=0;
    vdb::PointGroupSelection pointSelection;
    vdb::FileAuthoringSpecOptions fileAuthoring;
    vdb::FieldFilterOptions fieldFilter;
    auto fieldFilterOperation=vdb::FieldFilterOperation::Mean;
    vdb::FieldCompositeOptions fieldComposite;
    auto fieldCompositeOperation=vdb::FieldCompositeOperation::Sum;
    vdb::FieldValueTransformOptions fieldTransform;
    vdb::PointMaskOptions pointMask;
    vdb::TopologyOptions topology;
    auto topologyOperation=vdb::TopologyOperation::ActiveMask;
    auto topologyBoolean=vdb::TopologyBoolean::Union;
    vdb::PointFieldSamplingOptions pointFieldSampling;
    std::array<double, 3> velocity{};
    auto smoothingOperation = vdb::SmoothingOperation::Gaussian;
    double offsetDistance = 0.0;
    std::array<double, 3> samplePosition{};
    std::string command, inputA, inputB, inputC, output, gridA, gridB, inputGrid, velocityGrid, fieldGrid, domainGrid;
    std::string temperatureInput, temperatureGrid;
    std::string targetGrid;
    float importScale = 1.0f;
    bool sphereOption = false, meshOption = false, inputGridOption = false;
    bool csgOption = false, voxelOption = false, bandOption = false, gridNameOption = false;
    bool distanceOption = false, smoothingOption = false;
    bool particlesOption = false, vectorOption = false, timeOption = false;
    bool stepsOption = false, velocityGridOption = false;
    bool segmentationOption = false, meshExportOption = false, inspectionOption = false;
    bool triangleOption = false;
    bool pointsOption = false, poissonOption = false;
    bool scatterOption = false, pointAdvectionOption = false, fieldOption = false;
    bool projectionOption = false, solverOption = false;
    bool platonicOption = false, centerOption = false;
    bool simulationOption = false, fluidOption = false;
    bool sequenceOption = false;
    bool meshConversionOption = false;
    bool splatOption = false, companionDensityOption = false, viscosityOption = false;
    bool stateNameOption = false, domainGridOption = false;
    bool levelSetTransportOption = false, targetGridOption = false;
    bool sourceOption = false, ambientOption = false;
    bool pointAttributesOption = false;
    bool denseOriginOption=false,denseDimensionsOption=false,multiresolutionOption=false;
    bool groupSelectionOption=false;
    bool fileSpecOption=false, pointStringOption=false, statisticsOption=false;
    bool histogramBinsOption=false, histogramRangeOption=false;
    bool fieldFilterOption=false, filterMaskOption=false, fieldOffsetOption=false;
    bool sdfFilterOption=false, smoothingWidthOption=false;
    bool compositeOption=false, transformOption=false, fieldWorkOption=false;
    bool blendOption=false, mergeOption=false, extremaOption=false;
    bool valueAffineOption=false, valueClampOption=false, valueScopeOption=false;
    bool pointMaskOption=false, pointCountNameOption=false, pointEditLimitOption=false;
    bool topologyOption=false, topologyBooleanOption=false, topologyWorkOption=false;
    bool pointFieldSamplingOption=false;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      if (arg == "--help" || arg == "-h") { help(); return EXIT_SUCCESS; }
      if (arg == "--author-file" || arg == "--sphere" || arg == "--platonic" || arg == "--mesh" || arg == "--mesh-distance" || arg == "--union" ||
          arg == "--intersection" || arg == "--difference" || arg == "--fog" ||
          arg == "--resample" || arg == "--sample" || arg == "--offset" || arg == "--smooth" || arg == "--filter-field" ||
          arg == "--composite-fields" || arg == "--transform-field" || arg == "--point-mask" ||
          arg == "--topology-mask" || arg == "--topology-boolean" || arg == "--sample-point-attribute" ||
          arg == "--particles" || arg == "--velocity" || arg == "--advect" ||
          arg == "--segment" || arg == "--export-mesh" || arg == "--inspect" || arg == "--measure" ||
          arg == "--points" || arg == "--export-points" || arg == "--poisson" ||
          arg == "--scatter" || arg == "--advect-points" || arg == "--sample-point-velocity" ||
          arg == "--sample-point-radius" || arg == "--project" || arg == "--simulate" || arg == "--simulate-sequence" ||
          arg == "--points-sdf" || arg == "--splat-points-density" || arg == "--splat-points-velocity" ||
          arg == "--splat-points-radius" || arg == "--viscosity" ||
          arg == "--advect-sdf" || arg == "--morph-sdf" ||
          arg == "--fluid-sources" ||
          arg == "--edit-point-attributes" || arg == "--point-schema" || arg == "--point-attribute" || arg == "--point-group" ||
          arg == "--dense-export" || arg == "--dense-import" || arg == "--multires" || arg == "--multires-info" || arg == "--multires-sample" ||
          arg == "--select-points" || arg == "--delete-points" || arg == "--count-points" || arg == "--point-statistics" ||
          arg == "--sample-vector" || differentialOperation(arg).has_value()) {
        if (!command.empty()) throw std::invalid_argument("specify exactly one operation");
        command = arg;
        const bool strictPathOperands=arg=="--composite-fields" || arg=="--transform-field" || arg=="--point-mask" || arg=="--topology-mask" || arg=="--topology-boolean" || arg=="--sample-point-attribute";
        const auto operationPath=[&] {
          auto text=value(argc,argv,i,arg);
          if (strictPathOperands && (text.starts_with("--") || text=="-h"))
            throw std::invalid_argument("missing path operand for "+std::string(arg)+"; use ./ before a filename beginning with --");
          return text;
        };
        if (arg != "--sphere" && arg != "--platonic" && arg != "--velocity") inputA = operationPath();
        if (arg == "--inspect" || arg == "--measure" || arg == "--multires-info" || arg == "--count-points") continue;
        if (arg=="--point-statistics") { inputB=value(argc,argv,i,arg); continue; }
        if (arg == "--sample" || arg == "--sample-vector" || arg == "--multires-sample") {
          for (double& coordinate : samplePosition)
            coordinate = number(value(argc, argv, i, arg), arg);
          if (arg=="--multires-sample") sampleLevel=number(value(argc,argv,i,arg),arg);
          continue;
        }
        if (arg == "--union" || arg == "--intersection" || arg == "--difference" || arg == "--composite-fields" || arg == "--topology-boolean" || arg=="--sample-point-attribute" || arg == "--advect" ||
            arg == "--advect-points" || arg == "--advect-sdf" || arg == "--morph-sdf" ||
            arg == "--edit-point-attributes" || arg == "--point-attribute" || arg == "--point-group" || arg == "--sample-point-velocity" ||
            arg == "--sample-point-radius" || arg == "--project" || arg == "--simulate" || arg == "--simulate-sequence")
          inputB = operationPath();
        if (arg == "--simulate" || arg == "--simulate-sequence") inputC = value(argc, argv, i, arg);
        output = operationPath();
      } else if (arg == "--topology-operation") {
        const auto op=value(argc,argv,i,arg);
        if (op=="active-mask") topologyOperation=vdb::TopologyOperation::ActiveMask;
        else if (op=="dilate") topologyOperation=vdb::TopologyOperation::Dilate;
        else if (op=="erode") topologyOperation=vdb::TopologyOperation::Erode;
        else if (op=="open") topologyOperation=vdb::TopologyOperation::Open;
        else if (op=="close") topologyOperation=vdb::TopologyOperation::Close;
        else throw std::invalid_argument("invalid topology operation");
        topologyOption=true;
      } else if (arg == "--topology-boolean-operation") {
        const auto op=value(argc,argv,i,arg);
        if (op=="union") topologyBoolean=vdb::TopologyBoolean::Union;
        else if (op=="intersection") topologyBoolean=vdb::TopologyBoolean::Intersection;
        else if (op=="difference") topologyBoolean=vdb::TopologyBoolean::Difference;
        else throw std::invalid_argument("invalid topology Boolean operation");
        topologyBooleanOption=true;
      } else if (arg == "--topology-iterations") {
        const auto limit=count(value(argc,argv,i,arg),arg);
        if (limit>32) throw std::invalid_argument("topology iterations must be [1,32]");
        topology.iterations=static_cast<uint32_t>(limit); topologyOption=true;
      } else if (arg == "--topology-neighbors") {
        const auto neighbors=value(argc,argv,i,arg);
        if (neighbors=="6") topology.connectivity=vdb::TopologyConnectivity::Faces6;
        else if (neighbors=="18") topology.connectivity=vdb::TopologyConnectivity::FacesEdges18;
        else if (neighbors=="26") topology.connectivity=vdb::TopologyConnectivity::Full26;
        else throw std::invalid_argument("topology neighbors must be 6, 18 or 26");
        topologyOption=true;
      } else if (arg == "--topology-tiles") {
        const auto policy=value(argc,argv,i,arg);
        if (policy=="preserve") topology.tilePolicy=vdb::TopologyTilePolicy::Preserve;
        else if (policy=="expand") topology.tilePolicy=vdb::TopologyTilePolicy::Expand;
        else if (policy=="ignore") topology.tilePolicy=vdb::TopologyTilePolicy::Ignore;
        else throw std::invalid_argument("invalid topology tile policy");
        topologyOption=true;
      } else if (arg == "--max-topology-work") {
        topology.maxWorkItems=count(value(argc,argv,i,arg),arg);
        if (topology.maxWorkItems>10'000'000'000ull) throw std::invalid_argument("topology work cap exceeds 10000000000");
        topologyWorkOption=true;
      } else if (arg=="--attribute-name") {
        pointFieldSampling.attributeName=value(argc,argv,i,arg); pointFieldSamplingOption=true;
      } else if (arg=="--attribute-operation") {
        const auto op=value(argc,argv,i,arg);
        if (op=="add") pointFieldSampling.operation=vdb::PointEditOperation::Add;
        else if (op=="set") pointFieldSampling.operation=vdb::PointEditOperation::Set;
        else throw std::invalid_argument("sampled attribute operation must be add or set");
        pointFieldSamplingOption=true;
      } else if (arg=="--point-interpolation") {
        const auto mode=value(argc,argv,i,arg);
        if (mode=="nearest") pointFieldSampling.interpolation=vdb::PointFieldInterpolation::Nearest;
        else if (mode=="linear") pointFieldSampling.interpolation=vdb::PointFieldInterpolation::Linear;
        else if (mode=="quadratic") pointFieldSampling.interpolation=vdb::PointFieldInterpolation::Quadratic;
        else throw std::invalid_argument("invalid point field interpolation");
        pointFieldSamplingOption=true;
      } else if (arg=="--point-field-precision") {
        const auto precision=value(argc,argv,i,arg);
        if (precision=="promote") pointFieldSampling.precision=vdb::PointFieldPrecision::PromoteFloatingToDouble;
        else if (precision=="source") pointFieldSampling.precision=vdb::PointFieldPrecision::PreserveSource;
        else throw std::invalid_argument("point field precision must be promote or source");
        pointFieldSamplingOption=true;
      } else if (arg=="--max-point-field-work" || arg=="--max-point-field-grids" || arg=="--max-point-field-metadata") {
        const auto limit=count(value(argc,argv,i,arg),arg);
        if (arg=="--max-point-field-work") pointFieldSampling.maxWork=limit;
        else if (arg=="--max-point-field-grids") {
          if (limit>4096) throw std::invalid_argument("point field catalog cap must be [1,4096]");
          pointFieldSampling.maxGrids=static_cast<size_t>(limit);
        } else pointFieldSampling.maxMetadataBytes=limit;
        pointFieldSamplingOption=true;
      } else if (arg == "--field-operation") {
        const auto operation=value(argc,argv,i,arg);
        if (operation=="sum") fieldCompositeOperation=vdb::FieldCompositeOperation::Sum;
        else if (operation=="product") fieldCompositeOperation=vdb::FieldCompositeOperation::Product;
        else if (operation=="divide") fieldCompositeOperation=vdb::FieldCompositeOperation::Divide;
        else if (operation=="min") fieldCompositeOperation=vdb::FieldCompositeOperation::Minimum;
        else if (operation=="max") fieldCompositeOperation=vdb::FieldCompositeOperation::Maximum;
        else if (operation=="blend") fieldCompositeOperation=vdb::FieldCompositeOperation::Blend;
        else if (operation=="merge-active") fieldCompositeOperation=vdb::FieldCompositeOperation::MergeActive;
        else throw std::invalid_argument("--field-operation requires sum, product, divide, min, max, blend or merge-active");
        compositeOption=true;
      } else if (arg=="--field-activity") {
        const auto policy=value(argc,argv,i,arg);
        if (policy=="union") fieldComposite.activity=vdb::FieldActivity::Union;
        else if (policy=="intersection") fieldComposite.activity=vdb::FieldActivity::Intersection;
        else if (policy=="left") fieldComposite.activity=vdb::FieldActivity::Left;
        else if (policy=="right") fieldComposite.activity=vdb::FieldActivity::Right;
        else throw std::invalid_argument("--field-activity requires union, intersection, left or right");
        compositeOption=true;
      } else if (arg=="--blend-weight") {
        fieldComposite.blendWeight=number(value(argc,argv,i,arg),arg);
        compositeOption=true; blendOption=true;
      } else if (arg=="--merge-preference") {
        const auto policy=value(argc,argv,i,arg);
        if (policy=="left") fieldComposite.mergePreference=vdb::FieldMergePreference::Left;
        else if (policy=="right") fieldComposite.mergePreference=vdb::FieldMergePreference::Right;
        else throw std::invalid_argument("--merge-preference requires left or right");
        compositeOption=true; mergeOption=true;
      } else if (arg=="--vector-extrema") {
        const auto policy=value(argc,argv,i,arg);
        if (policy=="componentwise") fieldComposite.vectorExtrema=vdb::FieldVectorExtrema::Componentwise;
        else if (policy=="magnitude") fieldComposite.vectorExtrema=vdb::FieldVectorExtrema::ScaledMagnitude;
        else throw std::invalid_argument("--vector-extrema requires componentwise or magnitude");
        compositeOption=true; extremaOption=true;
      } else if (arg=="--value-operation") {
        const auto operation=value(argc,argv,i,arg);
        if (operation=="copy") fieldTransform.operation=vdb::FieldValueOperation::Copy;
        else if (operation=="affine") fieldTransform.operation=vdb::FieldValueOperation::Affine;
        else if (operation=="absolute") fieldTransform.operation=vdb::FieldValueOperation::Absolute;
        else if (operation=="clamp") fieldTransform.operation=vdb::FieldValueOperation::Clamp;
        else throw std::invalid_argument("--value-operation requires copy, affine, absolute or clamp");
        transformOption=true;
      } else if (arg=="--value-scope") {
        const auto scope=value(argc,argv,i,arg);
        if (scope=="active") fieldTransform.scope=vdb::FieldValueScope::ActiveOnly;
        else if (scope=="all") fieldTransform.scope=vdb::FieldValueScope::AllValues;
        else throw std::invalid_argument("--value-scope requires active or all");
        transformOption=true; valueScopeOption=true;
      } else if (arg=="--value-scale" || arg=="--value-offset" || arg=="--value-min" || arg=="--value-max") {
        auto& vector=arg=="--value-scale" ? fieldTransform.scale : arg=="--value-offset" ? fieldTransform.offset :
            arg=="--value-min" ? fieldTransform.minimum : fieldTransform.maximum;
        for (auto& component:vector) component=number(value(argc,argv,i,arg),arg);
        transformOption=true;
        if (arg=="--value-scale" || arg=="--value-offset") valueAffineOption=true;
        else valueClampOption=true;
      } else if (arg=="--max-field-work") {
        const auto limit=count(value(argc,argv,i,arg),arg);
        fieldComposite.maxWorkItems=limit; fieldTransform.maxWorkItems=limit; fieldWorkOption=true;
      } else if (arg=="--point-counts") {
        pointMask.writeCounts=true; pointMaskOption=true;
      } else if (arg=="--point-count-name") {
        pointMask.countGridName=value(argc,argv,i,arg); pointMaskOption=true; pointCountNameOption=true;
      } else if (arg == "--field-filter") {
        const auto filter=value(argc,argv,i,arg);
        if (filter=="mean") fieldFilterOperation=vdb::FieldFilterOperation::Mean;
        else if (filter=="gaussian") fieldFilterOperation=vdb::FieldFilterOperation::Gaussian;
        else if (filter=="median") fieldFilterOperation=vdb::FieldFilterOperation::Median;
        else if (filter=="offset") fieldFilterOperation=vdb::FieldFilterOperation::Offset;
        else throw std::invalid_argument("--field-filter requires mean, gaussian, median or offset");
        fieldFilterOption=true;
      } else if (arg=="--field-offset") {
        for (auto& component:fieldFilter.offset) component=number(value(argc,argv,i,arg),arg);
        fieldFilterOption=true; fieldOffsetOption=true;
      } else if (arg=="--filter-mask" || arg=="--filter-mask-grid") {
        const auto text=value(argc,argv,i,arg);
        if (arg=="--filter-mask") fieldFilter.mask=path(text);
        else fieldFilter.maskGridName=text;
        fieldFilterOption=true; filterMaskOption=true;
      } else if (arg=="--mask-min" || arg=="--mask-max") {
        (arg=="--mask-min" ? fieldFilter.maskMinimum : fieldFilter.maskMaximum)=number(value(argc,argv,i,arg),arg);
        fieldFilterOption=true; filterMaskOption=true;
      } else if (arg=="--invert-mask") {
        fieldFilter.invertMask=true; fieldFilterOption=true; filterMaskOption=true;
      } else if (arg=="--voxelize-tiles") {
        fieldFilter.voxelizeActiveTiles=true; fieldFilterOption=true;
      } else if (arg=="--max-filter-work") {
        fieldFilter.maxWorkItems=count(value(argc,argv,i,arg),arg); fieldFilterOption=true;
      } else if (arg == "--max-spec-bytes" || arg == "--max-spec-depth") {
        const auto amount=count(value(argc,argv,i,arg),arg);
        const uint64_t maximum=arg=="--max-spec-bytes" ? 64ull*1024ull*1024ull : 64ull;
        if (amount>maximum) throw std::invalid_argument(std::string(arg)+" exceeds "+std::to_string(maximum));
        if (arg=="--max-spec-bytes") fileAuthoring.maxSpecBytes=amount;
        else fileAuthoring.maxDepth=static_cast<size_t>(amount);
        fileSpecOption=true;
      } else if (arg == "--any-group" || arg == "--all-group" || arg == "--exclude-group") {
        auto& groups=arg=="--any-group" ? pointSelection.anyOf : arg=="--all-group" ? pointSelection.allOf : pointSelection.noneOf;
        groups.push_back(value(argc,argv,i,arg)); groupSelectionOption=true;
      } else if (arg == "--dense-origin") {
        for (auto& coordinate:denseRegion.origin) coordinate=indexCoordinate(value(argc,argv,i,arg),arg);
        denseOriginOption=true;
      } else if (arg == "--dense-dimensions") {
        for (auto& dimension:denseRegion.dimensions) {
          const auto amount=count(value(argc,argv,i,arg),arg);
          if (amount>UINT32_MAX) throw std::invalid_argument("dense dimensions must fit uint32");
          dimension=static_cast<uint32_t>(amount);
        }
        denseDimensionsOption=true;
      } else if (arg == "--levels" || arg == "--level" || arg == "--max-multires-work") {
        const auto amount=unsignedCount(value(argc,argv,i,arg),arg,arg=="--level");
        if (arg=="--level") {
          if (amount>7) throw std::invalid_argument("--level must be in [0,7]");
          selectedLevel=static_cast<uint32_t>(amount);
        } else if (arg=="--levels") {
          if (amount>8) throw std::invalid_argument("--levels must be in [1,8]");
          multiresolution.levels=static_cast<uint32_t>(amount);
        } else multiresolution.maxWork=amount;
        multiresolutionOption=true;
      } else if (arg == "--max-attributes" || arg == "--max-groups" || arg == "--max-edits" || arg == "--max-string-bytes") {
        const auto amount=count(value(argc,argv,i,arg),arg);
        if (arg=="--max-string-bytes") { pointAttributes.maxStringBytes=amount; pointStringOption=true; }
        else {
          const uint64_t maximum=arg=="--max-groups" ? 4096 : 1024;
          if (amount>maximum) throw std::invalid_argument(std::string(arg)+" exceeds "+std::to_string(maximum));
          auto& limit=arg=="--max-attributes" ? pointAttributes.maxAttributes : arg=="--max-groups" ? pointAttributes.maxGroups : pointAttributes.maxEdits;
          limit=static_cast<size_t>(amount);
          if (arg=="--max-attributes") fileAuthoring.maxPointAttributes=static_cast<size_t>(amount);
          else if (arg=="--max-groups") fileAuthoring.maxPointGroups=static_cast<size_t>(amount);
          else fileAuthoring.maxEdits=static_cast<size_t>(amount);
        }
        pointAttributesOption=true;
        if (arg=="--max-edits") pointEditLimitOption=true;
      } else if (arg == "--source") {
        if (sources.sources.size()>=64) throw std::invalid_argument("at most 64 source spheres are supported");
        vdb::FluidSourceSphere source;
        for (auto& coordinate:source.center) coordinate=number(value(argc,argv,i,arg),arg);
        source.radius=number(value(argc,argv,i,arg),arg);
        sources.sources.push_back(source); sourceOption=true;
      } else if (arg=="--density-rate" || arg=="--heat-rate" || arg=="--density-decay" || arg=="--temperature-relaxation" ||
                 arg=="--clear-density" || arg=="--reset-temperature") {
        if (sources.sources.empty()) throw std::invalid_argument("source rate/reset flags require a preceding --source X Y Z R");
        auto& source=sources.sources.back();
        if (arg=="--clear-density") source.clearDensity=true;
        else if (arg=="--reset-temperature") source.resetTemperature=true;
        else {
          auto& rate=arg=="--density-rate" ? source.densityRate : arg=="--heat-rate" ? source.temperatureRate :
              arg=="--density-decay" ? source.densityDecayRate : source.temperatureRelaxationRate;
          rate=number(value(argc,argv,i,arg),arg);
        }
        sourceOption=true;
      } else if (arg == "--target-grid") {
        targetGrid=value(argc,argv,i,arg); targetGridOption=true;
      } else if (arg == "--normalization-iterations" || arg == "--max-cfl-steps" || arg == "--max-transport-work") {
        const auto amount=count(value(argc,argv,i,arg),arg);
        if (arg=="--normalization-iterations") {
          if (amount>8) throw std::invalid_argument("--normalization-iterations must be in [1,8]");
          levelSetTransport.normalizationIterations=static_cast<uint32_t>(amount);
        } else if (arg=="--max-cfl-steps") {
          if (amount>4096) throw std::invalid_argument("--max-cfl-steps must be in [1,4096]");
          levelSetTransport.maxCflSteps=static_cast<uint32_t>(amount);
        } else levelSetTransport.maxWork=amount;
        levelSetTransportOption=true;
      } else if (arg == "--point-mass") {
        splat.massPerPoint=number(value(argc,argv,i,arg),arg); splatOption=true;
      } else if (arg == "--splat-density-name") {
        splat.densityGridName=value(argc,argv,i,arg); companionDensityOption=true;
      } else if (arg == "--kinematic-viscosity") {
        viscosity.kinematicViscosity=number(value(argc,argv,i,arg),arg); viscosityOption=true;
      } else if (arg == "--triangle-id-name") {
        meshConversion.triangleGridName=value(argc,argv,i,arg); meshConversionOption=true;
      } else if (arg == "--no-triangle-ids") {
        meshConversion.writeTriangleIds=false; meshConversionOption=true;
      } else if (arg == "--frames") {
        const auto frames = count(value(argc, argv, i, arg), arg);
        if (frames > 256) throw std::invalid_argument("--frames must be in [1,256]");
        sequence.frameCount = static_cast<uint32_t>(frames); sequenceOption = true;
      } else if (arg == "--max-output-bytes") {
        sequence.maxOutputBytes = count(value(argc, argv, i, arg), arg); sequenceOption = true;
      } else if (arg == "--radius") {
        sphere.radius = number(value(argc, argv, i, arg), arg); sphereOption = true;
      } else if (arg == "--center") {
        for (double& x : sphere.center) x = number(value(argc, argv, i, arg), arg);
        platonic.center = sphere.center; centerOption = true;
      } else if (arg == "--faces") {
        const auto faces = count(value(argc, argv, i, arg), arg);
        if (faces != 4 && faces != 6 && faces != 8 && faces != 12 && faces != 20)
          throw std::invalid_argument("--faces requires 4,6,8,12 or 20");
        platonic.faceCount = static_cast<uint32_t>(faces); platonicOption = true;
      } else if (arg == "--scale") {
        platonic.scale = number(value(argc, argv, i, arg), arg); platonicOption = true;
      } else if (arg == "--voxel-size") {
        options.voxelSize = number(value(argc, argv, i, arg), arg); voxelOption = true;
      } else if (arg == "--half-width") {
        options.halfWidth = number(value(argc, argv, i, arg), arg); bandOption = true;
      } else if (arg == "--import-scale") {
        const double scale = number(value(argc, argv, i, arg), arg);
        if (scale <= 0.0 || scale > std::numeric_limits<float>::max() ||
            static_cast<float>(scale) == 0.0f)
          throw std::invalid_argument("import scale must be a positive float");
        importScale = static_cast<float>(scale); meshOption = true;
      } else if (arg == "--grid-name") {
        options.gridName = value(argc, argv, i, arg);
        gridNameOption = true;
      } else if (arg == "--max-particles") {
        const auto limit = count(value(argc, argv, i, arg), arg);
        if (limit > std::numeric_limits<size_t>::max())
          throw std::invalid_argument("particle budget exceeds size_t range");
        particles.maxParticles = static_cast<size_t>(limit);
        particlesOption = true;
      } else if (arg == "--vector") {
        for (double& component : velocity) component = number(value(argc, argv, i, arg), arg);
        vectorOption = true;
      } else if (arg == "--time") {
        advection.timeStep = number(value(argc, argv, i, arg), arg);
        pointAdvection.timeStep = projection.timeStep = advection.timeStep;
        if (std::abs(advection.timeStep) > 3600.0)
          throw std::invalid_argument("--time must be in [-3600,3600]");
        timeOption = true;
      } else if (arg == "--steps") {
        const auto amount = count(value(argc, argv, i, arg), arg);
        if (amount > 64) throw std::invalid_argument("--steps must be in [1,64]");
        advection.steps = static_cast<uint32_t>(amount);
        pointAdvection.steps = advection.steps;
        stepsOption = true;
      } else if (arg == "--velocity-grid") {
        velocityGrid = value(argc, argv, i, arg);
        velocityGridOption = true;
      } else if (arg == "--max-components") {
        const auto limit = count(value(argc, argv, i, arg), arg);
        if (limit > 1024) throw std::invalid_argument("--max-components must be in [1,1024]");
        segmentation.maxComponents = static_cast<uint32_t>(limit);
        segmentationOption = true;
      } else if (arg == "--max-points") {
        const auto limit = count(value(argc, argv, i, arg), arg);
        if (limit > std::numeric_limits<size_t>::max())
          throw std::invalid_argument("point budget exceeds size_t range");
        points.maxPoints = static_cast<size_t>(limit);
        fileAuthoring.maxPoints=limit;
        pointsOption = true;
      } else if (arg == "--max-iterations") {
        const auto limit = count(value(argc, argv, i, arg), arg);
        if (limit > 10000) throw std::invalid_argument("--max-iterations must be in [1,10000]");
        poisson.maxIterations = static_cast<uint32_t>(limit);
        projection.maxIterations = poisson.maxIterations;
        solverOption = true;
      } else if (arg == "--tolerance") {
        poisson.tolerance = number(value(argc, argv, i, arg), arg);
        projection.tolerance = poisson.tolerance;
        if (poisson.tolerance < 1e-12 || poisson.tolerance > 1e-2)
          throw std::invalid_argument("--tolerance must be in [1e-12,1e-2]");
        solverOption = true;
      } else if (arg == "--point-count" || arg == "--seed") {
        const auto amount = unsignedCount(value(argc, argv, i, arg), arg, true);
        if (arg == "--point-count") scatter.pointCount = amount;
        else scatter.seed = amount;
        scatterOption = true;
      } else if (arg == "--point-radius") {
        scatter.radius = number(value(argc, argv, i, arg), arg);
        scatterOption = true;
      } else if (arg == "--integration") {
        const auto method = value(argc, argv, i, arg);
        if (method != "rk2" && method != "rk4") throw std::invalid_argument("--integration requires rk2 or rk4");
        pointAdvection.integration = method == "rk2" ? vdb::PointIntegration::RK2 : vdb::PointIntegration::RK4;
        pointAdvectionOption = true;
      } else if (arg == "--keep-velocity") {
        pointAdvection.updateVelocity = false; pointAdvectionOption = true;
      } else if (arg == "--field-grid") {
        fieldGrid = value(argc, argv, i, arg); fieldOption = true;
      } else if (arg == "--domain-grid") {
        domainGrid = value(argc, argv, i, arg); domainGridOption = true;
      } else if (arg == "--acceleration") {
        for (auto& component : projection.acceleration) component = number(value(argc, argv, i, arg), arg);
        projectionOption = true;
      } else if (arg == "--max-fluid-cells") {
        projection.maxFluidCells = count(value(argc, argv, i, arg), arg);
        if (projection.maxFluidCells > 1'000'000) throw std::invalid_argument("--max-fluid-cells must be in [1,1000000]");
        fluidOption = true;
      } else if (arg == "--velocity-name" || arg == "--pressure-name" || arg == "--domain-name") {
        auto& name = arg == "--velocity-name" ? projection.velocityGridName :
                     arg == "--pressure-name" ? projection.pressureGridName : projection.domainGridName;
        name = value(argc, argv, i, arg); fluidOption = true;
      } else if (arg == "--no-domain") {
        projection.writeDomain = false; projectionOption = true;
      } else if (arg == "--temperature-input") {
        temperatureInput = value(argc, argv, i, arg); simulationOption = true;
      } else if (arg == "--temperature-grid") {
        temperatureGrid = value(argc, argv, i, arg); simulationOption = true;
      } else if (arg == "--gravity" || arg == "--buoyancy-direction") {
        auto& vector = arg == "--gravity" ? simulation.gravity : simulation.buoyancyDirection;
        for (auto& component : vector) component = number(value(argc, argv, i, arg), arg);
        simulationOption = true;
      } else if (arg == "--ambient-temperature") {
        simulation.ambientTemperature=number(value(argc,argv,i,arg),arg); ambientOption=true;
      } else if (arg == "--cfl" ||
                 arg == "--buoyancy-density" || arg == "--buoyancy-temperature") {
        auto& parameter = arg == "--cfl" ? simulation.maxCfl :
            arg == "--buoyancy-density" ? simulation.buoyancyDensity : simulation.buoyancyTemperature;
        parameter = number(value(argc, argv, i, arg), arg); simulationOption = true;
      } else if (arg == "--max-substeps") {
        const auto amount = count(value(argc, argv, i, arg), arg);
        if (amount > 64) throw std::invalid_argument("--max-substeps must be in [1,64]");
        simulation.maxSubsteps = static_cast<uint32_t>(amount); simulationOption = true;
      } else if (arg == "--density-name" || arg == "--temperature-name") {
        auto& name = arg == "--density-name" ? simulation.densityGridName : simulation.temperatureGridName;
        name = value(argc, argv, i, arg); stateNameOption = true;
      } else if (arg == "--max-solver-voxels") {
        poisson.maxSolverVoxels = count(value(argc, argv, i, arg), arg);
        if (poisson.maxSolverVoxels > 1'000'000)
          throw std::invalid_argument("--max-solver-voxels must be in [1,1000000]");
        poissonOption = true;
      } else if (arg == "--iso-value") {
        meshExport.isoValue = number(value(argc, argv, i, arg), arg);
        meshExportOption = true;
      } else if (arg == "--adaptivity") {
        meshExport.adaptivity = number(value(argc, argv, i, arg), arg);
        if (meshExport.adaptivity < 0.0 || meshExport.adaptivity > 1.0)
          throw std::invalid_argument("--adaptivity must be in [0,1]");
        meshExportOption = true;
      } else if (arg == "--stats") {
        inspection.loadStatistics = true;
        statisticsOption=true;
        inspectionOption = true;
      } else if (arg == "--histogram-bins") {
        if (histogramBinsOption) throw std::invalid_argument("duplicate --histogram-bins");
        const auto bins = count(value(argc,argv,i,arg),arg);
        if (bins > 4096) throw std::invalid_argument("--histogram-bins must be in [1,4096]");
        inspection.histogramBins = static_cast<uint32_t>(bins);
        histogramBinsOption = statisticsOption = inspectionOption = true;
      } else if (arg == "--histogram-range") {
        if (histogramRangeOption) throw std::invalid_argument("duplicate --histogram-range");
        const double low = number(value(argc,argv,i,arg),arg);
        const double high = number(value(argc,argv,i,arg),arg);
        if (low >= high) throw std::invalid_argument("--histogram-range requires finite MIN < MAX");
        inspection.histogramRange = std::array{low,high};
        histogramRangeOption = statisticsOption = inspectionOption = true;
      } else if (arg == "--max-grids") {
        const auto limit = count(value(argc, argv, i, arg), arg);
        if (limit > 4096) throw std::invalid_argument("--max-grids must be in [1,4096]");
        inspection.maxGrids = static_cast<size_t>(limit);
        fileAuthoring.maxGrids=static_cast<size_t>(limit);
        inspectionOption = true;
      } else if (arg == "--max-metadata-bytes") {
        const auto limit = count(value(argc, argv, i, arg), arg);
        if (limit > std::numeric_limits<size_t>::max())
          throw std::invalid_argument("metadata budget exceeds size_t range");
        inspection.maxMetadataBytes = static_cast<size_t>(limit);
        fileAuthoring.maxMetadataBytes=static_cast<size_t>(limit);
        inspectionOption = true;
      } else if (arg == "--distance") {
        offsetDistance = number(value(argc, argv, i, arg), arg); distanceOption = true;
      } else if (arg == "--filter") {
        const auto filter = value(argc, argv, i, arg);
        if (filter == "gaussian") smoothingOperation = vdb::SmoothingOperation::Gaussian;
        else if (filter == "mean") smoothingOperation = vdb::SmoothingOperation::Mean;
        else if (filter == "median") smoothingOperation = vdb::SmoothingOperation::Median;
        else throw std::invalid_argument("--filter requires gaussian, mean or median");
        smoothingOption = true;
        sdfFilterOption=true;
      } else if (arg == "--width" || arg == "--iterations") {
        const auto amount = count(value(argc, argv, i, arg), arg);
        const auto maximum = arg == "--width" ? 4u : 32u;
        if (amount > maximum)
          throw std::invalid_argument(std::string(arg) + " exceeds " + std::to_string(maximum));
        if (arg == "--width") { smoothing.width = static_cast<uint32_t>(amount); smoothingWidthOption=true; }
        else smoothing.iterations = static_cast<uint32_t>(amount);
        smoothingOption = true;
      } else if (arg == "--input-grid") {
        inputGrid = value(argc, argv, i, arg); inputGridOption = true;
      } else if (arg == "--grid-a") {
        gridA = value(argc, argv, i, arg); csgOption = true;
      } else if (arg == "--grid-b") {
        gridB = value(argc, argv, i, arg); csgOption = true;
      } else if (arg == "--max-voxels") {
        options.maxVoxelCount = count(value(argc, argv, i, arg), arg);
      } else if (arg == "--max-triangles") {
        const auto limit = count(value(argc, argv, i, arg), arg);
        if (limit > std::numeric_limits<size_t>::max())
          throw std::invalid_argument("triangle budget exceeds size_t range");
        options.maxTriangles = static_cast<size_t>(limit);
        triangleOption = true;
      } else if (arg == "--max-input-bytes") {
        options.maxInputBytes = count(value(argc, argv, i, arg), arg);
      } else throw std::invalid_argument("unknown argument: " + std::string(arg));
    }
    if (command.empty()) throw std::invalid_argument("an operation is required; see --help");
    const bool samplePoints = command == "--sample-point-velocity" || command == "--sample-point-radius";
    const bool processPoints = command == "--scatter" || command == "--advect-points" || samplePoints;
    const bool splatPoints = command == "--splat-points-density" || command == "--splat-points-velocity" || command == "--splat-points-radius";
    const bool rasterizePoints = splatPoints || command == "--points-sdf";
    const bool viscosityOperation = command == "--viscosity";
    const bool sdfTransport = command == "--advect-sdf" || command == "--morph-sdf";
    const bool sourcesOperation = command == "--fluid-sources";
    const bool nativePointAttributes = command=="--edit-point-attributes" || command=="--point-schema" || command=="--point-attribute" || command=="--point-group";
    const bool denseOperation=command=="--dense-export" || command=="--dense-import";
    const bool multiresolutionOperation=command=="--multires" || command=="--multires-info" || command=="--multires-sample";
    const bool selectPointOutput=command=="--select-points" || command=="--delete-points";
    const bool pointSelectionOperation=selectPointOutput || command=="--count-points" || command=="--point-statistics";
    const bool smokeOperation = command == "--simulate" || command == "--simulate-sequence";
    const bool fluidOperation = command == "--project" || smokeOperation || viscosityOperation || sourcesOperation;
    const bool fileAuthoringOperation=command=="--author-file";
    const bool filterFieldOperation=command=="--filter-field";
    const bool compositeFieldOperation=command=="--composite-fields";
    const bool transformFieldOperation=command=="--transform-field";
    const bool pointMaskOperation=command=="--point-mask";
    const bool topologyMaskOperation=command=="--topology-mask";
    const bool topologyBooleanOperation=command=="--topology-boolean";
    const bool topologyOutput=topologyMaskOperation || topologyBooleanOperation;
    const bool sampleNativePointField=command=="--sample-point-attribute";
    if ((topologyOption && !topologyMaskOperation) ||
        (topologyBooleanOption && !topologyBooleanOperation) ||
        (topologyWorkOption && !topologyOutput) ||
        (pointFieldSamplingOption && !sampleNativePointField))
      throw std::invalid_argument("topology/point-field option is incompatible with the selected operation");
    if ((sphereOption && command != "--sphere") || (meshOption && command != "--mesh" && command != "--mesh-distance") ||
        (meshConversionOption && command != "--mesh-distance") ||
        (centerOption && command != "--sphere" && command != "--platonic") ||
        (platonicOption && command != "--platonic") ||
        (inputGridOption && command != "--fog" && command != "--resample" && command != "--sample" &&
         command != "--offset" && command != "--smooth" && command != "--filter-field" && command != "--transform-field" && command != "--point-mask" && !topologyMaskOperation && !sampleNativePointField && command != "--advect" &&
         command != "--segment" && command != "--export-mesh" && command != "--inspect" && command != "--measure" &&
         command != "--export-points" && command != "--sample-vector" &&
         command != "--poisson" && command != "--project" && command != "--dense-export" && !multiresolutionOperation && !smokeOperation && !processPoints && !rasterizePoints && !sdfTransport && !nativePointAttributes && !pointSelectionOperation && !differentialOperation(command)) ||
        (groupSelectionOption && !pointSelectionOperation && !pointMaskOperation && !sampleNativePointField) ||
        ((denseOriginOption || denseDimensionsOption) && command!="--dense-export") ||
        (multiresolutionOption && !multiresolutionOperation) || (selectedLevel && command!="--multires") ||
        (distanceOption && command != "--offset") || (smoothingOption && command != "--smooth" && !filterFieldOperation) ||
        (fieldFilterOption && !filterFieldOperation) ||
        (compositeOption && !compositeFieldOperation) || (transformOption && !transformFieldOperation) ||
        (fieldWorkOption && !compositeFieldOperation && !transformFieldOperation) ||
        (pointMaskOption && !pointMaskOperation) || (pointEditLimitOption && (pointMaskOperation || sampleNativePointField)) ||
        (sdfFilterOption && command!="--smooth") ||
        (csgOption && command != "--union" && command != "--intersection" && command != "--difference" && !compositeFieldOperation && !topologyBooleanOperation) ||
        (particlesOption && command != "--particles") || (vectorOption && command != "--velocity") ||
        (timeOption && command != "--advect" && command != "--advect-points" && !fluidOperation && !sdfTransport) ||
        (stepsOption && command != "--advect" && command != "--advect-points" && !smokeOperation) ||
        (velocityGridOption && command != "--advect" && command != "--advect-points" && command != "--advect-sdf" && !smokeOperation) ||
        (levelSetTransportOption && !sdfTransport) || (targetGridOption && command != "--morph-sdf") ||
        (segmentationOption && command != "--segment") ||
        (pointsOption && command != "--points" && command != "--export-points" && !processPoints && !rasterizePoints && !nativePointAttributes && !pointSelectionOperation && !pointMaskOperation && !fileAuthoringOperation && !sampleNativePointField) ||
        (pointAttributesOption && !nativePointAttributes && !pointSelectionOperation && !pointMaskOperation && !fileAuthoringOperation && !sampleNativePointField) ||
        (pointStringOption && fileAuthoringOperation) || (fileSpecOption && !fileAuthoringOperation) ||
        (splatOption && !splatPoints) || (companionDensityOption && (!splatPoints || command == "--splat-points-density")) ||
        (viscosityOption && !viscosityOperation && !smokeOperation) ||
        (sourceOption && !sourcesOperation && !smokeOperation) || (ambientOption && !sourcesOperation && !smokeOperation) ||
        (poissonOption && command != "--poisson") ||
        (solverOption && command != "--poisson" && command != "--project" && !smokeOperation && !viscosityOperation) ||
        (scatterOption && command != "--scatter") ||
        (pointAdvectionOption && command != "--advect-points") ||
        (fieldOption && !samplePoints && !sampleNativePointField) ||
        (projectionOption && command != "--project") ||
        (fluidOption && !fluidOperation) ||
        (domainGridOption && command != "--project" && !smokeOperation) ||
        (stateNameOption && !smokeOperation && !viscosityOperation && !sourcesOperation) ||
        (simulationOption && !smokeOperation) || (sequenceOption && command != "--simulate-sequence") ||
        (meshExportOption && command != "--export-mesh") ||
        (inspectionOption && command != "--inspect" && !fileAuthoringOperation) || (statisticsOption && fileAuthoringOperation) ||
        ((histogramBinsOption || histogramRangeOption) && (!inspection.loadStatistics || !histogramBinsOption)) ||
        (gridNameOption && (command == "--inspect" || command == "--measure" || command == "--export-mesh" ||
                            command == "--export-points" || command == "--sample-vector" || fluidOperation || denseOperation || multiresolutionOperation || fileAuthoringOperation || (pointSelectionOperation && !selectPointOutput) || (nativePointAttributes && command!="--edit-point-attributes"))) ||
        (triangleOption && (command == "--inspect" || command == "--measure" || viscosityOperation || sourcesOperation || rasterizePoints || sdfTransport || nativePointAttributes || denseOperation || multiresolutionOperation || pointSelectionOperation || pointMaskOperation || fileAuthoringOperation || filterFieldOperation || compositeFieldOperation || transformFieldOperation || topologyOutput || sampleNativePointField)) ||
        (voxelOption && command != "--sphere" && command != "--platonic" && command != "--mesh" && command != "--mesh-distance" && command != "--resample" &&
         command != "--particles" && command != "--velocity" && command != "--points" && !processPoints && !rasterizePoints) ||
        (bandOption && command != "--sphere" && command != "--platonic" && command != "--mesh" && command != "--mesh-distance" && command != "--resample" &&
         command != "--particles" && command != "--points-sdf"))
      throw std::invalid_argument("an option is incompatible with the selected operation");
    if (command == "--offset" && !distanceOption)
      throw std::invalid_argument("--offset requires --distance in world units");
    if (filterFieldOperation && filterMaskOption && fieldFilter.mask.empty())
      throw std::invalid_argument("mask options require --filter-mask");
    if (filterFieldOperation && fieldOffsetOption && fieldFilterOperation!=vdb::FieldFilterOperation::Offset)
      throw std::invalid_argument("--field-offset requires --field-filter offset");
    if (filterFieldOperation && smoothingWidthOption && fieldFilterOperation==vdb::FieldFilterOperation::Offset)
      throw std::invalid_argument("--width does not apply to field offsets");
    if (blendOption && fieldCompositeOperation!=vdb::FieldCompositeOperation::Blend)
      throw std::invalid_argument("--blend-weight requires --field-operation blend");
    if (mergeOption && fieldCompositeOperation!=vdb::FieldCompositeOperation::MergeActive)
      throw std::invalid_argument("--merge-preference requires --field-operation merge-active");
    if (compositeFieldOperation && fieldCompositeOperation==vdb::FieldCompositeOperation::MergeActive && fieldComposite.activity!=vdb::FieldActivity::Union)
      throw std::invalid_argument("merge-active requires --field-activity union");
    if (extremaOption && fieldCompositeOperation!=vdb::FieldCompositeOperation::Minimum && fieldCompositeOperation!=vdb::FieldCompositeOperation::Maximum)
      throw std::invalid_argument("--vector-extrema requires --field-operation min or max");
    if (valueAffineOption && fieldTransform.operation!=vdb::FieldValueOperation::Affine)
      throw std::invalid_argument("--value-scale/--value-offset require --value-operation affine");
    if (valueClampOption && fieldTransform.operation!=vdb::FieldValueOperation::Clamp)
      throw std::invalid_argument("--value-min/--value-max require --value-operation clamp");
    if (valueScopeOption && fieldTransform.operation==vdb::FieldValueOperation::Copy)
      throw std::invalid_argument("--value-scope requires a numeric value operation; copy always retains the whole field");
    if (pointCountNameOption && !pointMask.writeCounts)
      throw std::invalid_argument("--point-count-name requires --point-counts");
    if (command=="--dense-export" && (!denseOriginOption || !denseDimensionsOption))
      throw std::invalid_argument("--dense-export requires both --dense-origin and --dense-dimensions");
    if ((command == "--advect" || command == "--advect-points" || sdfTransport) && !timeOption)
      throw std::invalid_argument(command + " requires --time in seconds");
    if (command == "--velocity" && !vectorOption)
      throw std::invalid_argument("--velocity requires --vector in world units per second");
    if (smokeOperation && !temperatureGrid.empty() && temperatureInput.empty())
      throw std::invalid_argument("--temperature-grid requires --temperature-input");
    if (command == "--velocity" && !gridNameOption) options.gridName = "velocity";
    if (command == "--mesh-distance" && !gridNameOption) options.gridName = "distance";
    if ((command == "--points" || processPoints) && !gridNameOption) options.gridName = "points";
    if (command == "--edit-point-attributes" && !gridNameOption) options.gridName = "points";
    if (selectPointOutput && !gridNameOption) options.gridName="points";
    if (sampleNativePointField && !gridNameOption) options.gridName="points";
    if (topologyOutput && !gridNameOption) options.gridName="mask";
    if (pointMaskOperation && !gridNameOption) options.gridName="point_mask";
    if ((compositeFieldOperation || transformFieldOperation) && !gridNameOption) options.gridName="field";
    if (command == "--poisson" && !gridNameOption) options.gridName = "solution";
    if (differentialOperation(command) && !gridNameOption) options.gridName = command.substr(2);
    if (rasterizePoints && !gridNameOption) options.gridName = command == "--points-sdf" ? "surface" :
        command == "--splat-points-density" ? "point_density" : command == "--splat-points-velocity" ? "point_velocity" : "point_radius";
    if (topologyOutput) {
      const auto result=topologyMaskOperation ?
          vdb::WriteTopologyMask(path(inputA),path(output),topologyOperation,topology,inputGrid,options) :
          vdb::WriteTopologyBoolean(path(inputA),path(inputB),path(output),topologyBoolean,gridA,gridB,options,topology.maxWorkItems);
      std::cout << nlohmann::json{{"gridName",result.grid.gridName},{"activeVoxels",result.grid.activeVoxelCount},
          {"inputActiveVoxelsA",result.inputActiveVoxelCountA},{"inputActiveVoxelsB",result.inputActiveVoxelCountB},
          {"activeTiles",result.activeTileCount},{"storedLeaves",result.storedLeafCount},
          {"workItems",result.workItems},{"diagnostics",result.diagnostics}}
          .dump(2,' ',false,nlohmann::json::error_handler_t::replace) << '\n';
      return EXIT_SUCCESS;
    }
    if (sampleNativePointField) {
      pointAttributes.maxPoints=points.maxPoints; pointFieldSampling.groups=pointSelection;
      const auto result=vdb::WriteSampledPointAttribute(path(inputA),path(inputB),path(output),pointFieldSampling,inputGrid,fieldGrid,options,pointAttributes);
      std::cout << nlohmann::json{{"inputCount",result.inputCount},{"selectedCount",result.selectedCount},
          {"writtenCount",result.writtenCount},{"attribute",pointFieldSampling.attributeName},
          {"fieldGridName",result.fieldGridName},{"fieldValueType",result.fieldValueType},
          {"vectorSemantics",result.vectorSemantics}}
          .dump(2,' ',false,nlohmann::json::error_handler_t::replace) << '\n';
      return EXIT_SUCCESS;
    }
    if (compositeFieldOperation || transformFieldOperation) {
      const auto result=compositeFieldOperation ?
          vdb::WriteCompositeField(path(inputA),path(inputB),path(output),fieldCompositeOperation,fieldComposite,gridA,gridB,options) :
          vdb::WriteTransformedField(path(inputA),path(output),fieldTransform,inputGrid,options);
      std::cout << nlohmann::json{{"gridName",result.grid.gridName},{"gridClass",result.grid.gridClass},
          {"activeVoxels",result.grid.activeVoxelCount},{"voxelSize",result.grid.voxelSize},
          {"inputActiveVoxelsA",result.inputActiveVoxelCountA},{"inputActiveVoxelsB",result.inputActiveVoxelCountB},
          {"storedLeaves",result.storedLeafCount},{"workItems",result.workItems},{"diagnostics",result.diagnostics}}
          .dump(2,' ',false,nlohmann::json::error_handler_t::replace) << '\n';
      return EXIT_SUCCESS;
    }
    if (pointMaskOperation) {
      pointAttributes.maxPoints=points.maxPoints;
      const auto result=vdb::WritePointVoxelMask(path(inputA),path(output),pointSelection,pointMask,inputGrid,options,pointAttributes);
      std::cout << nlohmann::json{{"inputCount",result.inputCount},{"selectedCount",result.selectedCount},
          {"selectedActive",result.selectedActive},{"selectedInactive",result.selectedInactive},
          {"occupiedVoxels",result.occupiedVoxelCount},{"leaves",result.leafCount},
          {"maskGridName",result.maskGridName},{"countGridName",result.countGridName},
          {"sourceArchivedSchema",result.sourceArchivedSchema}}
          .dump(2,' ',false,nlohmann::json::error_handler_t::replace) << '\n';
      return EXIT_SUCCESS;
    }
    if (filterFieldOperation) {
      fieldFilter.width=smoothing.width; fieldFilter.iterations=smoothing.iterations;
      const auto filtered=vdb::WriteFilteredField(path(inputA),path(output),fieldFilterOperation,fieldFilter,inputGrid,options);
      std::cout << nlohmann::json{{"gridName",filtered.grid.gridName},{"activeVoxels",filtered.grid.activeVoxelCount},
          {"inputActiveVoxels",filtered.inputActiveVoxelCount},{"activeTiles",filtered.activeTileCount},
          {"voxelizedTiles",filtered.voxelizedTileCount},{"storedLeaves",filtered.storedLeafCount},
          {"kernelPasses",filtered.kernelPasses},{"maskUsed",filtered.maskUsed},{"diagnostics",filtered.diagnostics}}
          .dump(2,' ',false,nlohmann::json::error_handler_t::replace) << '\n';
      return EXIT_SUCCESS;
    }
    if (fileAuthoringOperation) {
      const auto result=vdb::WriteAuthoredVdbSpec(path(inputA),path(output),fileAuthoring,options);
      std::cout << nlohmann::json{{"inputGrids",result.inputGridCount},{"outputGrids",result.outputGridCount},
          {"removedGrids",result.removedGridCount},{"renamedGrids",result.renamedGridCount},
          {"metadataEdits",result.metadataEditCount},{"inputBytes",result.inputBytes},{"outputBytes",result.outputBytes},
          {"activeVoxels",result.activeVoxelCount},{"points",result.pointCount},{"gridNames",result.gridNames},
          {"diagnostics",result.diagnostics}}.dump(2,' ',false,nlohmann::json::error_handler_t::replace) << '\n';
      return EXIT_SUCCESS;
    }
    if (pointSelectionOperation) {
      pointAttributes.maxPoints=points.maxPoints;
      nlohmann::json report;
      if (selectPointOutput) {
        const auto result=vdb::WriteSelectedPoints(path(inputA),path(output),pointSelection,
            command=="--select-points" ? vdb::PointSelectionMode::KeepSelected : vdb::PointSelectionMode::DeleteSelected,
            inputGrid,options,pointAttributes);
        report={{"inputCount",result.inputCount},{"selectedCount",result.selectedCount},{"outputCount",result.outputCount},
            {"archivedSchema",result.archivedSchema},{"gridName",result.schema.gridName}};
      } else if (command=="--count-points") {
        const auto result=vdb::CountSelectedPoints(path(inputA),pointSelection,inputGrid,options,pointAttributes);
        report={{"total",result.total},{"selected",result.selected},{"selectedActive",result.selectedActive},
            {"selectedInactive",result.selectedInactive},{"archivedSchema",result.archivedSchema},{"gridName",result.schema.gridName}};
      } else {
        const auto result=vdb::MeasurePointAttribute(path(inputA),inputB,pointSelection,inputGrid,options,pointAttributes);
        const auto extreme=[](const auto& variant) {
          return std::visit([](const auto& values)->nlohmann::json {
            if (values.empty()) return nullptr;
            if constexpr (std::is_same_v<typename std::decay_t<decltype(values)>::value_type,uint8_t>) return values[0]!=0;
            else return values[0];
          },variant);
        };
        auto mean=nlohmann::json::array(),deviation=nlohmann::json::array();
        for (uint32_t component=0;component<result.componentCount;++component) {
          mean.push_back(result.mean[component]); deviation.push_back(result.populationStdDev[component]);
        }
        report={{"selectedCount",result.selectedCount},{"hasValues",result.hasValues},{"componentCount",result.componentCount},
            {"valueType",result.attribute.valueType},{"minimum",extreme(result.minimum)},{"maximum",extreme(result.maximum)},
            {"mean",std::move(mean)},{"populationStdDev",std::move(deviation)}};
      }
      std::cout << report.dump(2,' ',false,nlohmann::json::error_handler_t::replace) << '\n';
      return EXIT_SUCCESS;
    }
    if (denseOperation) {
      const auto volume=command=="--dense-export" ? vdb::ReadDenseVdb(path(inputA),denseRegion,inputGrid,options)
          : vdb::ReadDenseArrayFile(path(inputA),options);
      if (command=="--dense-export") vdb::WriteDenseArrayFile(path(output),volume,options);
      else (void)vdb::WriteDenseVdb(path(output),volume,options);
      constexpr std::array<const char*,4> types{"float","double","vec3f","vec3d"};
      const nlohmann::json report{{"gridName",volume.gridName},{"origin",volume.region.origin},
          {"dimensions",volume.region.dimensions},{"valueType",types[volume.values.index()]},
          {"activeVoxelsOutside",volume.diagnostics.activeVoxelsOutside},
          {"storedNonBackgroundOutside",volume.diagnostics.storedNonBackgroundOutside},
          {"omittedMetadata",volume.diagnostics.omittedMetadata}};
      std::cout << report.dump(2,' ',false,nlohmann::json::error_handler_t::replace) << '\n';
      return EXIT_SUCCESS;
    }
    if (multiresolutionOperation) {
      const auto volume=vdb::BuildMultiResolution(path(inputA),multiresolution,inputGrid,options);
      if (command=="--multires-sample") {
        std::visit([](const auto& value) { std::cout << nlohmann::json(value).dump() << '\n'; },volume->sampleWorld(samplePosition,sampleLevel));
      } else {
        if (command=="--multires") {
          if (selectedLevel) (void)volume->writeLevel(path(output),*selectedLevel,options);
          else volume->writeAll(path(output),options);
        }
        auto levels=nlohmann::json::array();
        for (const auto& level:volume->levels()) levels.push_back({{"level",level.level},{"gridName",level.gridName},
            {"valueType",level.valueType},{"activeVoxels",level.activeVoxelCount},{"leaves",level.leafCount},{"voxelSize",level.voxelSize}});
        std::cout << levels.dump(2,' ',false,nlohmann::json::error_handler_t::replace) << '\n';
      }
      return EXIT_SUCCESS;
    }
    if (nativePointAttributes) {
      pointAttributes.maxPoints=points.maxPoints;
      if (command=="--edit-point-attributes") {
        const auto result=vdb::ApplyPointAttributeEditsFile(path(inputA),path(inputB),path(output),inputGrid,options,pointAttributes);
        std::cout << nlohmann::json{{"gridName",result.gridName},{"points",result.pointCount},
            {"attributes",result.attributes.size()},{"groups",result.groups.size()}}.dump(2) << '\n';
      } else if (command=="--point-schema") {
        const auto result=vdb::ExportPointAttributeSchema(path(inputA),path(output),inputGrid,options,pointAttributes);
        std::cout << "Exported schema for " << result.pointCount << " native points\n";
      } else if (command=="--point-attribute") vdb::ExportPointAttribute(path(inputA),inputB,path(output),inputGrid,options,pointAttributes);
      else vdb::ExportPointGroup(path(inputA),inputB,path(output),inputGrid,options,pointAttributes);
      return EXIT_SUCCESS;
    }
    if (sdfTransport) {
      levelSetTransport.timeStep=advection.timeStep;
      const auto result=command == "--advect-sdf"
          ? vdb::WriteAdvectedLevelSet(path(inputA),path(inputB),path(output),levelSetTransport,inputGrid,velocityGrid,options)
          : vdb::WriteMorphedLevelSet(path(inputA),path(inputB),path(output),levelSetTransport,inputGrid,targetGrid,options);
      std::cout << nlohmann::json{{"gridName",result.levelSet.gridName},{"activeVoxels",result.levelSet.activeVoxelCount},
          {"voxelSize",result.levelSet.voxelSize},{"cflSteps",result.cflSteps},{"work",result.work}}.dump(2) << '\n';
      return EXIT_SUCCESS;
    }
    if (sourcesOperation) {
      sources.timeStep=projection.timeStep; sources.ambientTemperature=simulation.ambientTemperature;
      sources.maxFluidCells=projection.maxFluidCells;
      sources.densityGridName=simulation.densityGridName; sources.temperatureGridName=simulation.temperatureGridName;
      sources.velocityGridName=projection.velocityGridName; sources.pressureGridName=projection.pressureGridName;
      sources.domainGridName=projection.domainGridName;
      const auto result=vdb::WriteFluidSources(path(inputA),path(output),sources,options);
      std::cout << nlohmann::json{{"fluidCells",result.fluidCellCount},{"affectedCells",result.affectedCellCount},
          {"changedCells",result.changedCellCount},{"clearedDensityCells",result.clearedDensityCells},
          {"resetTemperatureCells",result.resetTemperatureCells},{"densityMassBefore",result.densityMassBefore},
          {"densityMassAfter",result.densityMassAfter},{"temperatureIntegralBefore",result.temperatureIntegralBefore},
          {"temperatureIntegralAfter",result.temperatureIntegralAfter},{"converged",result.converged}}.dump(2) << '\n';
      return EXIT_SUCCESS;
    }
    if (rasterizePoints) {
      if (command == "--points-sdf") {
        const auto result=vdb::WritePointsLevelSet(path(inputA),path(output),inputGrid,options,points);
        std::cout << nlohmann::json{{"gridName",result.gridName},{"activeVoxels",result.activeVoxelCount},
            {"voxelSize",result.voxelSize}}.dump(2) << '\n';
      } else {
        const auto mode=command == "--splat-points-density" ? vdb::PointSplatField::Density :
            command == "--splat-points-velocity" ? vdb::PointSplatField::Velocity : vdb::PointSplatField::Radius;
        const auto result=vdb::WritePointsSplat(path(inputA),path(output),mode,inputGrid,options,splat,points);
        std::cout << nlohmann::json{{"gridName",result.field.gridName},{"densityGrid",result.densityGridName},
            {"activeVoxels",result.field.activeVoxelCount},{"points",result.pointCount},
            {"totalMass",result.totalMass},{"depositedMass",result.depositedMass}}.dump(2) << '\n';
      }
      return EXIT_SUCCESS;
    }
    if (viscosityOperation) {
      viscosity.timeStep=projection.timeStep; viscosity.tolerance=projection.tolerance;
      viscosity.maxIterations=projection.maxIterations; viscosity.maxFluidCells=projection.maxFluidCells;
      viscosity.densityGridName=simulation.densityGridName; viscosity.temperatureGridName=simulation.temperatureGridName;
      viscosity.velocityGridName=projection.velocityGridName; viscosity.pressureGridName=projection.pressureGridName;
      viscosity.domainGridName=projection.domainGridName;
      const auto result=vdb::WriteViscousFluidState(path(inputA),path(output),viscosity,options);
      std::cout << nlohmann::json{{"fluidCells",result.fluidCellCount},{"faces",result.faceCount},
          {"diffusionIterations",result.diffusionIterations},{"projectionIterations",result.projectionIterations},
          {"diffusionResidual",result.diffusionResidual},{"pressureResidual",result.pressureResidual},
          {"maxDivergenceBefore",result.maxDivergenceBefore},{"maxDivergenceAfter",result.maxDivergenceAfter},
          {"divergenceRoundoffBound",result.divergenceRoundoffBound},{"roundoffLimited",result.roundoffLimited},
          {"kineticEnergyBefore",result.kineticEnergyBefore},{"kineticEnergyAfterDiffusion",result.kineticEnergyAfterDiffusion},
          {"kineticEnergyAfterProjection",result.kineticEnergyAfterProjection},
          {"skipped",result.skipped},{"converged",result.converged}}.dump(2) << '\n';
      return EXIT_SUCCESS;
    }
    if (command == "--mesh-distance") {
      const auto result=vdb::VoxelizeModelDistance(path(inputA),path(output),importScale,options,meshConversion);
      std::cout << nlohmann::json{{"distanceGrid",result.distance.gridName},
          {"triangleGrid",result.triangleGridName},{"triangles",result.triangleCount},
          {"activeVoxels",result.distance.activeVoxelCount},{"voxelSize",result.distance.voxelSize}}.dump(2) << '\n';
      return EXIT_SUCCESS;
    }
    if (command == "--measure") {
      const auto result = vdb::MeasureLevelSet(path(inputA), inputGrid, options);
      std::cout << nlohmann::json{{"gridName", result.gridName}, {"activeVoxels", result.activeVoxelCount},
          {"voxelSize", result.voxelSize}, {"area", result.area}, {"volume", result.volume},
          {"averageMeanCurvature", result.averageMeanCurvature},
          {"totalGaussianCurvature", result.totalGaussianCurvature}, {"eulerEstimate", result.eulerEstimate},
          {"estimatedEulerCharacteristic", result.estimatedEulerCharacteristic},
          {"eulerRoundingError", result.eulerRoundingError}, {"topologyDiagnostic", result.topologyDiagnostic}}.dump(2) << '\n';
      return EXIT_SUCCESS;
    }
    if (processPoints) {
      vdb::PointResult result;
      if (command == "--scatter") result = vdb::ScatterPoints(path(inputA), path(output), inputGrid, scatter, options, points);
      else if (command == "--advect-points") result = vdb::WriteAdvectedPoints(path(inputA), path(inputB), path(output),
          inputGrid, velocityGrid, pointAdvection, options, points);
      else result = vdb::WriteSampledPoints(path(inputA), path(inputB), path(output), command == "--sample-point-radius"
          ? vdb::PointAttribute::Radius : vdb::PointAttribute::Velocity, inputGrid, fieldGrid, options, points);
      std::cout << "Wrote " << output << ": " << result.gridName << ", " << result.pointCount << " points\n";
      return EXIT_SUCCESS;
    }
    if (command == "--project") {
      const auto result = vdb::WriteProjectedVelocity(path(inputA), path(inputB), path(output), projection,
          inputGrid, domainGrid, options);
      std::cout << nlohmann::json{{"fluidCells", result.fluidCellCount}, {"faces", result.faceCount},
          {"components", result.componentCount}, {"iterations", result.iterations},
          {"maxDivergenceBefore", result.maxDivergenceBefore}, {"maxDivergenceAfter", result.maxDivergenceAfter},
          {"divergenceRoundoffBound", result.divergenceRoundoffBound}, {"roundoffLimited", result.roundoffLimited},
          {"residual", result.residual}, {"converged", result.converged},
          {"velocityGrid", result.velocityGridName}, {"pressureGrid", result.pressureGridName},
          {"domainGrid", result.domainGridName}}.dump(2) << '\n';
      return EXIT_SUCCESS;
    }
    if (smokeOperation) {
      if (viscosityOption) simulation.kinematicViscosity=viscosity.kinematicViscosity;
      simulation.sources=sources.sources;
      simulation.timeStep = projection.timeStep; simulation.substeps = advection.steps;
      simulation.maxIterations = projection.maxIterations; simulation.tolerance = projection.tolerance;
      simulation.maxFluidCells = projection.maxFluidCells;
      simulation.velocityGridName = projection.velocityGridName;
      simulation.pressureGridName = projection.pressureGridName;
      simulation.domainGridName = projection.domainGridName;
      const vdb::FluidSimulationInputs inputs{
          .density = path(inputA), .velocity = path(inputB), .domain = path(inputC),
          .temperature = path(temperatureInput), .densityGrid = inputGrid,
          .velocityGrid = velocityGrid, .domainGrid = domainGrid, .temperatureGrid = temperatureGrid};
      if (command == "--simulate-sequence") {
        const auto result = vdb::WriteFluidSequence(inputs, path(output), sequence, simulation, options);
        nlohmann::json frames=nlohmann::json::array();
        for (const auto& frame:result.frames) {
          const auto utf8=frame.path.u8string();
          frames.push_back({{"path",std::string(utf8.begin(),utf8.end())},{"timeSeconds",frame.time},
              {"actualSubsteps",frame.simulation.actualSubsteps},{"converged",frame.simulation.converged},
              {"viscositySubsteps",frame.simulation.viscositySubsteps},{"sourceSubsteps",frame.simulation.sourceSubsteps}});
        }
        const auto utf8=result.manifest.u8string();
        std::cout << nlohmann::json{{"manifest",std::string(utf8.begin(),utf8.end())},
            {"encodedBytes",result.encodedBytes},{"frames",std::move(frames)}}.dump(2) << '\n';
        return EXIT_SUCCESS;
      }
      const auto result = vdb::WriteFluidStep(inputs, path(output), simulation, options);
      std::cout << nlohmann::json{{"fluidCells", result.fluidCellCount}, {"clippedTraces", result.clippedTraceCount},
          {"requestedSubsteps", result.requestedSubsteps}, {"actualSubsteps", result.actualSubsteps},
          {"simulatedTime", result.simulatedTime}, {"projectionIterations", result.maxProjectionIterations},
          {"maxDivergenceBefore", result.maxDivergenceBefore}, {"maxDivergenceAfter", result.maxDivergenceAfter},
          {"divergenceRoundoffBound", result.divergenceRoundoffBound}, {"roundoffLimited", result.roundoffLimited},
          {"residual", result.residual}, {"converged", result.converged},
          {"maxWorldSpeedBefore", result.maxWorldSpeedBefore}, {"maxWorldSpeedAfter", result.maxWorldSpeedAfter},
          {"densityMassBefore", result.densityMassBefore}, {"densityMassAfter", result.densityMassAfter},
          {"temperatureIntegralBefore", result.temperatureIntegralBefore},
          {"temperatureIntegralAfter", result.temperatureIntegralAfter},
          {"viscositySubsteps",result.viscositySubsteps},{"sourceSubsteps",result.sourceSubsteps},
          {"maxDiffusionIterations",result.maxDiffusionIterations},{"maxDiffusionResidual",result.maxDiffusionResidual},
          {"viscosityEnergyDissipated",result.viscosityEnergyDissipated},
          {"sourceAffectedCellApplications",result.sourceAffectedCellApplications},
          {"sourceChangedCellApplications",result.sourceChangedCellApplications},
          {"sourceDensityMassChange",result.sourceDensityMassChange},
          {"sourceTemperatureIntegralChange",result.sourceTemperatureIntegralChange}}.dump(2) << '\n';
      return EXIT_SUCCESS;
    }
    if (command == "--sample") {
      std::cout << std::setprecision(std::numeric_limits<double>::max_digits10)
                << vdb::SampleScalar(path(inputA), samplePosition, inputGrid, options) << '\n';
      return EXIT_SUCCESS;
    }
    if (command == "--sample-vector") {
      std::cout << nlohmann::json(vdb::SampleVector(path(inputA), samplePosition, inputGrid, options)).dump() << '\n';
      return EXIT_SUCCESS;
    }
    if (command == "--points" || command == "--export-points") {
      const auto pointResult = command == "--points"
          ? vdb::WritePointsFile(path(inputA), path(output), options, points)
          : vdb::ExportPoints(path(inputA), path(output), inputGrid, options, points);
      std::cout << "Wrote " << output << ": " << pointResult.gridName << ", "
                << pointResult.pointCount << " points\n";
      return EXIT_SUCCESS;
    }
    if (command == "--poisson") {
      const auto solution = vdb::WritePoissonSolution(path(inputA), path(output), inputGrid, poisson, options);
      std::cout << "Wrote " << output << ": " << solution.grid.gridName << ", "
                << solution.iterations << " iterations, residual " << std::setprecision(12)
                << solution.residual << '\n';
      return EXIT_SUCCESS;
    }
    if (const auto operation = differentialOperation(command)) {
      const auto field = vdb::WriteDifferential(path(inputA), path(output), *operation, inputGrid, options);
      std::cout << "Wrote " << output << ": " << field.gridName << ", "
                << field.activeVoxelCount << " active voxels\n";
      return EXIT_SUCCESS;
    }
    if (command == "--segment") {
      const auto components = vdb::WriteSegmentedLevelSet(
          path(inputA), path(output), inputGrid, options, segmentation);
      std::cout << "Wrote " << output << ": " << components.size() << " components\n";
      for (const auto& component : components)
        std::cout << "  " << component.gridName << ": " << component.activeVoxelCount
                  << " active voxels, voxel size " << component.voxelSize << '\n';
      return EXIT_SUCCESS;
    }
    if (command == "--export-mesh") {
      meshExport.gridName = inputGrid;
      meshExport.maxActiveVoxels = options.maxVoxelCount;
      meshExport.maxTriangles = options.maxTriangles;
      meshExport.maxInputBytes = options.maxInputBytes;
      const auto mesh = vdb::WriteObjSurface(path(inputA), path(output), meshExport);
      std::cout << "Wrote " << output << ": " << mesh.gridCount << " grids, "
                << mesh.vertexCount << " vertices, " << mesh.triangleCount << " triangles\n";
      for (const auto& diagnostic : mesh.diagnostics) std::cerr << diagnostic << '\n';
      return EXIT_SUCCESS;
    }
    if (command == "--inspect") {
      inspection.gridName = inputGrid;
      inspection.maxInputBytes = options.maxInputBytes;
      inspection.maxActiveVoxels = options.maxVoxelCount;
      printInspection(vdb::InspectFile(path(inputA), inspection));
      return EXIT_SUCCESS;
    }
    vdb::ProcessingResult result;
    if (command == "--sphere") result = vdb::WriteSphere(path(output), sphere, options);
    else if (command == "--platonic") result = vdb::WritePlatonicLevelSet(path(output), platonic, options);
    else if (command == "--particles")
      result = vdb::WriteParticlesLevelSetFile(path(inputA), path(output), options, particles);
    else if (command == "--velocity")
      result = vdb::WriteConstantVelocity(path(output), velocity, options);
    else if (command == "--advect")
      result = vdb::WriteAdvectedFog(path(inputA), path(inputB), path(output), advection,
                                    inputGrid, velocityGrid, options);
    else if (command == "--mesh")
      result = vdb::VoxelizeModel(path(inputA), path(output), importScale, options);
    else if (command == "--fog")
      result = vdb::WriteFog(path(inputA), path(output), inputGrid, options);
    else if (command == "--resample")
      result = vdb::WriteResampled(path(inputA), path(output), inputGrid, options);
    else if (command == "--offset")
      result = vdb::WriteOffset(path(inputA), path(output), offsetDistance, inputGrid, options);
    else if (command == "--smooth")
      result = vdb::WriteSmoothed(path(inputA), path(output), smoothingOperation,
                                smoothing, inputGrid, options);
    else {
      auto operation = vdb::CsgOperation::Union;
      if (command == "--intersection") operation = vdb::CsgOperation::Intersection;
      if (command == "--difference") operation = vdb::CsgOperation::Difference;
      result = vdb::WriteCsg(path(inputA), path(inputB), path(output), operation, gridA, gridB, options);
    }
    std::cout << "Wrote " << output << ": " << result.gridName << " ("
              << result.gridClass << "), " << result.activeVoxelCount
              << " active voxels, voxel size " << result.voxelSize << '\n';
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}

#ifdef _WIN32
// The narrow Windows CRT argv uses the active ANSI code page. All tool paths
// and JSON text use UTF-8, so retain the original Unicode command line instead.
int wmain(int argc, wchar_t** wideArguments) {
  try {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<size_t>(argc));
    for (int i=0;i<argc;++i) {
      const auto utf8=std::filesystem::path(wideArguments[i]).u8string();
      arguments.emplace_back(utf8.begin(),utf8.end());
    }
    std::vector<char*> pointers;
    pointers.reserve(arguments.size()+1);
    for (auto& argument:arguments) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return runOpenVdbTool(argc,pointers.data());
  } catch (const std::exception& error) {
    std::cerr << "OpenVdbTool command line: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
#else
int main(int argc, char** argv) { return runOpenVdbTool(argc,argv); }
#endif
