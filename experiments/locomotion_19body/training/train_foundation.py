#!/usr/bin/env python3
"""Train the Asterra whole-body foundation controller in Isaac Lab / RSL-RL.

Stage 1 implements standing and balance only. Locomotion, imitation and skill
adapters are deliberately layered later on the same 197-observation / 54-action ABI.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import json
from pathlib import Path
import sys

from isaaclab.app import AppLauncher

parser = argparse.ArgumentParser(description="Train Asterra humanoid foundation policy")
parser.add_argument("--stage", choices=("stand",), default="stand")
parser.add_argument("--num-envs", type=int, default=2048)
parser.add_argument("--max-iterations", type=int, default=1500)
parser.add_argument("--num-steps-per-env", type=int, default=32)
parser.add_argument("--save-interval", type=int, default=50)
parser.add_argument("--seed", type=int, default=1467)
parser.add_argument("--run-name", type=str, default="")
parser.add_argument("--resume", type=Path, default=None, help="Optional RSL-RL checkpoint to resume from.")
AppLauncher.add_app_launcher_args(parser)
args_cli = parser.parse_args()

if args_cli.num_envs < 1:
    parser.error("--num-envs must be >= 1")
if args_cli.max_iterations < 1:
    parser.error("--max-iterations must be >= 1")
if args_cli.num_steps_per_env < 4:
    parser.error("--num-steps-per-env must be >= 4")
if args_cli.save_interval < 1:
    parser.error("--save-interval must be >= 1")

app_launcher = AppLauncher(args_cli)
simulation_app = app_launcher.app

import torch  # noqa: E402
from rsl_rl.runners import OnPolicyRunner  # noqa: E402
from isaaclab_rl.rsl_rl import RslRlVecEnvWrapper  # noqa: E402

TRAINING_DIR = Path(__file__).resolve().parent
if str(TRAINING_DIR) not in sys.path:
    sys.path.insert(0, str(TRAINING_DIR))

from asterra_rl.observations import OBSERVATION_SIZE  # noqa: E402
from asterra_rl.actions import DOF_COUNT  # noqa: E402
from asterra_rl.stand_env import AsterraStandEnv, AsterraStandEnvCfg  # noqa: E402


def experiment_root() -> Path:
    return Path(__file__).resolve().parents[1]


def make_runner_cfg() -> dict:
    return {
        "class_name": "OnPolicyRunner",
        "seed": int(args_cli.seed),
        "device": str(args_cli.device),
        "num_steps_per_env": int(args_cli.num_steps_per_env),
        "max_iterations": int(args_cli.max_iterations),
        "obs_groups": {"policy": ["policy"], "critic": ["policy"]},
        "clip_actions": 1.0,
        "save_interval": int(args_cli.save_interval),
        "experiment_name": "asterra_foundation_stand",
        "run_name": str(args_cli.run_name),
        "logger": "tensorboard",
        "policy": {
            "class_name": "ActorCritic",
            "init_noise_std": 0.25,
            "noise_std_type": "scalar",
            "actor_obs_normalization": True,
            "critic_obs_normalization": True,
            "actor_hidden_dims": [512, 512, 256],
            "critic_hidden_dims": [512, 512, 256],
            "activation": "elu",
        },
        "algorithm": {
            "class_name": "PPO",
            "value_loss_coef": 1.0,
            "use_clipped_value_loss": True,
            "clip_param": 0.2,
            "entropy_coef": 0.006,
            "num_learning_epochs": 5,
            "num_mini_batches": 8,
            "learning_rate": 3.0e-4,
            "schedule": "adaptive",
            "gamma": 0.99,
            "lam": 0.95,
            "desired_kl": 0.01,
            "max_grad_norm": 1.0,
        },
    }


def make_log_dir() -> Path:
    stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    suffix = f"_{args_cli.run_name}" if args_cli.run_name else ""
    path = experiment_root() / "runs" / "training" / args_cli.stage / f"{stamp}{suffix}"
    path.mkdir(parents=True, exist_ok=False)
    return path


def main() -> int:
    torch.manual_seed(args_cli.seed)
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.backends.cudnn.allow_tf32 = True
    torch.backends.cudnn.deterministic = False
    torch.backends.cudnn.benchmark = False

    env_cfg = AsterraStandEnvCfg()
    env_cfg.scene.num_envs = int(args_cli.num_envs)
    env_cfg.seed = int(args_cli.seed)
    env_cfg.sim.device = str(args_cli.device)

    log_dir = make_log_dir()
    runner_cfg = make_runner_cfg()
    run_manifest = {
        "schema_version": 1,
        "stage": args_cli.stage,
        "status": "starting",
        "num_envs": args_cli.num_envs,
        "max_iterations": args_cli.max_iterations,
        "num_steps_per_env": args_cli.num_steps_per_env,
        "seed": args_cli.seed,
        "device": args_cli.device,
        "observation_size": OBSERVATION_SIZE,
        "action_size": DOF_COUNT,
        "physics_hz": 240,
        "policy_hz": 60,
        "runner": runner_cfg,
    }
    manifest_path = log_dir / "run_manifest.json"
    manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n", encoding="utf-8")

    print("Asterra foundation training")
    print(f"  stage: {args_cli.stage}")
    print(f"  envs: {args_cli.num_envs}")
    print(f"  observation/action: {OBSERVATION_SIZE}/{DOF_COUNT}")
    print("  physics/policy: 240/60 Hz")
    print(f"  iterations: {args_cli.max_iterations}")
    print(f"  log_dir: {log_dir}")

    env = AsterraStandEnv(env_cfg)
    wrapped = RslRlVecEnvWrapper(env, clip_actions=1.0)
    runner = OnPolicyRunner(wrapped, runner_cfg, log_dir=str(log_dir), device=str(args_cli.device))
    try:
        runner.add_git_repo_to_log(__file__)
    except Exception as exc:
        print(f"[WARN] Could not add git metadata to RSL-RL log: {exc}")

    if args_cli.resume is not None:
        checkpoint = args_cli.resume.expanduser().resolve()
        if not checkpoint.is_file():
            raise FileNotFoundError(f"Resume checkpoint does not exist: {checkpoint}")
        print(f"Resuming checkpoint: {checkpoint}")
        runner.load(str(checkpoint))

    run_manifest["status"] = "training"
    manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n", encoding="utf-8")
    try:
        runner.learn(
            num_learning_iterations=int(args_cli.max_iterations),
            init_at_random_ep_len=True,
        )
    except BaseException:
        run_manifest["status"] = "failed"
        manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n", encoding="utf-8")
        raise
    finally:
        wrapped.close()

    run_manifest["status"] = "complete"
    run_manifest["completed_at"] = datetime.now().isoformat(timespec="seconds")
    manifest_path.write_text(json.dumps(run_manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Asterra stand training complete: {log_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    finally:
        simulation_app.close()
