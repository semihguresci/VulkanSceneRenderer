"""Independent binary64 BSDF oracle for future extended-PBR GPU fixtures.

All vectors point away from the surface. Local GGX coordinates are (T, B, N).
Roughness is the effective perceptual roughness supplied by the caller. The
physical GGX functions add no renderer roughness floor, NDF cap, visibility cap,
or alpha rule; strength zero evaluates physical isotropic GGX. Separately named
legacy_compatible_* functions document the renderer's approximation profile
without changing that physical reference.

The NDF is obtained from a normalized microfacet slope probability density and
its solid-angle Jacobian. Visibility is obtained from Smith Lambda, rather than
the optimized length/product expressions used in raster shaders.

References:
https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_anisotropy
https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_diffuse_transmission
"""

from __future__ import annotations

import math
from typing import Sequence

Vec3 = tuple[float, float, float]
Frame = tuple[Vec3, Vec3, Vec3]
ZERO_STRENGTH_POLICY = "physical-isotropic-ggx; renderer-legacy-shortcut-excluded"


def _unit_interval(value: float, name: str) -> float:
    value = float(value)
    if not math.isfinite(value) or not 0.0 <= value <= 1.0:
        raise ValueError(f"{name} must be finite and in [0, 1]")
    return value


def _color(value: Sequence[float], name: str) -> Vec3:
    if len(value) != 3:
        raise ValueError(f"{name} must contain three channels")
    return tuple(_unit_interval(channel, name) for channel in value)


def dot(a: Sequence[float], b: Sequence[float]) -> float:
    return math.fsum(x * y for x, y in zip(a, b))


def normalize(value: Sequence[float]) -> Vec3:
    if len(value) != 3 or not all(math.isfinite(x) for x in value):
        raise ValueError("direction must contain three finite coordinates")
    length = math.hypot(*value)
    if length == 0.0:
        raise ValueError("direction must be nonzero")
    return tuple(x / length for x in value)


def _cross(a: Vec3, b: Vec3) -> Vec3:
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _positive_alphas(alpha_t: float, alpha_b: float) -> tuple[float, float]:
    if not all(math.isfinite(a) and 0.0 < a <= 1.0 for a in (alpha_t, alpha_b)):
        raise ValueError("alpha roughness must be finite and in (0, 1]")
    return float(alpha_t), float(alpha_b)


def anisotropic_alphas(roughness: float, strength: float) -> tuple[float, float]:
    """Return principal alpha roughness; zero roughness is a delta distribution."""
    roughness = _unit_interval(roughness, "roughness")
    strength = _unit_interval(strength, "strength")
    if roughness == 0.0:
        raise ValueError("zero roughness requires a delta distribution")
    alpha = roughness ** 2
    return alpha + (1.0 - alpha) * strength ** 2, alpha


def _log_add(a: float, b: float) -> float:
    maximum = max(a, b)
    if maximum == -math.inf:
        return maximum
    return maximum + math.log1p(math.exp(min(a, b) - maximum))


def ggx_anisotropic_ndf(half_local: Sequence[float], alpha_t: float,
                        alpha_b: float) -> float:
    """Projected-area NDF, from the elliptical GGX slope density.

    p(sx,sy) = 1 / [pi at ab (1 + (sx/at)^2 + (sy/ab)^2)^2].
    Slopes map to solid angle with |ds/domega| = h_n^-3, and the
    projected-area definition supplies one more h_n^-1: D = p / h_n^4.
    Logarithms keep that change of variables stable near the horizon.
    """
    alpha_t, alpha_b = _positive_alphas(alpha_t, alpha_b)
    hx, hy, hz = normalize(half_local)
    if hz < 0.0:
        return 0.0
    scaled_radius = math.hypot(hx / alpha_t, hy / alpha_b)
    log_scale = -math.log(math.pi) - math.log(alpha_t) - math.log(alpha_b)
    if hz == 0.0:
        # Analytic horizon limit of the slope-density/Jacobian product.
        return math.exp(log_scale - 4.0 * math.log(scaled_radius))
    log_hz = math.log(hz)
    log_radius = (math.log(scaled_radius) - log_hz
                  if scaled_radius else -math.inf)
    log_density = log_scale - 2.0 * _log_add(0.0, 2.0 * log_radius)
    return math.exp(log_density - 4.0 * log_hz)


