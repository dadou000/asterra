#!/usr/bin/env python3
"""Derive Asterra locomotion policy dimensions from the final articulation DOF count.

The humanoid has 19 rigid bodies, but body count is not action count. A ball joint,
for example, can contribute multiple controlled DOFs. This tool keeps the ONNX
contract tied to the exported articulation instead of hard-coding a guessed size.
"""

from __future__ import annotations

import argparse
import json


# Fixed observation terms in config/policy_io.json:
# root height 1
# root orientation tangent/normal representation 6
# root linear velocity 3
# root angular velocity 3
# key-body positions (head, hands, feet) 5 * 3 = 15
# foot contacts 2
# command (forward, lateral, yaw) 3
# phase sin/cos 2
FIXED_OBSERVATIONS = 35

# Per controlled DOF: joint position + joint velocity + previous action.
PER_DOF_OBSERVATIONS = 3


def dimensions(dof_count: int) -> dict[str, int]:
    if dof_count <= 0:
        raise ValueError("dof_count must be positive")
    return {
        "rigid_body_count": 19,
        "controlled_dof_count": dof_count,
        "action_dimension": dof_count,
        "observation_dimension": FIXED_OBSERVATIONS + PER_DOF_OBSERVATIONS * dof_count,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dof_count", type=int, help="Controllable DOFs reported by the final Isaac/PhysX articulation")
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args()

    try:
        result = dimensions(args.dof_count)
    except ValueError as exc:
        parser.error(str(exc))

    if args.as_json:
        print(json.dumps(result, indent=2))
    else:
        print(f"19-body articulation with {args.dof_count} controlled DOFs")
        print(f"  actions:      {result['action_dimension']}")
        print(f"  observations: {result['observation_dimension']}")
        print("  formula:      35 + 3 * DOFs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
