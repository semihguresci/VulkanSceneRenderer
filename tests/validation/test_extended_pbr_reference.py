"""CPU-only invariants for the independent extended-PBR oracle; no renderer."""

import math
import unittest

import extended_pbr_reference as pbr


N = (0.0, 0.0, 1.0)
T = (1.0, 0.0, 0.0)
WHITE = (1.0, 1.0, 1.0)


def gauss_legendre(count):
    """Generate independent quadrature nodes/weights on [0, 1]."""
    nodes = []
    for index in range((count + 1) // 2):
        root = math.cos(math.pi * (index + 0.75) / (count + 0.5))
        for _ in range(30):
            previous, current = 1.0, root
            for degree in range(2, count + 1):
                previous, current = current, ((2 * degree - 1) * root * current -
                                               (degree - 1) * previous) / degree
            derivative = count * (root * current - previous) / (root * root - 1.0)
            correction = current / derivative
            root -= correction
            if abs(correction) < 1e-15:
                break
        weight = 1.0 / ((1.0 - root * root) * derivative * derivative)
        nodes.append(((1.0 - root) / 2.0, weight))
        if count - index - 1 != index:
            nodes.append(((1.0 + root) / 2.0, weight))
    return sorted(nodes)


class ExtendedPbrReferenceTests(unittest.TestCase):
    def assertVectorAlmostEqual(self, actual, expected, places=12):
        for observed, wanted in zip(actual, expected):
            self.assertAlmostEqual(observed, wanted, places=places)

    def test_principal_roughness_and_known_ndf_axis_values(self):
        self.assertEqual(pbr.anisotropic_alphas(0.5, 0.0), (0.25, 0.25))
        self.assertEqual(pbr.anisotropic_alphas(0.5, 1.0), (1.0, 0.25))
        self.assertEqual(pbr.anisotropic_alphas(0.5, 0.5), (0.4375, 0.25))
        at, ab = 0.8, 0.2
        self.assertAlmostEqual(pbr.ggx_anisotropic_ndf(N, at, ab), 1 / (math.pi * at * ab))
        self.assertAlmostEqual(pbr.ggx_anisotropic_ndf(T, at, ab), at ** 3 / (math.pi * ab))
        self.assertAlmostEqual(pbr.ggx_anisotropic_ndf((0, 1, 0), at, ab),
                               ab ** 3 / (math.pi * at))
        self.assertEqual(pbr.ggx_anisotropic_ndf((0, 0, -1), at, ab), 0.0)

    def test_ndf_projected_solid_angle_normalizes_numerically(self):
        # Integrate in spherical coordinates, independent of the oracle's
        # slope-density coordinates: integral D(h) cos(theta) domega = 1.
        nodes = gauss_legendre(160)
        phi_count = 256
        for roughness, strength in ((0.3, 0.0), (0.45, 0.85), (0.7, 1.0)):
            at, ab = pbr.anisotropic_alphas(roughness, strength)
            rings = []
            for mu, weight in nodes:
                radius = math.sqrt(1.0 - mu * mu)
                ring = math.fsum(pbr.ggx_anisotropic_ndf(
                    (radius * math.cos(phi), radius * math.sin(phi), mu), at, ab)
                    for phi in ((i + 0.5) * 2.0 * math.pi / phi_count
                                for i in range(phi_count)))
                rings.append(weight * mu * ring * 2.0 * math.pi / phi_count)
            with self.subTest(roughness=roughness, strength=strength):
                self.assertAlmostEqual(math.fsum(rings), 1.0, places=8)

    def test_specular_brdf_reciprocity_across_principal_directions(self):
        directions = ((0, 0, 1), (0.8, 0.1, 0.3), (-0.2, 0.7, 0.5), (1, 0, 1e-7))
        for roughness, strength in ((0.045, 0.0), (0.28, 0.7), (0.8, 1.0)):
            for view in directions:
                for light in directions:
                    forward = pbr.anisotropic_specular(view, light, roughness, strength,
                                                       (0.03, 0.2, 0.7))
                    backward = pbr.anisotropic_specular(light, view, roughness, strength,
                                                        (0.03, 0.2, 0.7))
                    self.assertVectorAlmostEqual(forward, backward, places=10)

    def test_visibility_and_specular_have_known_analytic_values(self):
        self.assertEqual(pbr.ggx_smith_lambda(N, 0.5, 0.25), 0.0)
        self.assertEqual(pbr.ggx_correlated_visibility(N, N, 0.5, 0.25), 0.25)
        # With NdotV=1/2, tangent-aligned V and L=N, Smith's masking
        # reduces to this scalar expression; neither NDF nor Fresnel enters.
        view = (math.sqrt(3) / 2, 0, 0.5)
        self.assertAlmostEqual(pbr.ggx_correlated_visibility(view, N, 0.5, 0.25),
                               1.0 / (1.0 + math.sqrt(1.75)))
        f0 = (0.03, 0.2, 0.7)
        at, ab = pbr.anisotropic_alphas(0.5, 0.8)
        self.assertVectorAlmostEqual(pbr.anisotropic_specular(N, N, 0.5, 0.8, f0),
                                     tuple(f / (4 * math.pi * at * ab) for f in f0))

    def test_quarter_turn_is_counterclockwise_and_texture_rotation_composes(self):
        frame = pbr.anisotropy_frame(N, T, rotation=math.pi / 2)
        self.assertVectorAlmostEqual(frame[0], (0, 1, 0))
        self.assertVectorAlmostEqual(frame[1], (-1, 0, 0))
        texture_frame = pbr.anisotropy_frame(N, T, rotation=math.pi / 2,
                                            texture_rg=(0.5, 1.0))
        self.assertVectorAlmostEqual(texture_frame[0], (-1, 0, 0))
        self.assertVectorAlmostEqual(pbr.anisotropy_frame(N, T, texture_rg=(0.5, 0.5))[0], T)

    def test_mirrored_model_and_authored_handedness_have_independent_signs(self):
        mirrored = pbr.anisotropy_frame(N, T, rotation=math.pi / 2,
                                       model_determinant=-2.0)
        self.assertVectorAlmostEqual(mirrored[0], (0, -1, 0))
        restored = pbr.anisotropy_frame(N, T, rotation=math.pi / 2,
                                       model_determinant=-2.0, tangent_sign=-1.0)
        self.assertVectorAlmostEqual(restored[0], (0, 1, 0))
        # Gram-Schmidt is required even when the supplied world tangent is
        # not orthogonal after interpolation or a nonuniform transform.
        projected = pbr.anisotropy_frame((0, 1, 1), (1, 0, 1), rotation=0.3)
        for axis in projected:
            self.assertAlmostEqual(pbr.dot(axis, axis), 1.0)
        for first, second in ((0, 1), (0, 2), (1, 2)):
            self.assertAlmostEqual(pbr.dot(projected[first], projected[second]), 0.0)

    def test_world_rotation_moves_the_anisotropic_highlight(self):
        original = pbr.anisotropy_frame(N, T)
        turned = pbr.anisotropy_frame(N, T, rotation=math.pi / 2)
        along_t, along_b = pbr.normalize((0.6, 0, 0.8)), pbr.normalize((0, 0.6, 0.8))
        at, ab = pbr.anisotropic_alphas(0.35, 0.9)
        long_axis = pbr.ggx_anisotropic_ndf(pbr.to_local(along_t, original), at, ab)
        short_axis = pbr.ggx_anisotropic_ndf(pbr.to_local(along_b, original), at, ab)
        self.assertGreater(long_axis, short_axis * 5.0)
        self.assertAlmostEqual(pbr.ggx_anisotropic_ndf(pbr.to_local(along_b, turned), at, ab),
                               long_axis)
        self.assertAlmostEqual(pbr.ggx_anisotropic_ndf(pbr.to_local(along_t, turned), at, ab),
                               short_axis)

    def test_grazing_and_smooth_limits_are_finite_without_visibility_cap(self):
        for roughness in (0.045, 0.3, 1.0):
            for strength in (0.0, 0.5, 1.0):
                for view, light in (((1, 0, 1e-12), (-1, 0, 1e-12)),
                                    ((0, 1, 1e-12), (0.2, 0, 1)),
                                    ((0, 0, 1), (1, 0, 1e-12))):
                    result = pbr.anisotropic_specular(view, light, roughness, strength)
                    self.assertTrue(all(math.isfinite(x) and x >= 0 for x in result))
        self.assertGreater(pbr.ggx_correlated_visibility((1, 0, 1e-8),
                                                         (-1, 0, 1e-8), 0.1, 0.3), 1.0)
        self.assertEqual(pbr.ggx_correlated_visibility(T, N, 0.2, 0.3), 0.0)
        self.assertEqual(pbr.anisotropic_specular(N, (0, 0, -1), 0.4, 0.5), (0, 0, 0))

    def test_small_strength_converges_to_physical_isotropic_zero(self):
        self.assertIn("renderer-legacy-shortcut-excluded", pbr.ZERO_STRENGTH_POLICY)
        view, light = (0.3, 0.6, 0.8), (-0.4, 0.2, 0.9)
        physical_zero = pbr.anisotropic_specular(view, light, 0.3, 0.0)
        small = pbr.anisotropic_specular(view, light, 0.3, 1e-6)
        for actual, expected in zip(small, physical_zero):
            self.assertLess(abs(actual - expected) / expected, 1e-9)
        # At zero strength the tangent-plane axes have no physical preference.
        rotated = pbr.anisotropy_frame(N, T, rotation=0.73)
        self.assertVectorAlmostEqual(
            pbr.anisotropic_specular(pbr.to_local(view, rotated), pbr.to_local(light, rotated),
                                     0.3, 0.0), physical_zero)

    def test_factor_texture_multiplication_preserves_authored_zero(self):
        for channel in (0.0, 0.4, 1.0):
            self.assertEqual(pbr.sample_factor(0.0, channel), 0.0)
        self.assertEqual(pbr.sample_factor(0.0), 0.0)
        self.assertEqual(pbr.sample_factor(0.3), 0.3)
        self.assertAlmostEqual(pbr.sample_factor(0.3, 0.4), 0.12)

    def test_diffuse_color_texture_is_decoded_from_srgb_before_factor(self):
        actual = pbr.sample_transmission_color((0.5, 0.25, 0.75), (0.5, 0.04045, 1.0))
        self.assertVectorAlmostEqual(actual, (0.10702057024111628,
                                             0.25 * 0.04045 / 12.92, 0.75))
        self.assertEqual(pbr.sample_transmission_color((0.5, 0.25, 0.75)), (0.5, 0.25, 0.75))

    def test_diffuse_split_moves_energy_between_signed_hemispheres(self):
        for split in (0.0, 0.25, 1.0):
            front = pbr.diffuse_bsdf(N, N, N, WHITE, WHITE,
                                    diffuse_transmission=split,
                                    fresnel_reflectance=(0.04, 0.1, 0.3))
            back = pbr.diffuse_bsdf(N, N, (0, 0, -1), WHITE, WHITE,
                                   diffuse_transmission=split,
                                   fresnel_reflectance=(0.04, 0.1, 0.3))
            # A Lambert lobe integrates to pi times its constant BSDF.
            self.assertVectorAlmostEqual(tuple(math.pi * (f + b) for f, b in zip(front, back)),
                                         (0.96, 0.9, 0.7))
            self.assertVectorAlmostEqual(front, tuple((1 - split) * x / math.pi
                                                      for x in (0.96, 0.9, 0.7)))
        self.assertEqual(pbr.diffuse_bsdf(N, N, T, WHITE, WHITE,
                                         diffuse_transmission=1), (0, 0, 0))

    def test_diffuse_metallic_and_specular_transmission_exclude_both_lobes(self):
        for metallic, transmission in ((1.0, 0.0), (0.0, 1.0), (1.0, 1.0)):
            self.assertEqual(pbr.diffuse_weights(metallic, transmission, 0.7), (0, 0))
            for light in (N, (0, 0, -1)):
                self.assertEqual(pbr.diffuse_bsdf(N, N, light, WHITE, WHITE,
                                                 metallic=metallic,
                                                 specular_transmission=transmission,
                                                 diffuse_transmission=0.7), (0, 0, 0))
        reflected, transmitted = pbr.diffuse_weights(0.2, 0.3, 0.4)
        self.assertAlmostEqual(reflected, 0.8 * 0.7 * 0.6)
        self.assertAlmostEqual(transmitted, 0.8 * 0.7 * 0.4)

    def test_diffuse_transmission_color_is_independent_of_reflection_base_color(self):
        red, green = (1, 0, 0), (0, 1, 0)
        back_red = pbr.diffuse_bsdf(N, N, (0, 0, -1), red, green, diffuse_transmission=0.6)
        back_black = pbr.diffuse_bsdf(N, N, (0, 0, -1), (0, 0, 0), green,
                                     diffuse_transmission=0.6)
        self.assertEqual(back_red, back_black)
        self.assertVectorAlmostEqual(back_red, (0, 0.6 / math.pi, 0))
        reflected = pbr.diffuse_bsdf(N, N, N, red, green, diffuse_transmission=0.6)
        self.assertVectorAlmostEqual(reflected, (0.4 / math.pi, 0, 0))

    def test_diffuse_hemisphere_policy_is_reciprocal_and_normal_orientation_independent(self):
        view, light = (0.2, 0.3, 0.8), (-0.5, 0.1, -0.7)
        arguments = dict(base_color=(0.2, 0.4, 0.8), transmission_color=(0.8, 0.1, 0.4),
                         metallic=0.1, specular_transmission=0.2, diffuse_transmission=0.6)
        result = pbr.diffuse_bsdf(N, view, light, **arguments)
        self.assertEqual(result, pbr.diffuse_bsdf(N, light, view, **arguments))
        self.assertEqual(result, pbr.diffuse_bsdf((0, 0, -1), view, light, **arguments))

    def test_invalid_inputs_are_explicit_instead_of_hidden_renderer_clamps(self):
        for roughness, strength in ((0, 0.5), (-0.1, 0.5), (0.4, 1.1), (math.nan, 0.5)):
            with self.assertRaises(ValueError):
                pbr.anisotropic_alphas(roughness, strength)
        with self.assertRaises(ValueError):
            pbr.anisotropy_frame(N, N)
        with self.assertRaises(ValueError):
            pbr.sample_factor(0.0, math.nan)

    def test_compatibility_ndf_reduces_to_existing_isotropic_cap(self):
        for roughness in (0.045, 0.3, 0.8, 1.0):
            for direction in (N, (0.4, 0.2, 0.8), T):
                half = pbr.normalize(direction)
                alpha_squared = roughness ** 4
                denominator = half[2] ** 2 * (alpha_squared - 1.0) + 1.0
                legacy = alpha_squared / max(math.pi * denominator ** 2, 1e-4)
                self.assertAlmostEqual(pbr.legacy_compatible_ndf(half, roughness, 0.0),
                                       legacy, places=11)
        # The physical oracle stays uncapped and remains a distinct profile.
        at, ab = pbr.anisotropic_alphas(0.045, 0.0)
        self.assertGreater(pbr.ggx_anisotropic_ndf(N, at, ab),
                           pbr.legacy_compatible_ndf(N, 0.045, 0.0) * 1e6)

    def test_compatibility_specular_is_continuous_at_small_positive_strength(self):
        for roughness in (0.045, 0.3, 0.8):
            for view, light in ((N, N), ((0.3, 0.2, 0.8), (-0.2, 0.3, 0.7)),
                                ((1, 0, 1e-6), (0, 1, 1e-6))):
                zero = pbr.legacy_compatible_specular(view, light, roughness, 0.0)
                positive = pbr.legacy_compatible_specular(view, light, roughness, 1e-8)
                for actual, expected in zip(positive, zero):
                    self.assertLess(abs(actual - expected) / expected, 1e-10)

    def test_compatibility_direct_keeps_specular_when_diffuse_is_split(self):
        options = dict(view_local=N, light_local=N, roughness=0.3, strength=0.5,
                       transmission_color=(0, 1, 0), metallic=0.0,
                       specular_transmission=0.3, f0=(0.1, 0.2, 0.3), radiance=(2, 3, 4))
        reflected = pbr.extended_compatible_direct(base_color=(1, 0, 0),
                                                   diffuse_transmission=1.0, **options)
        pure_specular = pbr.extended_compatible_direct(base_color=(0, 0, 0),
                                                       diffuse_transmission=0.0, **options)
        self.assertVectorAlmostEqual(reflected, pure_specular)
        # Exact backlighting is finite and uses the independent green color.
        options["light_local"] = (0, 0, -1)
        back = pbr.extended_compatible_direct(base_color=(1, 0, 0),
                                              diffuse_transmission=0.5, **options)
        self.assertVectorAlmostEqual(back, (0, (1 - 0.2) * 0.7 * 0.5 * 3 / math.pi, 0))

    def test_compatibility_disabled_branch_retains_legacy_transmission_policy(self):
        options = dict(view_local=N, light_local=N, roughness=0.4, strength=0.0,
                       base_color=WHITE, transmission_color=WHITE)
        old_without_transmission = pbr.extended_compatible_direct(specular_transmission=0,
                                                                  **options)
        old_with_transmission = pbr.extended_compatible_direct(specular_transmission=1,
                                                               **options)
        self.assertEqual(old_without_transmission, old_with_transmission)
        active = pbr.extended_compatible_direct(specular_transmission=1, strength=1e-8,
                                                **{key: value for key, value in options.items()
                                                   if key != "strength"})
        specular = pbr.legacy_compatible_specular(N, N, 0.4, 1e-8)
        self.assertVectorAlmostEqual(active, specular)

    def test_ibl_mix_retains_raw_projected_view_magnitude(self):
        # Projection onto span(T,N) is (0.6,0,0). At s=1,r=0.5, the
        # mixture is (0.5625,0,0.0625), with ratio 9 rather than 15 that
        # an incorrectly normalized intermediate projection would produce.
        actual = pbr.anisotropic_ibl_normal(N, (0.6, 0.8, 0), 0.5, 1.0, T)
        self.assertVectorAlmostEqual(actual, (9 / math.sqrt(82), 0, 1 / math.sqrt(82)))
        self.assertVectorAlmostEqual(pbr.anisotropic_ibl_normal(N, (0.6, 0, 0.8), 0, 1, T),
                                     (0.6, 0, 0.8))

    def test_ibl_disabled_preserves_original_normal_and_full_roughness_returns_normal(self):
        unnormalized = (0.0, 0.0, 2.0)
        self.assertEqual(pbr.anisotropic_ibl_normal(unnormalized, (0.2, 0.8, 0.4),
                                                   0.3, 0.0, T), unnormalized)
        self.assertVectorAlmostEqual(pbr.anisotropic_ibl_normal(unnormalized, (0.2, 0.8, 0.4),
                                                               1.0, 1.0, T), N)

    def test_ibl_parallel_direction_invalid_input_and_back_view_remain_finite_oriented(self):
        self.assertEqual(pbr.anisotropic_ibl_normal(N, (0, 1, 0), 0, 1, T), N)
        for view, tangent in (((math.nan, 0, 0), T), ((0, 0, 0), T),
                              ((1e-6, 1, 1e-6), T), ((0.3, 0.4, -0.5), T),
                              ((0.3, 0.4, 0.5), (math.nan, 0, 0))):
            result = pbr.anisotropic_ibl_normal(N, view, 0.045, 1, tangent)
            self.assertTrue(all(math.isfinite(x) for x in result))
            self.assertAlmostEqual(pbr.dot(result, result), 1)
            self.assertGreaterEqual(pbr.dot(result, N), 0)

    def test_extended_diffuse_weights_keep_legacy_disabled_and_black_color_energy(self):
        self.assertEqual(pbr.extended_diffuse_weights(0, 0, 0.8), (1, 0))
        self.assertVectorAlmostEqual((*pbr.extended_diffuse_weights(0.5, 0.3, 0.2), 0),
                                     (0.56, 0.24, 0))
        self.assertVectorAlmostEqual((*pbr.extended_diffuse_weights(0, 0.3, 0.2), 0),
                                     (0.56, 0.24, 0))


if __name__ == "__main__":
    unittest.main()
