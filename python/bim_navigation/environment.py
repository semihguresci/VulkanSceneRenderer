"""Gymnasium environment for the native BIM navigation task."""

from copy import deepcopy
import math

try:
    import gymnasium as gym
    from gymnasium import spaces
except ModuleNotFoundError as error:
    if error.name != "gymnasium":
        raise
    raise ModuleNotFoundError(
        "BIM navigation requires Gymnasium. Install the optional navigation dependencies."
    ) from error
import numpy as np

from ._native import NativeNavigation, NavigationProtocolError


REWARD_VERSION = "bim-navigation-progress-v1"
EPISODE_SCHEMA_VERSION = 1
PROTOCOL_VERSION = 1
RAY_COUNT = 36
GOAL_TOLERANCE_METERS = 0.35
STEP_COST = 0.01
GOAL_BONUS = 5.0
COLLISION_PENALTY = 5.0
_MAX_GOAL_OBSERVATION = float(np.finfo(np.float32).max)


def _positive_int(value, name):
    if isinstance(value, (bool, np.bool_)) or not isinstance(value, (int, np.integer)) or value <= 0:
        raise ValueError(f"{name} must be a positive integer")
    return int(value)


def _native_number(value, name):
    if type(value) not in (int, float):
        raise NavigationProtocolError(f"Invalid native {name}")
    try:
        number = float(value)
    except OverflowError as error:
        raise NavigationProtocolError(f"Invalid native {name}") from error
    if not math.isfinite(number):
        raise NavigationProtocolError(f"Invalid native {name}")
    return number


def _native_vector(state, name, count):
    values = state.get(name)
    if not isinstance(values, list) or len(values) != count:
        raise NavigationProtocolError(f"Native {name} must contain {count} numbers")
    return [_native_number(value, name) for value in values]


