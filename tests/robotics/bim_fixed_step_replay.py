#!/usr/bin/env python3
"""Bounded CPU executable replay and independent metric checks for BimRoboticsSim."""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import time

HZ, TICKS, DURATION = 60, 180, 3_000_000_000


def digest(data):
    return hashlib.sha256(data).hexdigest()


def strict_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result

    def invalid(value):
        raise ValueError(f"Nonfinite JSON number: {value}")

    value = json.loads(data, object_pairs_hook=pairs, parse_constant=invalid)

    def finite(node, path=""):
        if isinstance(node, float) and not math.isfinite(node):
            raise ValueError(f"Nonfinite number at {path}")
        if isinstance(node, dict):
            for key, child in node.items():
                finite(child, f"{path}/{key}")
        elif isinstance(node, list):
            for index, child in enumerate(node):
                finite(child, f"{path}/{index}")

    finite(value)
    return value


def first_difference(expected, actual, path="", tick=None):
    if isinstance(expected, dict):
        tick = expected.get("tick", tick)
    context = f" (tick {tick})" if tick is not None else ""
    if type(expected) is not type(actual):
        return f"{path or '/'}{context}: expected {expected!r}, got {actual!r} (different types)"
    if isinstance(expected, dict):
        if expected.keys() != actual.keys():
            return f"{path or '/'}{context}: missing {sorted(expected.keys() - actual.keys())}, extra {sorted(actual.keys() - expected.keys())}"
        for key in expected:
            escaped = key.replace("~", "~0").replace("/", "~1")
            difference = first_difference(expected[key], actual[key], f"{path}/{escaped}", tick)
            if difference:
                return difference
    elif isinstance(expected, list):
        if len(expected) != len(actual):
            return f"{path}{context}: expected {len(expected)} entries, got {len(actual)}"
        for index, (left, right) in enumerate(zip(expected, actual)):
            difference = first_difference(left, right, f"{path}/{index}", tick)
            if difference:
                return difference
    elif expected != actual or (isinstance(expected, float) and expected == 0 and
                                math.copysign(1, expected) != math.copysign(1, actual)):
        delta = f", delta={actual - expected:.17g}" if isinstance(expected, (int, float)) else ""
        return f"{path or '/'}{context}: expected {expected!r}, got {actual!r}{delta}"
    return None


def equal(label, expected, actual):
    difference = first_difference(expected, actual)
    if difference:
        raise AssertionError(f"{label}: {difference}")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def entity_id(namespace, guid):
    return f"bim/{len(namespace)}:{namespace}/guid/{len(guid)}:{guid}"


