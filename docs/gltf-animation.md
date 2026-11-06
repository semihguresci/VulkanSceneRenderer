# glTF animation playback

Open **Scene Controls → glTF Animation** to select a clip, play or pause,
loop, set playback speed, or scrub the timeline. Select **Rest pose** to
restore the authored values. The **Camera** menu selects an authored glTF
camera on a node in the active scene, including its world transform and roll.
Perspective and orthographic cameras use the same projection for rendering
and temporal tracking. Directional shadow cascades use the authored camera
with finite far corners when the rendering projection has an infinite far plane.

Use a zero-based clip and time for reproducible captures:

```powershell
.\VulkanSceneRenderer.exe --model scene.gltf --animation-clip 0 --animation-time 0.5 --animation-paused --hidden --no-ui --screenshot frame.png
```

`--gltf-camera-node N` selects the camera attached to source node `N`.
`--animation-speed` supports negative rates and `--animation-no-loop`
stops at the first or last key as appropriate. `--fixed-dt` sets the
simulation step in a capture. A capture sequence event can set
`animationClip` (`-1` selects rest), `animationTime`, and
`animationPlaying`. A successful scene replacement restores the new scene
to rest and clears the previous camera selection. A malformed animation
is rejected before the renderer retires the live scene.

The player decodes checked glTF accessors and supports STEP, LINEAR, and
CUBICSPLINE interpolation. Linear rotation takes the shortest quaternion
arc. Cubic rotation scales authored tangents by the key interval and
normalizes the interpolated result. The GPU sees material factors, texture
transforms, lights and camera changes before the next frame. Material playback
retains uploaded textures and sampler bindings.

The supported `KHR_animation_pointer` subset covers numeric node TRS,
shader-backed material and texture-transform properties, punctual lights,
and perspective or orthographic cameras. Unsupported optional channels
appear in the panel diagnostics. A required unsupported pointer fails with
the asset, clip, and channel in the diagnostic. Morph weight tracks update
the skinning and morph stage for the selected mesh instance. See
[glTF deformation](gltf-deformation.md) for rendering and history details.
Arbitrary Object Model properties and animation editing remain separate.

The CPU suite is `gltf_animation_tests`. The two native regressions in
`tests/validation/gltf_animation_runtime_regression.py` and
`tests/validation/gltf_playback_lifecycle_regression.py` cover both raster
modes and check visible motion, asset replacement, required-target
diagnostics, and repeatable fixed-step playback with Vulkan validation.
