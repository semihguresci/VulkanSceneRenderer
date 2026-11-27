"""Verify exported BIM occupancy maps and sweep real Nav2 paths on Windows."""

import argparse
from collections import Counter, deque
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


OCCUPIED, UNKNOWN, FREE = 0, 127, 255
DOORWAY_DOOR = "bim/7:doorway/guid/9:door-guid"
ROOMS_DOOR = "bim/5:rooms/guid/22:0000000000000000000044"
CASES_SCHEMA = "container.bim-nav2-map-cases"
RESULTS_SCHEMA = "container.bim-nav2-planner-results"
START = [-1.55, 0.05]
GOAL = [1.55, 0.05]


@dataclass(frozen=True)
class Case:
    id: str
    source: str
    expected: str
    door_open: bool = False
    floor: float = 1.2

    @property
    def namespace(self):
        return "doorway" if self.source == "doorway.ifc" else "rooms"

    @property
    def door_id(self):
        return DOORWAY_DOOR if self.namespace == "doorway" else ROOMS_DOOR

    def request(self):
        return {
            "schema": "container.bim-navigation-map-request", "version": 1,
            "sourceNamespace": self.namespace, "originMeters": [-3.1, -3.1],
            "sizeCells": [62, 62], "resolutionMeters": 0.1, "floorMeters": self.floor,
            "floorToleranceMeters": 0.02, "radiusMeters": 0.25, "heightMeters": 1.6,
            "openDoorIds": [self.door_id] if self.door_open else [],
            "waypoints": [{"id": name, "positionMeters": point, "yawRadians": 0.0}
                          for name, point in [("start", START), ("goal", GOAL)]],
        }


CASES = (
    Case("doorway-open", "doorway.ifc", "aperture", True),
    Case("doorway-closed", "doorway.ifc", "detour"),
    Case("rooms-open", "room-connectivity.ifc", "aperture", True),
    Case("rooms-closed", "room-connectivity.ifc", "unreachable"),
    Case("rooms-upper", "room-connectivity.ifc", "direct", floor=3.7),
    Case("rooms-open-metres", "room-connectivity-metres.ifc", "aperture", True),
    Case("rooms-closed-metres", "room-connectivity-metres.ifc", "unreachable"),
    Case("rooms-upper-metres", "room-connectivity-metres.ifc", "direct", floor=3.7),
)


def case_index():
    return {"schema": CASES_SCHEMA, "version": 1,
            "cases": [{"id": case.id, "directory": case.id, "source": case.source,
                       "expected": case.expected} for case in CASES]}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def run_tool(command, *, expect_success=True):
    result = subprocess.run([str(part) for part in command], capture_output=True, text=True, timeout=35)
    assert (result.returncode == 0) == expect_success, (command, result.returncode, result.stdout, result.stderr)
    return result


@dataclass(frozen=True)
class Grid:
    origin: tuple
    pitch: float
    rows: tuple

    def at(self, x, y):
        if 0 <= y < len(self.rows) and 0 <= x < len(self.rows[y]):
            return self.rows[y][x]
        return UNKNOWN

    def grid_point(self, point):
        return tuple((value - origin) / self.pitch for value, origin in zip(point, self.origin, strict=True))

    def center(self, cell):
        return tuple(origin + (index + 0.5) * self.pitch
                     for index, origin in zip(cell, self.origin, strict=True))

    def probe(self, point):
        return self.at(*(math.floor(coord) for coord in self.grid_point(point)))

    def route(self):
        start, goal = (tuple(math.floor(value) for value in self.grid_point(point)) for point in (START, GOAL))
        assert self.at(*start) == self.at(*goal) == FREE
        previous = {start: None}
        queue = deque([start])
        while queue and goal not in previous:
            x, y = current = queue.popleft()
            for neighbor in ((x + 1, y), (x, y + 1), (x, y - 1), (x - 1, y)):
                if neighbor not in previous and self.at(*neighbor) == FREE:
                    previous[neighbor] = current
                    queue.append(neighbor)
        path = []
        point = goal if goal in previous else None
        while point is not None:
            path.append(self.center(point))
            point = previous[point]
        return list(reversed(path))

    def covers_segment(self, first, second):
        # Every span between consecutive grid lines stays in one cell or on
        # a shared edge. At a boundary, an adjacent free cell covers the point.
        start, finish = self.grid_point(first), self.grid_point(second)
        dx, dy = (b - a for a, b in zip(start, finish, strict=True))
        parameters = {0.0, 1.0}
        for a, b in zip(start, finish, strict=True):
            for crossing in range(math.floor(min(a, b)) + 1, math.ceil(max(a, b))):
                parameters.add((crossing - a) / (b - a))
        parameters = sorted(parameters)

        def candidate_cells(value):
            nearest = round(value)
            if abs(value - nearest) <= 1e-5:
                return nearest - 1, nearest
            return (math.floor(value),)

        samples = parameters + [(a + b) * 0.5 for a, b in zip(parameters, parameters[1:])]
        return all(any(self.at(x, y) == FREE for x in candidate_cells(start[0] + dx * t)
                       for y in candidate_cells(start[1] + dy * t)) for t in samples)


