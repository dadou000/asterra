#!/usr/bin/env python3
"""Train the Asterra whole-body foundation controller in Isaac Lab / RSL-RL.

Stage 1 implements standing and balance only. Locomotion, imitation and skill
adapters are deliberately layered later on the same 197-observation / 54-action ABI.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import importlib.metadata as metadata
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

WINDOWS_TENSORDICT_VERSION = "0.11.0"
RSL_RL_VERSION = "3.0.1"


def _distribution_version(name: str) -> str | None:
    try:
        return metadata.version(name)
    except metadata.PackageNotFoundError:
        return None


def _ensure_windows_rl_import_compatibility() -> None:
    """Repair the known TensorDict/Isaac Sim 5.1 Windows import crash.

    RSL-RL 3.0.1 declares ``tensordict>=0.7`` without an upper bound. TensorDict
    0.12.x can access-violate Python 3.11 while RSL-RL is imported after Isaac Sim
    has started. We inspect package metadata before importing either package and pin
    the known-good 0.11.0 wheel in-place. ``--no-deps`` keeps the already validated
    Torch/Isaac installation untouched.
    """
    rsl_version = _distribution_version("rsl-rl-lib")
    if rsl_version != RSL_RL_VERSION:
        raise RuntimeError(
            f"Asterra requires rsl-rl-lib=={RSL_RL_VERSION}; installed={rsl_version!r}. "
            "Run Setup Isaac from the in-game training panel."
        )

    if platform.system() != "Windows":
        return

    installed = _distribution_version("tensordict")
    if installed == WINDOWS_TENSORDICT_VERSION:
        return

    if os.environ.get("ASTERRA_SKIP_RL_AUTO_REPAIR", "") == "1":
        raise RuntimeError(
            f"Unsafe TensorDict version for Windows Isaac/RSL-RL: {installed!r}; "
            f"required={WINDOWS_TENSORDICT_VERSION}. Auto-repair is disabled by "
            "ASTERRA_SKIP_RL_AUTO_REPAIR=1."
        )

    print(
        f"[Asterra startup] TensorDict {installed!r} is unsafe with Windows Isaac Sim 5.1/RSL-RL; "
        f"repairing to {WINDOWS_TENSORDICT_VERSION} before Isaac starts...",
        flush=True,
    )
    command = [
        sys.executable,
        "-m",
        "pip",
        "install",
        "--disable-pip-version-check",
        "--force-reinstall",
        "--no-deps",
        f"tensordict=={WINDOWS_TENSORDICT_VERSION}",
    ]
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"TensorDict compatibility repair failed with exit code {result.returncode}")

    repaired = _distribution_version("tensordict")
    if repaired != WINDOWS_TENSORDICT_VERSION:
        raise RuntimeError(
            f"TensorDict repair completed but metadata reports {repaired!r}; "
            f"expected {WINDOWS_TENSORDICT_VERSION!r}"
        )
    print(f"[Asterra startup] TensorDict compatibility repair complete: {repaired}", flush=True)


# This must run before AppLauncher. The problematic TensorDict module must never be
# imported into the process before the compatible wheel is in place.
_ensure_windows_rl_import_compatibility()

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

print(
    "[Asterra startup] dependency metadata: "
    f"rsl-rl-lib={_distribution_version('rsl-rl-lib')} "
    f"tensordict={_distribution_version('tensordict')}",
    flush=True,
)
print("[Asterra startup] launching Isaac application...", flush=True)
app_launcher = AppLauncher(args_cli)
simulation_app = app_launcher.app
print("[Asterra startup] Isaac application launched", flush=True)

print("[Asterra startup] importing torch...", flush=True)
import torch  # noqa: E402
print("[Asterra startup] importing RSL-RL runner...", flush=True)
from rsl_rl.runners import OnPolicyRunner  # noqa: E402
print("[Asterra startup] importing Isaac Lab RSL-RL wrapper...", flush=True)
from isaaclab_rl.rsl_rl import RslRlVecEnvWrapper  # noqa: E402
print("[Asterra startup] RL imports complete", flush=True)

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
