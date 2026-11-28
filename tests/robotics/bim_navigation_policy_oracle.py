"""Shortest paths on an exported BIM capsule grid, with optional NavFn guard."""

from array import array
from collections import Counter
from dataclasses import dataclass, field
import hashlib
import heapq
import json
import math
from pathlib import Path
import re

from bim_nav2_planner import guarded_cells, read_binary_pgm


FREE, UNKNOWN, OCCUPIED = 0, -1, 100
DIAGONAL_LENGTH = math.sqrt(2)
MOVES = (
    (-1, 0, 1.0), (0, -1, 1.0), (1, 0, 1.0), (0, 1, 1.0),
    (-1, -1, DIAGONAL_LENGTH), (1, -1, DIAGONAL_LENGTH),
    (1, 1, DIAGONAL_LENGTH), (-1, 1, DIAGONAL_LENGTH),
)


def _number(value):
    return type(value) in (int, float) and math.isfinite(value)


def _digest(data):
    return hashlib.sha256(data).hexdigest()


def _require(condition, message):
    if not condition:
        raise ValueError(message)


@dataclass(frozen=True)
class MapIdentity:
    frame_id: str
    source_namespace: str
    size_cells: tuple[int, int]
    origin_m: tuple[float, float, float]
    resolution_m: float
    layer_fingerprints: tuple[tuple[str, int], ...]
    source_scale: float
    floor_m: float
    floor_tolerance_m: float
    capsule_radius_m: float
    capsule_height_m: float
    open_door_ids: tuple[str, ...]
    image_sha256: str
    yaml_sha256: str
    manifest_sha256: str

    @classmethod
    def from_manifest(cls, manifest, image_sha256, yaml_data, manifest_data):
        source = manifest["sceneSource"]
        layers = source["layers"]
        scale = source["metersPerUnscaledRendererUnit"]
        floor, tolerance = (manifest[key] for key in ("floorMeters", "floorToleranceMeters"))
        radius, height = (manifest[key] for key in ("radiusMeters", "heightMeters"))
        door_ids, namespace = (manifest[key] for key in ("openDoorIds", "sourceNamespace"))
        _require(isinstance(layers, list) and bool(layers) and
                 all(re.fullmatch(r"[0-9a-f]{64}", layer["sha256"]) and
                     type(layer["byteSize"]) is int and layer["byteSize"] > 0 for layer in layers) and
                 _number(scale) and scale > 0, "Invalid BIM source fingerprint")
        _require(all(_number(value) for value in (floor, tolerance, radius, height)) and
                 tolerance >= 0 and radius > 0 and height > 0, "Invalid exported floor or capsule")
        _require(isinstance(namespace, str) and bool(namespace) and
                 isinstance(door_ids, list) and all(isinstance(door, str) and door for door in door_ids),
                 "Invalid BIM namespace or door identities")
        return cls(manifest["frameId"], namespace, tuple(manifest["sizeCells"]),
                   tuple(manifest["originMeters"]), manifest["resolutionMeters"],
                   tuple((layer["sha256"], layer["byteSize"]) for layer in layers), scale,
                   floor, tolerance, radius, height, tuple(door_ids), image_sha256,
                   _digest(yaml_data), _digest(manifest_data))

    def as_dict(self):
        return {
            "frameId": self.frame_id,
            "sourceNamespace": self.source_namespace,
            "sizeCells": list(self.size_cells),
            "originMeters": list(self.origin_m),
            "resolutionMeters": self.resolution_m,
            "sceneSource": {
                "layers": [{"sha256": digest, "byteSize": size} for digest, size in self.layer_fingerprints],
                "metersPerUnscaledRendererUnit": self.source_scale,
            },
            "floorMeters": self.floor_m,
            "floorToleranceMeters": self.floor_tolerance_m,
            "radiusMeters": self.capsule_radius_m,
            "heightMeters": self.capsule_height_m,
            "openDoorIds": list(self.open_door_ids),
            "imageSha256": self.image_sha256,
            "yamlSha256": self.yaml_sha256,
            "manifestSha256": self.manifest_sha256,
        }


