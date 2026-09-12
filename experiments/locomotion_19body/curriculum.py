"""Curriculum schedule shared by the Asterra 19-body locomotion experiment.

This module has no Isaac Lab dependency so it can be unit-tested independently and
mirrored in analysis tooling. The RL environment should call schedule(progress)
with progress in [0, 1] and apply the returned weights/randomization intensity.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class CurriculumState:
    imitation_scale: float
    task_scale: float
    perturbation_scale: float
    speed_min: float
    speed_max: float
    slope_deg: float
    mass_randomization: float
    friction_randomization: float


def _lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def _segment(progress: float, start: float, end: float) -> float:
    if end <= start:
        return 1.0
    return max(0.0, min(1.0, (progress - start) / (end - start)))


def schedule(progress: float) -> CurriculumState:
    """Return curriculum parameters for normalized training progress."""

    p = max(0.0, min(1.0, float(progress)))

    if p < 0.20:
        t = _segment(p, 0.0, 0.20)
        return CurriculumState(
            imitation_scale=_lerp(1.00, 0.90, t),
            task_scale=_lerp(0.25, 0.45, t),
            perturbation_scale=0.0,
            speed_min=_lerp(0.80, 0.70, t),
            speed_max=_lerp(1.35, 1.50, t),
            slope_deg=0.0,
            mass_randomization=0.0,
            friction_randomization=0.0,
        )

    if p < 0.50:
        t = _segment(p, 0.20, 0.50)
        return CurriculumState(
            imitation_scale=_lerp(0.90, 0.68, t),
            task_scale=_lerp(0.45, 0.85, t),
            perturbation_scale=_lerp(0.05, 0.35, t),
            speed_min=_lerp(0.70, 0.55, t),
            speed_max=_lerp(1.50, 1.70, t),
            slope_deg=_lerp(0.0, 2.5, t),
            mass_randomization=_lerp(0.0, 0.04, t),
            friction_randomization=_lerp(0.0, 0.12, t),
        )

    if p < 0.80:
        t = _segment(p, 0.50, 0.80)
        return CurriculumState(
            imitation_scale=_lerp(0.68, 0.42, t),
            task_scale=_lerp(0.85, 1.10, t),
            perturbation_scale=_lerp(0.35, 0.72, t),
            speed_min=_lerp(0.55, 0.40, t),
            speed_max=_lerp(1.70, 2.05, t),
            slope_deg=_lerp(2.5, 5.0, t),
            mass_randomization=_lerp(0.04, 0.08, t),
            friction_randomization=_lerp(0.12, 0.28, t),
        )

    t = _segment(p, 0.80, 1.0)
    return CurriculumState(
        imitation_scale=_lerp(0.42, 0.28, t),
        task_scale=_lerp(1.10, 1.25, t),
        perturbation_scale=_lerp(0.72, 1.0, t),
        speed_min=_lerp(0.40, 0.25, t),
        speed_max=_lerp(2.05, 2.30, t),
        slope_deg=_lerp(5.0, 8.0, t),
        mass_randomization=_lerp(0.08, 0.10, t),
        friction_randomization=_lerp(0.28, 0.45, t),
    )


def schedule_dict(progress: float) -> dict[str, float]:
    return asdict(schedule(progress))


if __name__ == "__main__":
    for p in (0.0, 0.2, 0.5, 0.8, 1.0):
        print(f"{p:0.2f}: {schedule_dict(p)}")
