#!/usr/bin/env python3
"""Resolve the canonical Asterra skin skeleton into the 19-body physics proxy.

This tool intentionally fails closed. It never silently chooses between ambiguous
left/right limbs or helper/twist bones. The output is a semantic skin-landmark map;
actual rigid-body shapes/joints are generated from these landmarks in the training
asset pipeline.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "generated" / "asterrahuman_skeleton_manifest.json"
DEFAULT_RIG = ROOT / "config" / "rig_19body.json"
DEFAULT_OUTPUT = ROOT / "generated" / "asterrahuman_physics_proxy_map.json"

ROLE_TOKENS: dict[str, tuple[str, ...]] = {
    "pelvis": ("hips", "pelvis", "hip", "root"),
    "spine": ("spine", "lowerback", "abdomen", "waist"),
    "chest": ("chest", "thorax", "upperback", "spine2", "spine3"),
    "neck": ("neck",),
    "head": ("head",),
    "left_clavicle": ("clavicle", "collar", "shoulder"),
    "right_clavicle": ("clavicle", "collar", "shoulder"),
    "left_upper_arm": ("upperarm", "uparm", "humerus", "arm"),
    "right_upper_arm": ("upperarm", "uparm", "humerus", "arm"),
    "left_forearm": ("forearm", "lowerarm", "radius"),
    "right_forearm": ("forearm", "lowerarm", "radius"),
    "left_hand": ("hand", "wrist"),
    "right_hand": ("hand", "wrist"),
    "left_thigh": ("thigh", "upperleg", "upleg", "femur"),
    "right_thigh": ("thigh", "upperleg", "upleg", "femur"),
    "left_shin": ("shin", "calf", "lowerleg", "tibia"),
    "right_shin": ("shin", "calf", "lowerleg", "tibia"),
    "left_foot": ("foot", "ankle"),
    "right_foot": ("foot", "ankle"),
}

ROLE_EXCLUDES: dict[str, tuple[str, ...]] = {
    "left_upper_arm": ("fore", "lower", "hand", "wrist", "clav", "shoulder", "twist"),
    "right_upper_arm": ("fore", "lower", "hand", "wrist", "clav", "shoulder", "twist"),
    "left_forearm": ("upper", "hand", "wrist", "twist"),
    "right_forearm": ("upper", "hand", "wrist", "twist"),
    "left_thigh": ("lower", "shin", "calf", "foot", "toe", "twist"),
    "right_thigh": ("lower", "shin", "calf", "foot", "toe", "twist"),
    "left_shin": ("upper", "thigh", "foot", "toe", "twist"),
    "right_shin": ("upper", "thigh", "foot", "toe", "twist"),
    "left_foot": ("toe", "ball", "twist"),
    "right_foot": ("toe", "ball", "twist"),
}

ROLE_ORDER = [
    "pelvis",
    "spine",
    "chest",
    "neck",
    "head",
    "left_clavicle",
    "right_clavicle",
    "left_upper_arm",
    "right_upper_arm",
    "left_forearm",
    "right_forearm",
    "left_hand",
    "right_hand",
    "left_thigh",
    "right_thigh",
    "left_shin",
    "right_shin",
    "left_foot",
    "right_foot",
]


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def normalize(name: str) -> str:
    value = name.lower()
    value = re.sub(r"[^a-z0-9]+", "_", value).strip("_")
    return value


def compact(name: str) -> str:
    return normalize(name).replace("_", "")


def named_side(name: str) -> int:
    """Return -1 for left, +1 for right, 0 when the name is not explicit."""
    n = normalize(name)
    parts = [part for part in n.split("_") if part]
    if "left" in parts or "l" in parts or n.startswith("left") or n.endswith("left"):
        return -1
    if "right" in parts or "r" in parts or n.startswith("right") or n.endswith("right"):
        return 1

    c = compact(name)
    # Common embedded forms such as BipLUpperArm / BipRThigh.
    if re.search(r"(?:^|bip|def|mixamorig)l(?:upper|lower|arm|hand|thigh|leg|foot|clav)", c):
        return -1
    if re.search(r"(?:^|bip|def|mixamorig)r(?:upper|lower|arm|hand|thigh|leg|foot|clav)", c):
        return 1
    return 0


def role_side(role: str) -> int:
    if role.startswith("left_"):
        return -1
    if role.startswith("right_"):
        return 1
    return 0


def is_helper(name: str) -> bool:
    n = normalize(name)
    return any(token in n for token in ("twist", "helper", "mch", "ik", "pole", "ctrl", "control"))


def is_descendant(index: int, ancestor: int, parents: list[int]) -> bool:
    current = index
    seen: set[int] = set()
    while current >= 0 and current not in seen:
        if current == ancestor:
            return True
        seen.add(current)
        current = parents[current]
    return False


def score_candidate(
    role: str,
    bone: dict[str, Any],
    parent_role_bone: int | None,
    parents: list[int],
) -> tuple[float, list[str]]:
    name = str(bone["name"])
    n = normalize(name)
    c = compact(name)
    reasons: list[str] = []
    score = 0.0

    semantic_matches = []
    for token in ROLE_TOKENS[role]:
        tc = compact(token)
        if token in n or tc in c:
            semantic_matches.append(token)
    if semantic_matches:
        longest = max(semantic_matches, key=len)
        score += 100.0 + min(30.0, float(len(longest)) * 2.0)
        reasons.append("semantic:" + ",".join(semantic_matches))
    else:
        score -= 120.0

    for token in ROLE_EXCLUDES.get(role, ()):
        if token in n or compact(token) in c:
            score -= 100.0
            reasons.append("excluded:" + token)

    wanted_side = role_side(role)
    actual_side = named_side(name)
    if wanted_side:
        if actual_side == wanted_side:
            score += 60.0
            reasons.append("explicit-side")
        elif actual_side == -wanted_side:
            score -= 300.0
            reasons.append("wrong-side")
        else:
            # Spatial sign is useful only as a weak tiebreaker because some source
            # rigs face the opposite way. It must never override an explicit name.
            x = float(bone["global_rest_origin"][0])
            if abs(x) > 1e-5:
                score += 3.0
                reasons.append("side-unlabeled")

    if is_helper(name):
        score -= 160.0
        reasons.append("helper")

    index = int(bone["index"])
    if parent_role_bone is not None:
        if is_descendant(index, parent_role_bone, parents) and index != parent_role_bone:
            score += 55.0
            reasons.append("hierarchy")
        else:
            score -= 180.0
            reasons.append("outside-parent-chain")

    return score, reasons


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--rig", type=Path, default=DEFAULT_RIG)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--min-score", type=float, default=90.0)
    parser.add_argument("--min-margin", type=float, default=12.0)
    args = parser.parse_args()

    manifest = load_json(args.manifest)
    rig = load_json(args.rig)
    bones = list(manifest["skeleton"]["bones"])
    parents = [int(bone["parent_index"]) for bone in bones]
    by_index = {int(bone["index"]): bone for bone in bones}

    expected_parent: dict[str, str] = dict(rig["expected_parent"])
    resolved: dict[str, int] = {}
    candidates_report: dict[str, list[dict[str, Any]]] = {}
    errors: list[str] = []

    for role in ROLE_ORDER:
        parent_role = expected_parent.get(role)
        parent_index = resolved.get(parent_role) if parent_role else None
        scored: list[tuple[float, dict[str, Any], list[str]]] = []
        for bone in bones:
            index = int(bone["index"])
            if index in resolved.values():
                continue
            score, reasons = score_candidate(role, bone, parent_index, parents)
            scored.append((score, bone, reasons))
        scored.sort(key=lambda item: item[0], reverse=True)

        candidates_report[role] = [
            {
                "index": int(bone["index"]),
                "name": str(bone["name"]),
                "score": round(score, 3),
                "reasons": reasons,
                "global_rest_origin": bone["global_rest_origin"],
            }
            for score, bone, reasons in scored[:5]
        ]

        if not scored:
            errors.append(f"{role}: no candidate bones")
            continue
        best_score, best_bone, _ = scored[0]
        second_score = scored[1][0] if len(scored) > 1 else float("-inf")
        margin = best_score - second_score
        if best_score < args.min_score:
            errors.append(
                f"{role}: best candidate {best_bone['name']!r} scored {best_score:.1f}, "
                f"below minimum {args.min_score:.1f}"
            )
            continue
        if margin < args.min_margin:
            errors.append(
                f"{role}: ambiguous between {best_bone['name']!r} ({best_score:.1f}) and "
                f"{scored[1][1]['name']!r} ({second_score:.1f}); margin {margin:.1f}"
            )
            continue
        resolved[role] = int(best_bone["index"])

    output = {
        "schema_version": 1,
        "source_manifest": str(args.manifest),
        "canonical_character": manifest.get("source", {}).get("godot_path", ""),
        "body_count": int(rig["body_count"]),
        "complete": not errors and len(resolved) == int(rig["body_count"]),
        "skin_landmarks": {
            role: {
                "bone_index": index,
                "bone_name": str(by_index[index]["name"]),
                "global_rest_origin": by_index[index]["global_rest_origin"],
            }
            for role, index in resolved.items()
        },
        "candidates": candidates_report,
        "errors": errors,
        "notes": [
            "This maps the full skin skeleton to 19 physics landmarks; it does not remove skin bones.",
            "If complete is false, inspect candidates and add an explicit override instead of lowering confidence thresholds blindly.",
        ],
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")

    if errors:
        print("Physics proxy mapping is incomplete:")
        for error in errors:
            print(f"  - {error}")
        print(f"Candidate report written to {args.output}")
        return 1

    print(f"Resolved all {len(resolved)} canonical physics landmarks")
    for role in ROLE_ORDER:
        index = resolved[role]
        print(f"  {role:18s} -> #{index:02d} {by_index[index]['name']}")
    print(f"Physics proxy map written to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