@dataclass(frozen=True)
class FrozenGrid:
    width: int
    height: int
    resolution: float
    origin: tuple[float, float]
    cells: tuple[int, ...]
    identity: MapIdentity | None = None
    guarded_occupancy: tuple[int, ...] = field(init=False, repr=False)

    def __post_init__(self):
        _require(type(self.width) is int and type(self.height) is int and 1 <= self.width <= 4096 and
                 1 <= self.height <= 4096 and self.width * self.height <= 4194304,
                 "Invalid BIM map dimensions")
        _require(_number(self.resolution) and self.resolution > 0 and
                 len(self.origin) == 2 and all(map(_number, self.origin)), "Invalid BIM map geometry")
        for origin, size in zip(self.origin, (self.width, self.height), strict=True):
            end = origin + size * self.resolution
            _require(math.isfinite(end) and origin + self.resolution > origin and
                     end - self.resolution < end, "BIM cell coordinates must have finite distinct edges")
        _require(isinstance(self.cells, tuple) and len(self.cells) == self.width * self.height and
                 set(self.cells) <= {FREE, UNKNOWN, OCCUPIED}, "Invalid trinary occupancy values")
        object.__setattr__(self, "guarded_occupancy", guarded_cells(self))

    @classmethod
    def from_export(cls, directory):
        import yaml

        directory = Path(directory)
        manifest_data = (directory / "waypoints.json").read_bytes()
        manifest = json.loads(manifest_data)
        _require(manifest["schema"] == "container.bim-navigation-map" and manifest["version"] == 1 and
                 manifest["frameId"] == "map", "Expected version 1 navigation map in the map frame")
        width, height = manifest["sizeCells"]
        resolution = manifest["resolutionMeters"]
        origin = tuple(manifest["originMeters"])
        _require(len(origin) == 3 and _number(origin[2]) and origin[2] == 0,
                 "Expected axis-aligned map origin with zero yaw")
        yaml_data = (directory / "map.yaml").read_bytes()
        config = yaml.safe_load(yaml_data)
        _require(config["image"] == "map.pgm" and config["mode"] == "trinary" and
                 config["negate"] == 0 and config["free_thresh"] == .25 and
                 config["occupied_thresh"] == .65 and tuple(config["origin"]) == origin and
                 config["resolution"] == resolution, "Nav2 YAML does not match the exported trinary map")
        image = (directory / "map.pgm").read_bytes()
        image_sha256 = _digest(image)
        _require(image_sha256 == manifest["imageSha256"], "PGM SHA-256 differs from the waypoint manifest")
        cells = read_binary_pgm(image, width, height)
        counts = Counter(cells)
        _require(manifest["cells"] == {"free": counts[FREE], "occupied": counts[OCCUPIED],
                                      "unknown": counts[UNKNOWN]}, "Map occupancy counts differ from the manifest")
        identity = MapIdentity.from_manifest(manifest, image_sha256, yaml_data, manifest_data)
        return cls(width, height, resolution, origin[:2], cells, identity)

    def world_to_cell(self, point):
        coordinates = []
        for value, origin, size in zip(point, self.origin, (self.width, self.height), strict=True):
            if not origin <= value < origin + size * self.resolution:
                return None
            index = math.floor((value - origin) / self.resolution)
            coordinates.append(min(index, size - 1))
        return tuple(coordinates)

    def center(self, cell):
        return tuple(origin + (index + .5) * self.resolution
                     for index, origin in zip(cell, self.origin, strict=True))


@dataclass(frozen=True)
class Route:
    map_identity: MapIdentity | None
    guard: bool
    start_xy: tuple[float, float]
    goal_xy: tuple[float, float]
    start_cell: tuple[int, int] | None
    goal_cell: tuple[int, int] | None
    reason: str
    expanded: int
    points: tuple[tuple[float, float], ...] = ()
    length_m: float | None = None

    @property
    def reachable(self):
        return self.length_m is not None

    def as_dict(self):
        return {
            "reachable": self.reachable,
            "reason": self.reason,
            "startMeters": list(self.start_xy),
            "goalMeters": list(self.goal_xy),
            "startCell": None if self.start_cell is None else list(self.start_cell),
            "goalCell": None if self.goal_cell is None else list(self.goal_cell),
            "lengthMeters": self.length_m,
            "expandedCells": self.expanded,
            "pathMeters": [list(point) for point in self.points],
            "connectivity": "8-neighbor-no-corner-cutting",
            "guard": "navfn-cardinal-cell" if self.guard else "exported-capsule-only",
            "map": None if self.map_identity is None else self.map_identity.as_dict(),
        }


