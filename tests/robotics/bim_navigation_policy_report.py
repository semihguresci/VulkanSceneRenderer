#!/usr/bin/env python3
"""Verify matched native traces and summarize paired held-out trials."""

import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import random
from statistics import fmean


METRICS = ('success', 'collision', 'spl', 'wallSeconds')
TICKS_PER_SECOND = 60
NANOSECONDS_PER_SECOND = 1_000_000_000
BLOCKED_TICKS = 18
NATIVE_OUTCOMES = {'success', 'collision', 'blocked'}


def read(path):
    return json.loads(path.read_bytes())


def finite(value):
    return type(value) in (int, float) and math.isfinite(value)


def verify_trace(path, start_tick, end_tick, expected_start, goal, tolerance, *,
                 floor=None, floor_tolerance=None, initial_yaw=None, nav2=False, outcome=None):
    if type(start_tick) is not int or type(end_tick) is not int or not 0 <= start_tick <= end_tick:
        raise ValueError(f'Invalid scored tick window in {path}')
    rows = [json.loads(line) for line in path.read_bytes().splitlines()[:end_tick + 1]]
    if len(rows) != end_tick + 1:
        raise ValueError(f'Missing committed ticks in {path}')

    schema = rows[0].get('schema')
    if nav2 and schema != 'container.bim-ros-bridge-trace':
        raise ValueError(f'Expected native ROS bridge trace in {path}')
    if not nav2 and schema not in (None, 'container.bim-navigation-learner-trace'):
        raise ValueError(f'Unexpected learned policy trace schema in {path}')
    positions = []
    terminal, blocked_streak = None, 0
    for tick, row in enumerate(rows):
        position = row['base']['position']
        yaw = row['base'].get('yaw')
        obstruction = row['obstruction']
        if (type(row['tick']) is not int or row['tick'] != tick or len(position) != 3 or
                not all(map(finite, position)) or not isinstance(obstruction, str)):
            raise ValueError(f'Invalid native pose or clock at tick {tick} in {path}')
        if floor is not None and abs(position[2] - floor) > floor_tolerance:
            raise ValueError(f'Native pose left the expected BIM floor at tick {tick} in {path}')
        if initial_yaw is not None and not finite(yaw):
            raise ValueError(f'Invalid native yaw at tick {tick} in {path}')
        if tick <= start_tick:
            if math.dist(position[:2], expected_start) > 1e-5 or (
                    initial_yaw is not None and abs(math.remainder(yaw - initial_yaw, 2 * math.pi)) > 1e-7):
                raise ValueError(f'Native spawn or initial heading changed at tick {tick} in {path}')
        if schema is not None:
            stamp = tick * NANOSECONDS_PER_SECOND // TICKS_PER_SECOND
            if (row.get('schema') != schema or row.get('version') != 1 or
                    row.get('timestampNanoseconds') != stamp or
                    (nav2 and row.get('rosTimestampNanoseconds') != stamp + NANOSECONDS_PER_SECOND)):
                raise ValueError(f'Invalid declared trace clock at tick {tick} in {path}')
            command = row.get('command')
            if tick == 0:
                valid = command is None
            else:
                speed = command.get('linearMetersPerSecond') if isinstance(command, dict) else None
                turn = command.get('angularRadiansPerSecond') if isinstance(command, dict) else None
                valid = finite(speed) and finite(turn) and abs(speed) <= .4 + 1e-9 and abs(turn) <= .65 + 1e-9
                if tick <= start_tick:
                    valid = valid and speed == turn == 0
            if not valid:
                raise ValueError(f'Invalid applied command at tick {tick} in {path}')
        if tick < start_tick:
            continue
        positions.append(position)
        blocked_streak = 0 if obstruction == 'none' else blocked_streak + 1
        if obstruction == 'blocked':
            terminal = 'collision'
        elif math.dist(position[:2], goal) <= tolerance:
            terminal = 'success'
        elif blocked_streak >= BLOCKED_TICKS:
            terminal = 'blocked'
        if terminal is not None and tick != end_tick:
            raise ValueError(f'Native trace continued after {terminal} at tick {tick} in {path}')
    canonical_outcome = 'blocked' if outcome == 'obstruction' else outcome
    if outcome is not None and (terminal is not None or canonical_outcome in NATIVE_OUTCOMES):
        if canonical_outcome != terminal:
            raise ValueError(f'Terminal outcome does not match native trace in {path}')
    travelled = math.fsum(math.dist(first[:2], second[:2]) for first, second in zip(positions, positions[1:]))
    return terminal == 'success', terminal == 'collision', travelled


