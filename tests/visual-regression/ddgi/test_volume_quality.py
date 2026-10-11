"""CPU verification of independent room-volume fixtures and native acceptance checks."""
import copy
import math
from pathlib import Path
import unittest

from quality_fixtures import build_scene
import run_volume_quality as volume
import run_quality as quality
import run_material_quality as material
import run_runtime as runtime
from run_runtime import validate_selected_probes
from test_quality import expected_motion
from test_material_quality import live_samples


def cases(coarse=.08, detail=.002, open_detail=1., lit_detail=.9):
    result=[]
    for technique in volume.TECHNIQUES:
        for variant in ("sealed-thin", "open"):
            for mode in ("coarse", "detail"):
                shaded = (coarse if mode=="coarse" else detail) if variant=="sealed-thin" else (1. if mode=="coarse" else open_detail)
                result.append({"name":f"{technique}-{variant}-{mode}","passed":True,"frames":[{"receivers":{
                    "shaded-back":{"estimatedLuminance":shaded},
                    "lit-back":{"estimatedLuminance":1. if mode=="coarse" else lit_detail}}}]})
    return result


class VolumeQualityTests(unittest.TestCase):
    def test_shaded_room_has_full_blend_band_padding(self):
        volume.check_detail_padding()
        scene=build_scene("sealed-thin")
        for receiver in scene["receivers"]:
            if receiver["name"].startswith("shaded"):
                point=receiver["position"]
                for axis in range(3):
                    low=volume.DETAIL["origin"][axis]
                    high=low+volume.DETAIL["spacing"][axis]*(volume.DETAIL["counts"][axis]-1)
                    self.assertGreaterEqual(point[axis]-low,volume.DETAIL["spacing"][axis])
                    self.assertGreaterEqual(high-point[axis],volume.DETAIL["spacing"][axis])
        bad=copy.deepcopy(volume.DETAIL);bad["origin"][2]=-2.
        with self.assertRaisesRegex(AssertionError,"pad"):
            volume.check_detail_padding(bad)

    def test_fixture_is_closed_thin_solid_with_front_facing_winding(self):
        scene=build_scene("sealed-thin")
        self.assertEqual(scene["partitionThickness"],.03)
        faces=[quad for quad in scene["quads"] if quad["material"]=="partition"]
        self.assertEqual(len(faces),6)
        self.assertEqual({tuple(face["normal"]) for face in faces},
                         {(1.,0.,0.),(-1.,0.,0.),(0.,1.,0.),(0.,-1.,0.),(0.,0.,1.),(0.,0.,-1.)})
        self.assertFalse(any(quad["material"]=="partition" for quad in build_scene("open")["quads"]))

    def test_detail_preset_preserves_identity_and_all_primary_settings(self):
        seed={"version":1,"sceneIdentity":"scene.gltf|scale=1","settings":{
            "volume":copy.deepcopy(volume.COARSE),"additionalVolumes":[],"volumeBlendCells":0.,
            "resources":{"updates":{"maxProbesPerFrame":256}},"intensity":1.}}
        detail=volume.detail_preset(seed)
        self.assertEqual(detail["sceneIdentity"],seed["sceneIdentity"])
        self.assertEqual(detail["settings"]["volume"],seed["settings"]["volume"])
        self.assertEqual(detail["settings"]["resources"],seed["settings"]["resources"])
        self.assertEqual(detail["settings"]["additionalVolumes"],[volume.DETAIL])
        self.assertEqual(seed["settings"]["additionalVolumes"],[])
        self.assertEqual(math.prod(volume.COARSE["counts"])+math.prod(volume.DETAIL["counts"]),3600)

    def test_commands_keep_matching_scene_camera_exposure_budget_and_primary_lattice(self):
        scene=volume.prepared_scene(build_scene("sealed-thin"));scene["gltf"]="scene.gltf"
        args=(Path("renderer.exe"),Path("captures"),scene,"case",160)
        coarse=quality.command_for(*args,save_preset=Path("seed.json"))
        detail=quality.command_for(*args,preset=Path("detail.json"))
        self.assertEqual(coarse[:-2],detail[:-2])
        self.assertEqual(coarse[-2:],["--ddgi-save-preset","seed.json"])
        self.assertEqual(detail[-2:],["--ddgi-preset","detail.json"])
        self.assertEqual(coarse[coarse.index("--ddgi-counts")+1:coarse.index("--ddgi-counts")+4],["9","5","5"])

    def test_measurable_leak_reduction_retains_legitimate_lit_chamber(self):
        result=volume.compare(cases())
        self.assertEqual(set(result),set(volume.TECHNIQUES))
        self.assertAlmostEqual(result["deferred-raster"]["leakReductionFraction"],.975)
        self.assertAlmostEqual(result["forward-raster"]["litSideRetainedFraction"],.9)

    def test_comparison_rejects_unimproved_leak_or_overly_dark_lit_surfaces(self):
        for arguments in ({"detail":.079},{"detail":.02},{"lit_detail":.5},{"open_detail":.2},{"coarse":0.}):
            with self.subTest(arguments=arguments),self.assertRaises(AssertionError):
                volume.compare(cases(**arguments))

    def test_failed_or_missing_route_cannot_pass_comparison(self):
        records=cases();records[-1]["passed"]=False
        with self.assertRaisesRegex(AssertionError,"Missing matched"):
            volume.compare(records)

    def test_selected_telemetry_rejects_duplicate_fractional_and_out_of_range_indices(self):
        gi={"selectedProbes":[0,7],"updatedProbes":2,"firstProbe":0,"nextProbe":1}
        self.assertTrue(validate_selected_probes(gi,8))
        self.assertFalse(validate_selected_probes({"updatedProbes":2},8))
        for selected in ([0,0],[0,8],[0,1.5],[0,True],None,[0]):
            bad={**gi,"selectedProbes":selected}
            with self.subTest(selected=selected),self.assertRaises(AssertionError):
                validate_selected_probes(bad,8)

    def test_motion_accepts_priority_batches_and_uses_the_fairness_completion_bound(self):
        record=expected_motion()
        for sample in record["frames"]:
            gi=sample["ddgi"]
            gi["selectedProbes"]=[(gi["firstProbe"]+2*i)%637 for i in range(256)]
        next(sample for sample in record["frames"] if sample["frame"]==44)["ddgi"]["refreshRemainingProbes"]=1
        checks=quality.check_motion(record)
        self.assertEqual(checks["freshSweepSubmittedFrames"],5)
        self.assertTrue(checks["priorityScheduling"])
        bad=copy.deepcopy(record)
        next(sample for sample in bad["frames"] if sample["frame"]==124)["ddgi"]["refreshRemainingProbes"]=1
        with self.assertRaisesRegex(AssertionError,"stopped scene"):
            quality.check_motion(bad)

    def test_live_pause_disable_priority_batches_preserve_the_submitted_cursor(self):
        samples=live_samples();cursor=0
        for sample in samples:
            gi=sample["ddgi"]
            if gi["updatedProbes"]:
                gi["firstProbe"]=cursor
                gi["selectedProbes"]=[(cursor+2*i)%637 for i in range(256)]
                cursor=(cursor+128)%637
                gi["nextProbe"]=cursor
            else:
                gi["selectedProbes"]=[]
            gi["schedulingCursor"]=cursor
        self.assertEqual(material.check_live_frames(samples)["finalSchedulingCursor"],cursor)
        bad=copy.deepcopy(samples);bad[2]["ddgi"]["selectedProbes"][1]=bad[2]["ddgi"]["selectedProbes"][0]
        with self.assertRaisesRegex(AssertionError,"duplicate"):
            material.check_live_frames(bad)

    def test_runtime_priority_contract_accepts_noncontiguous_updates_and_rejects_cursor_restart(self):
        case=runtime.Case("priority",frame_count=2)
        frames=[]
        for frame in (1,2):
            first=(frame-1)*64
            frames.append({"frame":frame,"ddgi":{"enabled":True,"supported":True,"active":True,
                "status":"active","contentEpoch":1,"updateFrame":frame,"updatedProbes":128,
                "tracedRays":16384,"firstProbe":first,"nextProbe":frame*64,
                "selectedProbes":[(first+2*i)%343 for i in range(128)]}})
        runtime.check_frames(case,frames)
        bad=copy.deepcopy(frames);bad[1]["ddgi"]["firstProbe"]=0;bad[1]["ddgi"]["selectedProbes"][0]=0
        with self.assertRaisesRegex(AssertionError,"fairness cursor"):
            runtime.check_frames(case,bad)

    def test_compact_capture_keeps_optional_new_arrays_and_omits_legacy_missing_arrays(self):
        modern=runtime.compact_frame(1,{"ddgi":{"selectedProbes":[3,9]}})
        self.assertEqual(modern["ddgi"]["selectedProbes"],[3,9])
        self.assertNotIn("selectedProbes",runtime.compact_frame(1,{"ddgi":{}})["ddgi"])


if __name__=="__main__":
    unittest.main()