def due_ticks(settings, rate_key):
    first = settings.get("firstTick", 0)
    rate = settings[rate_key]
    return [tick for tick in range(first, TICKS + 1)
            if tick == first or (tick - first) * rate // (HZ * 1000) >
            (tick - first - 1) * rate // (HZ * 1000)]


def structural(report, scenario):
    equal("schema", "container.bim-robotics-simulation", report["schema"])
    equal("schema version", 1, report["version"])
    equal("tick frequency", HZ, report["ticksPerSecond"])
    history = report["history"]
    equal("history tick coverage", list(range(TICKS + 1)), [row["tick"] for row in history])
    equal("final state", history[-1], report["state"])
    for row in history:
        equal(f"history time at tick {row['tick']}", row["tick"] / HZ, row["seconds"])
        if "odometryCovariance" in row:
            covariance = row["odometryCovariance"]
            require(isinstance(covariance, list) and len(covariance) == 9 and
                    all(type(value) in (int, float) and math.isfinite(value) for value in covariance),
                    f"/history/{row['tick']}/odometryCovariance must contain nine finite numbers")
            for axis in range(3):
                require(covariance[axis * 3 + axis] >= 0,
                        f"/history/{row['tick']}/odometryCovariance has a negative diagonal")
                for other in range(axis):
                    equal(f"Covariance symmetry at tick {row['tick']} axes {axis}/{other}",
                          covariance[axis * 3 + other], covariance[other * 3 + axis])
    for name, rate_key, rows_key in (("lidar", "scansPerSecondMilliHz", "scans"),
                                      ("driveSensors", "samplesPerSecondMilliHz", "samples")):
        if name not in scenario:
            continue
        expected = due_ticks(scenario[name], rate_key)
        rows = report[name][rows_key]
        equal(f"{name} tick schedule", expected, [row["tick"] for row in rows])
        for row in rows:
            equal(f"{name} time at tick {row['tick']}", row["tick"] / HZ, row["seconds"])
        if name == "lidar":
            beams = scenario[name]["azimuthSamples"] * len(scenario[name].get("elevationAngles", [0]))
            for scan in rows:
                equal(f"LiDAR beam count at tick {scan['tick']}", beams, len(scan["returns"]))
                for beam, value in enumerate(scan["returns"]):
                    valid = value["status"] == "valid"
                    require(valid == (value["rangeMeters"] is not None),
                            f"LiDAR tick {scan['tick']} beam {beam}: invalid range/status combination")
                    if valid:
                        require(scenario[name]["minRangeMeters"] <= value["rangeMeters"] <= scenario[name]["maxRangeMeters"],
                                f"LiDAR tick {scan['tick']} beam {beam}: out-of-range valid return")


class Harness:
    def __init__(self, args):
        self.sim = Path(args.sim).resolve()
        self.fixtures = Path(args.fixtures).resolve()
        self.output = Path(args.output).resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.run_dir = Path(tempfile.mkdtemp(prefix="run-", dir=self.output))
        self.summary = {"schema": "container.bim-fixed-step-replay", "version": 1,
                        "status": "running", "runDirectory": str(self.run_dir),
                        "simulator": str(self.sim), "physicsWorkerThreads": 1, "ticksPerSecond": HZ,
                        "ticks": TICKS, "maxTicksPerAdvance": 7, "runs": [], "checks": [], "sla": {}}
        try:
            require(self.sim.is_file(), f"Simulator does not exist: {self.sim}")
            self.summary["simulatorSha256"] = digest(self.sim.read_bytes())
            self.summary["fixtureSha256"] = {
                name: digest((self.fixtures / name).read_bytes())
                for name in ("lidar-corner.ifc", "doorway.ifc")}
        except Exception as error:
            self.summary.update(status="failed", error=str(error))
            self.save()
            raise
        self.save()

    def save(self):
        data = (json.dumps(self.summary, indent=2, allow_nan=False) + "\n").encode()
        for directory in (self.run_dir, self.output):
            temporary = directory / f"summary-{self.run_dir.name}.tmp"
            temporary.write_bytes(data)
            temporary.replace(directory / "summary.json")

    def run(self, name, scenario, fixture="lidar-corner.ifc", namespace="replay-corner", rejection=None):
        folder = self.run_dir / name
        folder.mkdir()
        scenario_path, report_path = folder / "scenario.json", folder / "report.json"
        encoded = (json.dumps(scenario, indent=2, allow_nan=False) + "\n").encode()
        scenario_path.write_bytes(encoded)
        command = [str(self.sim), str(self.fixtures / fixture), str(report_path), namespace, str(scenario_path)]
        record = {"name": name, "command": command, "scenarioSha256": digest(encoded)}
        self.summary["runs"].append(record)
        started = time.monotonic()
        try:
            process = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
            record.update(exitCode=process.returncode, wallSeconds=time.monotonic() - started)
            (folder / "stdout.txt").write_text(process.stdout, encoding="utf-8")
            (folder / "stderr.txt").write_text(process.stderr, encoding="utf-8")
            if rejection:
                require(process.returncode == 1 and rejection in process.stderr and not report_path.exists(),
                        f"{name}: expected validation failure {rejection!r}, exit={process.returncode}, stderr={process.stderr.strip()!r}")
                record["result"] = "rejected-as-expected"
                print(f"PASS {name}", flush=True)
                return None
            require(process.returncode == 0, f"{name}: exit={process.returncode}, stderr={process.stderr.strip()!r}")
            raw = report_path.read_bytes()
            record["reportSha256"] = digest(raw)
            report = strict_json(raw)
            structural(report, scenario)
            record["result"] = "passed"
            print(f"PASS {name}", flush=True)
            return raw, report
        except Exception as error:
            record.update(result="failed", error=str(error), wallSeconds=time.monotonic() - started)
            raise
        finally:
            self.save()

    def exact(self, label, reference, actual):
        if reference[0] != actual[0]:
            difference = first_difference(reference[1], actual[1])
            if difference is None:
                index = next((i for i, pair in enumerate(zip(reference[0], actual[0])) if pair[0] != pair[1]),
                             min(len(reference[0]), len(actual[0])))
                difference = f"serialized bytes differ at offset {index}; parsed values match"
            raise AssertionError(f"{label}: {difference}")
        self.summary["checks"].append({"name": label, "result": "byte-identical"})

    def near(self, label, actual, expected, tolerance, metric):
        error = abs(actual - expected)
        self.summary["sla"][metric] = max(self.summary["sla"].get(metric, 0), error)
        require(error <= tolerance, f"{label}: expected {expected:.17g}, got {actual:.17g}, error={error:.6g} > {tolerance:g}")


def corner_scenario():
    return {"version": 1, "initialFeet": [0, 0, 0], "ticks": TICKS, "ticksPerSecond": HZ,
            "maxTicksPerAdvance": 7, "actuators": {},
            "commands": [{"tick": 12, "linear": .6, "angular": 0},
                         {"tick": 60, "linear": 0, "angular": .75},
                         {"tick": 108, "linear": .4, "angular": .5},
                         {"tick": 168, "linear": 0, "angular": 0}],
            "lidar": {"scansPerSecondMilliHz": 7000, "azimuthSamples": 8, "azimuthStart": 0,
                      "mount": {"xyz": [0, 0, 1.25]}, "minRangeMeters": .05, "maxRangeMeters": 8},
            "driveSensors": {"samplesPerSecondMilliHz": 12000, "encoders": {"countsPerRevolution": 2048}}}


def partitions():
    result = {}
    for hz in (30, 60, 144):
        count = 3 * hz
        result[f"{hz}hz"] = [index * DURATION // count - (index - 1) * DURATION // count
                              for index in range(1, count + 1)]
    irregular, remaining = [], DURATION
    while remaining:
        frame = min((13_000_000, 31_000_000, 7_000_000, 11_000_000)[len(irregular) % 4], remaining)
        irregular.append(frame)
        remaining -= frame
    result["irregular"] = irregular
    result["stalled"] = [13_000_000, 31_000_000, 7_000_000, 2_300_000_000, 111_000_000, 538_000_000]
    return result


def matrix(harness, name, scenario):
    baseline = harness.run(f"{name}-direct", scenario)
    harness.exact(f"{name} repeated process", baseline, harness.run(f"{name}-repeat", scenario))
    for cadence, sequence in partitions().items():
        framed = copy.deepcopy(scenario)
        framed["frameNanosecondsSequence"] = sequence
        if cadence != "60hz":
            del framed["ticks"]
            framed["durationNanoseconds"] = DURATION
        harness.exact(f"{name} {cadence}", baseline, harness.run(f"{name}-{cadence}", framed))
    return baseline


def quiet_oracles(harness, report):
    for row in report["history"]:
        tick = row["tick"]
        straight = min(.8, max(0, (tick - 12) / HZ))
        spin = min(.8, max(0, (tick - 60) / HZ))
        arc = min(1, max(0, (tick - 108) / HZ))
        expected = [.6 * straight + .8 * (math.sin(.6 + .5 * arc) - math.sin(.6)),
                    .8 * (math.cos(.6) - math.cos(.6 + .5 * arc)), 0]
        require(row["obstruction"] == "none", f"Quiet motion unexpectedly blocked at tick {tick}: {row['obstruction']}")
        for axis, value in enumerate(expected):
            harness.near(f"/history/{tick}/base/position/{axis}", row["base"]["position"][axis], value, 1e-5, "posePositionMeters")
        harness.near(f"/history/{tick}/base/yaw", row["base"]["yaw"], .75 * spin + .5 * arc, 1e-5, "poseYawRadians")
        for wheel, angle in (("left", 6 * straight - 1.5 * spin + 3 * arc),
                             ("right", 6 * straight + 1.5 * spin + 5 * arc)):
            harness.near(f"/history/{tick}/wheelAngles/{wheel}", row["wheelAngles"][wheel], angle, 1e-10, "wheelAngleRadians")
    samples = {row["tick"]: row for row in report["driveSensors"]["samples"]}
    for tick, gyro, force in ((0, 0, [0, 0, 9.81]), (30, 0, [0, 0, 9.81]),
                               (90, .75, [0, 0, 9.81]),
                               (130, .5, [.2 * math.sin(.5 / HZ), .2 * math.cos(.5 / HZ), 9.81]),
                               (180, 0, [0, 0, 9.81])):
        for axis, expected in enumerate(force):
            harness.near(f"IMU tick {tick} force axis {axis}", samples[tick]["specificForce"][axis], expected, 2e-4, "imuForceMetersPerSecondSquared")
        for axis, expected in enumerate((0, 0, gyro)):
            harness.near(f"IMU tick {tick} gyro axis {axis}", samples[tick]["angularVelocity"][axis], expected, 1e-10, "imuRateRadiansPerSecond")
    equal("tick30 measured counts", {"left": 587, "right": 587}, samples[30]["encoderCounts"])
    equal("tick90 measured counts", {"left": 1320, "right": 1809}, samples[90]["encoderCounts"])
    final = samples[180]["odometry"]
    expected = [.48 + .8 * (math.sin(1.1) - math.sin(.6)), .8 * (math.cos(.6) - math.cos(1.1)), 0]
    for axis, value in enumerate(expected):
        harness.near(f"Final encoder odometry axis {axis}", final["position"][axis], value, .002, "encoderOdometryMeters")
    harness.near("Final encoder odometry yaw", final["yaw"], 1.1, .002, "encoderOdometryRadians")
    scan = report["lidar"]["scans"][0]
    for beam, (metres, guid) in enumerate(((4, "0000000000000000000027"),
                                         (3 * math.sqrt(2), "0000000000000000000037"),
                                         (3, "0000000000000000000037"))):
        value = scan["returns"][beam]
        equal(f"Initial LiDAR beam {beam} status", "valid", value["status"])
        equal(f"Initial LiDAR beam {beam} identity", entity_id("replay-corner", guid), value["entityId"])
        harness.near(f"Initial LiDAR beam {beam} range", value["rangeMeters"], metres, 1e-5, "lidarRangeMeters")
    for beam, value in enumerate(scan["returns"][3:], 3):
        equal(f"Initial LiDAR beam {beam} clear", {"rangeMeters": None, "status": "no-hit", "entityId": None}, value)


def noisy_scenario(quiet):
    scenario = copy.deepcopy(quiet)
    scenario.update(seed=1234, encoderNoiseRadians=.001)
    scenario["driveSensors"].update(samplesPerSecondMilliHz=13000, firstTick=3, seed=253,
        accelerometer={"noiseStdDev": [.02] * 3}, gyroscope={"noiseStdDev": [.003] * 3})
    scenario["driveSensors"]["encoders"]["noiseStdDevRadians"] = {"left": .004, "right": .004}
    scenario["lidar"].update(firstTick=2, noiseStdDevMeters=.01, noiseStdDevPerMeter=.01,
                             rangeResolutionMeters=.02, dropoutProbability=.15,
                             recordIdealRanges=True, seed=149)
    return scenario


def seed_checks(harness, scenario, baseline):
    truth_keys = ("tick", "seconds", "base", "wheelAngles", "wheelRates", "obstruction", "blockingEntityId",
                  "commanded", "targetWheelRates", "groundWheelRates")
    changed = copy.deepcopy(scenario)
    changed["seed"] += 1
    other = harness.run("noisy-drive-seed", changed)[1]
    equal("Drive seed ground truth separation", [{key: row[key] for key in truth_keys} for row in baseline["history"]],
          [{key: row[key] for key in truth_keys} for row in other["history"]])
    require(any(a["encoderAngles"] != b["encoderAngles"] for a, b in zip(baseline["history"], other["history"])),
            "Changed drive seed did not change noisy encoder angles")
    equal("Drive seed LiDAR separation", baseline["lidar"], other["lidar"])
    equal("Drive seed IMU separation", [(row["specificForce"], row["angularVelocity"]) for row in baseline["driveSensors"]["samples"]],
          [(row["specificForce"], row["angularVelocity"]) for row in other["driveSensors"]["samples"]])
    for field in ("driveSensors", "lidar"):
        changed = copy.deepcopy(scenario)
        changed[field]["seed"] += 1
        other = harness.run(f"noisy-{field}-seed", changed)[1]
        equal(f"{field} seed trajectory separation", baseline["history"], other["history"])
        unaffected = "lidar" if field == "driveSensors" else "driveSensors"
        equal(f"{field} seed other-sensor separation", baseline[unaffected], other[unaffected])
        rows = "samples" if field == "driveSensors" else "scans"
        require(baseline[field][rows] != other[field][rows], f"Changed {field} seed did not change noisy measurements")
    harness.summary["checks"].append({"name": "independent seeds and ground truth", "result": "passed"})


def lidar_readout_checks(harness, scenario, noisy):
    ideal_scenario = copy.deepcopy(scenario)
    for field in ("noiseStdDevMeters", "noiseStdDevPerMeter", "rangeResolutionMeters", "dropoutProbability"):
        del ideal_scenario["lidar"][field]
    ideal = harness.run("lidar-ideal", ideal_scenario)[1]
    equal("LiDAR readout leaves trajectory unchanged", ideal["history"], noisy["history"])
    for field in ("recordIdealRanges", "noiseStdDevPerMeter", "rangeResolutionMeters"):
        equal(f"LiDAR calibration retains {field}", scenario["lidar"][field], noisy["lidar"][field])
    measured, dropped, perturbed = 0, 0, 0
    resolution = noisy["lidar"]["rangeResolutionMeters"]
    for clean_scan, noisy_scan in zip(ideal["lidar"]["scans"], noisy["lidar"]["scans"], strict=True):
        equal("LiDAR noise leaves sensor pose unchanged", clean_scan["worldFromSensor"], noisy_scan["worldFromSensor"])
        for beam, (clean, observation) in enumerate(zip(clean_scan["returns"], noisy_scan["returns"], strict=True)):
            context = f"LiDAR tick {noisy_scan['tick']} beam {beam}"
            equal(f"{context} identity", clean["entityId"], observation["entityId"])
            equal(f"{context} ideal distance", clean["idealRangeMeters"], observation["idealRangeMeters"])
            if observation["status"] == "dropped":
                dropped += 1
                require(observation["idealRangeMeters"] is not None, f"{context} lost its dropped hit's truth")
            if observation["status"] == "valid":
                measured += 1
                bins = observation["rangeMeters"] / resolution
                require(abs(bins - round(bins)) < 1e-9, f"{context} range is not quantized")
                perturbed += observation["rangeMeters"] != observation["idealRangeMeters"]
            if clean["status"] == "no-hit":
                equal(f"{context} remains a miss", clean, observation)
    require(measured and dropped and perturbed,
            "Expected quantized, dropped and changed readouts in the noisy BIM corner run")
    harness.summary["checks"].append({"name": "LiDAR range response and unchanged ideal hits", "result": "passed",
                                      "measured": measured, "dropped": dropped, "perturbed": perturbed})


def doorway_checks(harness):
    scenario = {"version": 1, "initialFeet": [-1.5, 0, 1.2], "ticks": TICKS, "ticksPerSecond": HZ,
                "maxTicksPerAdvance": 7, "commands": [{"tick": 0, "linear": 1, "angular": 0}]}
    door = entity_id("doorway", "door-guid")
    for opened in (False, True):
        name = "door-open" if opened else "door-closed"
        if opened:
            scenario["openDoorIds"] = [door]
        direct = harness.run(name, scenario, "doorway.ifc", "doorway")
        framed = copy.deepcopy(scenario)
        del framed["ticks"]
        framed.update(durationNanoseconds=DURATION, frameNanosecondsSequence=partitions()["stalled"])
        harness.exact(f"{name} stalled replay", direct,
                      harness.run(f"{name}-stalled", framed, "doorway.ifc", "doorway"))
        history = direct[1]["history"]
        for row in history:
            harness.near(f"{name} tick {row['tick']} floor", row["base"]["position"][2], 1.2, 1e-5, "doorFloorMeters")
        if opened:
            equal("Open doorway obstruction", "none", history[-1]["obstruction"])
            harness.near("Open doorway final X", history[-1]["base"]["position"][0], 1.5, 1e-5, "doorOpenPositionMeters")
        else:
            x = history[-1]["base"]["position"][0]
            require(-.30 <= x <= -.279, f"Closed door clearance violated: final X={x}, expected [-0.30,-0.279]")
            for row in history[120:]:
                equal(f"Closed door identity at tick {row['tick']}", door, row["blockingEntityId"])
                equal(f"Closed door obstruction at tick {row['tick']}", "blocked", row["obstruction"])
                for key in ("base", "wheelRates", "wheelAngles", "encoderAngles", "odometry"):
                    equal(f"Closed contact stability {key} at tick {row['tick']}", history[120][key], row[key])
            equal("Closed door stopped rates", {"left": 0.0, "right": 0.0}, history[-1]["wheelRates"])
    harness.summary["checks"].append({"name": "native doorway clearance, semantic identity and stable contact", "result": "passed"})


def negative_checks(harness, quiet):
    base = copy.deepcopy(quiet)
    del base["ticks"]
    base.update(durationNanoseconds=DURATION, frameNanosecondsSequence=[DURATION])
    cases = [("duration-mismatch", {"frameNanosecondsSequence": [1_000_000_000]},
              "frameNanosecondsSequence total must equal durationNanoseconds"),
             ("both-frame-modes", {"frameNanoseconds": 1_000_000},
              "Specify only one of frameNanoseconds and frameNanosecondsSequence"),
             ("empty-sequence", {"frameNanosecondsSequence": []},
              "frameNanosecondsSequence must be a nonempty array within the frame limit"),
             ("zero-frame", {"frameNanosecondsSequence": [0, DURATION]},
              "frameNanosecondsSequence entries must be positive")]
    for name, changes, diagnostic in cases:
        scenario = copy.deepcopy(base)
        scenario.update(changes)
        harness.run(f"reject-{name}", scenario, rejection=diagnostic)
    base.pop("durationNanoseconds")
    base.update(ticks=TICKS, frameNanosecondsSequence=[1_000_000_000])
    harness.run("reject-ticks-mismatch", base, rejection="frameNanosecondsSequence must reach exactly the requested ticks")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim", required=True)
    parser.add_argument("--fixtures", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    harness = None
    try:
        harness = Harness(args)
        quiet = corner_scenario()
        quiet_oracles(harness, matrix(harness, "quiet", quiet)[1])
        noisy = noisy_scenario(quiet)
        noisy_report = matrix(harness, "noisy", noisy)[1]
        seed_checks(harness, noisy, noisy_report)
        lidar_readout_checks(harness, noisy, noisy_report)
        doorway_checks(harness)
        negative_checks(harness, quiet)
        harness.summary["status"] = "passed"
        harness.summary["limitations"] = ["Exact replay applies to this executable and platform; cross-build bitwise equality is not promised.",
                                            "Same-process reset and dynamic rigid-body stability remain covered by native C++ tests.",
                                            "Wall duration is recorded, not used as a timing correctness threshold."]
        print(f"PASS: {len(harness.summary['runs'])} process runs; summary: {harness.output / 'summary.json'}")
        return 0
    except Exception as error:
        if harness:
            harness.summary.update(status="failed", error=str(error))
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        if harness:
            harness.save()


if __name__ == "__main__":
    sys.exit(main())