def expected_yaw(root, row, route):
    paths = (root / 'maps' / row['id'] / 'waypoints.json', Path(row['mapDirectory']) / 'waypoints.json')
    for path in paths:
        if path.is_file():
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != route['map']['manifestSha256']:
                continue
            points = {entry['id']: entry for entry in json.loads(data)['waypoints']}
            if points['start']['positionMeters'] != route['startMeters'] or points['goal']['positionMeters'] != route['goalMeters']:
                raise ValueError(f'Oracle route and map waypoints differ for {row["id"]}')
            yaw = points['start']['yawRadians']
            if finite(yaw):
                return yaw
    raise ValueError(f'No matching map manifest with start yaw for {row["id"]}')


def score(path, record, proof, *, nav2, tolerance, budget, initial_yaw):
    route = proof['route']
    keys = ('imageSha256', 'sceneSource', 'openDoorIds')
    if any(record[key] != route['map'][key] for key in keys):
        raise ValueError(f'Policy evaluated against a different BIM map: {path}')
    if ('sourceNamespace' in record and record['sourceNamespace'] != route['map']['sourceNamespace']) or (
            'initialYawRadians' in record and (not finite(record['initialYawRadians']) or
                abs(math.remainder(record['initialYawRadians'] - initial_yaw, 2 * math.pi)) > 1e-7)):
        raise ValueError(f'Policy identity or initial heading differs from the BIM map: {path}')
    if nav2:
        ticks = record['driveStartTick'], record['driveEndTick']
        expected_length = record['travelledMeters']
        wall = record['wallTime']['planSeconds'] + record['wallTime']['driveSeconds']
        setup = record['wallTime']['startupSeconds']
        status = record['status']
        limits = record['limits']
        if (limits['driveTicks'], limits['goalToleranceMeters'], limits['ticksPerSecond'],
                limits['linearMetersPerSecond'], limits['angularRadiansPerSecond']) != (budget, tolerance, 60, .4, .65):
            raise ValueError(f'Nav2 scorer or command bounds differ from the suite at {path}')
    else:
        ticks = 0, record['physicsTicks']
        expected_length = record['traveledMeters']
        wall, setup = record['driveWallSeconds'], record['setupWallSeconds']
        status = record['outcome']
    if not all(type(tick) is int for tick in ticks) or not 0 <= ticks[1] - ticks[0] <= budget:
        raise ValueError(f'Policy exceeded the native drive budget: {path}')
    if not nav2 and status == 'budget' and ticks[1] != budget:
        raise ValueError(f'Learned policy stopped before the native drive budget at {path}')
    success, collision, travelled = verify_trace(
        path, *ticks, route['startMeters'], route['goalMeters'], tolerance,
        floor=route['map']['floorMeters'], floor_tolerance=route['map']['floorToleranceMeters'],
        initial_yaw=initial_yaw, nav2=nav2, outcome=status)
    if (success != record['success'] or collision != record['collision'] or not finite(expected_length) or
            abs(travelled - expected_length) > 1e-7 or not all(finite(v) and v >= 0 for v in (wall, setup))):
        raise ValueError(f'Policy report disagrees with its native trace: {path}')
    ideal = route['lengthMeters']
    return {'success': success, 'collision': collision, 'spl': ideal / max(ideal, travelled) if success else 0.0,
            'wallSeconds': wall, 'setupSeconds': setup, 'travelledMeters': travelled,
            'simulatedSeconds': (ticks[1] - ticks[0]) / TICKS_PER_SECOND, 'status': status}


def verify_control(directory, record, proof, *, tolerance, budget, initial_yaw):
    """Confirm the disconnected control tests graph reachability without driving."""
    route = proof['route']
    planner = record['plannerAction']
    if (route['reachable'] or route['reason'] != 'disconnected' or record['status'] != 'unreachable' or
            not planner or planner['actionStatus'] != 6 or planner['errorCode'] != 208 or
            record['evidence']['route'] is not None or (directory / 'route.json').exists()):
        raise ValueError(f'Expected NavFn NO_VALID_PATH for the disconnected BIM control at {directory}')
    trial = score(directory / 'drive.jsonl', record, proof, nav2=True, tolerance=tolerance,
                  budget=budget, initial_yaw=initial_yaw)
    if trial['travelledMeters'] != 0 or trial['simulatedSeconds'] != 0:
        raise ValueError(f'Disconnected BIM control unexpectedly drove at {directory}')
    return {'oracleReachable': False, 'nav2Status': record['status']}


def average(matrix, families, repeats, metric):
    return fmean(fmean(matrix[replicate][case][metric] for replicate in repeats for case in ids) for ids in families)


def interval(draws):
    ordered = sorted(draws)
    return [ordered[round(q * (len(ordered) - 1))] for q in (.025, .975)]


