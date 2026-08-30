# Repo-local Isaac Lab training bootstrap

This directory is the training side of the stripped 19-body experiment.

Pinned first reproducible stack:

- Windows 11 or Ubuntu 22.04
- Python 3.11
- Isaac Sim 5.1
- Isaac Lab 2.3.1
- RSL-RL PPO
- CUDA PyTorch 2.7 / cu128 on x86_64

The repo owns all Asterra task, policy, runner and export code. Isaac Lab is a dependency, not a second project that contains our experiment.

## What works now

```powershell
powershell -ExecutionPolicy Bypass -File experiments/locomotion_19body/training/setup_isaac_windows.ps1
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/check_training_stack.py
```

`check_training_stack.py` checks Python, PyTorch, CUDA, Isaac Lab and RSL-RL imports.

## Next implementation gate

The next code to land is `scripts/build_articulation.py`. It will create the PhysX/Isaac articulation from the same 19-body physical contract used by the Jolt experiment. After that, `smoke_sim.py` and `train_foundation.py --stage stand` become the first actual trainable task.

Do not interpret the planned train commands in `TRAINING_PLAN.md` as implemented until those scripts exist.