def ggx_smith_lambda(direction_local: Sequence[float], alpha_t: float,
                     alpha_b: float) -> float:
    """Smith masking Lambda; a direction on/below the horizon has no support."""
    alpha_t, alpha_b = _positive_alphas(alpha_t, alpha_b)
    x, y, z = normalize(direction_local)
    if z <= 0.0:
        return math.inf
    slope_ratio = math.hypot(alpha_t * x, alpha_b * y) / z
    # Rationalized sqrt(1 + ratio^2) - 1 avoids cancellation near normal.
    return 0.5 * slope_ratio * (slope_ratio / (math.hypot(1.0, slope_ratio) + 1.0))


def ggx_correlated_visibility(view_local: Sequence[float],
                             light_local: Sequence[float], alpha_t: float,
                             alpha_b: float) -> float:
    """Height-correlated Smith G2 divided by 4 cos(view) cos(light).

    Exactly tangent/back-side directions return zero by explicit support
    convention; positive grazing directions are evaluated without a cap.
    """
    view, light = normalize(view_local), normalize(light_local)
    _positive_alphas(alpha_t, alpha_b)
    if view[2] <= 0.0 or light[2] <= 0.0:
        return 0.0
    lambda_sum = math.fsum((1.0,
                           ggx_smith_lambda(view, alpha_t, alpha_b),
                           ggx_smith_lambda(light, alpha_t, alpha_b)))
    geometry = 1.0 / lambda_sum
    return geometry / (4.0 * view[2] * light[2])


def anisotropic_specular(view_local: Sequence[float], light_local: Sequence[float],
                         roughness: float, strength: float,
                         f0: Sequence[float] = (0.04, 0.04, 0.04)) -> Vec3:
    """Return the microfacet reflection BRDF, without radiance or incident cosine."""
    view, light = normalize(view_local), normalize(light_local)
    alpha_t, alpha_b = anisotropic_alphas(roughness, strength)
    f0 = _color(f0, "f0")
    if view[2] <= 0.0 or light[2] <= 0.0:
        return (0.0, 0.0, 0.0)
    half = normalize(tuple(v + l for v, l in zip(view, light)))
    fresnel_tail = (1.0 - min(1.0, max(0.0, dot(view, half)))) ** 5
    distribution = ggx_anisotropic_ndf(half, alpha_t, alpha_b)
    visibility = ggx_correlated_visibility(view, light, alpha_t, alpha_b)
    return tuple((f + (1.0 - f) * fresnel_tail) * distribution * visibility
                 for f in f0)


def anisotropy_frame(normal: Sequence[float], tangent: Sequence[float],
                     rotation: float = 0.0,
                     texture_rg: Sequence[float] | None = None,
                     tangent_sign: float = 1.0,
                     model_determinant: float = 1.0) -> Frame:
    """Build a signed orthonormal (T, B, N), then rotate CCW in tangent space.

    Inputs are world-space normal/tangent, before any anisotropy rotation.
    Tangent handedness includes the sign of the model determinant. A degenerate
    texture direction uses +T as an explicit finite-input fallback policy.
    """
    if not all(math.isfinite(x) for x in (rotation, tangent_sign, model_determinant)):
        raise ValueError("frame parameters must be finite")
    if tangent_sign == 0.0 or model_determinant == 0.0:
        raise ValueError("frame handedness must be nonzero")
    normal, tangent = normalize(normal), normalize(tangent)
    projection = dot(normal, tangent)
    tangent = normalize(tuple(t - projection * n for t, n in zip(tangent, normal)))
    handedness = math.copysign(1.0, tangent_sign) * math.copysign(1.0, model_determinant)
    bitangent = tuple(handedness * x for x in _cross(normal, tangent))
    if texture_rg is None:
        dx, dy = 1.0, 0.0
    else:
        if len(texture_rg) != 2:
            raise ValueError("anisotropy direction must contain RG")
        dx, dy = (2.0 * _unit_interval(x, "texture RG") - 1.0 for x in texture_rg)
        length = math.hypot(dx, dy)
        dx, dy = (dx / length, dy / length) if length else (1.0, 0.0)
    c, s = math.cos(rotation), math.sin(rotation)
    dx, dy = c * dx - s * dy, s * dx + c * dy
    rotated_tangent = tuple(dx * t + dy * b for t, b in zip(tangent, bitangent))
    rotated_bitangent = tuple(-dy * t + dx * b for t, b in zip(tangent, bitangent))
    return normalize(rotated_tangent), normalize(rotated_bitangent), normal


def to_local(direction: Sequence[float], frame: Frame) -> Vec3:
    direction = normalize(direction)
    return tuple(dot(direction, axis) for axis in frame)


