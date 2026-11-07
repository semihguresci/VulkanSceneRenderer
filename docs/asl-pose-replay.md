# ASL observation replay

Container accepts `asl-pose-replay-v1` JSON from the ASL workbench in
`F:\Projects\ti_it`. This first integration replays observed hand, body and face
landmarks as colored joints and bones. It does not train a recognizer or skin an
avatar. The existing scene, BIM and physics code is independent of this replay.

## Export one prepared sample

Use the workbench's existing participant-disjoint landmark preparation run:

```powershell
F:\Projects\ti_it\.venv-data\Scripts\python.exe F:\Projects\ti_it\scripts\export_pose_replay.py `
  --manifest E:\ASL\workbench\kaggle\training\RUN\test.jsonl `
  --index 0 --output E:\ASL\workbench\pose-replay\sample-0.json
```

Replace the manifest with the actual preparation manifest. `--index` is zero
based. The exporter reads one full sequence lazily from the Kaggle ZIP or a
native recording. It refuses an existing output file and any silent truncation.
It does not download media, start a camera, or start training.

The exporter accepts `--split train|validation|test|unassigned`; normally the
split comes from the manifest filename. Conflicting record/filename splits are
rejected. Source sample ID, dataset, signer ID (or explicit null), label, source
manifest and parent sample IDs survive export. Synthetic descendants must keep
their source split and parent IDs in the upstream data pipeline.

## Run in Container

```powershell
.\out\build\visual-studio\Release\VulkanSceneRenderer.exe `
  --asl-pose E:\ASL\workbench\pose-replay\sample-0.json
```

The ASL view defaults to Lit display, with the front camera at `(0,0,3)` looking at the origin. Use the normal
renderer camera controls to inspect the plane. F8 pauses/resumes; F9 restarts.
The title shows the label and displayed observation number. Left hand is blue,
right hand orange, body green and face purple. Invalid points and their adjacent
bones are hidden. Frame sampling holds the most recent source observation,
preserving timestamp gaps without interpolation. The last frame is held for
one final source interval before looping. `--asl-pose-no-loop` holds the end.

`--asl-pose-speed 0` holds a pose. `--asl-pose-start-ms 300` seeks 300 ms relative
to the first observation. An explicit ASL replay hides startup model nodes;
the model payloads remain in the scene. Reloading a different scene retires the
replay rather than mutating reused node IDs.

The screenshot path uses the same clock and can produce repeatable captures:

```powershell
.\out\build\visual-studio\Release\VulkanSceneRenderer.exe `
  --asl-pose E:\ASL\workbench\pose-replay\sample-0.json `
  --asl-pose-speed 0 --asl-pose-start-ms 300 `
  --screenshot out\asl-frame.png --capture-frame 9 --no-ui --hidden
```

See `--help` for the existing screenshot/sequence option names and timing.

## Constrained hand preview

Use the normal Release build on this PC:
`F:\Projects\Container\out\build\visual-studio\Release\VulkanSceneRenderer.exe`.
The October 18 check verified that this build contains the reviewed v2 hand
solver and the current Jolt integration. A bounded hidden capture of the known
16-frame two-hand sample produced the same fit report as the earlier ASL
candidate, with no Vulkan validation errors. The capture exercised hand replay;
it did not create or step physics bodies. The older
`VulkanSceneRendererASL.exe` remains a historical candidate and is no longer the
recommended launcher target.

```powershell
.\out\build\visual-studio\Release\VulkanSceneRenderer.exe `
  --asl-pose E:\ASL\workbench\pose-replay\sample-0.json `
  --asl-hand-kinematics --asl-hand-aspect-ratio 1 `
  --asl-hand-report E:\ASL\experiments\sample-0-hand-fit.json
