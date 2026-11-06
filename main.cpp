
#include "Container/app/AppConfig.h"
#include "Container/app/Application.h"
#include "Container/geometry/OpenVdbLoader.h"
#include "Container/renderer/lighting/DdgiPreset.h"
#include "Container/renderer/effects/DisplayGradePreset.h"
#include "Container/renderer/lighting/SkyPreset.h"
#include "Container/renderer/pathtracing/ReferenceFireflyClamp.h"
#include "Container/utility/Platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::string lowerAscii(std::string_view value) {
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return result;
}

bool isAuxiliaryRenderModelPath(std::string_view path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos) {
    return false;
  }

  const std::string extension = lowerAscii(path.substr(dot));
  return extension == ".bim" || extension == ".ifc" || extension == ".ifcx" ||
         extension == ".usd" || extension == ".usda" ||
         extension == ".usdc" || extension == ".usdz" || extension == ".vdb";
}

std::string_view requireValue(int argc, char** argv, int& index,
                              std::string_view option) {
  if (index + 1 >= argc || argv[index + 1] == nullptr) {
    throw std::runtime_error("missing value for " + std::string(option));
  }
  ++index;
  return argv[index];
}

float parseFloat(std::string_view value, std::string_view option) {
  try {
    size_t consumed = 0;
    const float result = std::stof(std::string(value), &consumed);
    if (consumed != value.size() || !std::isfinite(result))
      throw std::invalid_argument("finite float required");
    return result;
  } catch (...) {
    throw std::runtime_error("invalid float for " + std::string(option) +
                             ": " + std::string(value));
  }
}

uint32_t parseUint(std::string_view value, std::string_view option) {
  uint32_t result{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw std::runtime_error("invalid integer for " + std::string(option) +
                             ": " + std::string(value));
  }
  return result;
}

double parseDouble(std::string_view value, std::string_view option) {
  try {
    size_t consumed=0; const double result=std::stod(std::string(value),&consumed);
    if (consumed!=value.size() || !std::isfinite(result)) throw std::invalid_argument("finite double required");
    return result;
  } catch (...) { throw std::runtime_error("invalid number for "+std::string(option)+": "+std::string(value)); }
}

uint64_t parseUint64(std::string_view value, std::string_view option) {
  uint64_t result{};
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw std::runtime_error("invalid integer for " + std::string(option) +
                             ": " + std::string(value));
  }
  return result;
}

void previewName(std::string_view value,std::string_view option) {
  if(value.empty() || value.size()>256 || value.find('\0')!=std::string_view::npos)
    throw std::invalid_argument(std::string(option)+" requires1..256 UTF-8 bytes without NUL");
}

int64_t parsePreviewInt64(std::string_view value,std::string_view option) {
  int64_t result{};
  const auto [end,error]=std::from_chars(value.data(),value.data()+value.size(),result);
  if(value.empty() || error!=std::errc{} || end!=value.data()+value.size())
    throw std::invalid_argument("invalid signed 64-bit integer for "+std::string(option)+": "+std::string(value));
  return result;
}

struct VdbPreviewFlags {
  bool vectorRoute=false,vectorFields=false,sliceRoute=false,sliceFields=false;
  bool nativeRoute=false,nativeFields=false,legacyColorFields=false;
  bool nativeConstant=false,nativeRadiusAttribute=false,nativeColorAttribute=false,nativeTint=false;
  bool pointLimit=false,bytesLimit=false,voxelLimit=false,metadataLimit=false,triangleLimit=false;
  bool sliceFloatRange=false,sliceIntegerRange=false;
  size_t metadataBytes=1024*1024;
};

