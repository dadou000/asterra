#!/usr/bin/env python3
"""GPU PhysX smoke test for the Asterra Stage-0A humanoid articulation.

This is intentionally not an RL environment. It proves that the generated USD,
manifest, Isaac Lab articulation, actuator contract, contact reporters and GPU
state tensors agree before PPO training is allowed to depend on them.

Examples:
    python smoke_sim.py --headless --num-envs 256 --mode hold
    python smoke_sim.py --headless --num-envs 256 --mode passive
"""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import random
import sys
import time
import traceback

from isaaclab.app import AppLauncher

parser = argparse.ArgumentParser(description="Asterra 19-body Isaac Lab smoke simulation")
parser.add_argument("--num-envs", type=int, default=256, help="Parallel PhysX environments.")
parser.add_argument("--env-spacing", type=float, default=2.5, help="Spacing between environment origins in meters.")
parser.add_argument("--mode", choices=("hold", "passive"), default="hold", help="Use neutral PD drives or passive joints.")
parser.add_argument("--seconds", type=float, default=2.0, help="Simulation duration after reset.")
parser.add_argument("--seed", type=int, default=1467, help="Deterministic reset/noise seed.")
parser.add_argument("--joint-noise-deg", type=float, default=0.0, help="Uniform initial joint perturbation magnitude.")
parser.add_argument("--usd", type=Path, default=None, help="Override generated Asterra training USD path.")
parser.add_argument("--manifest", type=Path, default=None, help="Override articulation manifest path.")
parser.add_argument("--report", type=Path, default=None, help="Write JSON report. Defaults to runs/smoke/latest_<mode>.json.")
parser.add_argument("--strict-contact", action="store_true", help="Fail if neither foot reports contact during the run.")
AppLauncher.add_app_launcher_args(parser)
args_cli = parser.parse_args()

if args_cli.num_envs < 1:
    parser.error("--num-envs must be >= 1")
if args_cli.seconds <= 0.0:
    parser.error("--seconds must be > 0")
if args_cli.joint_noise_deg < 0.0:
    parser.error("--joint-noise-deg must be >= 0")

app_launcher = AppLauncher(args_cli)
simulation_app = app_launcher.app

"""Isaac/torch imports must happen after AppLauncher."""

import torch  # noqa: E402

import isaaclab.sim as sim_utils  # noqa: E402
from isaaclab.assets import AssetBaseCfg, ArticulationCfg  # noqa: E402
from isaaclab.scene import InteractiveScene, InteractiveSceneCfg  # noqa: E402
from isaaclab.sensors import ContactSensorCfg  # noqa: E402
from isaaclab.utils import configclass  # noqa: E402

TRAINING_DIR = Path(__file__).resolve().parents[1]
if str(TRAINING_DIR) not in sys.path:
    sys.path.insert(0, str(TRAINING_DIR))

from asterra_rl.articulation import (  # noqa: E402
    experiment_root,
    load_articulation_manifest,
    make_articulation_cfg,
    resolve_training_usd,
    validate_live_articulation,
)

MANIFEST = load_articulation_manifest(args_cli.manifest)
USD_PATH = resolve_training_usd(args_cli.usd)
ROBOT_CFG = make_articulation_cfg(
    USD_PATH,
    MANIFEST,
    prim_path="{ENV_REGEX_NS}/Robot",
    passive=args_cli.mode == "passive",
)

MAX_HARD_LIMIT_VIOLATION_RAD = 0.20


