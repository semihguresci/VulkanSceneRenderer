"""Small CPU checks for geometric/oracle invariants, independent of native rendering."""

import base64
import tempfile
import unittest

import numpy as np

import quality_fixtures as fixtures
import reference


class QualityReferenceTests(unittest.TestCase):
    def test_exported_gltf_winding_and_normals_agree(self):
        for variant in fixtures.VARIANTS:
            gltf = fixtures.gltf_scene(fixtures.build_scene(variant))
            payload = base64.b64decode(gltf["buffers"][0]["uri"].split(",", 1)[1])
            for mesh in gltf["meshes"]:
                for primitive in mesh["primitives"]:
                    def read(index, dtype, components):
                        accessor = gltf["accessors"][index]
                        view = gltf["bufferViews"][accessor["bufferView"]]
                        return np.frombuffer(payload, dtype=dtype,
                                             count=accessor["count"] * components,
                                             offset=view["byteOffset"]).reshape(-1, components)
                    positions = read(primitive["attributes"]["POSITION"], "<f4", 3)
                    normals = read(primitive["attributes"]["NORMAL"], "<f4", 3)
                    indices = read(primitive["indices"], "<u2", 1).reshape(-1, 3)
                    cross = np.cross(positions[indices[:, 1]] - positions[indices[:, 0]],
                                     positions[indices[:, 2]] - positions[indices[:, 0]])
                    self.assertTrue(np.all(np.sum(cross * normals[indices[:, 0]], axis=1) > 0))
                    self.assertFalse(gltf["materials"][primitive["material"]]["doubleSided"])

    def test_partition_is_independent_node_for_dynamic_stress(self):
        gltf = fixtures.gltf_scene(fixtures.build_scene("doorway"))
        self.assertEqual(gltf["nodes"][2], {"name": "Partition", "mesh": 1})
        self.assertEqual(gltf["nodes"][1]["extensions"]["KHR_lights_punctual"]["light"], 0)
        self.assertEqual(len(fixtures.gltf_scene(fixtures.build_scene("open"))["nodes"]), 2)

    def test_fixture_export_is_byte_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            first = fixtures.write_fixtures(directory)
            second = fixtures.write_fixtures(directory)
            self.assertEqual(first, second)

    def test_receiver_rois_project_to_visible_capture_region(self):
        for receiver in fixtures.build_scene()["receivers"]:
            left, top, right, bottom = receiver["pixelRoi"]
            self.assertTrue(0 <= left < right <= 960 and 0 <= top < bottom <= 640)
            self.assertEqual([right - left, bottom - top], [12, 12])

    def test_exact_rectangle_direct_integral_matches_closed_form(self):
        a, b, height = 0.4, 0.3, 2.0 - 4 * reference.EPSILON
        scene = {"quads": [], "light": {"position": [0, 2, 0], "normal": [0, -1, 0],
                                          "u": [a, 0, 0], "v": [0, 0, b],
                                          "radiance": [1, 1, 1], "range": 0}}
        expected = 2 * (a / np.sqrt(a * a + height * height) *
                        np.arctan(b / np.sqrt(a * a + height * height)) +
                        b / np.sqrt(b * b + height * height) *
                        np.arctan(a / np.sqrt(b * b + height * height)))
        np.testing.assert_allclose(reference.direct_irradiance([0, 0, 0], [0, 1, 0], scene)[0],
                                   expected, rtol=1e-12, atol=1e-12)

    def test_partition_intersection_blocks_solid_but_not_door_aperture(self):
        origin, direction = np.asarray([[-1.0, 1.0, 0]]), np.asarray([[1.0, 0, 0]])
        _, doorway_hit = reference.intersect(origin, direction,
                                             reference.prepared_quads(fixtures.build_scene("doorway")), [2])
        distance, sealed_hit = reference.intersect(origin, direction,
                                                   reference.prepared_quads(fixtures.build_scene("sealed-thin")), [2])
        self.assertEqual(doorway_hit[0], -1)
        self.assertGreaterEqual(sealed_hit[0], 0)
        self.assertAlmostEqual(distance[0], 0.985)

    def test_closed_shaded_chamber_has_zero_transport_at_every_bounce(self):
        for variant in ("sealed-thin", "sealed-thick"):
            scene = fixtures.build_scene(variant)
            for receiver in (scene["receivers"][1], scene["receivers"][3]):
                result = reference.reference_receiver(scene, receiver, samples=256, bounces=12,
                                                      seed=123, direct_order=8)
                self.assertEqual(result["totalRadiance"], [0.0, 0.0, 0.0])
                self.assertEqual(result["standardError"], [0.0, 0.0, 0.0])

    def test_doorway_shaded_receiver_is_indirect_only_and_positive(self):
        scene = fixtures.build_scene("doorway")
        result = reference.reference_receiver(scene, scene["receivers"][1], samples=4096,
                                              bounces=6, seed=123, direct_order=8)
        self.assertEqual(result["directRadiance"], [0.0, 0.0, 0.0])
        self.assertGreater(result["indirectRadiance"][0], 3 * result["standardError"][0])
        self.assertGreater(result["indirectRadiance"][1], 3 * result["standardError"][1])

    def test_open_shaded_receiver_receives_direct_source(self):
        scene = fixtures.build_scene("open")
        illumination = reference.direct_irradiance(scene["receivers"][1]["position"],
                                                   scene["receivers"][1]["normal"], scene, order=8)
        self.assertTrue(np.all(illumination > 0))


if __name__ == "__main__":
    unittest.main()
