# Repo-local Asterra humanoid training

This directory owns the training side of the stripped 19-body humanoid experiment.

Pinned first reproducible stack:

- Windows 11 or Ubuntu 22.04
- Python 3.11
- Isaac Sim 5.1
- Isaac Lab 2.3.1
- RSL-RL PPO
- CUDA PyTorch on x86_64

Isaac Lab is a dependency. The Asterra repository owns the body contract, observations, actions, rewards, runner, checkpoints and runtime export ABI.

## Design rule

The neural controller is never allowed to bypass physics.

```text
intent / learned policy
        ↓
normalized 54-DOF action
        ↓
anatomical ROM + coupled-ROM clamp
        ↓
physical PD targets
        ↓
torque / velocity limits
        ↓
PhysX during training
Jolt at runtime
        ↓
actual motion
```

A policy may learn a better way to use the body. It may not increase joint range, torque, velocity, friction, reach or mass properties in order to satisfy a desired pose.

The neutral-PD baseline falling from about 0.94 m to about 0.44 m is therefore expected: a pose servo is not a balance controller. Stage 1 now trains the missing balance intelligence.

## 1. Deterministic physical articulation build

```powershell
python experiments/locomotion_19body/training/scripts/build_articulation.py
python experiments/locomotion_19body/training/scripts/test_build_articulation.py
```

`build_articulation.py` reads:

- `config/physics_contract_19body.json`
- `config/modular_policy.json`

and deterministically generates:

- `generated/articulation_manifest.json`
- `assets/asterra_19body_training.urdf`

Current Stage-0A representation:

- 19 massive/colliding semantic bodies
- 18 anatomical joints
- 54 controlled rotational DOFs
- 36 non-colliding near-massless coordinate-frame links
- 55 training links total
- approximately 72 kg physical mass
- Asterra `+Y` up / `-Z` forward converted to training `+Z` up / `+Y` forward

Each anatomical 3-axis joint is represented as three co-located X/Y/Z revolute joints for Isaac Lab. Runtime Jolt keeps the semantic 19-body articulation.

The action order in `articulation_manifest.json` is canonical. PhysX traversal order is never part of the policy ABI.

## 2. Manifest-driven Isaac articulation

`training/asterra_rl/articulation.py`:

- validates the generated manifest without starting Isaac Sim;
- derives the canonical 54-DOF order;
- derives all expected PhysX body/link names;
- computes neutral root spawn height from foot geometry;
- creates Isaac Lab actuator groups from torque, velocity, Kp and Kd values in the manifest;
- supports passive and driven modes;
- builds a name-based canonical-to-PhysX index map;
- validates live PhysX limits, gains and mass after `sim.reset()`.

A different internal PhysX ordering is accepted and explicitly remapped.

## 3. PhysX smoke tests

After USD conversion:

```powershell
# neutral pose-servo diagnostic
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/smoke_sim.py `
  --headless --num-envs 256 --mode hold --seconds 2

# passive fall/contact diagnostic
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/smoke_sim.py `
  --headless --num-envs 256 --mode passive --seconds 2 --strict-contact
```

The smoke stage rejects infrastructure failures such as:

- missing or unexpected joints/links;
- hard-limit, torque-limit, velocity-limit or gain mismatches;
- mass mismatch;
- NaN/Inf tensors;
- catastrophic hard-limit penetration;
- runaway velocities;
- optionally missing foot contact.

Falling itself is valid physics and is not an infrastructure failure.

## 4. Permanent foundation-policy ABI

The stand controller already uses the observation/action layout intended for later walking. We do not want to train a throwaway stand-only network.

### Observation: 197 values

`training/asterra_rl/observations.py` defines:

```text
root height                                      1
root forward + up orientation                   6
root linear velocity                            3
root angular velocity                           3
5 key body positions × 3                       15
left/right foot contact load                    2
locomotion/task command                         3
phase sin/cos                                   2
canonical joint position                       54
canonical joint velocity                       54
previous canonical action                      54
                                              ---
                                              197
```

The spatial portion is expressed in a gravity-aligned local frame. It does not assume global `+Z` or global `+Y` is the final planetary up direction.

