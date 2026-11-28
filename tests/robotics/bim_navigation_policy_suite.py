#!/usr/bin/env python3
"""Regenerate the benchmark's exact maps and sweep its analytic grid paths."""

import json
from pathlib import Path
import time
from types import SimpleNamespace

from bim_nav2_map import collision_walks, run_tool
from bim_navigation_policy_learner import write
from bim_navigation_policy_oracle import FrozenGrid, shortest_path


def check(condition, message):
    if not condition:
        raise ValueError(message)


def prepare(spec, exporter, collision, fixtures, output):
    check(spec.get('schema') == 'container.bim-navigation-policy-suite' and spec.get('version') == 1,
          'Unsupported BIM navigation policy suite')
    output.mkdir(parents=True)
    raw = output / 'verification'
    raw.mkdir()
    manifests = {}
    proofs = []
    ids = set()
    for group in ('train', 'test', 'controls'):
        manifests[group] = []
        for row in spec['cases'][group]:
            identifier = row['id']
            check(identifier not in ids and Path(identifier).name == identifier and identifier not in ('.', '..'),
                  f'Unexpected or reused case identifier: {identifier}')
            ids.add(identifier)
            source = (fixtures / row['source']).resolve(strict=True)
            check(source.is_file() and source.is_relative_to(fixtures), f'IFC is outside the fixture directory: {source}')
            request_file = raw / f'{identifier}-request.json'
            write(request_file, row['mapRequest'])
            directory = output / identifier
            run_tool([exporter, source, request_file, directory])

            manifest = json.loads((directory / 'waypoints.json').read_bytes())
            points = {point['id']: point for point in manifest['waypoints']}
            start, goal = (points[key]['positionMeters'] for key in ('start', 'goal'))
            grid = FrozenGrid.from_export(directory)
            route = shortest_path(grid, start, goal)
            check(route.reachable == (group != 'controls'), f'{identifier} reachability changed: {route.reason}')
            floor = manifest['floorMeters']
            pairs = list(zip(route.points, route.points[1:])) if route.reachable else [(start, goal)]
            walks = [{'from': [*a, floor], 'to': [*b, floor]} for a, b in pairs if tuple(a) != tuple(b)]
            check(walks, f'{identifier} does not exercise any geometry')
            parsed = SimpleNamespace(id=identifier, source=row['source'])
            evidence = collision_walks(collision, fixtures, raw, parsed, manifest, walks)
            if route.reachable:
                for index, sweep in enumerate(evidence):
                    check(sweep['reachable'] and sweep['blockingHit'] is None and sweep['supportSamples'] and all(
                        abs(point['feet'][2] - floor) <= manifest['floorToleranceMeters']
                        for point in sweep['supportSamples']), f'{identifier} invalid capsule sweep {index}')
            else:
                check(not evidence[0]['reachable'] and evidence[0]['reason'] == 'blocked',
                      f'{identifier} negative control failed its physical partition check')
            metadata = {'id': identifier, 'family': row['family'], 'source': str(source),
                        'mapDirectory': str(directory), 'expected': row['expected'],
                        'sceneSource': manifest['sceneSource'], 'imageSha256': manifest['imageSha256']}
            manifests[group].append(metadata)
            proofs.append({'case': identifier, 'family': row['family'], 'source': str(source),
                           'mapDirectory': str(directory), 'nativePassSweeps': len(walks) if route.reachable else 0,
                           'oracle': route.as_dict()})
    write(output / 'benchmark-cases.json', {'schema': 'container.bim-navigation-policy-cases', 'version': 1,
                                            'scorer': spec['scorer'], **manifests})
    write(output / 'oracle-proofs.json', {'schema': 'container.bim-navigation-policy-case-proofs', 'version': 1,
                                         'cases': proofs})
    return len(proofs), sum(row['nativePassSweeps'] for row in proofs)


def measure_oracle(proof):
    rows = []
    for case in proof['cases']:
        expected = case['oracle']
        start = time.perf_counter()
        grid = FrozenGrid.from_export(Path(case['mapDirectory']))
        ready = time.perf_counter()
        route = shortest_path(grid, expected['startMeters'], expected['goalMeters'])
        done = time.perf_counter()
        actual = route.as_dict()
        if actual != expected:
            raise ValueError(f"Recomputed oracle path differs from capsule proof for {case['case']}")
        rows.append({'case': case['case'], 'nativePassSweeps': case['nativePassSweeps'], 'route': actual,
                     'setupSeconds': ready - start, 'searchSeconds': done - ready})
    return {'cases': rows}