class BimNavigationEnv(gym.Env):
    """Navigate a fixed BIM scene using a persistent simulator process.

    The action is normalized linear and angular velocity. Positive angular
    velocity turns left. Observations include a planar goal vector in robot
    coordinates and one revolution of LiDAR samples in counter-clockwise order.
    """

    metadata = {"render_modes": []}
    render_mode = None

    def __init__(self, executable, scene, *, max_episode_steps=200, randomize=True, timeout_seconds=120):
        super().__init__()
        self.max_episode_steps = _positive_int(max_episode_steps, "max_episode_steps")
        if type(randomize) is not bool:
            raise ValueError("randomize must be a boolean")
        if (type(timeout_seconds) not in (int, float) or
                not math.isfinite(timeout_seconds) or timeout_seconds <= 0):
            raise ValueError("timeout_seconds must be finite and positive")
        self.randomize = randomize
        self._state = None
        self._episode = None
        self._needs_reset = True
        self._native = NativeNavigation(executable, scene, timeout=timeout_seconds)
        try:
            self._configure(self._native.request("describe"))
        except Exception:
            self.close()
            raise

    def _configure(self, description):
        if description.get("version") != PROTOCOL_VERSION or description.get("rayCount") != RAY_COUNT:
            raise NavigationProtocolError("Unsupported native navigation protocol or LiDAR sampling")
        scene = description.get("scene")
        fields = ("id", "family", "split", "sourceSha256", "geometryFingerprint")
        if not isinstance(scene, dict):
            raise NavigationProtocolError("Native scene identity is incomplete")
        if "sceneRecipeSha256" in scene:
            fields += ("sceneRecipeSha256",)
        if any(not isinstance(scene.get(field), str) or not scene[field].strip() for field in fields):
            raise NavigationProtocolError("Native scene identity is incomplete")
        if scene["split"] not in ("train", "held-out"):
            raise NavigationProtocolError("Native scene split must be train or held-out")
        self._scene = {field: scene[field] for field in fields}
        self.ticks_per_step = description.get("ticksPerStep")
        self.ticks_per_second = description.get("ticksPerSecond")
        if any(type(value) is not int or value <= 0 for value in (self.ticks_per_step, self.ticks_per_second)):
            raise NavigationProtocolError("Native tick rates must be positive integers")
        self.range_meters = _native_number(description.get("rangeMeters"), "LiDAR range")
        self.linear_limit = _native_number(description.get("linearLimit"), "linear velocity limit")
        self.angular_limit = _native_number(description.get("angularLimit"), "angular velocity limit")
        if (min(self.range_meters, self.linear_limit, self.angular_limit) <= 0 or
                self.range_meters > np.finfo(np.float32).max):
            raise NavigationProtocolError("Native LiDAR range and velocity limits must be supported positive values")
        self.action_space = spaces.Box(-1.0, 1.0, shape=(2,), dtype=np.float32)
        self.observation_space = spaces.Dict({
            "goal": spaces.Box(-_MAX_GOAL_OBSERVATION, _MAX_GOAL_OBSERVATION, shape=(2,), dtype=np.float32),
            "ranges": spaces.Box(0.0, self.range_meters, shape=(RAY_COUNT,), dtype=np.float32),
            "intensity": spaces.Box(0.0, 1.0, shape=(RAY_COUNT,), dtype=np.float32),
        })

    @property
    def scene(self):
        """A copy of the simulator's ID, source and train/held-out identity."""
        return dict(self._scene)

    def _parse_state(self, response):
        state = response.get("state")
        if not isinstance(state, dict):
            raise NavigationProtocolError("Simulator response omitted the navigation state")
        tick = state.get("tick")
        collision = state.get("collision")
        obstruction = state.get("obstruction")
        blocking_entity_id = state.get("blockingEntityId")
        if (type(tick) is not int or tick < 0 or type(collision) is not bool or
                not isinstance(obstruction, str) or not isinstance(blocking_entity_id, str)):
            raise NavigationProtocolError("Invalid native tick, collision or obstruction state")
        pose = _native_vector(state, "pose", 4)
        odometry = _native_vector(state, "odometry", 4)
        goal = _native_vector(state, "goal", 3)
        ranges = _native_vector(state, "ranges", RAY_COUNT)
        intensity = _native_vector(state, "intensity", RAY_COUNT)
        travelled_meters = _native_number(state.get("travelledMeters"), "distance travelled")
        if (any(value < 0 or value > self.range_meters for value in ranges) or
                any(value < 0 or value > 1 for value in intensity) or travelled_meters < 0):
            raise NavigationProtocolError("LiDAR samples or travelled distance are outside the native contract")
        return {
            "tick": tick, "pose": pose, "odometry": odometry, "goal": goal,
            "ranges": ranges, "intensity": intensity, "collision": collision,
            "obstruction": obstruction, "blockingEntityId": blocking_entity_id,
            "travelledMeters": travelled_meters,
        }

    @staticmethod
    def _observation(state):
        x, y, _, yaw = state["pose"]
        dx, dy = state["goal"][0] - x, state["goal"][1] - y
        distance = math.hypot(dx, dy)
        if not math.isfinite(distance) or distance > _MAX_GOAL_OBSERVATION:
            raise NavigationProtocolError("Native goal is outside the supported observation range")
        # REP-103 +X is forward and +Y is left. Rotate the world delta by -yaw.
        cosine, sine = math.cos(yaw), math.sin(yaw)
        return {
            "goal": np.asarray((cosine * dx + sine * dy, -sine * dx + cosine * dy), dtype=np.float32),
            "ranges": np.asarray(state["ranges"], dtype=np.float32),
            "intensity": np.asarray(state["intensity"], dtype=np.float32),
        }, distance

    @staticmethod
    def _info(state, distance):
        return {
            "tick": state["tick"], "distanceToGoalMeters": distance, "travelledMeters": state["travelledMeters"],
            "collision": state["collision"], "obstruction": state["obstruction"],
            "blockingEntityId": state["blockingEntityId"], "rewardVersion": REWARD_VERSION,
        }

    def reset(self, *, seed=None, options=None):
        if options is not None and not isinstance(options, dict):
            raise ValueError("Reset options must be a dictionary")
        options = options or {}
        if options.keys() - {"randomize"}:
            raise ValueError("The supported reset option is randomize")
        randomize = options.get("randomize", self.randomize)
        if type(randomize) is not bool:
            raise ValueError("randomize must be a boolean")
        super().reset(seed=seed)
        native_seed = int(self.np_random.bit_generator.random_raw())
        self._needs_reset = True
        self._episode = None
        self._state = None
        response = self._native.request("reset", seed=native_seed, randomize=randomize)
        state = self._parse_state(response)
        manifest = response.get("manifest")
        if not isinstance(manifest, dict):
            raise NavigationProtocolError("Simulator reset omitted the randomization manifest")
        if any(manifest.get(field) != value for field, value in self._scene.items()):
            raise NavigationProtocolError("Simulator reset changed the validated scene identity")
        observation, distance = self._observation(state)
        self._state = state
        self._distance = distance
        self._reward_total = 0.0
        self._episode = {
            "schemaVersion": EPISODE_SCHEMA_VERSION, "rewardVersion": REWARD_VERSION,
            "scene": self.scene, "seed": None if seed is None else int(seed), "nativeSeed": native_seed,
            "randomize": randomize, "manifest": manifest, "maxEpisodeSteps": self.max_episode_steps,
            "goalToleranceMeters": GOAL_TOLERANCE_METERS, "initialState": state, "steps": [],
        }
        self._episode["summary"] = self._summary("in-progress")
        self._needs_reset = False
        return observation, {**self._info(state, distance), "nativeSeed": native_seed, "manifest": deepcopy(manifest)}

    def step(self, action):
        if self._needs_reset:
            raise gym.error.ResetNeeded("Call reset before starting or restarting a navigation episode")
        action = np.asarray(action)
        if (action.shape != (2,) or action.dtype.kind not in "fiu" or
                not np.isfinite(action).all() or np.any(action < -1) or np.any(action > 1)):
            raise ValueError("Navigation action must be two finite values in [-1, 1]")
        action = action.astype(np.float64)
        command = [float(action[0] * self.linear_limit), float(action[1] * self.angular_limit)]
        self._needs_reset = True
        response = self._native.request("step", action=action.tolist())
        state = self._parse_state(response)
        if state["tick"] - self._state["tick"] != self.ticks_per_step:
            raise NavigationProtocolError("Simulator step advanced an unexpected number of ticks")
        observation, distance = self._observation(state)
        colliding = state["collision"]
        success = not colliding and distance <= GOAL_TOLERANCE_METERS
        terminated = success or colliding
        step = len(self._episode["steps"]) + 1
        truncated = not terminated and step == self.max_episode_steps
        reward_terms = {"progress": self._distance - distance, "step": -STEP_COST,
                        "goal": GOAL_BONUS if success else 0.0, "collision": -COLLISION_PENALTY if colliding else 0.0}
        reward = sum(reward_terms.values())
        self._state = state
        self._distance = distance
        self._reward_total += reward
        self._needs_reset = terminated or truncated
        self._episode["steps"].append({
            "step": step, "action": action.tolist(), "command": command, "state": state, "reward": reward,
            "rewardTerms": reward_terms, "terminated": terminated, "truncated": truncated,
        })
        outcome = "collision" if colliding else "success" if success else "time-limit" if truncated else "in-progress"
        self._episode["summary"] = self._summary(outcome)
        info = {**self._info(state, distance), "success": success, "rewardTerms": dict(reward_terms)}
        if self._needs_reset:
            info["episode"] = dict(self._episode["summary"])
        return observation, reward, terminated, truncated, info

    def _summary(self, outcome):
        return {
            "outcome": outcome, "success": outcome == "success", "collision": outcome == "collision",
            "truncated": outcome == "time-limit", "steps": len(self._episode["steps"]),
            "return": self._reward_total, "distanceToGoalMeters": self._distance,
            "travelledMeters": self._state["travelledMeters"],
            "simulatedSeconds": (self._state["tick"] - self._episode["initialState"]["tick"]) / self.ticks_per_second,
        }

    def episode_record(self):
        """Return a detached, JSON-serializable record of the current episode."""
        if self._episode is None:
            raise gym.error.ResetNeeded("Call reset before requesting an episode record")
        return deepcopy(self._episode)

    def close(self):
        self._needs_reset = True
        self._native.close()
