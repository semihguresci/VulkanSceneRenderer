import copy
import unittest

import run_extensions as extensions


def sample(frame, moved=False, asynchronous=False):
    return {"frame": frame, "ddgi": {
        "active": True, "historyResets": 1, "totalAllocatedBytes": 10000,
        "settings": {"origin": [-4., -2., -2.5]},
        "scroll": {"count": int(moved), "preservedProbes": 728 if moved else 0,
                   "origin": [-3., -2., -2.5] if moved else [-4., -2., -2.5]},
        "selectedProbes": [0, 1], "updatedProbes": 2, "tracedRays": 64,
        "schedulingCursor": 2, "refreshRemainingProbes": 0,
        "asyncActive": asynchronous, "asyncSubmittedUpdates": frame,
        "asyncPublishedUpdates": max(0, frame - 1), "paused": False,
        "updateFrame": frame}}


def lifecycle():
    result = [sample(frame, asynchronous=True) for frame in [1, 16, 32, 34, 36, 40, 41, 64]]
    for item in result:
        if 34 <= item["frame"] <= 40:
            item["ddgi"].update(paused=True, selectedProbes=[], updatedProbes=0,
                                tracedRays=0, updateFrame=33,
                                asyncSubmittedUpdates=33, asyncPublishedUpdates=33)
    return result


class ExtensionsHarnessTests(unittest.TestCase):
    def test_async_geometry_requires_invalidation_and_a_completed_refresh(self):
        samples = [sample(1, asynchronous=True), sample(20, asynchronous=True), sample(64, asynchronous=True)]
        for item in samples:
            item["ddgi"]["contentEpoch"] = 1 if item["frame"] == 1 else 2
        extensions.check_async_geometry(samples)
        for change in ("unchanged", "never_refreshed", "still_invalidating"):
            invalid = copy.deepcopy(samples)
            if change == "unchanged":
                for item in invalid:
                    item["ddgi"]["contentEpoch"] = 1
            elif change == "never_refreshed":
                invalid[-1]["ddgi"]["refreshRemainingProbes"] = 1
            else:
                invalid[-1]["ddgi"]["contentEpoch"] = 3
            with self.assertRaises(AssertionError):
                extensions.check_async_geometry(invalid)

    def test_detail_requires_tight_selected_bounds_and_retains_primary(self):
        gi = sample(48)["ddgi"]
        gi["selectedProbes"] = [0, 819]
        gi["settings"].update(spacing=[1., 1., 1.], counts=[9, 7, 13],
            additionalVolumes=[{"origin": [-.515, -.5, -2.5], "spacing": [.5, .5, .5], "counts": [4, 9, 9]}])
        extensions.check_detail(gi, [-.015, 0., -2.], [.015, 2.8, 1.])
        for change in ("whole_scene", "primary", "freshness", "primary_only"):
            invalid = copy.deepcopy(gi)
            if change == "whole_scene":
                invalid["settings"]["additionalVolumes"][0]["origin"][0] -= 1
            elif change == "primary":
                invalid["settings"]["counts"][0] += 1
            elif change == "freshness":
                invalid["refreshRemainingProbes"] = 1
            else:
                invalid["selectedProbes"] = [0, 1]
            with self.assertRaises(AssertionError):
                extensions.check_detail(invalid, [-.015, 0., -2.], [.015, 2.8, 1.])

    def test_single_cell_scroll_preserves_history_and_configuration(self):
        extensions.check_scroll([sample(1), sample(64, moved=True)])

    def test_scroll_cannot_pass_with_a_grid_that_was_already_moved(self):
        with self.assertRaisesRegex(AssertionError, "before"):
            extensions.check_scroll([sample(1, moved=True), sample(64, moved=True)])

    def test_scroll_must_wait_for_the_scripted_cell_crossing(self):
        with self.assertRaisesRegex(AssertionError, "cell crossing"):
            extensions.check_scroll([sample(1), sample(32, moved=True), sample(64, moved=True)])

    def test_scroll_cannot_claim_preservation_after_reallocation_or_reset(self):
        for key in ("historyResets", "totalAllocatedBytes"):
            samples = [sample(1), sample(64, moved=True)]
            samples[-1]["ddgi"][key] += 1
            with self.assertRaises(AssertionError):
                extensions.check_scroll(samples)

    def test_scroll_requires_exact_overlap_and_runtime_origin(self):
        for key, value in (("preservedProbes", 819), ("origin", [-4., -2., -2.5])):
            samples = [sample(1), sample(64, moved=True)]
            samples[-1]["ddgi"]["scroll"][key] = value
            with self.assertRaises(AssertionError):
                extensions.check_scroll(samples)

    def test_scroll_rejects_duplicate_work_and_unrefreshed_exposed_cells(self):
        for change in ({"selectedProbes": [0, 0]}, {"refreshRemainingProbes": 1}):
            samples = [sample(1), sample(64, moved=True)]
            samples[-1]["ddgi"].update(change)
            with self.assertRaises(AssertionError):
                extensions.check_scroll(samples)

    def test_async_pause_and_resume_accepts_delayed_publication(self):
        extensions.check_async(lifecycle())

    def test_async_rejects_unsubmitted_publication_and_counter_regression(self):
        for change in ({"asyncPublishedUpdates": 65}, {"asyncSubmittedUpdates": 0}):
            samples = lifecycle()
            samples[-1]["ddgi"].update(change)
            with self.assertRaises(AssertionError):
                extensions.check_async(samples)

    def test_pause_cannot_advance_compute(self):
        samples = lifecycle()
        samples[4]["ddgi"]["updateFrame"] += 1
        with self.assertRaisesRegex(AssertionError, "pause advanced"):
            extensions.check_async(samples)

    def test_resume_must_make_progress_and_finish_refreshing(self):
        samples = lifecycle()
        for item in samples:
            if item["frame"] > 40:
                item["ddgi"].update(updateFrame=33, asyncSubmittedUpdates=33, asyncPublishedUpdates=33)
        with self.assertRaisesRegex(AssertionError, "did not resume"):
            extensions.check_async(samples)
        samples = lifecycle()
        samples[-1]["ddgi"]["refreshRemainingProbes"] = 1
        with self.assertRaisesRegex(AssertionError, "never finished"):
            extensions.check_async(samples)

    def test_async_fallback_requires_explicit_portable_validation(self):
        samples = lifecycle()
        for item in samples:
            item["ddgi"]["asyncActive"] = False
        with self.assertRaises(AssertionError):
            extensions.check_async(copy.deepcopy(samples))
        extensions.check_async(samples, allow_fallback=True)


if __name__ == "__main__":
    unittest.main()
