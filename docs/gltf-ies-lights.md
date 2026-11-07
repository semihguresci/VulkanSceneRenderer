# Measured glTF light profiles

`EXT_lights_ies` attaches a measured point fixture to a glTF node. Import the
asset through the ordinary model loader, select the IES light in the light
inspector, and use its **Photometric multiplier** and color controls. Move
or rotate the source node to reposition or aim the fixture. The inspector
shows its aim, and the lit viewport previews the measured distribution.
[glTF extension](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Vendor/EXT_lights_ies)

The importer accepts LM-63-1995, LM-63-2002 and LM-63-2019 Type C profiles
with `TILT=NONE`. It retains strictly increasing, nonuniform vertical and
horizontal samples, with axial, quadrant, half-plane and full coverage.
The LM-63-1995 90°–270° half-plane is also supported. The renderer uses
bilinear interpolation over the original angular grid and emits zero outside
the authored vertical domain. Type A/B and both inline and external TILT data
produce diagnostics. [IES photometry](https://ies.org/education/learn-about-ies-files/)

For an unrotated fixture, 0° vertical aims down local −Z and the C0 plane
points along +X. Positive azimuth goes toward +Y. Node translation and
rotation define the world-space emission frame. Node scale does not multiply
the emitted intensity. The measured values are in candela after the file's
candela multiplier and ballast factors. The glTF instance multiplier and
linear RGB tint scale these values before the usual inverse-square punctual
attenuation. The renderer preserves the measured absolute intensities and
does not normalize each profile's peak. A valid IES profile takes precedence
over the punctual light on the same node.

The asset can refer to a confined relative `.ies` file, a base64 data URI of
type `application/x-ies-lm-63`, or a buffer view with that MIME type. Remote
self-contained imports and linked compositions accept embedded IES data. A
malformed optional profile leaves its companion punctual light in effect,
when present. Unsupported required profiles reject the asset. Portable Save
As embeds the original IES text and keeps the per-instance color and multiplier.

Forward opaque, transparent OIT and both deferred point-light routes use the
same angular lookup. The reference renderer, DDGI and OpenVDB local-light
evaluation use it as well. GPU memory uses four header floats followed by the
original angular vectors and candela samples. The light rows reserve 32 extra
bytes per local light. Identical profile payloads share an immutable VMA
allocation. The scene-manager cache limits combined resident profile payload
and key storage to 32 MiB and 256 entries. Each input file is limited to
2 MiB, each glTF table to 64 profiles and 16 MiB of profile text, and each
profile to 1,024 angles per axis and 262,144 measured values.

`gltf_loader_tests` exercises asymmetric interpolation, resource preparation
and required versus optional fallback. The native validation harness is
`tests/validation/gltf_ies_runtime_regression.py`.
