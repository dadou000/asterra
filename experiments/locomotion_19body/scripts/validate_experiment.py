#!/usr/bin/env python3
"""Validate the static contract for the Asterra 19-body walking experiment."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "config"


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def validate_rig(rig: dict) -> list[str]:
    errors: list[str] = []
    bodies = rig.get("canonical_bodies", [])
    body_set = set(bodies)
    expected_count = int(rig.get("body_count", -1))
    if len(bodies) != expected_count:
        errors.append(f"body_count says {expected_count}, found {len(bodies)} canonical bodies")
    if len(body_set) != len(bodies):
        errors.append("canonical_bodies contains duplicates")

    root = rig.get("root_body")
    if root not in body_set:
        errors.append(f"root_body {root!r} is not a canonical body")

    parents = rig.get("expected_parent", {})
    for child, parent in parents.items():
        if child not in body_set:
            errors.append(f"parent map child {child!r} is not a canonical body")
        if parent not in body_set:
            errors.append(f"parent map parent {parent!r} is not a canonical body")

    if len(parents) != max(0, len(bodies) - 1):
        errors.append("expected_parent must describe exactly one parent for every non-root body")

    for body in rig.get("required_contact_bodies", []):
        if body not in body_set:
            errors.append(f"required contact body {body!r} is missing")

    for body in bodies:
        seen: set[str] = set()
        current = body
        while current in parents:
            if current in seen:
                errors.append(f"cycle detected at {current!r}")
                break
            seen.add(current)
            current = parents[current]
        if current != root and body != root:
            errors.append(f"body {body!r} does not resolve to root {root!r}")

    runtime_map = rig.get("runtime_name_map", {})
    if runtime_map:
        values = [str(value) for value in runtime_map.values()]
        if len(values) != len(set(values)):
            errors.append("runtime_name_map maps multiple canonical roles to the same runtime body")
        unresolved = [body for body in bodies if body not in runtime_map]
        if unresolved:
            errors.append(f"runtime_name_map is incomplete: {', '.join(unresolved)}")

    return errors


def validate_experiment(exp: dict) -> list[str]:
    errors: list[str] = []
    sim = exp.get("simulation", {})
    physics_hz = int(sim.get("physics_hz", 0))
    policy_hz = int(sim.get("policy_hz", 0))
    decimation = int(sim.get("decimation", 0))
    if physics_hz <= 0 or policy_hz <= 0 or decimation <= 0:
        errors.append("physics_hz, policy_hz and decimation must be positive")
    elif physics_hz != policy_hz * decimation:
        errors.append(
            f"physics_hz ({physics_hz}) must equal policy_hz ({policy_hz}) * decimation ({decimation})"
        )
    if exp.get("policy", {}).get("action_mode") != "normalized_joint_position_targets":
        errors.append("v0 experiment must use normalized joint-position targets")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--require-runtime-map",
        action="store_true",
        help="Fail unless all 19 canonical roles map to final runtime/USD body names.",
    )
    args = parser.parse_args()

    rig = load_json(CONFIG / "rig_19body.json")
    exp = load_json(CONFIG / "walk_experiment.json")
    motion = load_json(CONFIG / "motion_sources.json")
    io = load_json(CONFIG / "policy_io.json")

    errors = validate_rig(rig) + validate_experiment(exp)
    if args.require_runtime_map and not rig.get("runtime_name_map"):
        errors.append("runtime_name_map is empty; export the final 19-body articulation and fill it first")

    if motion.get("source_fps") != 120:
        errors.append("CMU source manifest is expected to be 120 fps")
    if io.get("runtime_rate_hz") != 60:
        errors.append("v0 runtime policy contract is expected to be 60 Hz")

    if errors:
        print("Experiment validation FAILED:")
        for error in errors:
            print(f"  - {error}")
        return 1

    print("Experiment validation OK")
    print(f"  bodies: {rig['body_count']}")
    print(f"  physics: {exp['simulation']['physics_hz']} Hz")
    print(f"  policy: {exp['simulation']['policy_hz']} Hz")
    print(f"  initial parallel environments: {exp['simulation']['parallel_envs_initial']}")
    print(f"  target parallel environments: {exp['simulation']['parallel_envs_target']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