@configclass
class AsterraSmokeSceneCfg(InteractiveSceneCfg):
    ground = AssetBaseCfg(
        prim_path="/World/defaultGroundPlane",
        spawn=sim_utils.GroundPlaneCfg(
            physics_material=sim_utils.RigidBodyMaterialCfg(
                static_friction=0.90,
                dynamic_friction=0.90,
                restitution=0.02,
            )
        ),
    )
    robot: ArticulationCfg = ROBOT_CFG
    left_foot_contact = ContactSensorCfg(
        prim_path="{ENV_REGEX_NS}/Robot/left_foot",
        update_period=0.0,
        history_length=2,
        track_air_time=True,
        debug_vis=False,
    )
    right_foot_contact = ContactSensorCfg(
        prim_path="{ENV_REGEX_NS}/Robot/right_foot",
        update_period=0.0,
        history_length=2,
        track_air_time=True,
        debug_vis=False,
    )
    dome_light = AssetBaseCfg(
        prim_path="/World/Light",
        spawn=sim_utils.DomeLightCfg(intensity=2200.0, color=(0.75, 0.78, 0.84)),
    )


def _assert_finite(label: str, tensor: torch.Tensor) -> None:
    if not torch.isfinite(tensor).all():
        count = int((~torch.isfinite(tensor)).sum().item())
        raise RuntimeError(f"{label} contains {count} NaN/Inf values; shape={tuple(tensor.shape)}")


def _reset_scene(scene: InteractiveScene, seed: int, joint_noise_deg: float) -> None:
    torch.manual_seed(seed)
    random.seed(seed)
    robot = scene["robot"]

    root_state = robot.data.default_root_state.clone()
    root_state[:, :3] += scene.env_origins
    robot.write_root_pose_to_sim(root_state[:, :7])
    robot.write_root_velocity_to_sim(root_state[:, 7:])

    joint_pos = robot.data.default_joint_pos.clone()
    joint_vel = robot.data.default_joint_vel.clone()
    if joint_noise_deg > 0.0:
        amplitude = math.radians(joint_noise_deg)
        noise = (2.0 * torch.rand_like(joint_pos) - 1.0) * amplitude
        low = robot.data.joint_pos_limits[..., 0] + 1.0e-4
        high = robot.data.joint_pos_limits[..., 1] - 1.0e-4
        joint_pos = torch.clamp(joint_pos + noise, low, high)
    robot.write_joint_state_to_sim(joint_pos, joint_vel)
    robot.reset()
    scene.reset()


def _step(scene: InteractiveScene, sim: sim_utils.SimulationContext, mode: str) -> None:
    robot = scene["robot"]
    if mode == "hold":
        robot.set_joint_position_target(robot.data.default_joint_pos)
        robot.set_joint_velocity_target(torch.zeros_like(robot.data.default_joint_vel))
    scene.write_data_to_sim()
    sim.step(render=False)
    scene.update(sim.get_physics_dt())


def _contact_magnitudes(scene: InteractiveScene) -> tuple[torch.Tensor, torch.Tensor]:
    left = torch.linalg.vector_norm(scene["left_foot_contact"].data.net_forces_w, dim=-1)
    right = torch.linalg.vector_norm(scene["right_foot_contact"].data.net_forces_w, dim=-1)
    return left, right


def _worst_limit_violation(robot, joint_pos: torch.Tensor) -> dict:
    low = robot.data.joint_pos_limits[..., 0]
    high = robot.data.joint_pos_limits[..., 1]
    violation = torch.maximum(low - joint_pos, joint_pos - high).clamp_min(0.0)
    flat_index = int(torch.argmax(violation).item())
    joint_count = int(violation.shape[1])
    env_index = flat_index // joint_count
    joint_index = flat_index % joint_count
    value = float(violation[env_index, joint_index].item())
    position = float(joint_pos[env_index, joint_index].item())
    lower = float(low[env_index, joint_index].item())
    upper = float(high[env_index, joint_index].item())
    return {
        "violation_rad": value,
        "env_index": env_index,
        "joint_index": joint_index,
        "joint_name": str(robot.joint_names[joint_index]),
        "position_rad": position,
        "lower_rad": lower,
        "upper_rad": upper,
    }


