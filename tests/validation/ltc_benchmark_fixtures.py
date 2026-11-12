"""Matched receiver and emitter settings for the opt-in area-light benchmark."""

from dataclasses import dataclass
import hashlib
import math


BENCHMARK_MATERIALS = ("metallic", "dielectric")
BENCHMARK_COUNTS = (1, 8, 32, 128)
BENCHMARK_FOV_DEGREES = 36

# Match the disk and rectangle controls in distance, camera and material. A
# distance of two scene units keeps the disk's fitted rim bound in the coarse
# domain across the default 960×540 receiver viewport.
_CONTROL = dict(light=(0., 0., 2.), intensity=14.)
_PRESETS = {
    "coarse": _CONTROL,
    "rectangle": _CONTROL,
    # Put the receiver center just above the rim and outside the distance guard.
    "adaptive-near": dict(halfSize=(.6, .6), light=(.6, 0., .14),
                          camera=(0., 0., 1.8), intensity=1.),
    # The tilted disk spans z=0 and clips against the receiver horizon.
    "adaptive-horizon": dict(halfSize=(.65, .65), light=(.58, 0., .24),
                             camera=(0., 0., .8), yaw=math.pi / 3, intensity=1.),
    # Looking toward the opposite side of the disk widens the Schlick interval
    # while the emitter remains far enough for coarse geometry.
    "material-residual": dict(light=(-5., 0., 1.25), camera=(.5, 0., .4),
                              yaw=math.atan2(-5., 1.25), intensity=30.),
    # Roughness below .28 rejects LTC before evaluating any geometry.
    "sampled-fallback": dict(**_CONTROL, roughness=.15),
    # Preserve the historical disk sweep geometry as an explicit cohort.
    "legacy-disk": dict(halfSize=(.4, .4), light=(0, 0, 1.5), camera=(0, 0, 5),
                        intensity=8., roughness=.6),
}
BENCHMARK_DOMAINS = tuple(_PRESETS)
_DEFAULT_DOMAINS = tuple(domain for domain in BENCHMARK_DOMAINS if domain != "legacy-disk")
_CONTROL_DOMAINS = ("coarse", "rectangle")
_MATERIAL_FIELDS = (
    "albedo", "alpha", "metallic", "roughness", "specular", "clearcoat",
    "clearcoatRoughness", "sheen", "sheenRoughness", "iridescence", "iridescenceIor",
    "iridescenceThicknessMinimum", "iridescenceThicknessMaximum", "doubleSided",
)


@dataclass(frozen=True)
class BenchmarkFixture:
    id: str
    domain: str
    material: str
    count: int
    settings: dict

    @property
    def manual_exposure(self):
        """Use the historical capture exposure for the legacy cohort."""
        return .2 if self.domain == "legacy-disk" else None

    def manifest(self, model, width, height, samples, exposure):
        """Return comparable settings without machine-specific paths."""
        settings = self.settings
        return {
            "domain": self.domain,
            "fixtureSha256": hashlib.sha256(model.read_bytes()).hexdigest(),
            "receiver": {
                "materialDomain": self.material,
                **{key: settings[key] for key in _MATERIAL_FIELDS},
            },
            "emitter": {
                "shape": "disk" if settings["disk"] else "rectangle",
                "count": self.count,
                "layout": "coincident",
                "position": settings["light"],
                "halfSize": settings["halfSize"],
                "range": settings["range"],
                "lengthUnits": "scene units",
                "yawRadians": settings["yaw"],
                "mirrored": settings["mirrored"],
                "color": [1, 1, 1],
                "radiancePerEmitter": settings["intensity"] / self.count,
                "aggregateRadiance": settings["intensity"],
                "radianceUnits": "emitted scene-linear radiance in renderer units",
            },
            "camera": {
                "position": settings["camera"],
                "target": settings.get("target", [0, 0, 0]),
                "verticalFovDegrees": BENCHMARK_FOV_DEGREES,
            },
            "renderTechnique": "deferred-raster",
            "displayMode": "lit",
            "importScale": 1.0,
            "resolution": [width, height],
            "areaLightSamples": samples,
            "msaaSamples": 1,
            "taa": False,
            "bloom": False,
            "exposure": exposure,
            "environmentIntensity": 0,
            "directionalIntensity": 0,
            "bounceIntensity": 0,
            "ddgi": False,
            "rayQuery": False,
            "rayShadowMode": "raster",
            "activeLocalShadowLayers": 0,
        }


def benchmark_fixtures(make_case, materials=BENCHMARK_MATERIALS,
                       counts=BENCHMARK_COUNTS, domains=None):
    """Yield all control counts and one case per other domain by default.

    An explicit domain selection requests its full material and count matrix
    and can include the legacy disk sweep. The benchmark uses the same
    receiver builder as the accuracy checks.
    """
    counts = tuple(dict.fromkeys(counts))
    materials = tuple(dict.fromkeys(materials))
    selected_domains = _DEFAULT_DOMAINS if domains is None else tuple(dict.fromkeys(domains))
    for domain in selected_domains:
        domain_counts = counts if domains is not None or domain in _CONTROL_DOMAINS else counts[:1]
        disk = domain != "rectangle"
        preset = {"halfSize": (.4, .4 if disk else .3), **_PRESETS[domain]}
        for material in materials:
            for count in domain_counts:
                fixture_id = f"{domain}-{material}-{count}"
                # Use independent JSON arrays for each fixture and its manifest.
                parameters = {key: list(value) if isinstance(value, tuple) else value
                              for key, value in preset.items()}
                settings = make_case(fixture_id, disk=disk, metallic=float(material == "metallic"),
                                     **parameters)
                yield BenchmarkFixture(fixture_id, domain, material, count, settings)
