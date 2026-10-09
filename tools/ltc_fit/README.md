# Renderer LTC fitting

This offline fitter adapts the LTC parameterization, multiple importance
sampling objective, and Nelder-Mead solver from
[selfshadow/ltc_code](https://github.com/selfshadow/ltc_code), pinned at
`31e5e96b54f98f33098f8503003119ba2231a1c6`. The original BSD-style license,
copyright and required paper citation are retained in `reference/LICENSE`.

Reference: **Real-Time Polygonal-Light Shading with Linearly Transformed
Cosines**, Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt,
ACM Transactions on Graphics (Proceedings of SIGGRAPH 2016), 35(4).
[Project page](https://eheitzresearch.wordpress.com/415-2/).

The target is the existing `shaders/brdf_common.slang` BRDF: perceptual
roughness clamped to `[0.045,1]`, alpha = roughness squared, GGX distribution
denominator floored at `1e-4`, Schlick-GGX separable masking with
`k=(roughness+1)^2/8`, and direct-light denominator floored at `1e-4`.
The standard reference Smith-GGX fit is intentionally not reused.
Sampling analytically inverts the projected distribution of the actual
denominator-clamped NDF; its projected mass is accounted for in the PDF.
The fitted LTC is clipped to the physical receiver hemisphere as well as its
transformed cosine hemisphere. Its amplitude includes exact normalization for
that first clip: the retained mass is `(1+MrowZ.z/length(MrowZ))/2`.

Build in a Visual Studio Developer Console, using the project's GLM headers:

```powershell
cl /nologo /O2 /EHsc /std:c++20 /I out/build/windows-release/vcpkg_installed/x64-windows/include tools/ltc_fit/fit_renderer_ltc.cpp /Fe:out/fit_renderer_ltc.exe /Fo:out/fit_renderer_ltc.obj
out/fit_renderer_ltc.exe materials/ltc 64 200 8
```

The last arguments are samples per axis, maximum fitting iterations, and
worker threads. Rows are independent and deterministic; thread count changes
only scheduling. The tables always contain 64 by 64 texels. X is perceptual
roughness `x/63`, clamped to 0.045; Y is `sqrt(1-NdotV)=y/63`, with the final
view row capped at 1.57 radians to avoid a singular tangent view.

Both files begin with the 16-byte little-endian header `CLTC`, version 1,
width 64, height 64, followed by row-major RGBA float32 texels. Matrix RGBA
uses GLM column/row indices `[inverse[0][0], inverse[0][2], inverse[2][0],
inverse[2][2]]` after division by `inverse[1][1]`. Amplitude R is the unit-F
specular amplitude after division by retained physical-hemisphere mass;
G is its correspondingly normalized Schlick `pow5(1-HdotV)` moment; B is
the cosine-weighted diffuse Schlick moment; A is zero. Specular RGB combines
as `F0*R + (1-F0)*G`. Diffuse B is a hemisphere-average approximation rather
than an exact emitter-dependent Fresnel integral; numerical validation must
report its approximation error separately.

The shipped tables use 64 samples per axis and 200 maximum iterations.
`materials/ltc/provenance.json` records the exact inputs and SHA-256 hashes.
The renderer retains configured sampled integration below roughness 0.28,
where the target's distribution denominator floor flattens the specular peak.
For roughness 0.28 and above, the independent normal-incidence metallic
roughness sweep has below 3.6% maximum normalized raw specular error for its
rectangle/disk fixtures, including the runtime's 32-sided disk approximation.
This is a measured fixture result, not a bound for
every emitter/view domain. The hemisphere-average Schlick specular moment
can severely underfit opposite-view grazing emitters with low F0. Runtime
angular and material-energy guards blend the fitted Schlick residual toward
configured sampled residual integration in unsafe domains, while preserving
the analytic constant-F0 specular lobe, cosine diffuse and sheen. Geometry
and roughness guards still blend the whole response toward sampled integration.
An emitter's conservative projected angular half-extent also blends the whole
response over `1e-4` to `2e-4` radians, retaining sampled integration for thin,
distant polygons whose edge sums lose precision in float32.
Diffuse Fresnel uses
the table's moment only when an angular bound proves its accuracy; other
domains retain a sampled Schlick correction to the analytic cosine integral.
The independent near-field disk annulus includes sampled spokes, their gaps
and confidence transitions at 9, 25 and 64 samples; its geometrically eligible
hybrid paths retain the same 6% reference gate even when the fitted Schlick
residual weight is zero.

The binary writer assumes a little-endian IEEE-754 host, as used by the
Windows release build. Runtime shader matrices retain the engine's
column-major upload convention.