def _write_report(report: dict) -> Path:
    path = args_cli.report
    if path is None:
        path = experiment_root() / "runs" / "smoke" / f"latest_{args_cli.mode}.json"
    path = path.expanduser().resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return path


def run_smoke(sim: sim_utils.SimulationContext, scene: InteractiveScene) -> dict:
    robot = scene["robot"]
    contract_summary = validate_live_articulation(robot, MANIFEST)
    _reset_scene(scene, args_cli.seed, args_cli.joint_noise_deg)

    dt = sim.get_physics_dt()
    step_count = max(1, int(math.ceil(args_cli.seconds / dt)))
    max_joint_speed = 0.0
    max_limit_violation = 0.0
    worst_limit = {
        "violation_rad": 0.0,
        "env_index": -1,
        "joint_index": -1,
        "joint_name": "",
        "position_rad": 0.0,
        "lower_rad": 0.0,
        "upper_rad": 0.0,
        "step": -1,
    }
    max_root_speed = 0.0
    max_contact_force = 0.0
    any_foot_contact = False
    min_root_height = math.inf
    max_root_height = -math.inf

    start = time.perf_counter()
    for step_index in range(step_count):
        _step(scene, sim, args_cli.mode)

        joint_pos = robot.data.joint_pos
        joint_vel = robot.data.joint_vel
        root_state = robot.data.root_state_w
        body_state = robot.data.body_state_w
        left_force, right_force = _contact_magnitudes(scene)

        for label, tensor in (
            ("joint_pos", joint_pos),
            ("joint_vel", joint_vel),
            ("root_state_w", root_state),
            ("body_state_w", body_state),
            ("left_foot_contact", left_force),
            ("right_foot_contact", right_force),
        ):
            _assert_finite(label, tensor)

        current_limit = _worst_limit_violation(robot, joint_pos)
        if current_limit["violation_rad"] > max_limit_violation:
            max_limit_violation = float(current_limit["violation_rad"])
            worst_limit = dict(current_limit)
            worst_limit["step"] = step_index

        max_joint_speed = max(max_joint_speed, float(joint_vel.abs().max().item()))
        root_speed = torch.linalg.vector_norm(root_state[:, 7:10], dim=-1)
        max_root_speed = max(max_root_speed, float(root_speed.max().item()))
        max_contact_force = max(
            max_contact_force,
            float(torch.maximum(left_force.max(), right_force.max()).item()),
        )
        any_foot_contact = (
            any_foot_contact
            or bool((left_force > 1.0).any().item())
            or bool((right_force > 1.0).any().item())
        )
        env_relative_root_z = root_state[:, 2] - scene.env_origins[:, 2]
        min_root_height = min(min_root_height, float(env_relative_root_z.min().item()))
        max_root_height = max(max_root_height, float(env_relative_root_z.max().item()))

        # Infrastructure smoke test: reject catastrophic divergence, not ordinary falling.
        if max_limit_violation > MAX_HARD_LIMIT_VIOLATION_RAD:
            raise RuntimeError(
                "Hard joint limit violation exceeded "
                f"{MAX_HARD_LIMIT_VIOLATION_RAD:.2f} rad at step {worst_limit['step']}: "
                f"{worst_limit['violation_rad']:.6f} rad on {worst_limit['joint_name']} "
                f"(env {worst_limit['env_index']}, q={worst_limit['position_rad']:.6f}, "
                f"range=[{worst_limit['lower_rad']:.6f}, {worst_limit['upper_rad']:.6f}])"
            )
        if max_root_speed > 50.0 or max_joint_speed > 80.0:
            raise RuntimeError(
                f"Runaway state at step {step_index}: "
                f"root_speed={max_root_speed:.3f} joint_speed={max_joint_speed:.3f}"
            )

    elapsed = time.perf_counter() - start
    if args_cli.strict_contact and not any_foot_contact:
        raise RuntimeError("No left/right foot contact > 1 N was observed")

    final_root_z = robot.data.root_state_w[:, 2] - scene.env_origins[:, 2]
    final_root_quat = robot.data.root_link_quat_w
    _assert_finite("final_root_z", final_root_z)
    _assert_finite("final_root_quat", final_root_quat)

    return {
        "status": "pass",
        "mode": args_cli.mode,
        "num_envs": int(args_cli.num_envs),
        "device": str(sim.device),
        "physics_hz": 1.0 / dt,
        "steps": step_count,
        "simulated_seconds": step_count * dt,
        "wall_seconds": elapsed,
        "sim_steps_per_wall_second": (step_count * args_cli.num_envs) / max(elapsed, 1.0e-9),
        "seed": int(args_cli.seed),
        "joint_noise_deg": float(args_cli.joint_noise_deg),
        "usd": str(USD_PATH),
        "source_contract_sha256": MANIFEST["source_contract_sha256"],
        "contract": contract_summary,
        "diagnostics": {
            "max_joint_speed_rad_s": max_joint_speed,
            "max_joint_limit_violation_rad": max_limit_violation,
            "worst_joint_limit": worst_limit,
            "max_root_linear_speed_m_s": max_root_speed,
            "max_foot_contact_force_n": max_contact_force,
            "any_foot_contact_over_1n": any_foot_contact,
            "min_root_height_m": min_root_height,
            "max_root_height_m": max_root_height,
            "final_root_height_mean_m": float(final_root_z.mean().item()),
            "final_root_height_min_m": float(final_root_z.min().item()),
            "final_root_height_max_m": float(final_root_z.max().item()),
        },
    }