```

This optional view estimates an articulated hand from the landmarks. It fixes
each clip's palm offsets and fifteen phalanx lengths, fits twenty-one bounded
joint angles with inverse kinematics, and reconstructs the joints with forward
kinematics. The four fingers have MCP flexion/abduction and independent PIP/DIP
hinges; the thumb has CMC flexion/abduction/twist, MCP and IP hinges. One global
hand scale is estimated from palm anchors, so fingers cannot stretch
independently. Broad angle limits and the default `12 rad/s` angular velocity
bound are experimental settings, not validated measurements of a signer's
physiology. Use `--asl-hand-angular-velocity 0` to disable the velocity bound.
The bound applies to articulation angles, not world-space wrist translation or
palm rotation; tracking jitter in those global motions is not removed.

F10 compares the fitted view against the original observations at the same
source frame. The title labels raw comparison, per-hand fit residuals, missing
hands and raw fallbacks. Body and face coordinates remain unchanged. Missing
hand observations stay hidden, including any joint inferred internally by FK.
An absent or degenerate wrist/index-MCP/pinky-MCP basis gives an explicit raw
fallback; fitting does not manufacture an observed hand.

Every source frame is fitted once in timestamp order before playback starts.
Calibration uses all observations in this one clip; it never pools signers or
training splits. This offline preview is not a causal live-camera calibrator.
The solver uses source timestamp deltas; gaps over 250 ms, invalid preceding
fits, or a changed hand profile reset temporal history. Seeking, restarting,
changing playback speed and looping only sample this deterministic cache.
Kaggle clips may have an assumed frame rate recorded in their source timing
metadata: their velocity bounds therefore operate on estimated time. Recorded
camera timestamps and a calibrated camera are required to validate motion
against physical measurements.

`--asl-hand-aspect-ratio` is the source image width divided by height, bounded
to `[0.125,8]`. The default `1` explicitly assumes square normalized coordinates
when the source lacks image dimensions. Input X and hand-relative Z are
scaled by this ratio; Y and Z are rotated together into a right-handed +Y-up
solver space: `(aspect*x, -y, -aspect*z)`. This retains model-relative depth
without treating it as metric or shared with body/face depth. Fitted positions
are converted back to input coordinates for the existing XY display; no
projection or Vulkan viewport changes are involved.
The X/Y normalization and approximate X-scale relative Z follow the
[MediaPipe Hand Landmarker coordinate definition](https://developers.google.com/edge/mediapipe/solutions/vision/hand_landmarker/python#handle_and_display_results).

`--asl-hand-report` writes a new `asl-hand-kinematics-fit-v1` JSON report and
refuses an existing path. It records calibration counts, exact limits/options,
source timestamps, angles, proper rotation bases, fit residuals, fixed-length
errors, velocity limiting and temporal resets. Output positions use input
coordinates; pose/basis/calibration fields use the explicitly declared solver
space. `observed` always equals the original mask, `constrained` marks observed
points reconstructed by a valid fit, and `inferred` marks unobserved model
points. Invalid fits have null model positions/pose and false derived masks.
The source replay is never modified. For a durable training artifact with
source SHA-256, signer/split and parent provenance, use the ASL workbench's
`scripts/process_hand_kinematics.py` wrapper rather than treating a preview
report as a training example.
`valid` means that finite bounded kinematics were computed; it does not certify
optimizer convergence, a small residual or preservation of the original sign.
Review residuals, iteration counts and the raw comparison before accepting a
derived clip for a new training experiment.

The numerical method identifier is `experimental-hand-fk-ik-v2`; the JSON
schema version describes the unchanged exchange layout. The method uses a
proper-rotation palm alignment and flexion directed towards the palm rather
than the dorsal surface.

The numerical processor uses the same implementation without starting Vulkan:

```powershell
.\out\build\visual-studio\Release\ASLHandKinematicsProcessor.exe `
  --input E:\ASL\workbench\pose-replay\sample-0.json `
  --output E:\ASL\experiments\sample-0-fit-payload.json `
  --image-aspect-ratio 1
```

This is geometric regularization. It does not simulate tendons, muscle forces,
self-collision/contact, object interaction or a calibrated skinned avatar.

## Observation coordinates and bounds

The canonical array layout has 553 positions: left hand `[0,21)`, right hand
`[21,42)`, body `[42,75)`, face `[75,553)`. Native 478-face or Kaggle 468-face
tracking use the same layout; missing iris points stay null and invalid.
Each JSON frame contains 553 point entries and 553 boolean validity entries.
Valid points are finite XYZ triples; invalid points are JSON null, never NaN.

Original image-normalized X/Y and model-relative Z are retained unchanged.
**Relative Z is not calibrated depth and is not shared across body, hand and
face models.** The first renderer deliberately places points on an XY plane:
`world = (2*(x-.5), 2*(.5-y), 0)`. This square observation plane has no claimed
metric scale or original camera aspect ratio. The Y conversion follows the
renderer right-handed +Y-up convention; no projection or Vulkan Y flip changes.

The versioned schema is
`F:\Projects\ti_it\schemas\asl-pose-replay-v1.schema.json`. Runtime validation
also checks strictly increasing nonnegative integer timestamps, frame-count
agreement, group offsets, mask/null agreement, finite coordinates bounded to
absolute 10000 and bounded provenance. Files are limited to 64 MiB, 2048 frames
and 32 JSON nesting levels. Oversized clips must be explicitly segmented
upstream with parent identity and unchanged split membership.

Future work: confidence-aware rig retargeting, consistent camera reconstruction,
facial expressions, synthetic camera/render metadata and aligned numeric depth.
Rendered depth of the display plane would not be measured body depth.

## Focused checks

Use a Visual Studio Developer Command Prompt, following `build-and-test.md`:

```powershell
cmake --build out/build/visual-studio --config Release --target asl_pose_replay_tests hand_kinematics_tests ASLHandKinematicsProcessor
ctest --test-dir out/build/visual-studio -C Release -R '^(asl_pose_replay_tests|hand_kinematics_tests)$' --output-on-failure
```

The ASL project also has `tests/test_pose_replay.py`, including a native sample
export through the existing lazy `PreparedDataset` loader.
