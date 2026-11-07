# Runtime shader reload

Press **F5** or select **Reload Shaders** in Scene Controls after editing a Slang shader or one of its includes. A source build records the Slang compiler location, stage definitions, dependencies, and reflected shader interfaces. Packaged builds report that reload requires a source build and its shader manifest.

The compiler runs in the background during interactive editing. The reload compares dependency contents against the source build and compiles only affected stages. It reuses baseline SPIR-V for other stages. Changes to reflected interfaces require a rebuild so the renderer can recreate the associated layouts and bindings.

The renderer prepares graphics and compute candidates before publishing them at a frame boundary. In-flight graphics and probe work retire before the old pipelines are released. Ray-shadow, temporal and probe history refresh when the new generation publishes. Changes to environment bake or BRDF integration shaders rebuild their textures from retained inputs. Later resize and MSAA changes use the active shader generation.

A compilation, interface, or pipeline preparation failure preserves the current generation and reports the reason. The compiler has a two-minute deadline for the whole candidate. The renderer stops its compiler process tree on timeout or when the editor closes. Timeout diagnostics include the last stage being compiled when available.

`runtime_shader_compiler_tests` exercises candidate lifetime, diagnostics, deadline and cancellation with CMake child processes. The capture sequence field `reloadShaders` requests a synchronous reload at the specified frame, so successive captures can compare pixels and generation status reproducibly.