def summary(methods, cases_by_family):
    rng = random.Random(287040)
    families = list(cases_by_family.values())
    identities = {name: tuple(matrix) for name, matrix in methods.items()}
    point = {name: {m: average(matrix, families, identities[name], m) for m in METRICS}
             for name, matrix in methods.items()}
    draws = {name: defaultdict(list) for name in methods}
    differences = {name: defaultdict(list) for name in methods
                   if 'navfn-rpp' in methods and name.startswith('learned-')}
    for _ in range(4000):
        groups = rng.choices(families, k=len(families))
        sampled = {}
        for name, matrix in methods.items():
            repeats = rng.choices(identities[name], k=len(identities[name]))
            sampled[name] = {metric: average(matrix, groups, repeats, metric) for metric in METRICS}
            for metric, value in sampled[name].items():
                draws[name][metric].append(value)
        for name, output in differences.items():
            for metric in METRICS:
                output[metric].append(sampled[name][metric] - sampled['navfn-rpp'][metric])
    estimate = {name: {m: {'estimate': val, 'ci95': interval(draws[name][m])} for m, val in metrics.items()}
                for name, metrics in point.items()}
    contrasts = {name: {m: {'estimate': point[name][m] - point['navfn-rpp'][m], 'ci95': interval(values)}
                        for m, values in entries.items()} for name, entries in differences.items()}
    return estimate, contrasts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results', type=Path)
    args = parser.parse_args()
    root = args.results
    protocol = read(root / 'protocol.json')
    cases = protocol['cases']['test']
    proofs = {row['case']: row for row in read(root / 'oracle.json')['cases']}
    yaws = {row['id']: expected_yaw(root, row, proofs[row['id']]['route'])
            for row in cases + protocol['cases']['controls']}
    families = defaultdict(list)
    for row in cases:
        families[row['family']].append(row['id'])
    tolerance, budget = protocol['goalToleranceMeters'], protocol['driveTicks']
    methods = {'oracle': {'grid': {}}}
    for row in cases:
        case, proof = row['id'], proofs[row['id']]
        route = proof['route']
        if not route['reachable'] or proof['nativePassSweeps'] <= 0:
            raise ValueError(f'Scored oracle case has no verified route: {case}')
        methods['oracle']['grid'][case] = {
            'success': True, 'collision': False, 'spl': 1.0, 'wallSeconds': proof['searchSeconds'],
            'setupSeconds': proof['setupSeconds'], 'travelledMeters': route['lengthMeters'],
            'status': 'geometric-reference', 'simulatedSeconds': None,
        }
    nav2_repeats = protocol['nav2Repeats']
    for repeat in range(1, nav2_repeats + 1):
        group = root / 'nav2' / f'repeat-{repeat}'
        methods.setdefault('navfn-rpp', {})[str(repeat)] = {
            row['id']: score(group / row['id'] / 'drive.jsonl', read(group / row['id'] / 'episode.json'),
                             proofs[row['id']], nav2=True, tolerance=tolerance, budget=budget,
                             initial_yaw=yaws[row['id']]) for row in cases
        }
    training_wall = {}
    for seed in protocol['trainSeeds']:
        stage = root / 'training' / f'seed-{seed}'
        evidence = read(stage / 'evaluation.json')
        if evidence['seed'] != seed or evidence['budgets'] != protocol['checkpoints']:
            raise ValueError(f'Unexpected frozen training seed or checkpoints at {stage}')
        training_wall[str(seed)] = evidence['trainingWallSeconds']
        for count in protocol['checkpoints']:
            by_case = {e['case']: e for e in evidence['episodes'] if e['trainTransitions'] == count}
            methods.setdefault(f'learned-{count}', {})[str(seed)] = {
                row['id']: score(stage / by_case[row['id']]['trace'], by_case[row['id']], proofs[row['id']],
                                 nav2=False, tolerance=tolerance, budget=budget,
                                 initial_yaw=yaws[row['id']]) for row in cases
            }
    estimates, contrasts = summary(methods, families)
    controls = {}
    if nav2_repeats:
        for row in protocol['cases']['controls']:
            name = row['id']
            directory = root / 'nav2' / 'repeat-1' / name
            result = read(directory / 'episode.json')
            controls[name] = verify_control(directory, result, proofs[name], tolerance=tolerance,
                                            budget=budget, initial_yaw=yaws[name])
    report = {'schema': 'container.bim-navigation-benchmark-probe', 'version': 1, 'families': families,
              'sampleBudgets': protocol['checkpoints'], 'metrics': estimates, 'pairedDeltaFromNav2': contrasts,
              'replays': methods, 'trainingWallSecondsBySeed': training_wall, 'controlCases': controls,
              'intervalDefinition': '4000 paired bootstrap draws of held-out BIM families and whole method repeats or training seeds'}
    (root / 'comparison.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n',
                                          encoding='utf-8', newline='\n')
    for method, metrics in estimates.items():
        print(method, ', '.join(f'{key}={val["estimate"]:.3f}' for key, val in metrics.items()))


if __name__ == '__main__':
    main()