def sample_factor(factor: float, texture_channel: float | None = None) -> float:
    """Multiply a linear factor by its selected channel; absent texture means 1."""
    factor = _unit_interval(factor, "factor")
    channel = 1.0 if texture_channel is None else _unit_interval(texture_channel, "channel")
    return factor * channel


def sample_transmission_color(color_factor: Sequence[float],
                               texture_rgb: Sequence[float] | None = None) -> Vec3:
    """Multiply color factor by a decoded sRGB transmission-color texture."""
    factor = _color(color_factor, "color factor")
    if texture_rgb is None:
        return factor
    texel = _color(texture_rgb, "texture RGB")
    linear = tuple(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
                   for c in texel)
    return tuple(f * c for f, c in zip(factor, linear))


def diffuse_weights(metallic: float, specular_transmission: float,
                     diffuse_transmission: float) -> tuple[float, float]:
    """Residual dielectric diffuse reflection/transmission shares, before Fresnel."""
    metallic = _unit_interval(metallic, "metallic")
    specular_transmission = _unit_interval(specular_transmission, "specular transmission")
    diffuse_transmission = _unit_interval(diffuse_transmission, "diffuse transmission")
    residual = (1.0 - metallic) * (1.0 - specular_transmission)
    return residual * (1.0 - diffuse_transmission), residual * diffuse_transmission


def diffuse_bsdf(normal: Sequence[float], view: Sequence[float],
                  light: Sequence[float], base_color: Sequence[float],
                  transmission_color: Sequence[float], metallic: float = 0.0,
                  specular_transmission: float = 0.0,
                  diffuse_transmission: float = 0.0,
                  fresnel_reflectance: Sequence[float] = (0.0, 0.0, 0.0)) -> Vec3:
    """Lambert reflection/BTDF with signed hemispheres and explicit energy shares.

    Fresnel is supplied by the caller's layering model, independent of the
    diffuse split. The returned BSDF excludes incident cosine and radiance.
    It never changes coverage, opacity, ray visibility, or specular reflection.
    """
    normal, view, light = normalize(normal), normalize(view), normalize(light)
    base_color = _color(base_color, "base color")
    transmission_color = _color(transmission_color, "transmission color")
    fresnel = _color(fresnel_reflectance, "Fresnel reflectance")
    reflection, transmission = diffuse_weights(metallic, specular_transmission,
                                               diffuse_transmission)
    hemisphere = dot(normal, view) * dot(normal, light)
    if hemisphere == 0.0:
        return (0.0, 0.0, 0.0)
    color, weight = ((base_color, reflection) if hemisphere > 0.0
                     else (transmission_color, transmission))
    return tuple((1.0 - f) * weight * c / math.pi for f, c in zip(fresnel, color))


# Renderer compatibility model. Keep the physical functions above independent:
# this profile intentionally has the old roughness floor, capped distribution,
# Schlick masking approximation, and final 4*Nv*Nl denominator floor.
def legacy_compatible_ndf(half_local: Sequence[float], roughness: float,
                          strength: float) -> float:
    roughness = max(0.045, _unit_interval(roughness, "roughness"))
    alpha_t, alpha_b = anisotropic_alphas(roughness, strength)
    physical_distribution = ggx_anisotropic_ndf(half_local, alpha_t, alpha_b)
    # Obtain the cap as a bound on the physical NDF, independently of the
    # shader's optimized at*ab*q denominator expression.
    return min(physical_distribution, alpha_t * alpha_b / 1e-4)


def legacy_compatible_specular(view_local: Sequence[float],
                                light_local: Sequence[float], roughness: float,
                                strength: float,
                                f0: Sequence[float] = (0.04, 0.04, 0.04)) -> Vec3:
    """Capped elliptical GGX plus legacy isotropic Schlick masking, as a BRDF."""
    view, light = normalize(view_local), normalize(light_local)
    roughness = max(0.045, _unit_interval(roughness, "roughness"))
    strength = _unit_interval(strength, "strength")
    f0 = _color(f0, "f0")
    if view[2] <= 0.0 or light[2] <= 0.0:
        return (0.0, 0.0, 0.0)
    half = normalize(tuple(v + l for v, l in zip(view, light)))
    k = (roughness + 1.0) ** 2 / 8.0
    geometry = math.prod(w[2] / max(w[2] * (1.0 - k) + k, 1e-4)
                         for w in (view, light))
    distribution = legacy_compatible_ndf(half, roughness, strength)
    scale = distribution * geometry / max(4.0 * view[2] * light[2], 1e-4)
    fresnel_tail = (1.0 - min(1.0, max(0.0, dot(view, half)))) ** 5
    return tuple((f + (1.0 - f) * fresnel_tail) * scale for f in f0)