void finalizeVdbPreview(container::geometry::vdb::ImportOptions& o,const VdbPreviewFlags& f) {
  if(unsigned(f.vectorRoute)+unsigned(f.sliceRoute)+unsigned(f.nativeRoute)>1)
    throw std::invalid_argument("OpenVDB vector, slice and native-point preview routes are mutually exclusive");
  if(f.vectorFields && !f.vectorRoute)throw std::invalid_argument("vector preview settings require --vdb-vector-mode");
  if(f.sliceFields && !f.sliceRoute)throw std::invalid_argument("slice settings require --vdb-slice");
  if(f.nativeFields && !f.nativeRoute)throw std::invalid_argument("native point settings require --vdb-native-points");
  if(f.legacyColorFields && (f.sliceRoute || f.nativeRoute))
    throw std::invalid_argument("legacy point/vector color settings do not apply to slice/native previews; use their explicit color controls");
  if(f.pointLimit && (f.sliceRoute || f.vectorRoute))
    throw std::invalid_argument("--vdb-point-max-points applies only to standard or native point previews");
  if(f.metadataLimit && f.vectorRoute)
    throw std::invalid_argument("--vdb-preview-max-metadata-bytes applies to standard/native points or slices");
  if(!o.maxTriangles || !o.maxActiveVoxels || !o.maxInputBytes)
    throw std::invalid_argument("OpenVDB model triangle/topology/memory limits must be positive");
  for(const auto color:o.pointPreview.uniformColor)if(color<0 || color>1)
    throw std::invalid_argument("--vdb-preview-tint components must be in [0,1]");
  if(o.pointPreview.velocityScale<=0)throw std::invalid_argument("--vdb-preview-speed must be positive");
  if(f.bytesLimit)o.pointPreview.maxInputBytes=o.maxInputBytes;
  if(f.voxelLimit)o.pointPreview.maxActiveVoxels=o.maxActiveVoxels;
  if(f.triangleLimit)o.pointPreview.maxTriangles=o.maxTriangles;
  if(f.metadataLimit)o.pointPreview.maxMetadataBytes=f.metadataBytes;
  if(o.vectorPreview) {
    auto& preview=*o.vectorPreview;
    preview.colorMode=o.pointPreview.colorMode;preview.uniformColor=o.pointPreview.uniformColor;preview.velocityScale=o.pointPreview.velocityScale;
    if(f.bytesLimit)preview.maxInputBytes=o.maxInputBytes;if(f.voxelLimit)preview.maxActiveVoxels=o.maxActiveVoxels;
    if(f.triangleLimit)preview.maxTriangles=o.maxTriangles;
    uint64_t seeds=1;for(const auto count:preview.seedCounts){if(count<1 || count>256)throw std::invalid_argument("vector seed axes require [1,256]");seeds*=count;}
    if(seeds>4096)throw std::invalid_argument("vector seed product exceeds4096");
    for(const double value:{preview.displaySeconds,preview.maxGlyphLength,preview.worldRadius,preview.worldStep})
      if(value<1e-6 || value>1e6)throw std::invalid_argument("vector seconds/length/radius/step require [1e-6,1e6]");
    if(preview.maxSteps<1 || preview.maxSteps>4096 || !preview.maxWork || preview.maxWork>1'000'000'000)
      throw std::invalid_argument("vector steps/work exceed supported positive limits");
    if(preview.hasBounds)for(size_t k=0;k<3;++k)if(preview.boundsMin[k]>=preview.boundsMax[k])
      throw std::invalid_argument("vector bounds require ordered minima/maxima");
  }
  if(o.slicePreview) {
    auto& preview=*o.slicePreview;
    if(f.sliceFloatRange && f.sliceIntegerRange)
      throw std::invalid_argument("choose --vdb-slice-range OR --vdb-slice-integer-range");
    if(preview.integerRange && ((*preview.integerRange)[0]>=(*preview.integerRange)[1] ||
        preview.valueMode!=container::geometry::vdb::SliceValue::Scalar))
      throw std::invalid_argument("slice integer range requires exact minimum < maximum and scalar mode");
    if(f.bytesLimit)preview.maxInputBytes=o.maxInputBytes;if(f.voxelLimit)preview.maxActiveVoxels=o.maxActiveVoxels;
    if(f.triangleLimit)preview.maxTriangles=o.maxTriangles;if(f.metadataLimit)preview.maxMetadataBytes=f.metadataBytes;
    if(preview.opacity<0 || preview.opacity>1 || !(preview.colorMinimum<preview.colorMaximum) ||
       !std::isfinite(preview.colorMaximum-preview.colorMinimum))throw std::invalid_argument("slice requires opacity[0,1] and finite ordered color range/span");
    for(const auto value:preview.origin)if(std::abs(value)>1e12)throw std::invalid_argument("slice origin exceeds1e12 world units");
    const double u=std::hypot(preview.axisU[0],preview.axisU[1],preview.axisU[2]),v=std::hypot(preview.axisV[0],preview.axisV[1],preview.axisV[2]);
    if(!std::isfinite(u) || !std::isfinite(v) || u==0 || v==0)throw std::invalid_argument("slice U/V must be finite nonzero directions");
    std::array<double,3> a{},b{};for(size_t k=0;k<3;++k){a[k]=preview.axisU[k]/u;b[k]=preview.axisV[k]/v;}
    if(std::hypot(a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])<1e-6)
      throw std::invalid_argument("slice U/V must be nonparallel and sufficiently conditioned");
    for(const auto extent:preview.worldExtent)if(extent<1e-6 || extent>1e6)throw std::invalid_argument("slice extents require [1e-6,1e6] world units");
    for(const auto samples:preview.sampleCounts)if(samples<2 || samples>1024)throw std::invalid_argument("slice sample axes require [2,1024]");
    const uint64_t samples=uint64_t(preview.sampleCounts[0])*preview.sampleCounts[1],triangles=2ull*(preview.sampleCounts[0]-1)*(preview.sampleCounts[1]-1);
    if(!preview.maxSamples || preview.maxSamples>262144 || samples>preview.maxSamples || triangles>std::min(preview.maxTriangles,o.maxTriangles))
      throw std::invalid_argument("slice sample/triangle product exceeds limits");
    if(!preview.maxWork || preview.maxWork>1'000'000'000 || samples*32+triangles>preview.maxWork)
      throw std::invalid_argument("slice sampling/geometry work exceeds limits");
  }
  if(o.nativePointPreview) {
    auto& preview=*o.nativePointPreview;const auto& limits=o.nativePointLimits;
    if(f.metadataLimit)preview.maxMetadataBytes=f.metadataBytes;
    if(f.nativeConstant && f.nativeRadiusAttribute)throw std::invalid_argument("choose --vdb-native-radius OR --vdb-native-radius-attribute");
    if(preview.radiusAttribute=="P" || preview.radiusScale<=0 || (preview.radiusAttribute.empty() &&
       (preview.constantRadius<=0 || !std::isfinite(preview.constantRadius*preview.radiusScale) || preview.constantRadius*preview.radiusScale==0)))
      throw std::invalid_argument("native radius must be positive finite without underflow; P cannot be a scalar radius");
    if(preview.opacity<0 || preview.opacity>1)throw std::invalid_argument("native point opacity requires [0,1]");
    for(const auto color:preview.uniformColor)if(color<0 || color>1)throw std::invalid_argument("native point tint requires [0,1]");
    const bool uniform=preview.colorMode==container::geometry::vdb::NativePointColor::Uniform;
    if(uniform && f.nativeColorAttribute)throw std::invalid_argument("uniform native colors do not use --vdb-native-color-attribute");
    if(!uniform && preview.colorAttribute.empty())throw std::invalid_argument("nonuniform native colors require --vdb-native-color-attribute");
    if(!uniform && f.nativeTint)throw std::invalid_argument("--vdb-native-tint requires uniform native color");
    if(!limits.maxAttributes || limits.maxAttributes>1024 || !limits.maxGroups || limits.maxGroups>4096 || !limits.maxStringBytes ||
       !preview.maxGrids || preview.maxGrids>4096)throw std::invalid_argument("native attribute/group/string/catalog limits are outside supported ranges");
    if(preview.groups.anyOf.size()+preview.groups.allOf.size()+preview.groups.noneOf.size()>3*limits.maxGroups)
      throw std::invalid_argument("native point group predicate exceeds the shared group-name cap");
  }
}

std::array<float, 3> parseVec3(int argc, char** argv, int& index,
                               std::string_view option) {
  std::array<float, 3> result{};
  for (float& channel : result) {
    channel = parseFloat(requireValue(argc, argv, index, option), option);
  }
  return result;
}

glm::uvec3 parseCounts(int argc, char** argv, int& index,
                       std::string_view option) {
  glm::uvec3 result{};
  for (int axis = 0; axis < 3; ++axis)
    result[axis] = parseUint(requireValue(argc, argv, index, option), option);
  return result;
}

void printCommandLineHelp() {
  std::println(R"(Usage: VulkanSceneRenderer [model] [options]
  --model PATH                  glTF model or auxiliary BIM/IFC/USD/VDB model
  --bim-model PATH              Additional BIM/IFC/USD/VDB model
  --import-scale F              Model import scale
  --bim-import-scale F          Auxiliary model import scale
  --vdb-model-grid NAME         Scalar/point/vector model grid selector
  --vdb-slice                   Explicit sampled scalar/vector plane preview
  --vdb-slice-origin X Y Z      Source-world plane center (0 0 0)
  --vdb-slice-u X Y Z           Plane U direction (1 0 0)
  --vdb-slice-v X Y Z           Plane V direction (0 1 0)
  --vdb-slice-extent U V        Full source-world side lengths (2 2)
  --vdb-slice-samples U V       Samples per side [2,1024] (64 64)
  --vdb-slice-value MODE        scalar, x, y, z or magnitude (scalar)
  --vdb-slice-map MODE          gray, blue-red or diverging (blue-red)
  --vdb-slice-range MIN MAX     Explicit ordered color range (0 1)
  --vdb-slice-integer-range MIN MAX  Exact Int64 scalar range (default automatic)
  --vdb-slice-opacity F         Constant opacity [0,1] (1)
  --vdb-slice-max-samples N     Sample cap [1,262144] (262144)
  --vdb-slice-max-work N        Sampling/validation work cap (4000000)
  --vdb-native-points           Explicit selected native point marker preview
  --vdb-native-radius F         Constant source-world radius (0.05)
  --vdb-native-radius-attribute NAME  Positive native scalar radius, instead of constant
  --vdb-native-radius-scale F   Positive radius multiplier (1)
  --vdb-native-color MODE       uniform, scalar, direction or string (uniform)
  --vdb-native-color-attribute NAME  Required for nonuniform native color
  --vdb-native-tint R G B       Uniform RGB [0,1]; uniform native color only
  --vdb-native-opacity F        Constant native marker opacity [0,1] (1)
  --vdb-native-any-group NAME   Repeat: union group predicate
  --vdb-native-all-group NAME   Repeat: intersection group predicate
  --vdb-native-exclude-group NAME  Repeat: exclude group predicate
  --vdb-native-max-attributes N Native schema attribute cap [1,1024] (64)
  --vdb-native-max-groups N     Native group cap [1,4096] (256)
  --vdb-native-max-string-bytes N  Native dictionary byte cap (1048576)
  --vdb-native-max-grids N      Native catalog cap [1,4096] (256)
  --vdb-point-max-points N      Standard/native point cap (1000000)
  --vdb-vector-mode MODE        Explicit vector-only glyphs or streamlines
  --vdb-vector-bounds X Y Z X Y Z  Source-world preview bounds (required for constant fields)
  --vdb-vector-seeds X Y Z      Centered seed counts, each [1,256], product <=4096
  --vdb-vector-seconds F        Glyph displacement time (0.1)
  --vdb-vector-length F         Maximum glyph length (1; shortening is reported)
  --vdb-preview-radius F        Source-world glyph/tube radius (0.01)
  --vdb-vector-step F           Source-world streamline step (0.05)
  --vdb-vector-steps N          Streamline step cap [1,4096] (128)
  --vdb-vector-domain NAME      Same-file fluid mask for wall clipping
  --vdb-preview-color MODE      uniform, direction or magnitude (direction)
  --vdb-preview-tint R G B      Uniform preview color, each [0,1]
  --vdb-preview-speed F         Magnitude color scale in world units/second (1)
  --vdb-preview-max-triangles N Shared model triangle cap (2000000)
  --vdb-preview-max-voxels N    Shared model stored topology cap (8000000)
  --vdb-preview-max-bytes N     Shared model logical memory cap (536870912)
  --vdb-preview-max-metadata-bytes N  Standard/native point or slice property cap
  --vdb-preview-max-work N      Vector work cap (2000000)
                                Vector, slice and native-point routes are exclusive.
                                Route-specific options require an explicit route.
  --vdb-volume PATH             Density volume; repeat for up to four media
  --vdb-sequence DIR            Animate naturally ordered .vdb files in a directory
  --vdb-fps F                   Sequence frame rate [0.01,240] (24)
  --vdb-speed F                 Sequence speed [0,16] (1)
  --vdb-start-frame N           Initial zero-based sequence frame (0)
  --vdb-cache-mib N             Sequence CPU cache [16,4096] MiB (512)
  --vdb-async                   Load and stage sequence frames asynchronously
  --vdb-paused                  Start sequence playback paused
  --vdb-no-loop                 Hold the final sequence frame
  --vdb-sparse                  Preserve native sparse voxels using NanoVDB
  --no-vdb-shadows              Disable opaque geometry shadows in the medium
  --vdb-grid NAME               Density grid (default: density, then first scalar)
  --vdb-temperature-grid NAME   Optional scalar temperature grid for fire emission
  --vdb-temperature-scale F     Convert temperature values to Kelvin [0,100000] (1)
  --vdb-resolution N            Longest density texture axis [16,256] (128)
  --vdb-density F               Extinction multiplier [0,10000] (1)
  --vdb-scattering R G B        Scattering tint, each channel [0,1] (white)
  --vdb-anisotropy F            Phase asymmetry [-0.95,0.95] (0)
  --vdb-multiple-scattering F   Higher-order scattering preview [0,1] (0)
  --vdb-scattering-orders N     Additional scattering orders [1,4] (2)
  --vdb-emission R G B          Emission color, each channel [0,10000] (white)
  --vdb-emission-scale F        Emission per density [0,10000] (0)
  --vdb-step-scale F            Ray step multiplier [0.25,4] (1)
  --no-vdb-volume               Keep the loaded volume hidden
  --vdb-volume-scale F          Positive world-space volume scale (1)
  --width N --height N          Window dimensions
  --render-technique NAME       Render technique
  --path-spp N                 Reference samples per frame [1,64] (1)
  --path-target-ms F           Adapt samples to a reference GPU pass budget [0.1,1000] ms
  --path-samples N             Reference sample limit [1,16777216] (65536)
  --path-sample-series N,N,...  Export raw EXR at increasing reference sample counts
  --path-bounces N             Reference bounce limit [1,32] (8)
  --path-seed N                Deterministic reference sampling seed (1)
  --path-firefly-clamp F       Peak RGB limit per reference sample (0 disables)
  --path-ground MODE           Analytic ground off|surface|shadow-matte (off)
  --path-ground-height F       Ground Y position in world units (0)
  --path-ground-color R G B    Linear ground base color [0,1] (0.65)
  --path-ground-metallic F     Ground metallic factor [0,1] (0)
  --path-ground-roughness F    Ground roughness [0,1] (1)
  --path-aperture F            Perspective lens aperture radius in scene units (0)
  --path-focus-distance F      Perspective focal plane distance in scene units (10)
  --display-mode NAME           Display mode (--render-mode is an alias)
  --msaa N                     MSAA sample count
  --taa / --no-taa              Enable/disable temporal antialiasing
  --taa-history-weight F        History blend [0,0.98]
  --taa-depth-absolute F         Absolute depth tolerance [0,1000]
  --taa-depth-relative F         Relative depth tolerance [0,1]
  --taa-variance-gamma F         Variance gamma [0.5,4]
  --taa-jitter-seed N            Jitter sequence seed
  --taa-reset-frame N            Reset history at a capture frame
  --ray-shadows raster|hard|soft Ray shadow mode
  --ray-shadow-samples N         Area samples [1,32]
  --ray-shadow-light-budget N    Local light budget [0,8]
  --ray-shadow-debug-layer N     Debug view [0,10]
  --no-ray-shadow-denoise        Disable ray shadow denoising
  --no-ray-query                Disable optional ray queries
  --area-lighting sampled|ltc    Area light integration mode
  --area-light-samples 9|25|64    Sampled area light count
  --area-shadow-quality N        Raster area shadow quality [0,3]
  --area-emitter-debug           Display area emitters
  --ddgi / --no-ddgi             Enable/disable diffuse probe GI (default off)
  --ddgi-pause                  Pause updates while retaining frame budgets
  --ddgi-preset PATH             Load this scene's saved configuration
                                Explicit DDGI options override the preset
  --ddgi-save-preset PATH        Save configuration after startup scene fitting
  --ddgi-thin-glass / --no-ddgi-thin-glass
  --ddgi-dielectric / --no-ddgi-dielectric
                                Refract rough/solid glass in secondary GI rays
  --ddgi-camera-follow / --no-ddgi-camera-follow
                                Scroll the primary grid with the camera
  --ddgi-async / --no-ddgi-async  Use an available second compute queue
  --ddgi-detail-node NAME        Fit a detail volume to a named scene node/element
  --ddgi-detail-probes N         Maximum new detail probes, >=27 (512)
                                Enable/disable supported thin-glass transmission
  --ddgi-fit-scene              Fit probes and trace settings once to scene bounds
  --ddgi-fit-probes N           Maximum fitted probes, >=27 (2048); requires fit
  --ddgi-debug VIEW             off|irradiance|visibility|classification|relocation
  --ddgi-probe-radius F         Diagnostic sphere radius in world units (0.06)
  --ddgi-origin X Y Z            First world-space probe (-1.5,-0.1,-1.5)
  --ddgi-spacing X Y Z           Positive world-space spacing (0.5,0.5,0.5)
  --ddgi-counts X Y Z            Positive lattice counts (7,5,7)
  --ddgi-rays N                  Rays per updated probe [1,4096] (128)
  --ddgi-probes-per-frame N      Maximum updated probes (128); 0 pauses
  --ddgi-ray-budget N            Maximum rays per frame (16384); 0 pauses
  --ddgi-max-distance F          Ray distance (0,1000000] world units (20)
  --ddgi-hysteresis F            Previous-history weight [0,0.99] (0.95)
  --ddgi-normal-bias F           Normal offset [0,max-distance] (0.03)
  --ddgi-view-bias F             View offset [0,max-distance] (0.01)
  --ddgi-visibility-bias F       Distance-test bias [0,max-distance] (0.05)
  --ddgi-intensity F             Indirect light multiplier [0,10000] (1)
  --ddgi-memory-mb N             Positive probe payload cap in MiB (64)
  --ddgi-surface-memory-mb N     Positive hit-surface payload cap in MiB (128)
                                Fit origin/spacing/counts to each scene's bounds
  --camera-position X Y Z        Override world-space camera position
  --camera-target X Y Z          Override world-space camera target
  --camera-fov F                 Camera vertical field of view in degrees
  --exposure F                   Manual exposure
  --tone-map NAME                aces, reinhard, clip, filmic, uncharted, agx, or neutral
  --saturation F                 Display saturation (0 to 2)
  --contrast F                   Display contrast (0.5 to 2)
  --white-balance K              Illuminant temperature (2000 to 15000)
  --tint F                       Illuminant Duv (-0.025 to 0.025)
  --vibrance F                   Display vibrance (-1 to 1)
  --shadow-tone R G B            Display-linear shadow multipliers (0 to 1)
  --highlight-tone R G B         Display-linear highlight multipliers (0 to 1)
  --display-grade FILE           Load grading preset (later grading flags override it)
  --save-display-grade FILE      Save the resolved grading preset and exit
  --environment-intensity F      Environment intensity
  --environment FILE             Equirectangular OpenEXR or Radiance HDR environment
  --sky plain|gradient|atmosphere Generate a procedural environment
  --gltf-sky                       Preview optional OMI sky metadata in the selected glTF scene
  --sky-preset FILE                Load a sky preset (later sky flags override it)
  --save-sky-preset FILE           Save the resolved sky preset and exit
  --sky-horizon R G B             Scene-linear horizon or plain sky color (0 to 1)
  --sky-zenith R G B              Scene-linear upper sky color (0 to 1)
  --sky-ground R G B              Scene-linear lower sky color (0 to 1)
  --sky-sun-azimuth DEGREES        Atmosphere solar azimuth from +Z toward +X
  --sky-sun-elevation DEGREES      Atmosphere solar elevation above the horizon
  --sky-solar F                    Solar irradiance (0 to 64)
  --sky-multiple-scattering       Include higher-order scattering in atmosphere mode
  --sky-no-direct-sun              Keep independent scene directional lighting
  --sky-location LAT LON          Enable geolocated sun (north/east degrees)
  --sky-date YYYY-MM-DD            Local calendar date
  --sky-time HH:MM                 Local clock time, 24-hour format
  --sky-utc-offset MINUTES         Local time minus UTC (-720 to 840)
  --sky-planet-km F                Planet radius in kilometres (1000 to 10000)
  --sky-atmosphere-km F            Atmosphere thickness (40 to 200)
  --sky-observer-km F              Observer height (0.002 to atmosphere thickness)
  --sky-aerial-scale F             Aerial perspective metres per world unit (0 disables)
  --sky-rayleigh F --sky-mie F --sky-ozone F
                                  Density multipliers (0 to 4)
  --environment-yaw DEGREES       Rotate sky and lighting around world +Y (-180 to 180)
  --background-blur F             Visible sky blur (0 to 1)
  --background-color R G B        Solid scene-linear background, independent of lighting
  --directional-intensity F      Directional light intensity
  --directional-direction X Y Z  Directional light direction
  --directional-color R G B      Directional light color
  --bloom / --no-bloom            Enable/disable bloom
  --screenshot PATH             Capture window PNG or scene-linear EXR
  --screenshot-viewport         Omit editor overlays from PNG capture
  --save-scene PATH              Save primary editable scene as .glb or .gltf
  --gltf-variant N               Select material variant N (zero based; -1 Default)
  --gltf-scene N                 Select glTF scene N (zero based; authored default otherwise)
  --experimental-gltf-linked-assets Enable the pinned glTF 2.1 URI-only linked asset profile
  --animation-clip N             Select and play glTF animation N (zero based)
  --animation-time F             Seek selected glTF animation in seconds
  --animation-speed F            glTF playback multiplier (negative reverses)
  --animation-paused             Hold the configured glTF animation pose
  --gltf-camera-node N           Use the authored camera on source node N
  --capture-sequence PATH        Capture temporal sequence
  --warmup-frames N              Capture warmup frames
  --capture-frame N              Screenshot frame
  --fixed-dt F                   Capture timestep in seconds
  --gfxrecon-session PATH        GFXReconstruct session
  --validation / --no-validation Enable/disable Vulkan validation
  --no-ui                       Disable GUI
  --gpu-transforms              Compute scene hierarchy matrices on the GPU
  --max-scene-objects N         Primary scene object capacity (1 to 1000000)
  --physics-demo                Add and simulate a floor and falling rigid bodies
  --asl-pose PATH               Replay canonical ASL observations in image XY
                                F8 pause/resume, F9 restart (nonmetric Z retained)
  --asl-pose-speed F            Playback speed [0,16], 0 holds the start frame
  --asl-pose-start-ms N         Offset from first source observation (0)
  --asl-pose-no-loop            Hold the last observation at the end
  --asl-hand-kinematics         Fit bounded FK/IK hands; F10 compares raw/fitted
  --asl-hand-aspect-ratio F     Source width/height [0.125,8] (1, square assumed)
  --asl-hand-angular-velocity F Experimental angular bound rad/s [0,100] (12)
  --asl-hand-report PATH        Write derived fit diagnostics to a new JSON file
  --hidden                      Hide window
  --help                        Show this help without starting the renderer)");
}

void applyCommandLine(container::app::AppConfig& config, int argc,
                      char** argv, std::filesystem::path& displayGradeOutput,
                      std::filesystem::path& skyPresetOutput) {
  bool positionalModelConsumed = false;
  bool explicitBimModel = false;
  bool explicitBimImportScale = false;
  bool explicitDdgiFitField = false;
  bool explicitDdgiFitBudget = false;
  bool explicitDdgiDetailBudget = false;
  bool explicitFrameCapture = false;
  bool explicitPathSampleLimit = false;
  container::geometry::vdb::ImportOptions vdbModelOptions;
  bool vdbModelOptionsRequested=false;
  VdbPreviewFlags previewFlags;
  const auto vectorPreview=[&]() -> container::geometry::vdb::VectorPreviewOptions& {
    if (!vdbModelOptions.vectorPreview) vdbModelOptions.vectorPreview.emplace();
    return *vdbModelOptions.vectorPreview;
  };
  const auto slicePreview=[&]() -> container::geometry::vdb::SlicePreviewOptions& {
    if(!vdbModelOptions.slicePreview)vdbModelOptions.slicePreview.emplace();
    return *vdbModelOptions.slicePreview;
  };
  const auto nativePreview=[&]() -> container::geometry::vdb::NativePointPreviewOptions& {
    if(!vdbModelOptions.nativePointPreview)vdbModelOptions.nativePointPreview.emplace();
    return *vdbModelOptions.nativePointPreview;
  };
  const auto parseExtendedPreviewOption=[&](std::string_view arg,int& i) {
    if(arg=="--vdb-slice") {
      previewFlags.sliceRoute=true;(void)slicePreview();
    } else if(arg.starts_with("--vdb-slice-")) {
      auto& preview=slicePreview();previewFlags.sliceFields=true;
      if(arg=="--vdb-slice-origin" || arg=="--vdb-slice-u" || arg=="--vdb-slice-v") {
        auto& values=arg=="--vdb-slice-origin"?preview.origin:arg=="--vdb-slice-u"?preview.axisU:preview.axisV;
        for(auto& value:values)value=parseDouble(requireValue(argc,argv,i,arg),arg);
      } else if(arg=="--vdb-slice-extent") {
        for(auto& value:preview.worldExtent)value=parseDouble(requireValue(argc,argv,i,arg),arg);
      } else if(arg=="--vdb-slice-samples") {
        for(auto& value:preview.sampleCounts)value=parseUint(requireValue(argc,argv,i,arg),arg);
      } else if(arg=="--vdb-slice-value") {
        const auto value=requireValue(argc,argv,i,arg);using Value=container::geometry::vdb::SliceValue;
        if(value=="scalar")preview.valueMode=Value::Scalar;else if(value=="x")preview.valueMode=Value::VectorX;
        else if(value=="y")preview.valueMode=Value::VectorY;else if(value=="z")preview.valueMode=Value::VectorZ;
        else if(value=="magnitude")preview.valueMode=Value::VectorMagnitude;
        else throw std::invalid_argument("--vdb-slice-value requires scalar, x, y, z or magnitude");
      } else if(arg=="--vdb-slice-map") {
        const auto value=requireValue(argc,argv,i,arg);using Map=container::geometry::vdb::SliceColorMap;
        if(value=="gray")preview.colorMap=Map::Gray;else if(value=="blue-red")preview.colorMap=Map::BlueRed;
        else if(value=="diverging")preview.colorMap=Map::Diverging;else throw std::invalid_argument("--vdb-slice-map requires gray, blue-red or diverging");
      } else if(arg=="--vdb-slice-range") {
        previewFlags.sliceFloatRange=true;
        preview.colorMinimum=parseDouble(requireValue(argc,argv,i,arg),arg);preview.colorMaximum=parseDouble(requireValue(argc,argv,i,arg),arg);
      } else if(arg=="--vdb-slice-integer-range") {
        if(previewFlags.sliceIntegerRange)throw std::invalid_argument("duplicate --vdb-slice-integer-range");
        previewFlags.sliceIntegerRange=true;std::array<int64_t,2> range;
        for(auto& bound:range)bound=parsePreviewInt64(requireValue(argc,argv,i,arg),arg);
        preview.integerRange=range;
      } else if(arg=="--vdb-slice-opacity")preview.opacity=parseFloat(requireValue(argc,argv,i,arg),arg);
      else if(arg=="--vdb-slice-max-samples")preview.maxSamples=parseUint(requireValue(argc,argv,i,arg),arg);
      else if(arg=="--vdb-slice-max-work")preview.maxWork=parseUint64(requireValue(argc,argv,i,arg),arg);
      else return false;
    } else if(arg=="--vdb-native-points") {
      previewFlags.nativeRoute=true;(void)nativePreview();
    } else if(arg.starts_with("--vdb-native-")) {
      auto& preview=nativePreview();previewFlags.nativeFields=true;
      if(arg=="--vdb-native-radius" || arg=="--vdb-native-radius-scale") {
        const auto value=parseDouble(requireValue(argc,argv,i,arg),arg);
        if(arg=="--vdb-native-radius"){preview.constantRadius=value;previewFlags.nativeConstant=true;}else preview.radiusScale=value;
      } else if(arg=="--vdb-native-radius-attribute" || arg=="--vdb-native-color-attribute") {
        const auto value=requireValue(argc,argv,i,arg);previewName(value,arg);
        if(arg=="--vdb-native-radius-attribute"){preview.radiusAttribute=value;previewFlags.nativeRadiusAttribute=true;}
        else {preview.colorAttribute=value;previewFlags.nativeColorAttribute=true;}
      } else if(arg=="--vdb-native-color") {
        const auto value=requireValue(argc,argv,i,arg);using Color=container::geometry::vdb::NativePointColor;
        if(value=="uniform")preview.colorMode=Color::Uniform;else if(value=="scalar")preview.colorMode=Color::ScalarRamp;
        else if(value=="direction")preview.colorMode=Color::VectorDirection;else if(value=="string")preview.colorMode=Color::StringPalette;
        else throw std::invalid_argument("--vdb-native-color requires uniform, scalar, direction or string");
      } else if(arg=="--vdb-native-tint") {preview.uniformColor=parseVec3(argc,argv,i,arg);previewFlags.nativeTint=true;}
      else if(arg=="--vdb-native-opacity")preview.opacity=parseFloat(requireValue(argc,argv,i,arg),arg);
      else if(arg=="--vdb-native-any-group" || arg=="--vdb-native-all-group" || arg=="--vdb-native-exclude-group") {
        const auto value=requireValue(argc,argv,i,arg);previewName(value,arg);
        auto& names=arg=="--vdb-native-any-group"?preview.groups.anyOf:arg=="--vdb-native-all-group"?preview.groups.allOf:preview.groups.noneOf;
        if(names.size()>=4096)throw std::invalid_argument("native point CLI group-name list exceeds4096 entries");names.emplace_back(value);
      } else if(arg=="--vdb-native-max-attributes" || arg=="--vdb-native-max-groups" || arg=="--vdb-native-max-string-bytes" || arg=="--vdb-native-max-grids") {
        const auto value=parseUint64(requireValue(argc,argv,i,arg),arg);
        if(!value || value>std::numeric_limits<size_t>::max())throw std::invalid_argument("native point limits must be positive and fit size_t");
        if(arg=="--vdb-native-max-attributes")vdbModelOptions.nativePointLimits.maxAttributes=size_t(value);
        else if(arg=="--vdb-native-max-groups")vdbModelOptions.nativePointLimits.maxGroups=size_t(value);
        else if(arg=="--vdb-native-max-string-bytes")vdbModelOptions.nativePointLimits.maxStringBytes=size_t(value);
        else preview.maxGrids=size_t(value);
      } else return false;
    } else if(arg=="--vdb-point-max-points") {
      const auto value=parseUint64(requireValue(argc,argv,i,arg),arg);
      if(!value || value>uint64_t(INT64_MAX) || value>std::numeric_limits<size_t>::max())throw std::invalid_argument("--vdb-point-max-points requires1..INT64_MAX points fitting size_t");
      vdbModelOptions.pointPreview.maxPoints=size_t(value);vdbModelOptions.nativePointLimits.maxPoints=value;previewFlags.pointLimit=true;
    } else if(arg=="--vdb-preview-max-bytes" || arg=="--vdb-preview-max-voxels" || arg=="--vdb-preview-max-metadata-bytes") {
      const auto value=parseUint64(requireValue(argc,argv,i,arg),arg);if(!value)throw std::invalid_argument("OpenVDB preview limits must be positive");
      if(arg=="--vdb-preview-max-bytes"){vdbModelOptions.maxInputBytes=value;previewFlags.bytesLimit=true;}
      else if(arg=="--vdb-preview-max-voxels"){vdbModelOptions.maxActiveVoxels=value;previewFlags.voxelLimit=true;}
      else {if(value>std::numeric_limits<size_t>::max())throw std::invalid_argument("preview metadata cap must fit size_t");previewFlags.metadataBytes=size_t(value);previewFlags.metadataLimit=true;}
    } else return false;
    vdbModelOptionsRequested=true;return true;
  };
  std::vector<std::function<void(container::renderer::DdgiSettings &)>>
      ddgiOverrides;
  const auto rememberDdgi = [&](auto accessor) {
    const auto value = accessor(config.ddgi);
    ddgiOverrides.emplace_back([accessor, value](auto &settings) {
      accessor(settings) = value;
    });
  };
  // Keep option dispatch below MSVC's syntactic nesting limit as independent
  // feature groups grow. Consumed values still advance the shared argv index.
  const auto parseVolumeOption = [&](std::string_view arg, int& i) {
    if (arg == "--vdb-model-grid") {
      vdbModelOptions.gridName=requireValue(argc,argv,i,arg); vdbModelOptionsRequested=true;
    } else if (arg == "--vdb-vector-mode") {
      const auto mode=requireValue(argc,argv,i,arg);
      if (mode!="glyphs" && mode!="streamlines") throw std::invalid_argument("--vdb-vector-mode requires glyphs or streamlines");
      vectorPreview().mode=mode=="glyphs" ? container::geometry::vdb::VectorPreviewMode::Glyphs : container::geometry::vdb::VectorPreviewMode::Streamlines;
      previewFlags.vectorRoute=true;
      vdbModelOptionsRequested=true;
    } else if (arg == "--vdb-vector-bounds") {
      previewFlags.vectorFields=true;
      auto& preview=vectorPreview(); preview.hasBounds=true;
      for (auto* bounds:{&preview.boundsMin,&preview.boundsMax})
        for (double& value:*bounds) value=parseDouble(requireValue(argc,argv,i,arg),arg);
      vdbModelOptionsRequested=true;
    } else if (arg == "--vdb-vector-seeds") {
      previewFlags.vectorFields=true;
      for (auto& value:vectorPreview().seedCounts) value=parseUint(requireValue(argc,argv,i,arg),arg);
      vdbModelOptionsRequested=true;
    } else if (arg == "--vdb-vector-domain") {
      previewFlags.vectorFields=true;
      vectorPreview().domainGrid=requireValue(argc,argv,i,arg); vdbModelOptionsRequested=true;
    } else if (arg == "--vdb-vector-seconds" || arg == "--vdb-vector-length" ||
               arg == "--vdb-preview-radius" || arg == "--vdb-vector-step") {
      previewFlags.vectorFields=true;
      const double value=parseDouble(requireValue(argc,argv,i,arg),arg);
      auto& preview=vectorPreview();
      if (arg=="--vdb-vector-seconds") preview.displaySeconds=value;
      else if (arg=="--vdb-vector-length") preview.maxGlyphLength=value;
      else if (arg=="--vdb-preview-radius") preview.worldRadius=value;
      else preview.worldStep=value;
      vdbModelOptionsRequested=true;
    } else if (arg == "--vdb-vector-steps" || arg == "--vdb-preview-max-work") {
      previewFlags.vectorFields=true;
      const auto value=parseUint(requireValue(argc,argv,i,arg),arg);
      if (arg=="--vdb-vector-steps") vectorPreview().maxSteps=value;
      else vectorPreview().maxWork=value;
      vdbModelOptionsRequested=true;
    } else if (arg == "--vdb-preview-max-triangles") {
      vdbModelOptions.maxTriangles=parseUint(requireValue(argc,argv,i,arg),arg); vdbModelOptionsRequested=true;previewFlags.triangleLimit=true;
    } else if (arg == "--vdb-preview-color") {
      previewFlags.legacyColorFields=true;
      const auto mode=requireValue(argc,argv,i,arg);
      using Color=container::geometry::vdb::PointPreviewColor;
      if (mode!="uniform" && mode!="direction" && mode!="magnitude")
        throw std::invalid_argument("--vdb-preview-color requires uniform, direction or magnitude");
      const auto color=mode=="uniform" ? Color::Uniform : mode=="direction" ? Color::VelocityDirection : Color::VelocityMagnitude;
      vdbModelOptions.pointPreview.colorMode=color;
      // Display settings do not implicitly enable vector-only import.
      vdbModelOptionsRequested=true;
    } else if (arg == "--vdb-preview-tint") {
      previewFlags.legacyColorFields=true;
      vdbModelOptions.pointPreview.uniformColor=parseVec3(argc,argv,i,arg); vdbModelOptionsRequested=true;
    } else if (arg == "--vdb-preview-speed") {
      previewFlags.legacyColorFields=true;
      vdbModelOptions.pointPreview.velocityScale=parseDouble(requireValue(argc,argv,i,arg),arg); vdbModelOptionsRequested=true;
    } else if (arg == "--vdb-volume") {
      const std::string path(requireValue(argc, argv, i, arg));
      if (path.empty())
        throw std::invalid_argument("--vdb-volume requires a file path");
      if (config.vdbVolumePath.empty()) config.vdbVolumePath = path;
      else config.vdbAdditionalVolumePaths.push_back(path);
      if (config.displayModeOverride.empty())
        config.displayModeOverride = "lit";
    } else if (arg == "--vdb-sequence") {
      if (!config.vdbSequenceDirectory.empty())
        throw std::invalid_argument("--vdb-sequence may be specified only once");
      config.vdbSequenceDirectory = requireValue(argc, argv, i, arg);
      if (config.vdbSequenceDirectory.empty())
        throw std::invalid_argument("--vdb-sequence requires a directory");
      if (config.displayModeOverride.empty()) config.displayModeOverride = "lit";
    } else if (arg == "--vdb-fps") {
      config.vdbSequenceFps = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (config.vdbSequenceFps < 0.01f || config.vdbSequenceFps > 240.0f)
        throw std::invalid_argument("--vdb-fps must be in [0.01,240]");
    } else if (arg == "--vdb-speed") {
      config.vdbSequenceSpeed = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (config.vdbSequenceSpeed < 0 || config.vdbSequenceSpeed > 16)
        throw std::invalid_argument("--vdb-speed must be in [0,16]");
    } else if (arg == "--vdb-start-frame") {
      config.vdbSequenceStartFrame = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--vdb-cache-mib") {
      config.vdbSequenceCacheMiB = parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.vdbSequenceCacheMiB < 16 || config.vdbSequenceCacheMiB > 4096)
        throw std::invalid_argument("--vdb-cache-mib must be in [16,4096]");
    } else if (arg == "--vdb-paused") {
      config.vdbSequencePaused = true;
    } else if (arg == "--vdb-async") {
      config.vdbSequenceAsync = true;
    } else if (arg == "--vdb-no-loop") {
      config.vdbSequenceLoop = false;
    } else if (arg == "--vdb-sparse") {
      config.vdbSparse = true;
    } else if (arg == "--no-vdb-shadows" || arg == "--no-vdb-geometry-shadows") {
      config.vdbVolumeSettings.receiveShadows = false;
    } else if (arg == "--vdb-grid") {
      config.vdbGridName = requireValue(argc, argv, i, arg);
    } else if (arg == "--vdb-temperature-grid") {
      config.vdbTemperatureGridName = requireValue(argc, argv, i, arg);
      if (config.vdbTemperatureGridName.empty())
        throw std::invalid_argument("--vdb-temperature-grid requires a grid name");
    } else if (arg == "--vdb-temperature-scale") {
      auto &value = config.vdbVolumeSettings.temperatureScale;
      value = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (value < 0.0f || value > 100000.0f)
        throw std::runtime_error("--vdb-temperature-scale must be in [0,100000]");
    } else if (arg == "--vdb-resolution") {
      config.vdbVolumeResolution = parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.vdbVolumeResolution < 16 || config.vdbVolumeResolution > 256)
        throw std::runtime_error("--vdb-resolution must be in [16,256]");
    } else if (arg == "--vdb-density") {
      config.vdbVolumeSettings.densityScale = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (config.vdbVolumeSettings.densityScale < 0.0f || config.vdbVolumeSettings.densityScale > 10000.0f)
        throw std::runtime_error("--vdb-density must be in [0,10000]");
    } else if (arg == "--vdb-scattering" || arg == "--vdb-emission") {
      const auto color = parseVec3(argc, argv, i, arg);
      const float maximum = arg == "--vdb-scattering" ? 1.0f : 10000.0f;
      for (const float channel : color)
        if (channel < 0.0f || channel > maximum)
          throw std::runtime_error(std::string(arg) + " color channels must be in [0," +
                                   std::to_string(maximum) + "]");
      if (arg == "--vdb-scattering") config.vdbVolumeSettings.scatteringColor = color;
      else config.vdbVolumeSettings.emissionColor = color;
    } else if (arg == "--vdb-anisotropy") {
      auto &value = config.vdbVolumeSettings.anisotropy;
      value = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (value < -0.95f || value > 0.95f)
        throw std::runtime_error("--vdb-anisotropy must be in [-0.95,0.95]");
    } else if (arg == "--vdb-multiple-scattering") {
      auto &value = config.vdbVolumeSettings.multipleScatteringStrength;
      value = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (value < 0.0f || value > 1.0f)
        throw std::runtime_error("--vdb-multiple-scattering must be in [0,1]");
    } else if (arg == "--vdb-scattering-orders") {
      auto &value = config.vdbVolumeSettings.multipleScatteringOrders;
      value = parseUint(requireValue(argc, argv, i, arg), arg);
      if (value < 1 || value > 4)
        throw std::runtime_error("--vdb-scattering-orders must be in [1,4]");
    } else if (arg == "--vdb-emission-scale") {
      auto &value = config.vdbVolumeSettings.emissionScale;
      value = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (value < 0.0f || value > 10000.0f)
        throw std::runtime_error("--vdb-emission-scale must be in [0,10000]");
    } else if (arg == "--vdb-step-scale") {
      auto &value = config.vdbVolumeSettings.stepScale;
      value = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (value < 0.25f || value > 4.0f)
        throw std::runtime_error("--vdb-step-scale must be in [0.25,4]");
    } else if (arg == "--no-vdb-volume") {
      config.vdbVolumeSettings.enabled = false;
    } else if (arg == "--vdb-volume-scale") {
      config.vdbVolumeImportScale = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (config.vdbVolumeImportScale <= 0.0f)
        throw std::runtime_error("--vdb-volume-scale must be positive");
    } else {
      return false;
    }
    return true;
  };
  const auto parseReferencePathOption = [&](std::string_view arg, int& i) {
    if (arg == "--path-spp") {
      config.pathSamplesPerFrame = parseUint(requireValue(argc, argv, i, arg), arg);
      if (!config.pathSamplesPerFrame || config.pathSamplesPerFrame > 64)
        throw std::invalid_argument("--path-spp must be in [1,64]");
    } else if (arg == "--path-target-ms") {
      config.pathTargetMs = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (config.pathTargetMs < 0.1f || config.pathTargetMs > 1000.0f)
        throw std::invalid_argument("--path-target-ms must be in [0.1,1000]");
    } else if (arg == "--path-samples") {
      explicitPathSampleLimit = true;
      config.pathSampleLimit = parseUint(requireValue(argc, argv, i, arg), arg);
      if (!config.pathSampleLimit || config.pathSampleLimit > 16777216)
        throw std::invalid_argument("--path-samples must be in [1,16777216]");
    } else if (arg == "--path-sample-series") {
      if (!config.pathSampleSeries.empty())
        throw std::invalid_argument("--path-sample-series may only be specified once");
      auto samples = requireValue(argc, argv, i, arg);
      uint32_t previous = 0;
      for (;;) {
        const auto comma = samples.find(',');
        const uint32_t count = parseUint(samples.substr(0, comma), arg);
        if (count <= previous || count > 16777216 || config.pathSampleSeries.size() >= 64)
          throw std::invalid_argument(
              "--path-sample-series requires up to 64 strictly increasing counts in [1,16777216]");
        config.pathSampleSeries.push_back(count);
        if (comma == std::string_view::npos)
          break;
        previous = count;
        samples.remove_prefix(comma + 1);
      }
    } else if (arg == "--path-bounces") {
      config.pathMaxBounces = parseUint(requireValue(argc, argv, i, arg), arg);
      if (!config.pathMaxBounces || config.pathMaxBounces > 32)
        throw std::invalid_argument("--path-bounces must be in [1,32]");
    } else if (arg == "--path-seed") {
      config.pathSeed = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--path-firefly-clamp") {
      config.pathFireflyClamp = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (!container::renderer::validReferenceFireflyClamp(config.pathFireflyClamp))
        throw std::invalid_argument("--path-firefly-clamp must be 0 or in [1e-6,1e20]");
    } else if (arg == "--path-ground") {
      using Ground = container::renderer::ReferenceGroundMode;
      const auto mode = requireValue(argc, argv, i, arg);
      if (mode == "off") config.pathGround.mode = Ground::Off;
      else if (mode == "surface") config.pathGround.mode = Ground::Surface;
      else if (mode == "shadow-matte") config.pathGround.mode = Ground::ShadowMatte;
      else throw std::invalid_argument("--path-ground requires off, surface or shadow-matte");
    } else if (arg == "--path-ground-color") {
      const auto color = parseVec3(argc, argv, i, arg);
      config.pathGround.color = {color[0], color[1], color[2]};
      if (!config.pathGround.valid())
        throw std::invalid_argument("--path-ground-color components must be in [0,1]");
    } else if (arg == "--path-ground-height" || arg == "--path-ground-metallic" ||
               arg == "--path-ground-roughness") {
      const float value = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (arg == "--path-ground-height") config.pathGround.height = value;
      else if (arg == "--path-ground-metallic") config.pathGround.metallic = value;
      else config.pathGround.roughness = value;
      if (!config.pathGround.valid()) throw std::invalid_argument("Invalid " + std::string(arg));
    } else if (arg == "--path-aperture") {
      config.pathLens.apertureRadius = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (!config.pathLens.valid()) throw std::invalid_argument("--path-aperture must be in [0,1e10]");
    } else if (arg == "--path-focus-distance") {
      config.pathLens.focusDistance = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (!config.pathLens.valid()) throw std::invalid_argument("--path-focus-distance must be in [1e-6,1e20]");
    } else {
      return false;
    }
    return true;
  };
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i] ? std::string_view(argv[i]) : "";
    if (parseExtendedPreviewOption(arg,i))continue;
    if (parseVolumeOption(arg, i)) continue;
    if (parseReferencePathOption(arg, i)) continue;
    if (arg == "--model") {
      config.modelPath = std::string(requireValue(argc, argv, i, arg));
      positionalModelConsumed = true;
    } else if (arg == "--physics-demo") {
      config.physicsDemo = true;
    } else if (arg == "--asl-pose") {
      if (!config.aslPosePath.empty())
        throw std::invalid_argument("--asl-pose may be specified only once");
      config.aslPosePath = std::string(requireValue(argc, argv, i, arg));
      if (config.aslPosePath.empty())
        throw std::invalid_argument("--asl-pose requires a file path");
      if (config.displayModeOverride.empty())
        config.displayModeOverride = "lit";
      config.hasCameraOverride = true;
      config.cameraPosition = {0, 0, 3};
      config.cameraTarget = {0, 0, 0};
    } else if (arg == "--asl-pose-speed") {
      config.aslPoseSpeed = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (config.aslPoseSpeed < 0 || config.aslPoseSpeed > 16)
        throw std::invalid_argument("--asl-pose-speed must be in [0,16]");
    } else if (arg == "--asl-pose-start-ms") {
      config.aslPoseStartMs = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--asl-pose-no-loop") {
      config.aslPoseLoop = false;
    } else if (arg == "--asl-hand-kinematics") {
      config.aslHandKinematics = true;
    } else if (arg == "--asl-hand-aspect-ratio") {
      config.aslHandAspectRatio = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (config.aslHandAspectRatio < .125f || config.aslHandAspectRatio > 8)
        throw std::invalid_argument("--asl-hand-aspect-ratio must be in [0.125,8]");
    } else if (arg == "--asl-hand-angular-velocity") {
      config.aslHandAngularVelocity = parseFloat(requireValue(argc, argv, i, arg), arg);
      if (config.aslHandAngularVelocity < 0 || config.aslHandAngularVelocity > 100)
        throw std::invalid_argument("--asl-hand-angular-velocity must be in [0,100]");
    } else if (arg == "--asl-hand-report") {
      if (!config.aslHandReportPath.empty())
        throw std::invalid_argument("--asl-hand-report may be specified only once");
      config.aslHandReportPath = std::string(requireValue(argc, argv, i, arg));
      if (config.aslHandReportPath.empty())
        throw std::invalid_argument("--asl-hand-report requires a new file path");
    } else if (arg == "--width") {
      config.windowWidth = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--height") {
      config.windowHeight = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--ddgi") {
      config.ddgi.enabled = true;
      rememberDdgi([](auto &s) -> auto & { return s.enabled; });
    } else if (arg == "--no-ddgi") {
      config.ddgi.enabled = false;
      rememberDdgi([](auto &s) -> auto & { return s.enabled; });
    } else if (arg == "--ddgi-preset") {
      if (!config.ddgiPresetPath.empty())
        throw std::invalid_argument("--ddgi-preset may be specified only once");
      config.ddgiPresetPath = std::string(requireValue(argc, argv, i, arg));
      if (config.ddgiPresetPath.empty())
        throw std::invalid_argument("--ddgi-preset requires a file path");
    } else if (arg == "--ddgi-save-preset") {
      config.ddgiSavePresetPath = std::string(requireValue(argc, argv, i, arg));
      if (config.ddgiSavePresetPath.empty())
        throw std::invalid_argument("--ddgi-save-preset requires a file path");
    } else if (arg == "--ddgi-thin-glass" || arg == "--no-ddgi-thin-glass") {
      config.ddgi.thinGlassTransmission = arg == "--ddgi-thin-glass";
      rememberDdgi([](auto &s) -> auto & { return s.thinGlassTransmission; });
    } else if (arg == "--ddgi-dielectric" || arg == "--no-ddgi-dielectric") {
      config.ddgi.dielectricTransmission = arg == "--ddgi-dielectric";
      rememberDdgi([](auto &s) -> auto & { return s.dielectricTransmission; });
    } else if (arg == "--ddgi-camera-follow" || arg == "--no-ddgi-camera-follow") {
      config.ddgi.cameraFollow = arg == "--ddgi-camera-follow";
      rememberDdgi([](auto &s) -> auto & { return s.cameraFollow; });
    } else if (arg == "--ddgi-async" || arg == "--no-ddgi-async") {
      config.ddgi.asyncCompute = arg == "--ddgi-async";
      rememberDdgi([](auto &s) -> auto & { return s.asyncCompute; });
    } else if (arg == "--ddgi-detail-node") {
      config.ddgiDetailNode = std::string(requireValue(argc, argv, i, arg));
      if (config.ddgiDetailNode.empty())
        throw std::invalid_argument("--ddgi-detail-node requires a nonempty node name");
    } else if (arg == "--ddgi-detail-probes") {
      explicitDdgiDetailBudget = true;
      config.ddgiDetailProbeBudget = parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.ddgiDetailProbeBudget < 27)
        throw std::invalid_argument("--ddgi-detail-probes must be at least 27");
    } else if (arg == "--ddgi-fit-scene") {
      config.ddgiFitScene = true;
    } else if (arg == "--ddgi-fit-probes") {
      config.ddgiFit.maxProbes = parseUint(requireValue(argc, argv, i, arg), arg);
      explicitDdgiFitBudget = true;
    } else if (arg == "--ddgi-origin" || arg == "--ddgi-spacing") {
      explicitDdgiFitField = true;
      const auto value = parseVec3(argc, argv, i, arg);
      const glm::vec3 vector(value[0], value[1], value[2]);
      if (arg == "--ddgi-origin") {
        config.ddgi.volume.origin = vector;
        rememberDdgi([](auto &s) -> auto & { return s.volume.origin; });
      } else {
        config.ddgi.volume.spacing = vector;
        rememberDdgi([](auto &s) -> auto & { return s.volume.spacing; });
      }
    } else if (arg == "--ddgi-counts") {
      explicitDdgiFitField = true;
      config.ddgi.volume.counts = parseCounts(argc, argv, i, arg);
      rememberDdgi([](auto &s) -> auto & { return s.volume.counts; });
    } else if (arg == "--ddgi-rays") {
      config.ddgi.resources.updates.raysPerProbe =
          parseUint(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.resources.updates.raysPerProbe; });
    } else if (arg == "--ddgi-probes-per-frame") {
      config.ddgi.resources.updates.maxProbesPerFrame =
          parseUint(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.resources.updates.maxProbesPerFrame; });
    } else if (arg == "--ddgi-ray-budget") {
      config.ddgi.resources.updates.maxRaysPerFrame =
          parseUint(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.resources.updates.maxRaysPerFrame; });
    } else if (arg == "--ddgi-max-distance") {
      explicitDdgiFitField = true;
      config.ddgi.maxDistance = parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.maxDistance; });
    } else if (arg == "--ddgi-hysteresis") {
      config.ddgi.hysteresis = parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.hysteresis; });
    } else if (arg == "--ddgi-normal-bias") {
      explicitDdgiFitField = true;
      config.ddgi.normalBias = parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.normalBias; });
    } else if (arg == "--ddgi-view-bias") {
      explicitDdgiFitField = true;
      config.ddgi.viewBias = parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.viewBias; });
    } else if (arg == "--ddgi-visibility-bias") {
      explicitDdgiFitField = true;
      config.ddgi.visibilityBias =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.visibilityBias; });
    } else if (arg == "--ddgi-intensity") {
      config.ddgi.intensity = parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.intensity; });
    } else if (arg == "--ddgi-pause") {
      config.ddgi.paused = true;
      rememberDdgi([](auto &s) -> auto & { return s.paused; });
    } else if (arg == "--ddgi-debug") {
      const auto view = requireValue(argc, argv, i, arg);
      using container::renderer::DdgiDebugView;
      if (view == "off")
        config.ddgi.debugView = DdgiDebugView::Off;
      else if (view == "irradiance")
        config.ddgi.debugView = DdgiDebugView::Irradiance;
      else if (view == "visibility")
        config.ddgi.debugView = DdgiDebugView::Visibility;
      else if (view == "classification")
        config.ddgi.debugView = DdgiDebugView::Classification;
      else if (view == "relocation")
        config.ddgi.debugView = DdgiDebugView::Relocation;
      else
        throw std::invalid_argument("Unknown DDGI debug view");
      rememberDdgi([](auto &s) -> auto & { return s.debugView; });
    } else if (arg == "--ddgi-probe-radius") {
      explicitDdgiFitField = true;
      config.ddgi.debugProbeRadius =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      rememberDdgi([](auto &s) -> auto & { return s.debugProbeRadius; });
    } else if (arg == "--ddgi-memory-mb") {
      config.ddgi.payloadBudgetBytes =
          uint64_t(parseUint(requireValue(argc, argv, i, arg), arg)) * 1024 * 1024;
      rememberDdgi([](auto &s) -> auto & { return s.payloadBudgetBytes; });
    } else if (arg == "--ddgi-surface-memory-mb") {
      config.ddgi.surfaceBudgetBytes =
          uint64_t(parseUint(requireValue(argc, argv, i, arg), arg)) * 1024 * 1024;
      rememberDdgi([](auto &s) -> auto & { return s.surfaceBudgetBytes; });
    } else if (arg == "--taa") {
      config.taa.enabled = true;
    } else if (arg == "--no-taa") {
      config.taa.enabled = false;
    } else if (arg == "--taa-history-weight") {
      config.taa.historyWeight =
          parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--taa-depth-absolute") {
      config.taa.depthAbsoluteTolerance =
          parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--taa-depth-relative") {
      config.taa.depthRelativeTolerance =
          parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--taa-variance-gamma") {
      config.taa.varianceGamma =
          parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--taa-jitter-seed") {
      config.taa.jitterSeed = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--taa-reset-frame") {
      config.taaResetFrame = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--msaa") {
      config.msaaSamples = parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--ray-shadows") {
      const auto mode = requireValue(argc, argv, i, arg);
      if (mode == "raster")
        config.rayShadows.mode = container::renderer::RayShadowMode::Raster;
      else if (mode == "hard")
        config.rayShadows.mode = container::renderer::RayShadowMode::Hard;
      else if (mode == "soft")
        config.rayShadows.mode = container::renderer::RayShadowMode::Soft;
      else
        throw std::invalid_argument(
            "Ray shadow mode must be raster, hard or soft");
    } else if (arg == "--ray-shadow-samples") {
      config.rayShadows.areaSamples =
          parseUint(requireValue(argc, argv, i, arg), arg);
      if (!config.rayShadows.areaSamples || config.rayShadows.areaSamples > 32u)
        throw std::invalid_argument("Ray shadow samples must be in [1,32]");
    } else if (arg == "--ray-shadow-light-budget") {
      config.rayShadows.localLightBudget =
          parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.rayShadows.localLightBudget > 8u)
        throw std::invalid_argument(
            "Ray shadow local light budget must be in [0,8]");
    } else if (arg == "--no-ray-shadow-denoise") {
      config.rayShadows.denoise = false;
    } else if (arg == "--ray-shadow-debug-layer") {
      config.rayShadows.debugLayer =
          parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.rayShadows.debugLayer > 10u)
        throw std::invalid_argument("Ray shadow debug view must be in [0,10]");
    } else if (arg == "--area-emitter-debug") {
      config.areaEmitterDebug = true;
    } else if (arg == "--area-lighting") {
      const auto mode = requireValue(argc, argv, i, arg);
      if (mode == "sampled")
        config.areaLightingMode = 0u;
      else if (mode == "ltc")
        config.areaLightingMode = 1u;
      else
        throw std::invalid_argument("Area lighting must be sampled or ltc");
    } else if (arg == "--area-light-samples") {
      config.areaLightSampleCount =
          parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.areaLightSampleCount != 9u &&
          config.areaLightSampleCount != 25u &&
          config.areaLightSampleCount != 64u)
        throw std::invalid_argument("Area light samples must be 9, 25 or 64");
    } else if (arg == "--area-shadow-quality") {
      config.areaShadowQuality =
          parseUint(requireValue(argc, argv, i, arg), arg);
      if (config.areaShadowQuality > 3u)
        throw std::invalid_argument("Area shadow quality must be in [0,3]");
    } else if (arg == "--import-scale") {
      config.importScale = parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--bim-model") {
      config.bimModelPath = std::string(requireValue(argc, argv, i, arg));
      explicitBimModel = true;
    } else if (arg == "--bim-import-scale") {
      config.bimImportScale =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      explicitBimImportScale = true;
    } else if (arg == "--screenshot-viewport") {
      config.screenshotViewportOnly = true;
    } else if (arg == "--visual-regression-capture" || arg == "--screenshot") {
      config.screenshotCapturePath =
          std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--save-scene") {
      config.sceneExportPath = std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--animation-clip") {
      const auto clip = parseUint(requireValue(argc, argv, i, arg), arg);
      if (clip > uint32_t(std::numeric_limits<int>::max()))
        throw std::runtime_error("Animation clip index is too large");
      config.gltfAnimationClip = static_cast<int>(clip);
    } else if (arg == "--experimental-gltf-linked-assets") {
      config.experimentalGltfLinkedAssets = true;
    } else if (arg == "--gltf-scene") {
      const auto scene=parseUint(requireValue(argc,argv,i,arg),arg);
      if(scene>uint32_t(std::numeric_limits<int>::max())) throw std::runtime_error("Scene index is too large");
      config.gltfSceneIndex=int(scene);
    } else if (arg == "--gltf-variant") {
      const auto value=requireValue(argc,argv,i,arg);
      if (value=="-1" || value=="default") config.gltfMaterialVariant=-1;
      else {
        const auto variant=parseUint(value,arg);
        if (variant>uint32_t(std::numeric_limits<int>::max())) throw std::runtime_error("Material variant index is too large");
        config.gltfMaterialVariant=int(variant);
      }
    } else if (arg == "--animation-time") {
      config.gltfAnimationTime = parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--animation-speed") {
      config.gltfAnimationSpeed = parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--animation-paused") {
      config.gltfAnimationPlaying = false;
    } else if (arg == "--gltf-camera-node") {
      const auto node = parseUint(requireValue(argc, argv, i, arg), arg);
      if (node > uint32_t(std::numeric_limits<int>::max()))
        throw std::runtime_error("Camera node index is too large");
      config.gltfCameraNode = static_cast<int>(node);
    } else if (arg == "--capture-sequence") {
      config.temporalCaptureSequencePath =
          std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--gfxrecon-session") {
      config.gfxrecon.sessionPath = std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--warmup-frames") {
      explicitFrameCapture = true;
      config.screenshotWarmupFrames =
          parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--capture-frame") {
      explicitFrameCapture = true;
      config.screenshotCaptureFrame =
          parseUint(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--fixed-dt") {
      config.screenshotFixedTimestepSeconds =
          parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--camera-position") {
      config.cameraPosition = parseVec3(argc, argv, i, arg);
      config.hasCameraOverride = true;
    } else if (arg == "--camera-target") {
      config.cameraTarget = parseVec3(argc, argv, i, arg);
      config.hasCameraOverride = true;
    } else if (arg == "--camera-fov") {
      config.cameraVerticalFovDegrees =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasCameraOverride = true;
    } else if (arg == "--exposure") {
      config.manualExposure = parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasManualExposureOverride = true;
    } else if (arg == "--tone-map") {
      config.displayGrade.toneMapper =
          container::renderer::parseToneMapper(requireValue(argc, argv, i, arg));
    } else if (arg == "--display-grade" || arg == "--save-display-grade") {
      const auto path = container::util::pathFromUtf8(
          std::string(requireValue(argc, argv, i, arg)));
      if (path.empty()) throw std::invalid_argument(std::string(arg) + " requires a path");
      if (arg == "--display-grade") config.displayGrade = container::renderer::loadDisplayGrade(path);
      else displayGradeOutput = path;
    } else if (arg == "--saturation" || arg == "--contrast") {
      const float value = parseFloat(requireValue(argc, argv, i, arg), arg);
      const bool saturation = arg == "--saturation";
      if (value < (saturation ? 0.0f : 0.5f) || value > 2.0f)
        throw std::runtime_error(std::string(arg) + " is outside its supported range");
      if (saturation) config.displayGrade.saturation = value;
      else config.displayGrade.contrast = value;
    } else if (arg == "--white-balance" || arg == "--tint" || arg == "--vibrance") {
      const float value = parseFloat(requireValue(argc, argv, i, arg), arg);
      auto& grade = config.displayGrade;
      const auto setBounded = [&](float& field, float min, float max) {
        if (value < min || value > max)
          throw std::runtime_error(std::string(arg) + " is outside its supported range");
        field = value;
      };
      if (arg == "--white-balance") setBounded(grade.temperatureKelvin, 2000.0f, 15000.0f);
      else if (arg == "--tint") setBounded(grade.tintDuv, -0.025f, 0.025f);
      else setBounded(grade.vibrance, -1.0f, 1.0f);
    } else if (arg == "--shadow-tone" || arg == "--highlight-tone") {
      const auto color = parseVec3(argc, argv, i, arg);
      if (std::ranges::any_of(color, [](float channel) { return channel < 0.0f || channel > 1.0f; }))
        throw std::runtime_error(std::string(arg) + " channels must be in [0,1]");
      if (arg == "--shadow-tone") config.displayGrade.shadowTone = color;
      else config.displayGrade.highlightTone = color;
    } else if (arg == "--environment") {
      config.environmentPath = requireValue(argc, argv, i, arg);
      if (config.environmentPath.empty())
        throw std::invalid_argument("--environment requires a file");
    } else if (arg == "--gltf-sky") {
      config.useGltfSky = true;
    } else if (arg == "--sky") {
      config.sky.mode = container::renderer::parseProceduralSkyMode(requireValue(argc, argv, i, arg));
    } else if (arg == "--sky-preset" || arg == "--save-sky-preset") {
      const auto path = container::util::pathFromUtf8(std::string(requireValue(argc, argv, i, arg)));
      if (path.empty()) throw std::invalid_argument(std::string(arg) + " requires a path");
      if (arg == "--sky-preset") config.sky = container::renderer::loadSkyPreset(path);
      else skyPresetOutput = path;
    } else if (arg == "--sky-horizon" || arg == "--sky-zenith" || arg == "--sky-ground") {
      const auto color = parseVec3(argc, argv, i, arg);
      if (std::ranges::any_of(color, [](float channel) { return channel < 0.0f || channel > 1.0f; }))
        throw std::invalid_argument(std::string(arg) + " channels must be in [0,1]");
      if (arg == "--sky-horizon") config.sky.horizon = color;
      else if (arg == "--sky-zenith") config.sky.zenith = color;
      else config.sky.ground = color;
    } else if (arg == "--sky-multiple-scattering") {
      config.sky.atmosphere.multipleScattering = true;
    } else if (arg == "--sky-no-direct-sun") {
      config.sky.atmosphere.lightScene = false;
    } else if (arg == "--sky-location" || arg == "--sky-date" ||
               arg == "--sky-time" || arg == "--sky-utc-offset") {
      auto& local = config.sky.atmosphere.localSun;
      local.enabled = true;
      const std::string_view value = requireValue(argc, argv, i, arg);
      if (arg == "--sky-location") {
        local.latitudeDegrees = parseFloat(value, arg);
        local.longitudeDegrees = parseFloat(requireValue(argc, argv, i, arg), arg);
      } else if (arg == "--sky-date") {
        if (value.size() != 10 || value[4] != '-' || value[7] != '-')
          throw std::invalid_argument("--sky-date requires YYYY-MM-DD");
        local.year = int(parseUint(value.substr(0, 4), arg));
        local.month = int(parseUint(value.substr(5, 2), arg));
        local.day = int(parseUint(value.substr(8, 2), arg));
      } else if (arg == "--sky-time") {
        if (value.size() != 5 || value[2] != ':')
          throw std::invalid_argument("--sky-time requires HH:MM");
        local.hour = int(parseUint(value.substr(0, 2), arg));
        local.minute = int(parseUint(value.substr(3, 2), arg));
      } else {
        const auto minutes = parsePreviewInt64(value.starts_with('+') ? value.substr(1) : value, arg);
        if (minutes < -720 || minutes > 840)
          throw std::invalid_argument("--sky-utc-offset requires minutes in [-720,840]");
        local.utcOffsetMinutes = int(minutes);
      }
    } else if (arg.starts_with("--sky-")) {
      using Atmosphere = container::renderer::AtmosphereSettings;
      using Entry = std::pair<std::string_view, float Atmosphere::*>;
      static constexpr std::array fields{
          Entry{"--sky-sun-azimuth", &Atmosphere::sunAzimuthDegrees},
          Entry{"--sky-sun-elevation", &Atmosphere::sunElevationDegrees},
          Entry{"--sky-solar", &Atmosphere::solarIrradiance},
          Entry{"--sky-planet-km", &Atmosphere::planetRadiusKm},
          Entry{"--sky-atmosphere-km", &Atmosphere::atmosphereHeightKm},
          Entry{"--sky-observer-km", &Atmosphere::observerHeightKm},
          Entry{"--sky-rayleigh", &Atmosphere::rayleighScale},
          Entry{"--sky-mie", &Atmosphere::mieScale},
          Entry{"--sky-ozone", &Atmosphere::ozoneScale},
          Entry{"--sky-aerial-scale", &Atmosphere::aerialMetersPerUnit},
      };
      const auto field = std::ranges::find(fields, arg, &Entry::first);
      if (field == fields.end())
        throw std::invalid_argument("unknown atmosphere option " + std::string(arg));
      config.sky.atmosphere.*(field->second) = parseFloat(requireValue(argc, argv, i, arg), arg);
    } else if (arg == "--environment-yaw" || arg == "--background-blur") {
      const auto value = parseFloat(requireValue(argc, argv, i, arg), arg);
      const bool yaw = arg == "--environment-yaw";
      if (value < (yaw ? -180.0f : 0.0f) || value > (yaw ? 180.0f : 1.0f))
        throw std::invalid_argument(std::string(arg) + " is outside its supported range");
      if (yaw) config.environmentPresentation.yawDegrees = value;
      else config.environmentPresentation.backgroundBlur = value;
    } else if (arg == "--background-color") {
      const auto color = parseVec3(argc, argv, i, arg);
      if (std::ranges::any_of(color, [](float channel) { return channel < 0.0f || channel > 1.0f; }))
        throw std::invalid_argument("--background-color channels must be in [0,1]");
      config.environmentPresentation.backgroundColor = color;
      config.environmentPresentation.solidBackground = true;
    } else if (arg == "--environment-intensity") {
      config.environmentIntensity =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasEnvironmentIntensityOverride = true;
    } else if (arg == "--directional-intensity") {
      config.directionalIntensity =
          parseFloat(requireValue(argc, argv, i, arg), arg);
      config.hasDirectionalIntensityOverride = true;
    } else if (arg == "--directional-direction") {
      config.directionalDirection = parseVec3(argc, argv, i, arg);
      config.hasDirectionalDirectionOverride = true;
    } else if (arg == "--directional-color") {
      config.directionalColor = parseVec3(argc, argv, i, arg);
      config.hasDirectionalColorOverride = true;
    } else if (arg == "--display-mode" || arg == "--render-mode") {
      config.displayModeOverride =
          std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--render-technique") {
      config.renderTechnique = std::string(requireValue(argc, argv, i, arg));
    } else if (arg == "--no-bloom") {
      config.bloomEnabled = false;
      config.hasBloomEnabledOverride = true;
    } else if (arg == "--bloom") {
      config.bloomEnabled = true;
      config.hasBloomEnabledOverride = true;
    } else if (arg == "--no-ray-query") {
      config.enableRayQueries = false;
    } else if (arg == "--validation") {
      config.enableValidationLayers = true;
    } else if (arg == "--no-validation") {
      config.enableValidationLayers = false;
    } else if (arg == "--no-ui") {
      config.enableGui = false;
    } else if (arg == "--gpu-transforms") {
      config.gpuHierarchyTransforms = true;
    } else if (arg == "--max-scene-objects") {
      config.maxSceneObjects=parseUint(requireValue(argc,argv,i,arg),arg);
      if(config.maxSceneObjects==0 || config.maxSceneObjects>1000000)
        throw std::invalid_argument("--max-scene-objects must be 1 to 1000000");
    } else if (arg == "--hidden") {
      config.windowVisible = false;
    } else if (!arg.starts_with("--") && !positionalModelConsumed) {
      config.modelPath = std::string(arg);
      positionalModelConsumed = true;
    } else {
      throw std::runtime_error("unknown argument: " + std::string(arg));
    }
  }

  const size_t vdbSourceCount = (!config.vdbVolumePath.empty() ? 1u : 0u) +
      config.vdbAdditionalVolumePaths.size() + (!config.vdbSequenceDirectory.empty() ? 1u : 0u);
  if (vdbSourceCount > 4)
    throw std::invalid_argument("at most four OpenVDB volumes, including a sequence, are supported");
  if (vdbSourceCount && !positionalModelConsumed && !explicitBimModel)
    config.modelPath.clear();
  if (!explicitBimModel && isAuxiliaryRenderModelPath(config.modelPath)) {
    config.bimModelPath = config.modelPath;
    if (!explicitBimImportScale)
      config.bimImportScale = config.importScale;
    config.modelPath.clear();
  }
  if (config.aslHandKinematics && config.aslPosePath.empty())
    throw std::invalid_argument("--asl-hand-kinematics requires --asl-pose");
  if (vdbModelOptionsRequested) {
    finalizeVdbPreview(vdbModelOptions,previewFlags);
    const auto extension=lowerAscii(container::util::pathFromUtf8(config.bimModelPath).extension().string());
    if (extension!=".vdb") throw std::invalid_argument("OpenVDB model preview settings require a .vdb --model or --bim-model");
    if (!container::geometry::vdb::IsAvailable()) throw std::runtime_error("OpenVDB previews are unavailable; configure with -DENABLE_OPENVDB=ON.");
    config.vdbModelOptions=std::make_shared<const container::geometry::vdb::ImportOptions>(std::move(vdbModelOptions));
  }
  if (!config.aslHandReportPath.empty() && !config.aslHandKinematics)
    throw std::invalid_argument("--asl-hand-report requires --asl-hand-kinematics");
  if (!config.ddgiPresetPath.empty()) {
    auto preset = container::renderer::loadDdgiPreset(
        container::util::pathFromUtf8(config.ddgiPresetPath));
    const auto sceneIdentity = container::renderer::ddgiSceneIdentity(
        container::util::pathFromUtf8(config.modelPath),
        container::util::pathFromUtf8(config.bimModelPath), config.importScale,
        config.bimImportScale);
    if (preset.sceneIdentity != sceneIdentity)
      throw std::invalid_argument(
          "DDGI preset scene paths/import scales do not match the selected scene");
    for (const auto &overrideSettings : ddgiOverrides)
      overrideSettings(preset.settings);
    config.ddgi = std::move(preset.settings);
  }

  container::temporal::validateSettings(config.taa, config.msaaSamples);
  if (!config.pathSampleSeries.empty()) {
    if (config.pathTargetMs > 0.0f)
      throw std::invalid_argument("--path-target-ms cannot be combined with --path-sample-series");
    const auto extension = container::util::pathToUtf8(
        container::util::pathFromUtf8(config.screenshotCapturePath).extension());
    if (config.renderTechnique != "path-tracing" || !config.enableRayQueries ||
        lowerAscii(extension) != ".exr")
      throw std::invalid_argument(
          "--path-sample-series requires path-tracing with ray queries and --screenshot FILE.exr");
    if (explicitPathSampleLimit || explicitFrameCapture || !config.temporalCaptureSequencePath.empty() ||
        config.gfxrecon.enabled() || !displayGradeOutput.empty() || !skyPresetOutput.empty())
      throw std::invalid_argument(
          "--path-sample-series cannot be combined with --path-samples, frame capture, or preset export");
    if (config.displayModeOverride.empty())
      config.displayModeOverride = "lit";
    config.pathSampleLimit = config.pathSampleSeries.back();
  }
  if (!config.environmentPath.empty() && config.sky.mode != container::renderer::SkyMode::Hdr)
    throw std::invalid_argument("--environment and --sky are mutually exclusive");
  if (config.useGltfSky && (!config.environmentPath.empty() ||
                            config.sky.mode != container::renderer::SkyMode::Hdr))
    throw std::invalid_argument("--gltf-sky requires independent environment and sky inputs to be omitted");
  if (!container::renderer::validAtmosphere(config.sky.atmosphere))
    throw std::invalid_argument("Atmosphere settings are outside the supported range");
  if (config.ddgiFitScene && explicitDdgiFitField)
    throw std::invalid_argument(
        "--ddgi-fit-scene cannot be combined with explicit DDGI volume, trace "
        "distance, bias or probe-radius settings; use manual setup instead.");
  if (explicitDdgiFitBudget && !config.ddgiFitScene)
    throw std::invalid_argument("--ddgi-fit-probes requires --ddgi-fit-scene");
  if (explicitDdgiDetailBudget && config.ddgiDetailNode.empty())
    throw std::invalid_argument("--ddgi-detail-probes requires --ddgi-detail-node");
  if (config.ddgiFitScene && config.ddgiFit.maxProbes < 27)
    throw std::invalid_argument("--ddgi-fit-probes must be at least 27");
  // Scene bounds become available after importing. Validate/prepare the
  // minimum padded lattice meanwhile, so a small fit budget is not rejected
  // against the unrelated default volume or allocated before fitting.
  if (config.ddgiFitScene)
    config.ddgi.volume.counts = {3, 3, 3};
  container::renderer::validateDdgiSettings(config.ddgi);
}

}  // namespace

