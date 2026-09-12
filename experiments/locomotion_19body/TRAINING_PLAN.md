# Asterra Humanoid Training Implementation Plan

Status: architecture contract for the `experiment/19body-neural-walk-jolt` branch.

The goal is not an animation state machine. The goal is a physics-authoritative humanoid that attempts intents through a fixed physical body. Skills may improve what the controller knows how to attempt, but they never increase joint range, torque, friction, reach, mass distribution, or any other physical capability.

## 1. Non-negotiable runtime contract

```text
high-level intent
    -> skill adapters / task context
    -> shared whole-body foundation policy
    -> normalized joint targets
    -> physical actuator envelope (PD/impedance + torque limits)
    -> Jolt @ 120 Hz
    -> actual body motion
    -> full Asterra skin follows physics
```

There is no animation-authoritative path around physics.

A failed reach, slip, weak lift, bad landing, loss of balance, or inability to fit through a pose is a valid physical result.

## 2. Training/runtime split

Training:

- Isaac Lab 2.3.1 / Isaac Sim 5.1
- Python 3.11
- GPU PhysX at 240 Hz
- policy at 60 Hz (decimation 4)
- RSL-RL PPO
- motion imitation / learned motion prior added after the foundation controller is stable

Runtime:

- Godot/Jolt at 120 Hz
- actor inference at 60 Hz
- targets interpolated/held by physical joint motors at 120 Hz
- gravity-aligned observations; never bake global +Y into the policy contract
- ONNX exports are the deployable artifact

The training simulator is not authoritative. Jolt is. Sim-to-sim validation is therefore a required gate before a policy is accepted.

## 3. Policy architecture

The first serious policy must already be modular. Do not train a monolithic walker and split it later.

### Shared foundation

One shared proprioceptive encoder receives whole-body state:

- root height along local gravity-up
- gravity direction in root frame
- root tangent-frame orientation representation
- root local linear/angular velocity
- joint position and velocity
- foot/contact state
- previous action
- commanded planar velocity/yaw rate
- phase/reference features when enabled
- external/task context slots reserved from v1

### Body-region heads

The actor is partitioned into logical output regions while retaining a shared latent representation:

- core / balance
- left leg
- right leg
- left arm
- right arm
- head / neck

The exact action indices are generated from the articulation manifest. No hard-coded assumption that `19 bodies == 19 actions` is allowed.

### Skill adapters

A skill does not replace the foundation actor. It produces a masked residual:

```text
a_final = clamp(
    a_base
    + sum(gate_k * mask_k * residual_k),
    -1,
    +1
)
```

A skill may also contribute a small residual into a shared balance/task latent when whole-body compensation is required.

Examples:

- `phone_pickup_right`: right arm 1.0, head 0.7, spine/core 0.35, balance 0.2, legs 0.0
- `wave_right`: right arm 1.0, core 0.1, everything else near zero
- `heavy_door_right`: right arm 1.0, core 0.8, balance 0.8, legs 0.4

Gates are continuous and low-pass filtered. Skills can overlap.

### Adapter training rule

When adding a new skill:

1. load the frozen foundation policy;
2. freeze the shared encoder and foundation heads;
3. train only the skill residual adapter and its task encoder;
4. optionally unfreeze the small balance head late in training if the task requires new compensation;
5. regression-test locomotion with the skill gate at 0 and during partial blending.

Do not interpolate weights between two independently trained full policies. Adapters are trained from the same frozen base so their residuals remain semantically aligned.

## 4. Physical capability is a separate contract

The policy is never allowed to define physical capability. Capability belongs to the rig/actuator data:

- rigid-body dimensions
- mass and inertia
- joint anchors and anatomical frames
- hard angular ROM
- state-dependent coupled ROM
- actuator torque vs joint/velocity limits
- passive damping/friction
- contact friction
- maximum angular velocity

All policy outputs pass through this actuator envelope.

Changing character strength or injury must modify capability, not silently give the network larger actions.

## 5. Repository target layout

