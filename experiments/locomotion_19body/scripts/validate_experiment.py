#!/usr/bin/env python3
"""Validate the static contract for the Asterra 19-body walking experiment."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "config"
GENERATED = ROOT / "generated"
CANONICAL_REPO_PATH = "assets/character/asterrahuman.glb"
CANONICAL_GODOT_PATH = "res://assets/character/asterrahuman.glb"
EXPECTED_BODY_COUNT = 19


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def validate_character_source(character: dict, rig: dict, retarget: dict) -> list[str]:
    errors: list[str] = []
    if character.get("canonical") is not True:
        errors.append("character_source.json must mark asterrahuman as canonical")

    source = character.get("source", {})
    if source.get("repository_path") != CANONICAL_REPO_PATH:
        errors.append(f"canonical repository path must be {CANONICAL_REPO_PATH}")
    if source.get("godot_path") != CANONICAL_GODOT_PATH:
        errors.append(f"canonical Godot path must be {CANONICAL_GODOT_PATH}")

    skin_policy = character.get("skin_skeleton_policy", {})
    if skin_policy.get("preserve_all_deform_bones") is not True:
        errors.append("canonical character must preserve all skin/deform bones")
    if skin_policy.get("do_not_reduce_skin_to_19_bones") is not True:
        errors.append("skin skeleton must remain independent of the 19-body physics proxy")
    if int(skin_policy.get("physics_proxy_body_count", -1)) != EXPECTED_BODY_COUNT:
        errors.append("character source physics proxy must contain exactly 19 bodies")

    canonical_source = rig.get("canonical_character_source", {})
    if canonical_source.get("repository_path") != CANONICAL_REPO_PATH:
        errors.append("rig canonical_character_source repository path disagrees with character_source.json")
    if canonical_source.get("godot_path") != CANONICAL_GODOT_PATH:
        errors.append("rig canonical_character_source Godot path disagrees with character_source.json")
    if canonical_source.get("preserve_full_skin_skeleton") is not True:
        errors.append("rig must preserve the full canonical skin skeleton")

    target_character = retarget.get("target_character", {})
    if target_character.get("repository_path") != CANONICAL_REPO_PATH:
        errors.append("CMU retarget target is not the canonical Character Editor GLB")
    if target_character.get("godot_path") != CANONICAL_GODOT_PATH:
        errors.append("CMU retarget Godot path disagrees with canonical character")
    if target_character.get("preserve_full_skin_skeleton") is not True:
        errors.append("CMU retarget must preserve the canonical skin skeleton")

    coordinate = character.get("coordinate_contract", {})
    if coordinate.get("derive_character_forward_from_manifest") is not True:
        errors.append("character forward must be derived from the imported manifest, not assumed")
    if coordinate.get("runtime_forward") != "-Z":
        errors.append("Asterra neural runtime contract expects -Z forward")

    return errors


def validate_character_manifest(path: Path) -> list[str]:
    errors: list[str] = []
    if not path.exists():
        return [
            "canonical character manifest is missing; run export_character_manifest.gd first"
        ]
    try:
        manifest = load_json(path)
    except (OSError, json.JSONDecodeError) as exc:
        return [f"could not read canonical character manifest: {exc}"]

    source = manifest.get("source", {})
    if source.get("godot_path") != CANONICAL_GODOT_PATH:
        errors.append("generated manifest does not describe the canonical asterrahuman GLB")
    skeleton = manifest.get("skeleton", {})
    bone_count = int(skeleton.get("bone_count", 0))
    bones = skeleton.get("bones", [])
    if bone_count <= 0 or len(bones) != bone_count:
        errors.append("generated skeleton manifest has an invalid bone inventory")
    names = [str(bone.get("name", "")) for bone in bones]
    if not all(names):
        errors.append("generated skeleton manifest contains unnamed bones")
    return errors


def validate_proxy_map(path: Path) -> list[str]:
    if not path.exists():
        return ["19-body physics proxy map is missing; run derive_character_physics_proxy.py"]
    try:
        proxy = load_json(path)
    except (OSError, json.JSONDecodeError) as exc:
        return [f"could not read 19-body physics proxy map: {exc}"]

    errors: list[str] = []
    if proxy.get("complete") is not True:
        details = proxy.get("errors", [])
        suffix = "; ".join(str(item) for item in details[:4])
        errors.append("19-body physics proxy map is incomplete" + (f": {suffix}" if suffix else ""))
    landmarks = proxy.get("skin_landmarks", {})
    if len(landmarks) != EXPECTED_BODY_COUNT:
        errors.append(
            f"physics proxy map must resolve {EXPECTED_BODY_COUNT} unique landmarks, found {len(landmarks)}"
        )
    indices = [entry.get("bone_index") for entry in landmarks.values()]
    if len(indices) != len(set(indices)):
        errors.append("physics proxy maps multiple body roles onto the same skin bone")
    return errors


def validate_rig(rig: dict) -> list[str]:
    errors: list[str] = []
    bodies = rig.get("canonical_bodies", [])
    body_set = set(bodies)
    expected_count = int(rig.get("body_count", -1))
    if expected_count != EXPECTED_BODY_COUNT:
        errors.append(f"v0 experiment requires {EXPECTED_BODY_COUNT} physics bodies, config says {expected_count}")
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
        "--require-character-manifest",
        action="store_true",
        help="Require the manifest extracted from the imported canonical GLB and the resolved 19-body proxy map.",
    )
    parser.add_argument(
        "--require-runtime-map",
        action="store_true",
        help="Fail unless all 19 canonical roles map to final runtime/USD body names.",
    )
    args = parser.parse_args()

    character = load_json(CONFIG / "character_source.json")
    rig = load_json(CONFIG / "rig_19body.json")
    exp = load_json(CONFIG / "walk_experiment.json")
    motion = load_json(CONFIG / "motion_sources.json")
    retarget = load_json(CONFIG / "cmu_retarget.json")
    io = load_json(CONFIG / "policy_io.json")

    errors = (
        validate_character_source(character, rig, retarget)
        + validate_rig(rig)
        + validate_experiment(exp)
    )

    if args.require_character_manifest:
        errors += validate_character_manifest(GENERATED / "asterrahuman_skeleton_manifest.json")
        errors += validate_proxy_map(GENERATED / "asterrahuman_physics_proxy_map.json")

    if args.require_runtime_map and not rig.get("runtime_name_map"):
        errors.append("runtime_name_map is empty; generate/freeze the canonical 19-body articulation first")

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
    print(f"  canonical character: {character['source']['godot_path']}")
    print(f"  physics proxy bodies: {rig['body_count']}")
    print(f"  training physics: {exp['simulation']['physics_hz']} Hz")
    print(f"  policy: {exp['simulation']['policy_hz']} Hz")
    print(f"  initial parallel environments: {exp['simulation']['parallel_envs_initial']}")
    print(f"  target parallel environments: {exp['simulation']['parallel_envs_target']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