def load_map(case, fixture_dir, directory):
    manifest = read_json(directory / "waypoints.json")
    assert manifest["schema"] == "container.bim-navigation-map" and manifest["version"] == 1, case.id
    assert manifest["frameId"] == "map", case.id
    request = case.request()
    assert manifest["originMeters"] == request["originMeters"] + [0], case.id
    for key in ("sourceNamespace", "sizeCells", "resolutionMeters", "floorMeters", "floorToleranceMeters",
                "radiusMeters", "heightMeters", "openDoorIds", "waypoints"):
        assert manifest[key] == request[key], (case.id, key, manifest[key], request[key])
    source = (fixture_dir / case.source).read_bytes()
    assert manifest["sceneSource"]["layers"] == [{"sha256": sha256(source), "byteSize": len(source)}], case.id
    scale = manifest["sceneSource"]["metersPerUnscaledRendererUnit"]
    assert math.isfinite(scale) and scale > 0, (case.id, scale)

    raw = (directory / "map.pgm").read_bytes()
    header = re.match(rb"P5\r?\n([0-9]+) ([0-9]+)\r?\n255\r?\n", raw)
    assert header, (case.id, raw[:30])
    width, height = map(int, header.groups())
    assert [width, height] == manifest["sizeCells"], case.id
    pixels = raw[header.end():]
    assert len(pixels) == width * height and set(pixels) <= {OCCUPIED, UNKNOWN, FREE}, case.id
    assert sha256(raw) == manifest["imageSha256"], case.id
    counts = Counter(pixels)
    assert manifest["cells"] == {"free": counts[FREE], "occupied": counts[OCCUPIED], "unknown": counts[UNKNOWN]}, case.id
    yaml_lines = (directory / "map.yaml").read_text(encoding="utf-8").splitlines()
    metadata = dict(line.split(":", 1) for line in yaml_lines if line.strip())
    assert metadata["image"].strip().strip("\"'") == "map.pgm", case.id
    assert metadata["mode"].strip().strip("\"'") == "trinary", case.id
    assert int(metadata["negate"]) == 0, case.id
    assert float(metadata["free_thresh"]) == 0.25 and float(metadata["occupied_thresh"]) == 0.65, case.id
    assert float(metadata["resolution"]) == manifest["resolutionMeters"], case.id
    assert json.loads(metadata["origin"]) == manifest["originMeters"], case.id
    rows = tuple(pixels[start:start + width] for start in range(0, len(pixels), width))
    return manifest, Grid(tuple(manifest["originMeters"][:2]), manifest["resolutionMeters"], rows[::-1])


def check_probes(case, grid):
    expected = {(3.05, 0.05): UNKNOWN, (-3.05, 0.05): UNKNOWN, (1.75, 3.05): UNKNOWN,
                (0.05, 0.05): FREE if case.door_open or case.floor == 3.7 else OCCUPIED,
                (0.05, 0.45): FREE if case.floor == 3.7 else OCCUPIED}
    if case.namespace == "doorway":
        expected.update({(1.85, -2.05): OCCUPIED, (1.85, 2.05): FREE, (0.05, 2.55): FREE})
    else:
        expected[(0.05, 2.55)] = FREE if case.floor == 3.7 else OCCUPIED
        # The lower partition reaches the edge. Its inflated footprint extends
        # beyond the slab even though the exterior cell has no support.
        expected[(0.05, 3.05)] = UNKNOWN if case.floor == 3.7 else OCCUPIED
    for point, occupancy in expected.items():
        assert grid.probe(point) == occupancy, (case.id, point, grid.probe(point), occupancy)


