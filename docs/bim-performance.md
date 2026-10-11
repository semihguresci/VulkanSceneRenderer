# BIM inspection performance

Tekla House IFCX has 8,790 render objects. The former GUI path built schedule
and comparison inputs from all imported metadata and aggregated three schedules
on every frame, before testing whether Scene Controls or its BIM sections were
visible. Visual Studio Debug amplified the repeated string allocations and map
lookups: the measured GUI phase took 295.4 ms, while known GPU pass time was 8.2 ms.

`BimInspectionCache` now owns the derived tables and keeps their spans stable
until the model identity, geometry revision or metadata storage changes. Unload
clears them. Imported metadata bounds are immutable in the current contract;
future metadata/bounds edits must advance the revision. Runtime GPU color edits
do not rebuild these tables.

Measurements on October 11, 2026, NVIDIA GeForce RTX 2080 SUPER, Vulkan 1.4.325,
800x600, deferred raster, open GUI panels, TAA and validation disabled:

| Measurement | Before | After |
| --- | ---: | ---: |
| Debug GUI | 295.4 ms | 1.98 ms |
| Debug CPU frame excluding screenshot | 302.1 ms | 15.80 ms |
| Release GUI | 13.27 ms | 0.27 ms |
| Release steady CPU frame | Not sampled as a history | 8.86 ms |
| Release steady CPU frame P95 | Not sampled as a history | 9.45 ms |

Before values are captured-frame phase measurements. After values average
completed frames after the first 20 warmup frames, excluding screenshots:
24 Debug frames and 99 Release frames. No temporal sequence was used, so frame
synchronization remained inside the measured draw-frame timer. Debug used
`/Od /Ob0 /RTC1`; Release used `/O2 /Ob2`. These are single-device measurements,
not universal frame-rate guarantees.

CPU Frame is elapsed draw-frame time, including waits, presentation and
screenshot readback/PNG writing. Release presentation waits account for about
7.5 ms after the cache fix. Synchronization validation increases command
recording from under 1 ms to about 17.5 ms. At 2560x1440, a screenshot alone
takes about 330 ms; steady Release frames average 11.35 ms. Screenshot telemetry
now exports bounded `frameHistory` with all CPU phases so capture overhead can
be excluded from steady-state summaries.

The scene-only before/after images were pixel-identical. Visual Studio Release
and Debug builds passed, with tests for cache reuse, replacement, unload, bounds
updates, schedule/comparison semantics and independent telemetry phase history.
Native reload/replacement/cancellation checks also passed synchronization
validation. Detailed local evidence is in `out/tekla-cpu-profile`.

Remaining CPU, UI, import and rendering improvements are tracked in
[renderer follow-ups](renderer-follow-ups.md).
