"""CPU checks for the native BIM leakage geometry and unit conversions."""

import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

import numpy as np

import bim_leakage_fixtures as fixtures
from reference import intersect, prepared_quads


class BimLeakageFixturesTests(unittest.TestCase):
    def test_every_variant_keeps_both_providers_and_reproducible_world_geometry(self):
        with TemporaryDirectory() as temporary:
            output = Path(temporary)
            manifest = fixtures.write_fixtures(output)
            by_name = {scene["name"]: scene for scene in manifest["fixtures"]}
            self.assertEqual(len(by_name), 2 * len(fixtures.VARIANTS))
            self.assertEqual(json.loads((output / "bim-leakage-fixture-manifest.json").read_text()), manifest)
            for variant in fixtures.VARIANTS:
                metre, millimetre = (by_name[f"bim-{variant}-{units}"] for units in fixtures.UNIT_SCALES)
                self.assertEqual(metre["gltfSha256"], millimetre["gltfSha256"])
                self.assertNotEqual(metre["bimSha256"], millimetre["bimSha256"])
                for key in ("camera", "light", "quads", "receivers", "feature", "bimBounds"):
                    self.assertEqual(metre[key], millimetre[key])
                for scene in (metre, millimetre):
                    for key in ("gltf", "bim"):
                        self.assertEqual(hashlib.sha256((output / scene[key]).read_bytes()).hexdigest(),
                                         scene[f"{key}Sha256"])
                    self.assertEqual({quad["provider"] for quad in scene["quads"]}, {"bim", "gltf"})
                    self.assertEqual({item["provider"] for item in scene["receivers"]}, {"bim", "gltf"})

    def test_ifcx_coordinates_round_trip_without_flipping_normals(self):
        for variant in fixtures.VARIANTS:
            for units, scale in fixtures.UNIT_SCALES.items():
                with self.subTest(variant=variant, units=units):
                    scene = fixtures.build_scene(variant, units)
                    for element in fixtures.ifcx_scene(scene)["data"]:
                        attrs = element["attributes"]
                        mesh = attrs["usd::usdgeom::mesh"]
                        quads = [quad for quad in scene["quads"] if quad.get("bimElement") == element["path"]]
                        world = np.array([[x * scale, z * scale, -y * scale] for x, y, z in mesh["points"]])
                        np.testing.assert_allclose(world, np.reshape([quad["corners"] for quad in quads], (-1, 3)),
                                                   atol=1e-12)
                        self.assertEqual(mesh["faceVertexCounts"], [4] * len(quads))
                        self.assertEqual(mesh["faceVertexIndices"], list(range(len(world))))
                        self.assertEqual(attrs["gltf::material"]["doubleSided"], element["path"] == "partition-sheet")
                        for face, quad in zip(world.reshape(-1, 4, 3), quads):
                            normal = np.cross(face[1] - face[0], face[2] - face[0])
                            np.testing.assert_allclose(normal / np.linalg.norm(normal), quad["normal"], atol=1e-12)

    def test_partition_masks_separate_open_gap_and_closed_geometry(self):
        rays = [[-1, 1.4, -0.5], [-1, 1.4, -0.8], [1, 1.4, -0.5]]
        directions = [[1, 0, 0], [1, 0, 0], [-1, 0, 0]]
        for variant, expected in {"open": [False] * 3, "gap": [False, True, False],
                                  **{closed: [True] * 3 for closed in ("sheet", "sealed", "overlap", "mixed")}}.items():
            with self.subTest(variant=variant):
                scene = fixtures.build_scene(variant, "metre")
                divider = {"quads": [quad for quad in scene["quads"] if quad["material"] == "partition"]}
                _, hits = intersect(rays, directions, prepared_quads(divider))
                self.assertEqual((hits >= 0).tolist(), expected)

    def test_only_mixed_glass_uses_transmission_and_reference_scope_is_explicit(self):
        for variant in fixtures.VARIANTS:
            scene = fixtures.build_scene(variant, "metre")
            gltf = fixtures.primary_gltf(scene)
            extension = "KHR_materials_transmission"
            self.assertEqual(extension in gltf["extensionsUsed"], variant == "mixed")
            self.assertEqual(scene["referencePolicy"]["supported"], variant not in ("sheet", "mixed"))
            if variant == "mixed":
                glass = next(mat for mat in gltf["materials"] if extension in mat.get("extensions", {}))
                self.assertTrue(glass["doubleSided"])
                self.assertEqual(glass["extensions"][extension]["transmissionFactor"], 1)
                self.assertEqual(glass["extensions"]["KHR_materials_volume"]["thicknessFactor"], 0)
