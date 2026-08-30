#!/usr/bin/env python3
"""Training-topology viewport adapter for :mod:`policy_inference_server`.

The compact 18x6DOF viewport sends one composite quaternion per anatomical joint.
The exact training-topology viewport instead has the same 54 serial revolute
coordinates as Isaac/PhysX, so it can send those canonical joint positions
straight through without an Euler decomposition step.
"""

from __future__ import annotations

from pathlib import Path
import sys
import traceback

import torch

import policy_inference_server as base

_ORIGINAL_JOINT_STATE = base.PolicyRuntime._joint_state


def _training_topology_joint_state(
    self: base.PolicyRuntime,
    packet: dict,
) -> tuple[torch.Tensor, torch.Tensor]:
    direct = packet.get("joint_pos")
    if not isinstance(direct, list):
        return _ORIGINAL_JOINT_STATE(self, packet)
    if len(direct) != base.DOF_COUNT:
        raise ValueError(f"Expected {base.DOF_COUNT} canonical joint positions, got {len(direct)}")

    q = torch.tensor(direct, device=self.device, dtype=torch.float32).reshape(1, base.DOF_COUNT)
    if not torch.isfinite(q).all():
        raise ValueError("joint_pos contains non-finite values")
    q = torch.maximum(torch.minimum(q, self.upper), self.lower)

    dt = max(1.0e-4, min(0.10, float(packet.get("dt", 1.0 / 60.0))))
    if self.previous_q is None:
        qd = torch.zeros_like(q)
    else:
        delta = q - self.previous_q
        delta = torch.atan2(torch.sin(delta), torch.cos(delta))
        qd = delta / dt
    self.previous_q = q.detach().clone()
    return q, qd


base.PolicyRuntime._joint_state = _training_topology_joint_state


if __name__ == "__main__":
    try:
        raise SystemExit(base.main())
    except BaseException as exc:
        status_path = None
        try:
            if "--status-file" in sys.argv:
                index = sys.argv.index("--status-file")
                if index + 1 < len(sys.argv):
                    status_path = Path(sys.argv[index + 1])
        except Exception:
            pass
        base._write_status(
            status_path,
            "failed",
            f"{type(exc).__name__}: {exc}",
            traceback=traceback.format_exc(),
        )
        raise