static int runApplication(int argc, char** argv) {
  try {
    for (int i = 1; i < argc; ++i) {
      if (argv[i] && std::string_view(argv[i]) == "--help") {
        printCommandLineHelp();
        return EXIT_SUCCESS;
      }
    }
    auto config = container::app::DefaultAppConfig();
    std::filesystem::path displayGradeOutput;
    std::filesystem::path skyPresetOutput;
    applyCommandLine(config, argc, argv, displayGradeOutput, skyPresetOutput);
    if (!skyPresetOutput.empty()) {
      if (!displayGradeOutput.empty())
        throw std::invalid_argument("Select one preset output per invocation");
      container::renderer::saveSkyPreset(skyPresetOutput, config.sky);
      return EXIT_SUCCESS;
    }
    if (!displayGradeOutput.empty()) {
      container::renderer::saveDisplayGrade(displayGradeOutput, config.displayGrade);
      return EXIT_SUCCESS;
    }
    container::app::Application application{std::move(config)};
    application.run();
  } catch (const std::exception& e) {
    std::println(stderr, "{}", e.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

#ifdef _WIN32
// Preserve the original Unicode arguments instead of the CRT ANSI-code-page
// argv. Every path/selector in AppConfig uses UTF-8, including async snapshots.
int wmain(int argc,wchar_t** wideArguments) {
  try {
    std::vector<std::string> arguments;arguments.reserve(size_t(argc));
    for(int i=0;i<argc;++i) {
      const auto utf8=std::filesystem::path(wideArguments[i]).u8string();
      arguments.emplace_back(utf8.begin(),utf8.end());
    }
    std::vector<char*> pointers;pointers.reserve(arguments.size()+1);
    for(auto& argument:arguments)pointers.push_back(argument.data());
    pointers.push_back(nullptr); // Retain the standard argv[argc] sentinel.
    return runApplication(argc,pointers.data());
  } catch(const std::exception& error) {
    std::println(stderr,"Command line: {}",error.what());return EXIT_FAILURE;
  }
}
#else
int main(int argc,char** argv){return runApplication(argc,argv);}
#endif
