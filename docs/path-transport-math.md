# Reference transport math

The shared Slang transport libraries provide numerical building blocks for the
progressive reference renderer. They do not depend on raster lighting, DDGI or
display transforms. This foundation can be built and tested independently of
the viewport integration.

`path_transport_common.slang` implements diffuse/GGX reflection, thin-sheet
transmission, complete mixture PDFs, visible-normal sampling, delta events,
power-heuristic MIS, compensated roulette, deterministic sample addresses and
unclamped float32 Welford moments. Camera rays use separate column-major inverse
projection/view matrices, reverse-Z and the negative-height Vulkan viewport.

`path_dielectric_common.slang` adds smooth/rough solid dielectric interfaces,
Snell refraction, glTF Schlick Fresnel with the exit angle and total internal
reflection, the refractive half-vector Jacobian, radiance-mode IOR scaling and
homogeneous Beer absorption. Its relative IOR is transmitted IOR / incident IOR;
directions point away from a shading frame facing the incident medium. The
caller supplies the media on either side and applies absorption over actual
world distances. Index-matched transmission is a straight-through delta even
when the surface has nonzero roughness.

Metallic volume surfaces use a weighted conductor/dielectric mixture. Total
internal reflection affects the dielectric fraction while the conductor retains
its color. Evaluation and sampling use the complete mixture PDF, and coincident
smooth reflection events merge their discrete masses. Transmission is weighted
by the dielectric fraction once.

Numerical validation uses the real Vulkan compute pipeline with synchronization
validation, GPU buffers and readback. The **41 cases** compare independent
double-precision BRDF/BTDF/PDF/throughput references, reciprocity, furnace energy,
sphere quadrature, Snell/TIR limits, camera conventions, sampling sequences,
batching, moments and absorption endpoints. Null sampling outcomes are kept
separate from invalid-input rejections. Roughness is not clamped to disguise
unrepresentable inputs.

Build `path_transport_gpu_tests` with tests enabled and a configured Vulkan SDK,
then run:

```powershell
$env:CONTAINER_RUN_GPU_PATH_TRANSPORT = '1'
ctest --test-dir <build-directory> -C Release -R '^path_transport_gpu_tests$' --output-on-failure
```

The shader source is `tests/shaders/path_transport_contract.slang`. Tests require
a Vulkan device and display runtime even though the test window is hidden. The
library introduces no external dependency or editor control. Viewport integration,
medium tracking and scene-level quality acceptance are separate milestones.

Mathematical references:

- [Heitz: GGX visible-normal sampling](https://jcgt.org/published/0007/04/01/).
- [PBRT: dielectric reflection, refraction and radiance transport](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF).
- [Khronos: thin-walled transmission](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_transmission/README.md).
- [Khronos: volume refraction, absorption and specular interaction](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_volume/README.md).
