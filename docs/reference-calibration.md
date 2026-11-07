# Reference convergence and raster calibration

The native scene matrix passes 95 captures across 40 processes on the RTX 2080
SUPER with driver 610.88, Vulkan validation 1.4.363.1 and renderer
`dfc887d53da4bb81ac75196f7d554ce0e9bf40e8110aaac72a6c592c8b9e84a4`.
The frozen receipt, generated fixtures, launch commands, image comparisons and
calibration guide are retained under
`build/codex-gltf-validation/reference-convergence-sized-dfc887-native`.
The harness is `tests/validation/reference_scene_convergence_runtime_regression.py`.

Four independent seeds at 32, 128 and 512 spp use the mean of two separate
4096-spp seeds as a finite, noisy target. Values below are scene-linear RGB
RMSE, averaged across the four low-sample seeds; target noise is estimated from
the two high-sample images. These are empirical image/seed statistics, not a
pixelwise monotonicity or formal heavy-tail confidence guarantee.

| Scene | 32 spp | 128 spp | 512 spp | Measured target noise |
| --- | ---: | ---: | ---: | ---: |
| Cornell | 0.044231 | 0.022186 | 0.012431 | 0.002441 |
| Area lights | 0.011381 | 0.005738 | 0.002929 | 0.000710 |
| Blocker | 0.006110 | 0.003071 | 0.001571 | 0.000381 |
| Roughness | 0.023379 | 0.011546 | 0.006157 | 0.001370 |
| Interior | 0.041546 | 0.021116 | 0.010745 | 0.002727 |

Compare raw linear EXRs for energy differences. Match camera, import units,
lighting, clipping and exposure before comparing engines. The captured display
views use manual exposure 0.5, Reinhard and disabled bloom, TAA, upscaling and
DDGI, at the same unjittered camera and resolution. Raster PNGs retain the
editor's selected-node outline; raw scene-linear captures omit it. For clean
interactive display comparisons, clear selection or use Render Preview.

Use the area-light and roughness fixtures to inspect LTC fit differences, and
the blocker fixture to distinguish hard raster visibility from finite-area path
visibility. To calibrate DDGI, freeze a matching raster configuration with DDGI
enabled, let probes converge and compare indirect regions with the raw target.
This baseline keeps DDGI off and does not claim DDGI quality acceptance. For
reflections, use matching roughness/occlusion views and account for unsupported
or off-screen SSR paths. The standard-error view estimates sampling noise;
it does not measure transport bias.

The receipt retains per-capture GPU pass timings and owned reference allocations
(about 3.15 MB for these fixtures). Timings include validation and concurrent CPU
build activity; they are fixture measurements without a comparative performance
claim. The first checkpoint can precede a retired GPU timing and records it as
unavailable. Later checkpoints supply actual measurements for every scene.
The documented material/medium limits and numerical diagnostic counters apply.

After the remaining library upgrades, renderer
`5796c721b60b3967619008f72fc4de67e33e0b6bc53870798b271f5fc2ca292a`
replayed the Cornell seed-23 checkpoints at 32/128/512 spp. All decoded float32
bits and encoded EXR bytes match this accepted baseline exactly. The bridge
receipt is
`build/codex-gltf-validation/latest-all-5796-native-v1/reference-smoke/receipt.json`.