```text
experiments/locomotion_19body/
  TRAINING_PLAN.md
  config/
    character_source.json
    rig_19body.json
    policy_io.json
    walk_experiment.json
    modular_policy.json             # added now
    skill_adapters.json             # added now
  generated/
    asterrahuman_skeleton_manifest.json
    asterrahuman_physics_proxy_map.json
    articulation_manifest.json
    asterra_19body.usd
  motions/
    raw/                             # ignored
    processed/                       # ignored except manifests
  training/
    README.md
    setup_isaac_windows.ps1
    check_training_stack.py
    asterra_rl/
      articulation.py
      env_cfg.py
      observations.py
      actions.py
      commands.py
      rewards.py
      terminations.py
      events.py
      motion_reference.py
      policy/
        modular_actor_critic.py
        adapter.py
        masks.py
    scripts/
      build_articulation.py
      smoke_sim.py
      train_foundation.py
      train_adapter.py
      play_checkpoint.py
      export_onnx.py
      sim2sim_jolt_replay.py
  runs/                              # ignored
  artifacts/
    foundation/
      actor.onnx
      normalization.json
      manifest.json
    adapters/
      <skill>/adapter.onnx
      <skill>/manifest.json
```

## 6. Implementation stages

### Stage 0A - immediate training-pipeline smoke test

Purpose: prove Isaac Lab launch, GPU simulation, observations, actions, rewards, checkpointing and ONNX export before spending time on motion quality.

Use the current 19-body mockup dimensions if necessary.

Task:

- flat rigid ground
- zero desired velocity
- randomized small initial pose error
- learn to remain upright for 10 s
- no mocap
- no skill adapters

Pass criteria:

- 4096-env-capable code path (start lower while debugging)
- deterministic seed option
- no NaN/Inf in obs/actions/rewards
- policy survives 10 s from nominal start
- ONNX export reproduces PyTorch action within tolerance

This stage is disposable; it validates infrastructure only.

### Stage 0B - exact articulation contract

Before meaningful locomotion training, replace the mockup with the canonical Asterra-derived physical rig.

Inputs:

- `assets/character/asterrahuman.glb`
- generated skeleton manifest
- 19 semantic landmark map
- accepted anatomical joint model from the Jolt ragdoll test

Generate one authoritative `articulation_manifest.json` containing:

- body hierarchy
- body-local collision geometry
- mass/inertia/COM
- joint anchors in parent/child frames
- anatomical axes
- hard limits
- coupling metadata
- actuator parameters
- neutral pose

Both the Isaac asset generator and Godot runtime rig should consume/validate against this manifest. Avoid maintaining two manually edited rigs.

Pass criteria:

- neutral pose overlays the canonical mesh correctly
- total mass agrees between Isaac and Jolt
- every joint axis/limit agrees
- passive fall test is qualitatively equivalent in both simulators

### Stage 1 - foundation: stand

No motion imitation yet.

Teach:

- upright balance
- quiet stance
- small pose disturbances
- small external pushes
- recovery without stepping first

Rewards emphasize survival, gravity alignment, low velocity, stable contacts, low power and joint-limit avoidance.

Reason: if the character only stands because a walk reference drags it along, the physical foundation is weak.

### Stage 2 - foundation: step and recover

Add:

- stronger pushes
- COM excursions
- allowed recovery stepping
- variable friction
- small slopes
- randomized body/actuator parameters

The target is a reusable balance controller, not pretty locomotion.

Pass criteria include recovery from pushes from all horizontal directions and graceful failure when the perturbation exceeds capability.

### Stage 3 - locomotion prior / walking

Add retargeted CMU walking references and commanded velocity.

Initial curriculum:

1. forward walk 0.8-1.35 m/s;
2. broader forward speeds;
3. start/stop;
4. lateral motion;
5. yaw turns;
6. slopes/friction/random pushes;
7. reference-style variation.

The reference is a reward/prior, never a kinematic pose override.

Preserve the existing 240 Hz physics / 60 Hz policy timing unless profiling gives a strong reason to change it.

### Stage 4 - nuance and motion quality

Once locomotion is robust, add motion-quality learning rather than hard-coded animation polish:

- larger mocap diversity
- style/phase latent
- symmetry augmentation where appropriate
- discriminator or AMP-style motion prior
- penalties for foot skate, unnecessary jerk and energy
- upper-body natural-motion rewards that do not override contacts/balance

This is where AnimGen-like nuance becomes relevant, while the final action remains physical.

### Stage 5 - modular adapter infrastructure

Implement adapter modules before the first interaction skill:

- action masks by semantic region/joint
- continuous gate input
- residual action composition
- optional balance-latent residual
- frozen-base training mode
- multi-adapter composition
- regression suite with all gates at zero

Export contract:

