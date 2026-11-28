#!/usr/bin/env python3
"""Regenerate, train, and compare the matched BIM policy benchmark."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
from xml.etree import ElementTree

from bim_navigation_policy_learner import Case, DRIVE_HORIZON, GOAL_TOLERANCE, TICKS_PER_SECOND, write
from bim_navigation_policy_suite import measure_oracle, prepare


def run(program, *args):
    script = Path(__file__).with_name(program)
    subprocess.run([sys.executable, '-B', str(script), *map(str, args)], check=True)


def ros_versions():
    from ament_index_python.packages import get_package_share_directory

    packages = ('bim_ros_bridge', 'nav2_navfn_planner', 'nav2_regulated_pure_pursuit_controller', 'rclpy')
    return {package: ElementTree.parse(Path(get_package_share_directory(package)) / 'package.xml').findtext('version')
            for package in packages}


def suite_config_hashes(config):
    return {'configSha256': hashlib.sha256(config).hexdigest(),
            'configLfSha256': hashlib.sha256(config.replace(b'\r\n', b'\n')).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('config', 'exporter', 'collision', 'simulator', 'fixtures', 'output'):
        parser.add_argument(f'--{name}', required=True, type=Path)
    parser.add_argument('--learned-only', action='store_true',
                        help='Train and score frozen policies with the native simulator and geometric oracle, without ROS')
    args = parser.parse_args()
    for name in ('config', 'exporter', 'collision', 'simulator', 'fixtures'):
        setattr(args, name, getattr(args, name).resolve(strict=True))
    args.output = args.output.resolve()

    config_bytes = args.config.read_bytes()
    suite = json.loads(config_bytes)
    params = suite['evaluation']
    if suite['scorer'] != {'goalToleranceMeters': GOAL_TOLERANCE, 'driveTicks': DRIVE_HORIZON,
                           'ticksPerSecond': TICKS_PER_SECOND}:
        raise ValueError('The suite scorer differs from the native policy implementation')
    seeds, checkpoints = params['trainSeeds'], params['checkpoints']
    repeats = 0 if args.learned_only else params['nav2Repeats']
    if (not seeds or any(type(seed) is not int or seed < 0 for seed in seeds) or len(set(seeds)) != len(seeds) or
            not checkpoints or any(type(count) is not int for count in checkpoints) or checkpoints[0] != 0 or
            checkpoints != sorted(set(checkpoints))):
        raise ValueError('Use distinct training seeds and increasing checkpoints starting at zero')
    if not args.learned_only and (type(repeats) is not int or repeats <= 0):
        raise ValueError('Nav2 repeats must be a positive integer')
    packages = {} if args.learned_only else ros_versions()
    args.output.mkdir(parents=True)
    maps = args.output / 'maps'
    prepare(suite, args.exporter, args.collision, args.fixtures, maps)
    cases = maps / 'benchmark-cases.json'
    definitions = json.loads(cases.read_bytes())
    write(args.output / 'protocol.json', {
        **suite['scorer'], 'trainSeeds': params['trainSeeds'], 'checkpoints': params['checkpoints'],
        'nav2Repeats': repeats, 'cases': definitions, 'platform': platform.platform(),
        'pythonVersion': platform.python_version(), 'rosPackages': packages,
        **suite_config_hashes(config_bytes),
        'executableSha256': {name: hashlib.sha256(getattr(args, name).read_bytes()).hexdigest()
                             for name in ('exporter', 'collision', 'simulator')},
    })
    proof = json.loads((maps / 'oracle-proofs.json').read_bytes())
    write(args.output / 'oracle.json', measure_oracle(proof))
    if repeats:
        scenarios = args.output / 'scenarios'
        scenarios.mkdir()
        for group in ('test', 'controls'):
            for row in definitions[group]:
                case = Case.load(row['id'], row['family'], Path(row['source']), Path(row['mapDirectory']))
                native = case.scenario(case.yaw)
                native['ticks'] = 5400
                scenario = scenarios / f'{case.name}.json'
                write(scenario, native)
                for repeat in range(1, (repeats if group == 'test' else 1) + 1):
                    run('bim_navigation_policy_nav2.py', '--sim-executable', args.simulator, '--scene', case.source,
                        '--map-directory', case.map_directory, '--scenario', scenario,
                        '--output', args.output / 'nav2' / f'repeat-{repeat}' / case.name)
    for seed in params['trainSeeds']:
        run('bim_navigation_policy_learner.py', '--simulator', args.simulator, '--cases', cases, '--seed', seed,
            '--output', args.output / 'training' / f'seed-{seed}', '--budgets', *params['checkpoints'])
    run('bim_navigation_policy_report.py', args.output)


if __name__ == '__main__':
    main()