def main() -> int:
    torch.manual_seed(args_cli.seed)
    random.seed(args_cli.seed)

    sim_cfg = sim_utils.SimulationCfg(
        dt=1.0 / 240.0,
        device=args_cli.device,
        gravity=(0.0, 0.0, -9.81),
    )
    sim = sim_utils.SimulationContext(sim_cfg)
    if not args_cli.headless:
        sim.set_camera_view(eye=[4.0, 4.0, 2.8], target=[0.0, 0.0, 0.9])

    scene_cfg = AsterraSmokeSceneCfg(
        num_envs=args_cli.num_envs,
        env_spacing=args_cli.env_spacing,
        replicate_physics=True,
    )
    scene = InteractiveScene(scene_cfg)
    sim.reset()

    report = run_smoke(sim, scene)
    report_path = _write_report(report)
    diag = report["diagnostics"]
    worst = diag["worst_joint_limit"]
    print("\nAsterra PhysX smoke PASS")
    print(f"  mode: {report['mode']}  envs: {report['num_envs']}  device: {report['device']}")
    print(f"  joints/bodies: {report['contract']['joint_count']}/{report['contract']['body_count']}")
    print(f"  max joint-limit violation: {diag['max_joint_limit_violation_rad']:.6f} rad")
    if worst["joint_name"]:
        print(
            "  worst limit: "
            f"{worst['joint_name']} env={worst['env_index']} step={worst['step']} "
            f"q={worst['position_rad']:.5f} "
            f"range=[{worst['lower_rad']:.5f},{worst['upper_rad']:.5f}]"
        )
    print(f"  max foot contact: {diag['max_foot_contact_force_n']:.2f} N")
    print(f"  final root height mean: {diag['final_root_height_mean_m']:.3f} m")
    print(f"  throughput: {report['sim_steps_per_wall_second']:.0f} env-steps/s")
    print(f"  report: {report_path}")
    return 0


if __name__ == "__main__":
    try:
        exit_code = main()
    except Exception:
        # Isaac Sim's fast close path can terminate the process with status 0 on
        # Windows before Python's later SystemExit is observed. Flush the traceback
        # and terminate non-zero immediately on validator failure.
        traceback.print_exc()
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(1)

    simulation_app.close(skip_cleanup=True)
    raise SystemExit(exit_code)
