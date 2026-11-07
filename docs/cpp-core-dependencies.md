# Selected C++ core dependencies

The scoped vcpkg overlays select EnTT 4.0.0, nlohmann/json 3.12.0 and
GoogleTest 1.18.0 without moving the project's registry baseline or unrelated
ports. These are the official latest stable releases verified on 2026-11-07.

| Library | Official source commit | License |
| --- | --- | --- |
| [EnTT 4.0.0](https://github.com/skypjack/entt/releases/tag/v4.0.0) | `85c6bba014049b5de8fad49d25424df2f1f6a8c1` | MIT |
| [JSON 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0) | `55f93686c01528224f448c19128836e7df245f72` | MIT |
| [GoogleTest 1.18.0](https://github.com/google/googletest/releases/tag/v1.18.0) | `063de7e9578f82b369302001269680b4b1553359` | BSD-3-Clause |

The overlay portfiles pin complete source archives by SHA-512. Core CMake
requires exact package versions and checks every selected EnTT/JSON header
plus the authentic copyright files. Header changes trigger reconfiguration.
Imported targets expose ordinary include paths so unrelated aggregate SDK or
package directories cannot take precedence solely because selected headers
were treated as system includes. The selected libraries must be visible through
the normal vcpkg toolchain or `CMAKE_PREFIX_PATH`; an offline prefix must also
provide its authentic `share/<port>/copyright` files.

EnTT 4 replaces component-pool `type()` with `info()`. The engine's World and
SceneController guards use that API while retaining type-info equality,
unknown-component rejection, bounded clone admission and context ownership.
The project uses C++23; EnTT's C++20 rewritten inequality therefore follows
its hash-based equality operator. No component layout or entity-ID rewrite is
needed for the covered engine paths.

JSON 3.12.0 changes the versioned inline namespace. Rebuild every source that
passes JSON values across a boundary; do not mix new-header objects with old
engine libraries. GoogleTest is likewise rebuilt from the pinned source with
the selected CRT. It is a test dependency and is not linked into the renderer.

Core configuration stages `EnTT-4.0.0-LICENSE.txt` and
`nlohmann-json-3.12.0-LICENSE.txt` from the actual imported-target prefixes,
retaining their raw bytes. `selected-ecs-json-dependencies.json` records name,
version, notice filename and SHA-256 for package integration. Test configuration
separately records `GTest-1.18.0-LICENSE.txt` in
`selected-test-dependencies.json`; this development-only notice is required if
test binaries are distributed, rather than being implied by the main package.

Isolated compatibility validation compiled copied production World,
SceneController, XMP and capability sources with enforced release-version
assertions. Twenty existing World test cases and two production XMP/JSON
probes passed, including allocation denial, opaque context retention, unknown
component rejection, large unsigned metadata and packet conflict diagnostics.
The integrated renderer subsequently passed 1,408 executed cases across 49
suites, 46 CLI/package checks and validation of all 115 SPIR-V modules; two
external sample-asset cases were skipped explicitly. The 235-capture native
queue and local Windows package also pass on renderer
`5796c721b60b3967619008f72fc4de67e33e0b6bc53870798b271f5fc2ca292a`.
See the dated final acceptance entry in [feature parity progress](feature-parity-progress.md)
for retained receipts and the C++ compatibility interface limits.
