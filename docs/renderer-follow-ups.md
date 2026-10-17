# Renderer follow-up improvements

The DDGI and model-loading implementation includes bounded probe transport,
scene fitting, detail volumes, camera following, optional async updates,
responsive imports, complete scrolling and cached BIM inspection tables.
The October 11 review found no correctness merge blockers. The following issues
track the remaining measured costs and deliberately unsupported extensions.
Each issue contains its scope, acceptance criteria and required validation.
Static BIM statistics and immutable provider metadata caching (#74) is now
implemented and validated; see [BIM frame caching](bim-frame-cache.md).

| Issue | Remaining improvement |
| --- | --- |
| [#73](https://github.com/semihguresci/VulkanSceneRenderer/issues/73) | Enable concurrent graphics frames with frame-owned GPU resources |
| [#75](https://github.com/semihguresci/VulkanSceneRenderer/issues/75) | Virtualize expanded scene hierarchy and cache BIM element searches |
| [#76](https://github.com/semihguresci/VulkanSceneRenderer/issues/76) | Profile and reduce BIM import and relationship-graph preparation cost |
| [#77](https://github.com/semihguresci/VulkanSceneRenderer/issues/77) | Add cooperative cancellation checkpoints to active model import jobs |
| [#78](https://github.com/semihguresci/VulkanSceneRenderer/issues/78) | Avoid full DDGI scroll planning when the camera stays in the same cell |
| [#79](https://github.com/semihguresci/VulkanSceneRenderer/issues/79) | Support DDGI updates on dedicated compute queue families |
| [#80](https://github.com/semihguresci/VulkanSceneRenderer/issues/80) | Validate DDGI and ray-query rendering across AMD, Intel and newer NVIDIA devices |
| [#81](https://github.com/semihguresci/VulkanSceneRenderer/issues/81) | Expand DDGI leakage coverage for thin and double-sided architectural BIM surfaces |
| [#82](https://github.com/semihguresci/VulkanSceneRenderer/issues/82) | Add texture footprint and mip selection to secondary ray transport |
| [#83](https://github.com/semihguresci/VulkanSceneRenderer/issues/83) | Plan DDGI detail volumes automatically from BIM rooms |
| [#84](https://github.com/semihguresci/VulkanSceneRenderer/issues/84) | Extend bounded DDGI dielectric transport to nested media |

Continue with the CPU improvements that have a reproducible benchmark:
UI virtualization/search (#75), and unchanged camera-follow planning (#78).
Active importer cancellation (#77) improves rapid
replacement and shutdown. Graphics concurrency (#73) needs resource lifetime
work before the serialized safety policy can change.

Existing work remains tracked in the original issues: [disk LTC optimization
#70](https://github.com/semihguresci/VulkanSceneRenderer/issues/70), [repeatable LTC
profiling #71](https://github.com/semihguresci/VulkanSceneRenderer/issues/71),
[native IFC coverage #67](https://github.com/semihguresci/VulkanSceneRenderer/issues/67),
and epics [reflections #43](https://github.com/semihguresci/VulkanSceneRenderer/issues/43),
[temporal upscaling #44](https://github.com/semihguresci/VulkanSceneRenderer/issues/44),
[LOD/streaming #45](https://github.com/semihguresci/VulkanSceneRenderer/issues/45),
[IES/volumetrics #46](https://github.com/semihguresci/VulkanSceneRenderer/issues/46),
and [reference tracing #47](https://github.com/semihguresci/VulkanSceneRenderer/issues/47).
These have not been duplicated as new issues.

See [BIM performance](bim-performance.md) for the Tekla measurements,
[model-loading validation](model-loading-validation.md) for lifecycle coverage,
and [DDGI](ddgi-probe-foundation.md) for transport, quality and device limits.