def check_route(case, grid, path):
    assert len(path) >= 2, (case.id, path)
    tolerance = grid.pitch * math.sqrt(2) + 1e-6
    assert math.dist(path[0], START) <= tolerance and math.dist(path[-1], GOAL) <= tolerance, case.id
    # Include the short trajectories from an exact requested pose to a
    # planner's discretized endpoint when checking the physical route.
    route = [START, *path, GOAL]
    crossings = []
    for first, second in zip(route, route[1:]):
        assert grid.covers_segment(first, second), (case.id, "path leaves free cells", first, second)
        if min(first[0], second[0]) <= 0 <= max(first[0], second[0]):
            if first[0] == second[0]:
                crossings.extend([first[1], second[1]])
            else:
                crossings.append(first[1] - first[0] * (second[1] - first[1]) / (second[0] - first[0]))
    assert crossings, (case.id, "path did not cross the partition's X coordinate")
    if case.expected in ("aperture", "direct"):
        assert all(abs(y) < 0.35 + 1e-6 for y in crossings), (case.id, crossings)
    elif case.expected == "detour":
        assert all(abs(y) > 2.25 - 1e-6 for y in crossings), (case.id, crossings)
    return route


def collision_walks(collision, fixture_dir, scratch, case, manifest, walks):
    config = scratch / f"{case.id}-walks.json"
    report = scratch / f"{case.id}-collision.json"
    write_json(config, {"version": 1, "openDoorIds": manifest["openDoorIds"], "walks": walks,
                        "agent": {"radius": manifest["radiusMeters"], "height": manifest["heightMeters"],
                                  "sampleSpacing": 0.025}})
    run_tool([collision, fixture_dir / case.source, report, manifest["sourceNamespace"], config])
    result = read_json(report)["walks"]
    assert len(result) == len(walks), case.id
    return result


def verify_sweeps(collision, fixture_dir, scratch, case, manifest, route, *, test_blocker=False):
    def edge(first, second):
        return {"from": [*first, case.floor], "to": [*second, case.floor]}

    walks = [edge(first, second) for first, second in zip(route, route[1:]) if tuple(first) != tuple(second)]
    path_edges = len(walks)
    blockers = []
    if test_blocker and case.floor == 1.2:
        walks.append(edge((-1.55, 0.45), (1.55, 0.45)))
        blockers.append("wall-guid" if case.namespace == "doorway" else "0000000000000000000027")
        if not case.door_open:
            walks.append(edge(START, GOAL))
            blockers.append(case.door_id)
    if not walks:
        return path_edges
    results = collision_walks(collision, fixture_dir, scratch, case, manifest, walks)
    support = ("lower-slab" if case.namespace == "doorway" else
               "0000000000000000000017" if case.floor == 1.2 else "0000000000000000000053")
    for result in results[:path_edges]:
        assert result["reachable"] and result["blockingHit"] is None, (case.id, result)
        assert result["supportSamples"], (case.id, result)
        for sample in result["supportSamples"]:
            assert sample["entityId"].endswith(support), (case.id, sample)
            assert abs(sample["feet"][2] - case.floor) <= manifest["floorToleranceMeters"], (case.id, sample)
    for result, blocker in zip(results[path_edges:], blockers, strict=True):
        assert not result["reachable"] and result["reason"] == "blocked", (case.id, result)
        assert result["blockingHit"]["entityId"].endswith(blocker), (case.id, result)
    return path_edges


