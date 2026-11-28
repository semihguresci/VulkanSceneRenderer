"""CPU checks for the policy benchmark's path, learning, and scoring rules."""

import hashlib
import json
import math
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

from bim_navigation_policy_benchmark import suite_config_hashes
from bim_navigation_policy_learner import TileQ, features, validate_case_split, write
from bim_navigation_policy_oracle import FREE, OCCUPIED, FrozenGrid, shortest_path
from bim_navigation_policy_report import summary, verify_control, verify_trace


class PolicyBenchmarkTests(unittest.TestCase):
    def test_suite_provenance_records_raw_and_lf_hashes(self):
        lf = b'{\n  "transitions": 120,\n  "seed": 287\n}\n'
        crlf = lf.replace(b'\n', b'\r\n')
        normalized = hashlib.sha256(lf).hexdigest()
        self.assertEqual(suite_config_hashes(lf), {
            'configSha256': normalized, 'configLfSha256': normalized,
        })
        self.assertEqual(suite_config_hashes(crlf), {
            'configSha256': hashlib.sha256(crlf).hexdigest(), 'configLfSha256': normalized,
        })
        changed = crlf.replace(b'120', b'121')
        self.assertNotEqual(suite_config_hashes(changed)['configLfSha256'], normalized)

    def test_reference_matches_exact_suite_path_protocol_and_ifc_byte_identities(self):
        repo = Path(__file__).resolve().parents[2]
        config_name = 'tests/fixtures/robotics/navigation/policy-benchmark-v1.json'
        reference = json.loads((repo / 'tests/benchmarks/results/bim-navigation-policy-jazzy.json').read_bytes())
        self.assertEqual(reference['suiteConfig'], config_name)
        config_bytes = (repo / config_name).read_bytes()
        config = json.loads(config_bytes)
        self.assertEqual(reference['protocol'], config['evaluation'])
        # The historical Linux run read the host's CRLF checkout of the suite JSON.
        lf_config = config_bytes.replace(b'\r\n', b'\n')
        historical_sha = hashlib.sha256(lf_config.replace(b'\n', b'\r\n')).hexdigest()
        self.assertEqual(reference['suiteConfigSha256'], historical_sha)
        self.assertEqual(reference['suiteConfigLfSha256'], suite_config_hashes(config_bytes)['configLfSha256'])

        fixtures = repo / 'tests/fixtures/robotics'
        held_out = config['cases']['test'] + config['cases']['controls']
        train_sources = {case['source'] for case in config['cases']['train']}
        held_out_sources = {case['source'] for case in held_out}
        self.assertEqual(len(train_sources | held_out_sources), 4)
        self.assertTrue(train_sources.isdisjoint(held_out_sources))
        identities = {}
        for source in train_sources | held_out_sources:
            data = (fixtures / source).read_bytes()
            identities[source] = [{'byteSize': len(data), 'sha256': hashlib.sha256(data).hexdigest()}]
        self.assertEqual(reference['trainingSourceFingerprints'],
                         {source: identities[source] for source in train_sources})
        self.assertEqual(set(reference['caseFingerprints']), {case['id'] for case in held_out})
        for case in held_out:
            with self.subTest(case=case['id']):
                self.assertEqual(reference['caseFingerprints'][case['id']]['source']['layers'],
                                 identities[case['source']])

    def test_benchmark_json_writer_preserves_lf_on_all_platforms(self):
        with tempfile.TemporaryDirectory() as out:
            target = Path(out) / 'protocol.json'
            write(target, {'seed': 287})
            self.assertEqual(target.read_bytes(), b'{"seed":287}\n')

    def test_diagonal_cannot_pass_through_touching_wall_corners(self):
        grid = FrozenGrid(2, 2, 1., (0., 0.), (FREE, OCCUPIED, OCCUPIED, FREE))
        route = shortest_path(grid, (.5, .5), (1.5, 1.5), guarded=False)
        self.assertFalse(route.reachable)
        self.assertEqual(route.reason, 'disconnected')

    def test_oracle_retains_world_endpoints_and_shortest_grid_length(self):
        grid = FrozenGrid(3, 2, .5, (-1.5, -2.), (FREE,) * 6)
        start, goal = (-1.25, -1.75), (-.25, -1.25)
        route = shortest_path(grid, start, goal, guarded=False)
        self.assertTrue(route.reachable)
        self.assertEqual(route.points[0], start)
        self.assertEqual(route.points[-1], goal)
        self.assertAlmostEqual(route.length_m, .5 * (1 + math.sqrt(2)))

    def test_checkpoint_includes_the_learned_update_and_exact_sample_count(self):
        agent = TileQ(287)
        state = features((1., .3, (4.,) * 32))
        agent.learn(state, 3, 1.5, None, True)
        self.assertEqual(agent.steps, 1)
        frozen = TileQ.from_checkpoint(json.loads(json.dumps(agent.checkpoint())))
        self.assertEqual(frozen.checkpoint(), agent.checkpoint())
        self.assertEqual(frozen.choose(state), 3)

    def test_family_weighting_and_paired_interval_keep_the_same_cases(self):
        good = {'success': True, 'collision': False, 'spl': 1., 'wallSeconds': 2.}
        bad = {'success': False, 'collision': True, 'spl': 0., 'wallSeconds': 4.}
        trials = {'repeat': {'lower': good, 'upper': good, 'hall': bad}}
        methods = {'navfn-rpp': trials, 'learned-100': trials}
        metrics, contrasts = summary(methods, {'rooms': ['lower', 'upper'], 'hall': ['hall']})
        self.assertEqual(metrics['navfn-rpp']['success']['estimate'], .5)
        self.assertEqual(contrasts['learned-100']['success'], {'estimate': 0., 'ci95': [0., 0.]})

    def test_oracle_and_learned_summary_does_not_require_nav2(self):
        good = {'success': True, 'collision': False, 'spl': 1., 'wallSeconds': 1.}
        bad = {'success': False, 'collision': True, 'spl': 0., 'wallSeconds': 2.}
        methods = {'oracle': {'grid': {'room': good, 'hall': good}},
                   'learned-0': {'287': {'room': good, 'hall': bad}}}
        metrics, contrasts = summary(methods, {'rooms': ['room'], 'halls': ['hall']})
        self.assertEqual(set(metrics), {'oracle', 'learned-0'})
        self.assertEqual(metrics['oracle']['success'], {'estimate': 1., 'ci95': [1., 1.]})
        self.assertEqual(metrics['learned-0']['success']['estimate'], .5)
        self.assertEqual(metrics['learned-0']['collision']['estimate'], .5)
        self.assertEqual(contrasts, {})

    def test_native_blocking_tick_overrides_goal_proximity(self):
        rows = [{'tick': 0, 'base': {'position': [0, 0, 1]}, 'obstruction': 'none'},
                {'tick': 1, 'base': {'position': [.8, 0, 1]}, 'obstruction': 'blocked'}]
        with tempfile.TemporaryDirectory() as out:
            trace = Path(out) / 'trace.jsonl'
            trace.write_text(''.join(json.dumps(row) + '\n' for row in rows), encoding='utf-8')
            self.assertEqual(verify_trace(trace, 0, 1, (0., 0.), (1., 0.), .25), (False, True, .8))

    def test_native_trace_rejects_motion_after_first_goal_tick(self):
        rows = [{'tick': i, 'base': {'position': [x, 0, 1], 'yaw': 0}, 'obstruction': 'none'}
                for i, x in enumerate((0, .8, .95))]
        trace = SimpleNamespace(read_bytes=lambda: '\n'.join(map(json.dumps, rows)).encode())
        with self.assertRaisesRegex(ValueError, 'continued after success'):
            verify_trace(trace, 0, 2, (0, 0), (1, 0), .25)

    def test_native_trace_requires_floor_spawn_yaw_and_finite_pose(self):
        row = {'tick': 0, 'base': {'position': [0, 0, 2], 'yaw': 0}, 'obstruction': 'none'}
        trace = SimpleNamespace(read_bytes=lambda: json.dumps(row).encode())
        with self.assertRaisesRegex(ValueError, 'BIM floor'):
            verify_trace(trace, 0, 0, (0, 0), (1, 0), .25, floor=1, floor_tolerance=.02)
        row['base']['position'] = [0, 0, 1]
        with self.assertRaisesRegex(ValueError, 'initial heading'):
            verify_trace(trace, 0, 0, (0, 0), (1, 0), .25, initial_yaw=.4)
        row['base']['position'] = [math.nan, 0, 1]
        with self.assertRaisesRegex(ValueError, 'Invalid native pose'):
            verify_trace(trace, 0, 0, (0, 0), (1, 0), .25)

    def test_typed_learner_trace_rejects_out_of_bounds_commands(self):
        rows = [
            {'schema': 'container.bim-navigation-learner-trace', 'version': 1, 'tick': tick,
             'timestampNanoseconds': tick * 1_000_000_000 // 60,
             'base': {'position': [tick * .006, 0, 1], 'yaw': 0}, 'obstruction': 'none',
             'command': command}
            for tick, command in enumerate((None, {'linearMetersPerSecond': .41, 'angularRadiansPerSecond': 0}))
        ]
        trace = SimpleNamespace(read_bytes=lambda: '\n'.join(map(json.dumps, rows)).encode())
        with self.assertRaisesRegex(ValueError, 'Invalid applied command'):
            verify_trace(trace, 0, 1, (0, 0), (1, 0), .25)
        rows[1]['command']['linearMetersPerSecond'] = .4
        self.assertEqual(verify_trace(trace, 0, 1, (0, 0), (1, 0), .25), (False, False, .006))

    def test_disconnected_control_requires_no_valid_path_from_navfn(self):
        row = {'status': 'unreachable', 'plannerAction': {'actionStatus': 6, 'errorCode': 205},
               'evidence': {'route': None}}
        proof = {'route': {'reachable': False, 'reason': 'disconnected'}}
        with self.assertRaisesRegex(ValueError, 'NO_VALID_PATH'):
            verify_control(Path('unused-control'), row, proof, tolerance=.25, budget=1800, initial_yaw=0)

    def test_disconnected_control_requires_no_scored_drive(self):
        map_identity = {'imageSha256': 'same-image', 'sceneSource': {}, 'openDoorIds': [],
                        'sourceNamespace': 'rooms', 'floorMeters': 1, 'floorToleranceMeters': .02}
        proof = {'route': {'reachable': False, 'reason': 'disconnected', 'startMeters': [0, 0],
                           'goalMeters': [1, 0], 'lengthMeters': None, 'map': map_identity}}
        record = {**map_identity, 'status': 'unreachable', 'plannerAction': {'actionStatus': 6, 'errorCode': 208},
                  'evidence': {'route': None}, 'success': False, 'collision': False,
                  'driveStartTick': 0, 'driveEndTick': 0, 'travelledMeters': 0,
                  'wallTime': {'planSeconds': .01, 'driveSeconds': 0, 'startupSeconds': 1},
                  'limits': {'driveTicks': 1800, 'goalToleranceMeters': .25, 'ticksPerSecond': 60,
                             'linearMetersPerSecond': .4, 'angularRadiansPerSecond': .65}}
        rows = [{'schema': 'container.bim-ros-bridge-trace', 'version': 1, 'tick': 0,
                 'timestampNanoseconds': 0, 'rosTimestampNanoseconds': 1_000_000_000,
                 'base': {'position': [0, 0, 1], 'yaw': 0}, 'obstruction': 'none', 'command': None}]
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            trace = directory / 'drive.jsonl'
            trace.write_text(json.dumps(rows[0]) + '\n', encoding='utf-8')
            result = verify_control(directory, record, proof, tolerance=.25, budget=1800, initial_yaw=0)
            self.assertEqual(result, {'oracleReachable': False, 'nav2Status': 'unreachable'})
            rows.append({**rows[0], 'tick': 1, 'timestampNanoseconds': 16_666_666,
                         'rosTimestampNanoseconds': 1_016_666_666, 'base': {'position': [.006, 0, 1], 'yaw': 0},
                         'command': {'linearMetersPerSecond': .4, 'angularRadiansPerSecond': 0}})
            trace.write_text('\n'.join(map(json.dumps, rows)) + '\n', encoding='utf-8')
            record.update(driveEndTick=1, travelledMeters=.006)
            with self.assertRaisesRegex(ValueError, 'unexpectedly drove'):
                verify_control(directory, record, proof, tolerance=.25, budget=1800, initial_yaw=0)

    def test_holdout_rejects_the_same_ifc_with_a_different_family_name_and_map(self):
        source = {'layers': [{'sha256': 'ifc-content'}]}
        train = SimpleNamespace(family='train-building',
                                manifest={'imageSha256': 'map-1', 'sceneSource': source})
        test = SimpleNamespace(family='renamed-building',
                               manifest={'imageSha256': 'map-2', 'sceneSource': source})
        with self.assertRaisesRegex(ValueError, 'BIM source fingerprints overlap'):
            validate_case_split({'train': [train], 'test': [test]})
        test.manifest['sceneSource'] = {'layers': [{'sha256': 'independent-ifc-content'}]}
        validate_case_split({'train': [train], 'test': [test]})


if __name__ == '__main__':
    unittest.main()