def _world_point(point):
    point = tuple(point)
    _require(len(point) == 2 and all(map(_number, point)), "Expected two finite world coordinates")
    return point


def _endpoint_reason(grid, occupancy, cell, label):
    if cell is None:
        return f"{label}-outside-map"
    index = cell[1] * grid.width + cell[0]
    if occupancy[index] == FREE:
        return None
    cause = {UNKNOWN: "unknown", OCCUPIED: "occupied", FREE: "guarded"}[grid.cells[index]]
    return f"{label}-{cause}"


def _octile(cell, goal):
    x, y = (abs(first - second) for first, second in zip(cell, goal, strict=True))
    diagonal = min(x, y)
    return abs(x - y) + diagonal * DIAGONAL_LENGTH


def _world_path(grid, parents, end, start_xy, goal_xy):
    centers = []
    while end >= 0:
        centers.append(grid.center((end % grid.width, end // grid.width)))
        end = parents[end]
    path = [start_xy]
    for point in (*reversed(centers), goal_xy):
        if path[-1] != point:
            path.append(point)
    return tuple(path)


def shortest_path(grid, start_xy, goal_xy, *, guarded=True):
    """Find the minimum-length grid route, with exact endpoint connectors.

    Interior points are cell centers. Endpoints connect to the center of their
    containing cell, or directly when both points lie in the same cell. Every
    diagonal requires both side cells to be free. No cost or clearance gets
    inferred for unknown cells. In guarded mode the exported capsule map gets
    the same additional cardinal-cell guard used by the Jazzy planner.
    """
    start_xy, goal_xy = _world_point(start_xy), _world_point(goal_xy)
    occupancy = grid.guarded_occupancy if guarded else grid.cells
    start_cell, goal_cell = grid.world_to_cell(start_xy), grid.world_to_cell(goal_xy)
    report = (grid.identity, guarded, start_xy, goal_xy, start_cell, goal_cell)
    reason = _endpoint_reason(grid, occupancy, start_cell, "start") or _endpoint_reason(
        grid, occupancy, goal_cell, "goal")
    if reason:
        return Route(*report, reason, 0)
    if start_cell == goal_cell:
        points = (start_xy,) if start_xy == goal_xy else (start_xy, goal_xy)
        return Route(*report, "reached", 1, points, math.dist(start_xy, goal_xy))

    start = start_cell[1] * grid.width + start_cell[0]
    goal = goal_cell[1] * grid.width + goal_cell[0]
    distances = array("d", [math.inf]) * len(occupancy)
    parents = array("i", [-1]) * len(occupancy)
    distances[start] = 0
    estimate = _octile(start_cell, goal_cell)
    queue = [(estimate, estimate, start, 0.0)]
    expanded = 0
    while queue:
        _, _, current, distance = heapq.heappop(queue)
        if distance != distances[current]:
            continue
        expanded += 1
        if current == goal:
            points = _world_path(grid, parents, goal, start_xy, goal_xy)
            measured = math.fsum(math.dist(a, b) for a, b in zip(points, points[1:]))
            return Route(*report, "reached", expanded, points, measured)
        x, y = current % grid.width, current // grid.width
        for dx, dy, step in MOVES:
            column, row = x + dx, y + dy
            if not (0 <= column < grid.width and 0 <= row < grid.height):
                continue
            neighbor = row * grid.width + column
            if occupancy[neighbor] != FREE:
                continue
            if dx and dy and (occupancy[current + dx] != FREE or
                              occupancy[current + dy * grid.width] != FREE):
                continue
            candidate = distance + step
            if candidate < distances[neighbor]:
                distances[neighbor] = candidate
                parents[neighbor] = current
                estimate = _octile((column, row), goal_cell)
                heapq.heappush(queue, (candidate + estimate, estimate, neighbor, candidate))
    return Route(*report, "disconnected", expanded)