def export_and_verify(exporter, collision, fixtures, output):
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bim-nav-map-") as directory:
        scratch = Path(directory)
        raster_by_case = {}
        for case in CASES:
            request_path = scratch / f"{case.id}-request.json"
            write_json(request_path, case.request())
            case_dir = scratch / case.id
            run_tool([exporter, fixtures / case.source, request_path, case_dir])
            manifest, grid = load_map(case, fixtures, case_dir)
            check_probes(case, grid)
            path = grid.route()
            assert bool(path) == (case.expected != "unreachable"), case.id
            route = check_route(case, grid, path) if path else []
            verify_sweeps(collision, fixtures, scratch, case, manifest, route, test_blocker=True)
            raster_by_case[case.id] = grid.rows
            if case.id.endswith("-metres"):
                assert grid.rows == raster_by_case[case.id.removesuffix("-metres")], case.id

        case = CASES[0]
        original = scratch / case.id
        existing = {file.name: file.read_bytes() for file in original.iterdir()}
        request_path = scratch / f"{case.id}-request.json"
        run_tool([exporter, fixtures / case.source, request_path, original], expect_success=False)
        run_tool([exporter, fixtures / case.source, request_path, scratch / "repeat"])
        for published in (original, scratch / "repeat"):
            assert {file.name: file.read_bytes() for file in published.iterdir()} == existing
        assert set(existing) == {"map.yaml", "map.pgm", "waypoints.json"}
        invalid = case.request()
        invalid["openDoorIds"] = ["unknown-door"]
        write_json(request_path, invalid)
        rejected = scratch / "rejected"
        result = run_tool([exporter, fixtures / case.source, request_path, rejected], expect_success=False)
        assert "unknown-door" in result.stderr and not rejected.exists(), result.stderr

        run_dir = Path(tempfile.mkdtemp(prefix="run-", dir=output))
        for case in CASES:
            shutil.copytree(scratch / case.id, run_dir / case.id)
        write_json(run_dir / "cases.json", case_index())
    return run_dir


def verify_nav2(collision, fixtures, output, results_path):
    if not (output / "cases.json").is_file():
        indexes = list(output.glob("run-*/cases.json"))
        assert len(indexes) == 1, "Use the exact map artifact run directory containing cases.json"
        output = indexes[0].parent
    assert read_json(output / "cases.json") == case_index(), "unexpected map artifact index"
    results = read_json(results_path)
    assert results["schema"] == RESULTS_SCHEMA and results["version"] == 1, results_path
    by_id = {result["id"]: result for result in results["cases"]}
    assert set(by_id) == {case.id for case in CASES} and len(results["cases"]) == len(CASES), results_path
    evidence = []
    with tempfile.TemporaryDirectory(prefix="bim-nav2-sweeps-") as directory:
        for case in CASES:
            manifest, grid = load_map(case, fixtures, output / case.id)
            record = by_id[case.id]
            assert record["frameId"] == "map" and record["imageSha256"] == manifest["imageSha256"], record
            path = record["pathMeters"]
            assert isinstance(path, list) and len(path) <= 4096, case.id
            assert all(isinstance(point, list) and len(point) == 2
                       and all(type(value) in (int, float) and math.isfinite(value) and abs(value) < 10
                               for value in point) for point in path), case.id
            poses = record["poses"]
            assert len(poses) == len(path), (case.id, "Nav2 pose count does not match the route")
            assert all(pose["header"]["frameId"] == "map" and pose["positionMeters"][:2] == point
                       for pose, point in zip(poses, path, strict=True)), (case.id, "Nav2 poses differ from swept route")
            if case.expected == "unreachable":
                assert record["status"] == "unreachable" and not path and not grid.route(), record
                assert record["actionStatus"] == 6 and record["errorCode"] == 208, record
                route = []
            else:
                assert record["status"] == "succeeded" and record["actionStatus"] == 4 and record["errorCode"] == 0, record
                route = check_route(case, grid, path)
            swept = verify_sweeps(collision, fixtures, Path(directory), case, manifest, route, test_blocker=True)
            evidence.append({"id": case.id, "imageSha256": manifest["imageSha256"], "floorMeters": case.floor,
                             "status": record["status"], "sweptSegments": swept})
    summary = output / "nav2-sweeps.json"
    write_json(summary, {"schema": "container.bim-nav2-physical-verification", "version": 1, "cases": evidence})
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exporter", type=Path)
    parser.add_argument("--collision", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True, help="Directory containing doorway.ifc")
    parser.add_argument("--output", type=Path, required=True, help="Directory for generated or downloaded map artifacts")
    parser.add_argument("--verify-nav2", type=Path, help="Check planner results using existing maps in --output")
    args = parser.parse_args()
    collision = args.collision.resolve(strict=True)
    if args.verify_nav2:
        summary = verify_nav2(collision, args.fixtures, args.output, args.verify_nav2)
        print(f"Swept each returned Nav2 route against BIM collision geometry. Evidence in {summary}")
    elif args.exporter:
        run_dir = export_and_verify(args.exporter.resolve(strict=True), collision, args.fixtures, args.output)
        print(f"Verified map encoding, door and floor isolation, metric units, and collision clearance in {run_dir}")
    else:
        parser.error("--exporter is required unless --verify-nav2 is supplied")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
