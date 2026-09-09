#!/usr/bin/env python3
"""Lightweight RSL-RL checkpoint inference server for the Godot/Jolt ragdoll.

The server intentionally does not launch Isaac Sim. Godot sends raw physical state
through a loopback UDP socket; this process rebuilds the exact 197-value foundation
observation, evaluates the trained policy, applies the canonical action/ROM mapping,
and returns 54 physical joint-position targets.

This keeps the cross-engine contract explicit:

    Godot/Jolt physical state -> foundation observation -> policy -> canonical targets

Jolt remains authoritative for joint limits, torque-limited drives, contacts and
external forces. The policy never writes body transforms.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import socket
import sys
import time
import traceback
from typing import Any

TRAINING_DIR = Path(__file__).resolve().parents[1]
if str(TRAINING_DIR) not in sys.path:
    sys.path.insert(0, str(TRAINING_DIR))

import torch
from rsl_rl.modules import ActorCritic

from asterra_rl.actions import CanonicalActionProcessor, DOF_COUNT
from asterra_rl.articulation import load_articulation_manifest
from asterra_rl.observations import OBSERVATION_SIZE, build_foundation_observation

KEY_BODY_COUNT = 5
DEFAULT_TARGET_FRACTION = 0.85
DEFAULT_POLICY_CFG = {
    "init_noise_std": 0.25,
    "noise_std_type": "scalar",
    "actor_obs_normalization": True,
    "critic_obs_normalization": True,
    "actor_hidden_dims": [512, 512, 256],
    "critic_hidden_dims": [512, 512, 256],
    "activation": "elu",
}

# Godot/Asterra source frame -> training frame:
#   +X right -> +X
#   +Y up    -> +Z
#   -Z fwd   -> +Y
SOURCE_TO_TRAINING = (
    (1.0, 0.0, 0.0),
    (0.0, 0.0, -1.0),
    (0.0, 1.0, 0.0),
)


def _write_status(path: Path | None, state: str, detail: str, **extra: Any) -> None:
    if path is None:
        return
    payload = {
        "schema_version": 1,
        "state": state,
        "detail": detail,
        "updated_unix": time.time(),
        **extra,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    temp.replace(path)


def _mat_mul(a, b):
    return tuple(
        tuple(sum(a[r][k] * b[k][c] for k in range(3)) for c in range(3))
        for r in range(3)
    )


def _mat_transpose(a):
    return tuple(tuple(a[c][r] for c in range(3)) for r in range(3))


def _quat_xyzw_to_matrix(q):
    x, y, z, w = (float(v) for v in q)
    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if norm < 1.0e-10:
        return ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))
    x, y, z, w = x / norm, y / norm, z / norm, w / norm
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    return (
        (1.0 - 2.0 * (yy + zz), 2.0 * (xy - wz), 2.0 * (xz + wy)),
        (2.0 * (xy + wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz - wx)),
        (2.0 * (xz - wy), 2.0 * (yz + wx), 1.0 - 2.0 * (xx + yy)),
    )


def _matrix_to_quat_wxyz(m):
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        w = 0.25 * s
        x = (m[2][1] - m[1][2]) / s
        y = (m[0][2] - m[2][0]) / s
        z = (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(max(1.0e-12, 1.0 + m[0][0] - m[1][1] - m[2][2])) * 2.0
        w = (m[2][1] - m[1][2]) / s
        x = 0.25 * s
        y = (m[0][1] + m[1][0]) / s
        z = (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(max(1.0e-12, 1.0 + m[1][1] - m[0][0] - m[2][2])) * 2.0
        w = (m[0][2] - m[2][0]) / s
        x = (m[0][1] + m[1][0]) / s
        y = 0.25 * s
        z = (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(max(1.0e-12, 1.0 + m[2][2] - m[0][0] - m[1][1])) * 2.0
        w = (m[1][0] - m[0][1]) / s
        x = (m[0][2] + m[2][0]) / s
        y = (m[1][2] + m[2][1]) / s
        z = 0.25 * s
    norm = math.sqrt(w * w + x * x + y * y + z * z)
    return (w / norm, x / norm, y / norm, z / norm)


def _source_vec_to_training(v):
    x, y, z = (float(c) for c in v)
    return (x, -z, y)


def _source_quat_to_training_wxyz(q):
    source = _quat_xyzw_to_matrix(q)
    transform = SOURCE_TO_TRAINING
    training = _mat_mul(_mat_mul(transform, source), _mat_transpose(transform))
    return _matrix_to_quat_wxyz(training)


def _joint_xyz_from_relative_quat(q):
    """Recover qx,qy,qz for the training chain R = Rx(qx) Ry(qy) Rz(qz)."""
    m = _quat_xyzw_to_matrix(q)
    sy = max(-1.0, min(1.0, m[0][2]))
    qy = math.asin(sy)
    cy = math.cos(qy)
    if abs(cy) > 1.0e-6:
        qx = math.atan2(-m[1][2], m[2][2])
        qz = math.atan2(-m[0][1], m[0][0])
    else:
        # The current anatomical envelopes stay away from exact +/-90 deg twist,
        # but keep a deterministic fallback for malformed/external states.
        qx = math.atan2(m[2][1], m[1][1])
        qz = 0.0
    return (qx, qy, qz)


def _find_run_manifest(checkpoint: Path) -> dict[str, Any]:
    path = checkpoint.parent / "run_manifest.json"
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    return data if isinstance(data, dict) else {}


class PolicyRuntime:
    def __init__(self, checkpoint: Path, manifest_path: Path | None, device: str) -> None:
        self.checkpoint = checkpoint.resolve()
        if not self.checkpoint.is_file():
            raise FileNotFoundError(f"Checkpoint does not exist: {self.checkpoint}")

        self.device = torch.device(device)
        self.manifest = load_articulation_manifest(manifest_path)
        self.run_manifest = _find_run_manifest(self.checkpoint)
        self.target_fraction = float(self.run_manifest.get("action_target_fraction", DEFAULT_TARGET_FRACTION))
        self.action_processor = CanonicalActionProcessor(
            self.manifest,
            device=self.device,
            dtype=torch.float32,
            target_fraction=self.target_fraction,
        )

        runner_cfg = self.run_manifest.get("runner", {})
        policy_cfg = dict(DEFAULT_POLICY_CFG)
        if isinstance(runner_cfg, dict) and isinstance(runner_cfg.get("policy"), dict):
            policy_cfg.update(runner_cfg["policy"])
        policy_cfg.pop("class_name", None)
        obs_groups = {"policy": ["policy"], "critic": ["policy"]}
        if isinstance(runner_cfg, dict) and isinstance(runner_cfg.get("obs_groups"), dict):
            obs_groups = runner_cfg["obs_groups"]

        dummy_obs = {"policy": torch.zeros((1, OBSERVATION_SIZE), device=self.device)}
        self.policy = ActorCritic(
            dummy_obs,
            obs_groups,
            DOF_COUNT,
            **policy_cfg,
        ).to(self.device)

        checkpoint_data = torch.load(self.checkpoint, map_location=self.device, weights_only=False)
        if not isinstance(checkpoint_data, dict) or "model_state_dict" not in checkpoint_data:
            raise ValueError("Expected an RSL-RL checkpoint containing model_state_dict")
        self.policy.load_state_dict(checkpoint_data["model_state_dict"], strict=True)
        self.policy.eval()
        self.model_iteration = int(checkpoint_data.get("iter", -1))

        self.joint_names = [str(j["name"]) for j in self.manifest["anatomical_joints"]]
        if len(self.joint_names) * 3 != DOF_COUNT:
            raise ValueError("Anatomical joint order does not describe 54 canonical DOFs")
        self.key_body_ids = torch.arange(KEY_BODY_COUNT, device=self.device, dtype=torch.long)
        self.lower = self.action_processor.lower
        self.upper = self.action_processor.upper
        self.velocity_limits = self.action_processor.velocity_limits
        self.previous_action = torch.zeros((1, DOF_COUNT), device=self.device)
        self.previous_q: torch.Tensor | None = None
        self.total_mass = float(self.manifest["total_physical_mass_kg"])
        self.half_body_weight = max(1.0, 0.5 * self.total_mass * 9.81)

    def reset_history(self) -> None:
        self.previous_action.zero_()
        self.previous_q = None

    def ready_packet(self) -> dict[str, Any]:
        dofs = self.manifest["dofs"]
        return {
            "type": "ready",
            "schema_version": 1,
            "checkpoint": str(self.checkpoint),
            "model_iteration": self.model_iteration,
            "observation_size": OBSERVATION_SIZE,
            "action_size": DOF_COUNT,
            "policy_hz": 60,
            "joint_names": self.joint_names,
            "dof_names": self.action_processor.names,
            "stiffness": [float(d["stiffness_nm_per_rad"]) for d in dofs],
            "damping": [float(d["damping_nms_per_rad"]) for d in dofs],
            "effort_limit": [float(d["effort_limit_nm"]) for d in dofs],
            "velocity_limit": [float(d["velocity_limit_rad_s"]) for d in dofs],
            "target_fraction": self.target_fraction,
        }

    def _joint_state(self, packet: dict[str, Any]) -> tuple[torch.Tensor, torch.Tensor]:
        quats = packet.get("joint_quat", [])
        if len(quats) != len(self.joint_names):
            raise ValueError(f"Expected {len(self.joint_names)} joint quaternions, got {len(quats)}")
        values: list[float] = []
        for quat in quats:
            if len(quat) != 4:
                raise ValueError("Every joint quaternion must contain x,y,z,w")
            values.extend(_joint_xyz_from_relative_quat(quat))
        q = torch.tensor(values, device=self.device, dtype=torch.float32).reshape(1, DOF_COUNT)
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

    def _observation(self, packet: dict[str, Any]) -> torch.Tensor:
        q, qd = self._joint_state(packet)

        root_pos = torch.tensor(
            [_source_vec_to_training(packet["root_pos"])], device=self.device, dtype=torch.float32
        )
        root_quat = torch.tensor(
            [_source_quat_to_training_wxyz(packet["root_quat"])], device=self.device, dtype=torch.float32
        )
        root_lin = torch.tensor(
            [_source_vec_to_training(packet["root_lin_vel"])], device=self.device, dtype=torch.float32
        )
        root_ang = torch.tensor(
            [_source_vec_to_training(packet["root_ang_vel"])], device=self.device, dtype=torch.float32
        )

        key_positions_raw = packet.get("key_body_pos", [])
        if len(key_positions_raw) != KEY_BODY_COUNT:
            raise ValueError(f"Expected {KEY_BODY_COUNT} key-body positions, got {len(key_positions_raw)}")
        key_positions = torch.tensor(
            [[_source_vec_to_training(v) for v in key_positions_raw]],
            device=self.device,
            dtype=torch.float32,
        )

        foot_force = packet.get("foot_force_n", [0.0, 0.0])
        if len(foot_force) != 2:
            raise ValueError("foot_force_n must contain left/right values")
        foot_load = torch.tensor(
            [[float(foot_force[0]) / self.half_body_weight, float(foot_force[1]) / self.half_body_weight]],
            device=self.device,
            dtype=torch.float32,
        )

        return build_foundation_observation(
            root_pos_w=root_pos,
            root_quat_w=root_quat,
            root_lin_vel_w=root_lin,
            root_ang_vel_w=root_ang,
            body_pos_w=key_positions,
            env_origins_w=torch.zeros((1, 3), device=self.device),
            key_body_ids=self.key_body_ids,
            joint_pos_canonical=q,
            joint_vel_canonical=qd,
            joint_lower_canonical=self.lower,
            joint_upper_canonical=self.upper,
            joint_velocity_limits_canonical=self.velocity_limits,
            previous_action=self.previous_action,
            foot_contact_load=foot_load,
            command=torch.zeros((1, 3), device=self.device),
            phase=torch.tensor([[0.0, 1.0]], device=self.device),
            gravity_w=torch.tensor((0.0, 0.0, -9.81), device=self.device),
            reference_forward_w=torch.tensor((0.0, 1.0, 0.0), device=self.device),
        )

    def step(self, packet: dict[str, Any]) -> dict[str, Any]:
        start = time.perf_counter()
        observation = self._observation(packet)
        if not torch.isfinite(observation).all():
            raise RuntimeError("Non-finite foundation observation received from Godot")

        with torch.inference_mode():
            action = self.policy.act_inference({"policy": observation})
            action = action.clamp(-1.0, 1.0)
            target = self.action_processor.normalized_to_targets(action)
        self.previous_action.copy_(action)

        return {
            "type": "action",
            "seq": int(packet.get("seq", -1)),
            "action": action[0].cpu().tolist(),
            "target": target[0].cpu().tolist(),
            "latency_ms": (time.perf_counter() - start) * 1000.0,
            "model_iteration": self.model_iteration,
        }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Serve an Asterra RSL-RL policy to the Godot Jolt ragdoll")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--manifest", type=Path, default=None)
    parser.add_argument("--device", default="cpu", help="Inference device; CPU is recommended for viewport use.")
    parser.add_argument("--status-file", type=Path, default=None)
    args = parser.parse_args()
    if not (1024 <= args.port <= 65535):
        parser.error("--port must be between 1024 and 65535")
    return args


def main() -> int:
    args = parse_args()
    _write_status(args.status_file, "loading", "Loading checkpoint", checkpoint=str(args.checkpoint))
    runtime = PolicyRuntime(args.checkpoint, args.manifest, args.device)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", args.port))
    sock.settimeout(0.25)
    _write_status(
        args.status_file,
        "ready",
        "Policy loaded and waiting for Godot",
        checkpoint=str(runtime.checkpoint),
        model_iteration=runtime.model_iteration,
        port=args.port,
    )
    print(
        f"Asterra policy server ready: model={runtime.checkpoint.name} iter={runtime.model_iteration} port={args.port}",
        flush=True,
    )

    while True:
        try:
            payload, address = sock.recvfrom(65507)
        except socket.timeout:
            continue
        try:
            packet = json.loads(payload.decode("utf-8"))
            if not isinstance(packet, dict):
                raise ValueError("UDP payload must decode to a JSON object")
            message_type = str(packet.get("type", ""))
            if message_type == "ping":
                response = runtime.ready_packet()
            elif message_type == "reset":
                runtime.reset_history()
                response = {"type": "reset_ack"}
            elif message_type == "step":
                response = runtime.step(packet)
            elif message_type == "quit":
                sock.sendto(json.dumps({"type": "bye"}).encode("utf-8"), address)
                break
            else:
                response = {"type": "error", "error": f"Unknown message type: {message_type!r}"}
        except Exception as exc:
            response = {"type": "error", "error": f"{type(exc).__name__}: {exc}"}
        sock.sendto(json.dumps(response, separators=(",", ":")).encode("utf-8"), address)

    _write_status(args.status_file, "stopped", "Policy server stopped")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BaseException as exc:
        # parse_args may not have run far enough to resolve the requested status path.
        status_path = None
        try:
            argv = sys.argv
            if "--status-file" in argv:
                index = argv.index("--status-file")
                if index + 1 < len(argv):
                    status_path = Path(argv[index + 1])
        except Exception:
            pass
        _write_status(status_path, "failed", f"{type(exc).__name__}: {exc}", traceback=traceback.format_exc())
        raise