```text
foundation/actor.onnx
adapters/<skill>/adapter.onnx
adapters/<skill>/manifest.json
```

The adapter manifest includes input schema, output indices/mask, expected foundation model hash and training rig hash.

### Stage 6 - first interaction skill: reach/grasp small object

Do not start with a phone-specific animation.

Train a generic `reach_small_object_right` adapter with procedural targets:

- target pose relative to torso/hand
- variable object position
- variable starting gait phase
- object sometimes outside immediate reach
- task can require torso compensation or a step

Success reward is hand approach/contact/grasp quality. No teleporting hand target.

Then `phone_pickup_right` becomes mostly task semantics/grip targets on top of the generic reach/grasp capability.

### Stage 7 - Jolt sim-to-sim validation

For every promoted foundation checkpoint:

1. record an Isaac rollout: observations, actions, contacts, body transforms;
2. replay equivalent commands/initial state in the stripped Godot/Jolt harness;
3. compare stability, gait frequency, joint excursions, foot slip and failure cases;
4. expand domain randomization where PhysX->Jolt mismatch appears;
5. reject a model that only works in PhysX.

This is mandatory because runtime physics is authoritative.

## 7. First observation contract

Keep the observation contract compact and physically meaningful.

Foundation observation v1:

```text
root height along gravity-up                          1
gravity direction in root frame                      3
root local linear velocity                           3
root local angular velocity                          3
joint positions                                      N
joint velocities                                     N
key body positions relative to root                 15
left/right foot contact                              2
command planar velocity + yaw rate                   3
phase sin/cos                                        2 (0 before motion prior)
previous action                                      N
reserved task/adapter context                        fixed block
```

Use a gravity-aligned tangent frame so the eventual spherical world does not require changing the policy coordinate contract.

## 8. Action / actuator contract

Policy output is normalized target offset, not raw torque in v1:

```text
q_target = q_default + action_scale * action

tau = clamp(
    Kp * (q_target - q) - Kd * qdot,
    -tau_limit(q, qdot),
    +tau_limit(q, qdot)
)
```

Why start here:

- easier imitation learning;
- easier PhysX/Jolt parity;
- physical forces remain explicit;
- later residual torque can be added if needed.

The controller never receives permission to exceed the actuator envelope.

## 9. Run reproducibility

Every run directory must write:

- git commit SHA
- Isaac Lab / Isaac Sim / PyTorch / RSL-RL versions
- rig manifest hash
- motion dataset manifest hash
- resolved config
- random seed
- reward statistics
- checkpoint
- export metadata

Checkpoints and bulk logs stay ignored. Promoted ONNX artifacts should be versioned deliberately, ideally with Git LFS once they become large/frequent.

## 10. First concrete implementation order

Implement next in this exact order:

1. `build_articulation.py`: convert the current rig contract to Isaac/PhysX USD and emit `articulation_manifest.json`.
2. `smoke_sim.py`: load the USD in many headless environments and verify all DOFs/limits/contact sensors.
3. `asterra_rl/observations.py` and `actions.py`: freeze the policy I/O ordering.
4. `train_foundation.py --stage stand`: first actual PPO run.
5. `export_onnx.py`: export and numerically compare PyTorch vs ONNX.
6. fit the articulation to the canonical Asterra skeleton and repeat passive validation.
7. balance/recovery training.
8. CMU retarget processing and walking imitation.
9. modular actor heads + adapter API.
10. generic right-hand reach/grasp adapter.

Do not begin interaction-skill training before the stand/balance policy and articulation parity gates pass.

## 11. Command targets

Once the files above are implemented, the repo workflow should be:

```powershell
# one-time environment
powershell -ExecutionPolicy Bypass -File experiments/locomotion_19body/training/setup_isaac_windows.ps1

# verify stack
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/check_training_stack.py

# build exact training articulation
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/build_articulation.py

# physics smoke test
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/smoke_sim.py --headless --num-envs 256

# first PPO controller
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/train_foundation.py --stage stand --headless --num-envs 2048

# later: locomotion
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/train_foundation.py --stage walk --headless --num-envs 4096

# later: skill adapter
.\.venv-isaac\Scripts\python.exe experiments/locomotion_19body/training/scripts/train_adapter.py --skill reach_small_object_right --foundation <checkpoint>
```

The setup/check scripts exist now. The build/smoke/train commands are implementation targets and must not be presented as working until their corresponding scripts land.
