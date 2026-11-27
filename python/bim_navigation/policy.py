"""A deterministic point-goal controller using the current LiDAR observation."""

import numpy as np


BASELINE_POLICY_VERSION = "bim-lidar-point-goal-v1"
_CLEARANCE_RADIUS_METERS = 0.30


def baseline_action(observation):
    """Steer toward the goal through headings with sufficient LiDAR clearance."""
    goal = np.asarray(observation["goal"], dtype=np.float64)
    ranges = np.asarray(observation["ranges"], dtype=np.float64)
    if (goal.shape != (2,) or ranges.ndim != 1 or len(ranges) < 4 or
            not np.isfinite(goal).all() or not np.isfinite(ranges).all() or np.any(ranges < 0)):
        raise ValueError("Baseline policy requires a finite planar goal and LiDAR samples")

    distance = float(np.hypot(*goal))
    if distance <= 0.35:
        return np.zeros(2, dtype=np.float32)

    goal_angle = np.arctan2(goal[1], goal[0])
    ray_angles = np.arange(len(ranges)) * (2.0 * np.pi / len(ranges))
    headings = np.concatenate(([goal_angle], (ray_angles + np.pi) % (2 * np.pi) - np.pi))
    # Intersect a disc swept along each heading with the measured points.
    delta = ray_angles[None, :] - headings[:, None]
    along = ranges[None, :] * np.cos(delta)
    lateral = ranges[None, :] * np.sin(delta)
    radius_squared = _CLEARANCE_RADIUS_METERS ** 2
    in_corridor = (along > 0) & (lateral ** 2 < radius_squared)
    clearance = np.min(np.where(
        in_corridor, np.maximum(0.0, along - np.sqrt(np.maximum(0.0, radius_squared - lateral ** 2))), np.inf
    ), axis=1)
    lookahead = np.clip(distance, 0.45, 1.30)
    clear_fraction = np.minimum(1.0, clearance / lookahead)
    score = 2.0 * clear_fraction + np.cos(headings - goal_angle) - 0.18 * np.abs(headings)
    choice = int(np.argmax(score))
    angle = headings[choice]
    angular = np.clip(angle / 0.65, -1.0, 1.0)
    linear = np.clip(1.0 - abs(angle) / 0.70, 0.0, 1.0) * min(1.0, distance / 0.65, clearance[choice] / 0.70)
    return np.asarray((linear, angular), dtype=np.float32)
