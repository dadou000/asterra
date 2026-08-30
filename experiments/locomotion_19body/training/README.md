# Repo-local Isaac Lab training bootstrap

This directory is the training side of the stripped 19-body experiment.

Pinned first reproducible stack:

- Windows 11 or Ubuntu 22.04
- Python 3.11
- Isaac Sim 5.1
- Isaac Lab 2.3.1
- RSL-RL PPO
- CUDA PyTorch 2.7 / cu128 on x86_64

The repo owns all Asterra task, policy, runner and export code. Isaac Lab is a dependency, not a second project containing our experiment.

## Implemented now

The first physical bridge is live:

```powershell
python experiments/locomotion_19body/training/scripts/build_articulation.py
python experiments/locomotion_19body/training/scripts/test_build_articulation.py
```

`build_articulation.py` reads `config/physics_contract_19body.json` and `config/modular_policy.json`, validates the complete tree/action contract, and deterministically generates:

- `generated/articulation_manifest.json`
- `assets/asterra_19body_training.urdf`

Current Stage-0A mockup:

- 19 massive/colliding semantic bodies
- 18 anatomical joints
- 54 controlled rotational DOFs
- 36 non-colliding near-massless coordinate-frame links
- 72.0 kg physical mass (+ 0.0036 kg total virtual-frame mass)
- explicit right-handed Asterra `+Y`-up -> training `+Z`-up conversion

Each anatomical joint is expanded as three co-located X/Y/Z revolute joints for the training asset. This is intentional: Isaac Lab's mature vectorized actuator path is revolute/prismatic, while runtime Jolt retains the true 19-body anatomical articulation.

The generated action order is explicit in `articulation_manifest.json`; never infer policy indices from PhysX traversal order.

The URDF is pelvis-centered. Asterra `+X,+Y,-Z` (right, up, forward) maps to training `+X,+Z,+Y`. Policy observations remain gravity-aligned, so this simulator-axis conversion does not leak into runtime policy semantics.

Stage-0A actuator effort/velocity/gain values are deliberately marked **provisional**. They exist to prove the pipeline and must be calibrated before serious locomotion training.

## Isaac setup

```powershell
powershell -ExecutionPolicy Bypass -File experiments/locomotion_19body/training/setup_isaac_windows.ps1
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/check_training_stack.py
```

Then build and convert the asset:

```powershell
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/build_articulation.py
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/convert_training_asset.py --headless --force
```

`convert_training_asset.py` uses Isaac Lab's URDF converter with fixed-joint merging disabled and leaves per-DOF joint-drive gains to the Asterra `ArticulationCfg`.

### Isaac Lab 2.3.1 / Isaac Sim 5.1 URDF note

NVIDIA's official Isaac Lab 2.3.1 pip instructions use `isaaclab[isaacsim,all]==2.3.1`. Some Isaac Sim 5.1 pip combinations have also had a reported URDF-importer extension version mismatch. If AppLauncher fails during extension resolution, do not mutate the Asterra asset to work around it; treat it as an installation/toolchain issue and use a compatible NVIDIA package/source combination.

The pure-Python builder/tests do not depend on Isaac and can run regardless.

## Next implementation gate

1. `scripts/smoke_sim.py` — spawn copies of the generated USD and verify body/joint names, limits, contacts and finite tensors.
2. `asterra_rl/articulation.py` — build actuator groups directly from `articulation_manifest.json`.
3. `train_foundation.py --stage stand` — first actual PPO controller.

Do not begin motion imitation until the passive PhysX articulation passes the same neutral-pose and fall sanity checks as the Jolt test.
