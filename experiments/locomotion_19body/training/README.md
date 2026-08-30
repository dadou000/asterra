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

### 1. Deterministic physical articulation build

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

### 2. Manifest-driven Isaac articulation config

`training/asterra_rl/articulation.py` now:

- loads and validates `articulation_manifest.json` without requiring Isaac Sim;
- derives the exact 54-DOF name/order contract;
- derives the 55 expected PhysX link names (19 real + 36 virtual);
- computes neutral pelvis spawn height from foot geometry;
- builds Isaac Lab `ArticulationCfg` actuator groups directly from manifest torque/velocity/Kp/Kd values;
- supports a zero-drive passive mode without changing hard joint limits;
- validates live PhysX joint order, body names, limits, effort/velocity caps, gains and total mass after `sim.reset()`.

Pure helper checks:

```powershell
python experiments/locomotion_19body/training/scripts/test_articulation_helpers.py
```

### 3. Vectorized GPU PhysX smoke simulation

After USD conversion, the smoke harness can spawn many copies through `InteractiveScene`:

```powershell
# neutral PD hold diagnostics
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/smoke_sim.py `
  --headless --num-envs 256 --mode hold --seconds 2

# passive-fall / contact diagnostics
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/smoke_sim.py `
  --headless --num-envs 256 --mode passive --seconds 2 --strict-contact
```

The smoke test is intentionally not an RL task. It fails on infrastructure errors such as:

- PhysX joint name/order drift from the canonical 54-action manifest;
- missing/extra physical or virtual links;
- wrong hard limits, torque limits, velocity limits or PD gains;
- mass mismatch;
- NaN/Inf in joint, root, body or contact tensors;
- excessive hard-limit penetration;
- runaway root/joint velocities;
- optionally, missing left/right foot contact.

Ordinary falling is **not** considered an infrastructure failure. `hold` and `passive` write diagnostic root heights, joint speeds, limit excursions, contact force and throughput to ignored JSON reports under `runs/smoke/`.

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

Once both PhysX smoke modes pass on the training machine:

1. `asterra_rl/observations.py` — freeze gravity-aligned proprioception and action-history tensor ordering.
2. `asterra_rl/actions.py` — normalized target offsets -> physical PD target envelope with coupled-ROM clamping.
3. `train_foundation.py --stage stand` — first actual RSL-RL PPO controller.

Do not begin motion imitation until the passive PhysX articulation passes the same neutral-pose and fall sanity checks as the Jolt test.