def extended_compatible_direct(
        view_local: Sequence[float], light_local: Sequence[float], roughness: float,
        strength: float, base_color: Sequence[float], transmission_color: Sequence[float],
        metallic: float = 0.0, specular_transmission: float = 0.0,
        diffuse_transmission: float = 0.0, f0: Sequence[float] = (0.04, 0.04, 0.04),
        radiance: Sequence[float] = (1.0, 1.0, 1.0)) -> Vec3:
    """Base direct-light oracle including cosine and arbitrary nonnegative radiance.

    Both new features disabled deliberately preserves the renderer's legacy
    omission of specular-transmission diffuse scaling. For active features,
    both diffuse lobes carry the (1-specular_transmission) factor. Back-side
    Fresnel follows the Khronos punctual policy: mirror L across N before H.
    Clearcoat and sheen are outside this base-only oracle.
    """
    view, light = normalize(view_local), normalize(light_local)
    strength = _unit_interval(strength, "strength")
    diffuse_transmission = _unit_interval(diffuse_transmission, "diffuse transmission")
    f0 = _color(f0, "f0")
    if len(radiance) != 3 or not all(math.isfinite(c) and c >= 0.0 for c in radiance):
        raise ValueError("radiance must contain three finite nonnegative channels")
    if view[2] <= 0.0 or light[2] == 0.0:
        return (0.0, 0.0, 0.0)
    reflected_light = light if light[2] > 0.0 else (light[0], light[1], -light[2])
    half = normalize(tuple(v + l for v, l in zip(view, reflected_light)))
    fresnel_tail = (1.0 - min(1.0, max(0.0, dot(view, half)))) ** 5
    fresnel = tuple(f + (1.0 - f) * fresnel_tail for f in f0)
    active = strength > 0.0 or diffuse_transmission > 0.0
    diffuse = diffuse_bsdf((0.0, 0.0, 1.0), view, light, base_color, transmission_color,
                           metallic, specular_transmission if active else 0.0,
                           diffuse_transmission, fresnel)
    specular = legacy_compatible_specular(view, light, roughness, strength, f0)
    return tuple((d + s) * r * abs(light[2]) for d, s, r in zip(diffuse, specular, radiance))


def extended_diffuse_weights(strength: float, diffuse_transmission: float,
                              specular_transmission: float) -> tuple[float, float]:
    """Feature-active diffuse shares; disabled reflection retains legacy weight 1."""
    strength = _unit_interval(strength, "strength")
    reflection, transmission = diffuse_weights(0.0, specular_transmission,
                                               diffuse_transmission)
    return (reflection if strength > 0.0 or diffuse_transmission > 0.0 else 1.0,
            transmission)


def anisotropic_ibl_normal(normal: Sequence[float], view: Sequence[float],
                            roughness: float, strength: float,
                            tangent: Sequence[float]) -> Vec3:
    """Khronos bent-normal IBL heuristic, independent of the physical BRDF oracle.

    The bitangent-constrained normal is the orthogonal projection of V into
    span(T,N), kept unnormalized before blending. Strength zero preserves the
    caller's original normal exactly, including its magnitude. This changes
    reflection direction only; the caller retains the material roughness mip.
    """
    strength = _unit_interval(strength, "strength")
    if strength == 0.0:
        return tuple(normal)
    roughness = _unit_interval(roughness, "roughness")

    def unit_or(value, fallback):
        try:
            return normalize(value)
        except ValueError:
            return fallback

    normal = unit_or(normal, (0.0, 1.0, 0.0))
    view = unit_or(view, normal)
    axis = (0.0, 1.0, 0.0) if abs(normal[1]) < 0.999 else (1.0, 0.0, 0.0)
    fallback_tangent = normalize(_cross(axis, normal))
    tangent = unit_or(tangent, fallback_tangent)
    normal_projection = dot(normal, tangent)
    tangent = unit_or(tuple(t - n * normal_projection for t, n in zip(tangent, normal)),
                       fallback_tangent)
    bitangent = _cross(normal, tangent)
    excluded = dot(view, bitangent)
    projected = tuple(v - b * excluded for v, b in zip(view, bitangent))
    blend = (1.0 - strength * (1.0 - roughness)) ** 4
    mixed = tuple((1.0 - blend) * v + blend * n for v, n in zip(projected, normal))
    result = unit_or(mixed, normal)
    return tuple(-x for x in result) if dot(result, normal) < 0.0 else result