Stand supplies a zero movement command and a fixed phase. Walking will reuse the same tensor layout.

### Action: 54 values

`training/asterra_rl/actions.py` maps normalized `[-1, +1]` policy actions to joint-position targets.

Before targets reach PhysX it applies the same anatomical coupling contract used by the ragdoll model:

- shoulder elevation-dependent axial-twist reduction;
- hip deep-flexion capsule tightening;
- knee screw-home / flexion-dependent axial and frontal freedom;
- ankle sagittal-dependent subtalar tightening.

The action processor never changes the manifest torque or velocity capability.

Fast tensor/ABI checks:

```powershell
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/test_foundation_policy.py
```

These checks are also part of `Run tests` and `FULL PREFLIGHT` in Godot.

## 5. Stage 1 — learned standing and balance

`training/asterra_rl/stand_env.py` is a vectorized Isaac Lab `DirectRLEnv`:

- PhysX: 240 Hz
- policy: 60 Hz (`decimation = 4`)
- default parallel environments: 2048
- episode length: 8 s
- small joint/linear/angular perturbations on reset
- actual foot contact sensing
- actual whole-body center of mass
- canonical observation/action ordering

The reward is not simply “stay in the neutral pose”. It emphasizes physical balance:

- root height near standing height;
- pelvis/body uprightness;
- whole-body COM near the support midpoint of the feet;
- foot support/contact;
- low unnecessary root and joint velocity;
- low normalized torque;
- smooth actions;
- a weak neutral-pose preference only as a regularizer.

This allows hips, knees, ankles, spine and arms to move when those movements improve balance.

Episodes terminate on a clear fall/invalid state, not because the body deviates from the T-pose.

### Train from the command line

```powershell
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/train_foundation.py `
  --stage stand `
  --headless `
  --device cuda:0 `
  --num-envs 2048 `
  --max-iterations 1500
```

Training runs are written under:

```text
experiments/locomotion_19body/runs/training/stand/<timestamp>/
```

with RSL-RL checkpoints/TensorBoard data and an Asterra `run_manifest.json`.

The first success criterion is **not animation quality**. Watch for:

1. average episode duration increasing;
2. fewer fall terminations and more timeouts;
3. root height remaining near standing height;
4. COM-support reward improving;
5. policy avoiding large constant torque/action-rate penalties.

Only once it can stand reliably do we add progressively stronger push recovery.

## 6. In-game training console

The stripped Godot scene includes `scripts/training_control_panel.gd`. Press **F3** to hide/show it.

The panel controls:

- setup/update `.venv-isaac` on Windows;
- check the Isaac/PyTorch/RSL-RL stack;
- build articulation;
- run contract + foundation tensor tests;
- convert URDF to USD;
- run PhysX smoke diagnostics;
- run the full preflight pipeline;
- start the stand PPO run;
- choose CUDA device, random seed, smoke environment count, training environment count and iteration count;
- cancel the active process;
- inspect live process output without leaving Godot.

### Recommended in-game sequence

```text
FULL PREFLIGHT
      ↓
all checks PASS
      ↓
Training envs = 2048
Iterations = 1500
      ↓
TRAIN STAND
```

Godot does not block while Isaac runs. `training/scripts/game_training_bridge.py` owns the child process and the game polls status/log files under `runs/control/`.

## Isaac setup

```powershell
powershell -ExecutionPolicy Bypass -File experiments/locomotion_19body/training/setup_isaac_windows.ps1
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/check_training_stack.py
```

Then build/convert if not using the in-game preflight:

```powershell
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/build_articulation.py
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/convert_training_asset.py --headless --force
```

## After stand

Do not jump directly to motion imitation. The intended progression is:

```text
stand
  ↓
push recovery / balance curriculum
  ↓
velocity-command locomotion
  ↓
CMU/reference-motion retargeting
  ↓
nuanced motion prior / imitation
  ↓
modular body-region skill adapters
  ↓
reach / grasp / phone / tools / vehicle controls
```

Throughout the sequence the physical body contract remains authoritative.
