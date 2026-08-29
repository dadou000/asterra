# Asterra 19-body neural walking experiment

This directory defines the first neural locomotion experiment for Asterra. It is deliberately independent of the old procedural/active-ragdoll walker.

## Goal

Train one physically simulated **19-rigid-body** humanoid to produce a stable natural walk from reference motion, then export the actor policy to ONNX for runtime inference in Asterra.

The first policy is **walking only**. Standing, starts/stops, turning and running should be added after the basic walking controller is physically stable.

## Training stack

- **Simulator:** NVIDIA Isaac Lab with GPU PhysX.
- **RL:** PPO, with motion imitation / AMP-style reference features.
- **Training physics:** 240 Hz.
- **Policy:** 60 Hz, action held/interpolated across 4 physics substeps.
- **Runtime Asterra physics:** 120 Hz Jolt; ONNX inference stays at 60 Hz.
- **Action:** bounded normalized joint-position targets, executed by physical PD/impedance motors. No rigid-body teleporting and no direct animation authority.
- **Initial parallelism:** 2048 environments. Increase to 4096 if VRAM headroom is healthy.

The 240 Hz training step is intentional: it gives contact-rich feet and a high-DOF humanoid more solver headroom than the runtime simulation. Domain randomization then teaches the policy not to depend on perfect training physics.

## 19-body contract

`config/rig_19body.json` defines the semantic body contract:

1. pelvis
2. spine
3. chest
4. neck
5. head
6. left clavicle
7. right clavicle
8. left upper arm
9. right upper arm
10. left forearm
11. right forearm
12. left hand
13. right hand
14. left thigh
15. right thigh
16. left shin
17. right shin
18. left foot
19. right foot

The actual articulation may use different object names. Put those exact exported names in `runtime_name_map`. Training should fail if a role is unresolved or duplicated. **Body count is not action count**: action dimension comes from the articulation's controllable joint DOFs.

The source animation does **not** need exactly 19 bones. A richer motion-capture skeleton is retargeted and collapsed into these 19 physical segments. `config/cmu_retarget.json` defines that mapping explicitly.

## Motion set

Use the **CMU Graphics Lab Motion Capture Database** for this experiment because CMU explicitly permits copying/modifying the data and inclusion in commercially sold products. This gives the project a clear provenance path for a production locomotion policy.

The first dataset is intentionally conservative:

- Subject 7, trials 1-12: normal, slow and brisk walking.
- Subject 8, trials 1-11: normal, slow, stride and exaggerated-stride walking.
- Subject 17, trials 1-9: hold out initially for style/generalization evaluation, then optionally add late in training.
- Subject 16 and subject 69 are reserved for later walk/run transitions and walk/turn training.

CMU source data is 120 fps. Keep the original source files outside Git. `config/motion_sources.json` is the auditable manifest.

## Reference retargeting

`config/cmu_retarget.json` maps the CMU ASF hierarchy onto the 19 physical bodies. Examples:

- `lfemur/rfemur` -> thighs;
- `ltibia/rtibia` -> shins;
- `lhumerus/rhumerus` -> upper arms;
- `lradius/rradius` -> forearms;
- `lowerback + upperback` -> Asterra spine;
- `upperback + thorax` -> Asterra chest;
- toe trajectories remain available for contact inference even though toes are not independent physical bodies.

Preserve these quantities per reference frame:

- root position/orientation;
- joint position and velocity for every controllable DOF;
- body positions/orientations for at least head, hands and feet;
- root linear/angular velocity;
- left/right foot contact state.

Resample the final training reference to 60 Hz after retargeting. Keep source 120 Hz data available so velocities and contacts can be estimated before downsampling.

A reference animation is a **target**, never an authoritative transform. The policy always moves the physical articulation through motors.

## Curriculum

`curriculum.py` implements four phases:

- **0-20%:** strong imitation, 0.8-1.35 m/s, flat ground, no pushes.
- **20-50%:** imitation begins to weaken, task velocity reward grows, mild pushes/randomization.
- **50-80%:** balance/task performance becomes dominant, broader speeds, slopes and physical variation.
- **80-100%:** imitation remains as a style prior, but robustness and commanded velocity dominate.

Do not progressively turn animation forces down, because animation never applies force in this design. Progressively reduce the **reward weight for exact imitation** while increasing task and robustness rewards.

Episodes should initialize from random reference frames. This prevents the policy from learning only the first few steps of a clip and is critical for a cyclic walk.

## Reward structure

The v0 contract contains:

- pose imitation;
- joint-velocity imitation;
- root velocity imitation;
- key-body tracking (head/hands/feet);
- foot-contact timing;
- commanded forward velocity;
- upright/balance reward;
- foot-slip penalty;
- joint-power penalty;
- action-rate penalty;
- joint-limit penalty.

Once the agent can walk, reduce exact pose imitation before adding more animation variety. This allows physical corrections to emerge instead of forcing a kinematic-looking clone of the mocap.

## Perturbations

Late curriculum adds:

- ±10% body mass;
- ±15% joint strength;
- friction variation;
- 0-1 policy-step latency;
- random lateral/forward pushes;
- ±8° slope.

Later experiments should add uneven terrain, moving supports and partial foot contact only after flat-ground walking is reliable.

## ONNX runtime contract

`config/policy_io.json` defines the inference interface. Runtime should:

1. construct observations in exactly the training order;
2. apply the exported observation normalization;
3. infer at 60 Hz;
4. interpolate/hold targets at Asterra's 120 Hz Jolt step;
5. feed targets into bounded joint motors.

The ONNX actor should not contain the critic or training-only AMP discriminator.

The final network dimensions are intentionally not guessed from the 19 body count. Once the final articulation reports its controllable DOFs, derive them with:

```bash
python experiments/locomotion_19body/scripts/derive_policy_dimensions.py DOF_COUNT
```

For this v0 observation contract the formulas are:

- actions = `N`;
- observations = `35 + 3*N`;

where `N` is the final controllable articulation DOF count.

## Local layout

Expected local-only files:

```text
experiments/locomotion_19body/
  data/cmu/                 # downloaded ASF/AMC, never committed
  data/retargeted/          # generated reference motions
  assets/asterra_humanoid.usd
  logs/
  checkpoints/
  artifacts/
```

## Validation

Run without Isaac Lab:

```bash
python experiments/locomotion_19body/scripts/validate_experiment.py
```

After exporting the final physics articulation and filling `runtime_name_map`:

```bash
python experiments/locomotion_19body/scripts/validate_experiment.py --require-runtime-map
```

## Isaac Lab integration point

Use Isaac Lab's current `HumanoidAmpEnv` as the reference implementation for the environment loop, but register an Asterra-specific task rather than modifying Isaac Lab itself. The current task already demonstrates:

- a GPU articulation;
- motion-file loading;
- random reference-frame resets;
- key-body AMP observations;
- joint-position actions;
- thousands of replicated environments.

The Asterra environment must replace its fixed humanoid asset and fixed observation/action dimensions with the final 19-body USD articulation and its resolved DOFs. Do this only after the articulation export is frozen; otherwise every rig change invalidates the policy interface.

## First convergence gate

Do not move to running until the walking policy passes all of these:

- 60 s simulated flat-ground survival without fall;
- command tracking from 0.5 to 2.0 m/s;
- no persistent foot skating during stance;
- survives a 0.8 m/s random pelvis impulse from front/back/left/right;
- remains stable with ±5% body mass and friction 0.7-1.2;
- can start from random phases of every core walking clip.
