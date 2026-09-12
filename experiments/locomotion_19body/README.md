# Asterra 19-body neural walking experiment

This directory defines the first neural locomotion experiment for Asterra. It is deliberately independent of the old procedural/active-ragdoll walker.

## Canonical character

The existing Character Editor humanoid is the single source of truth:

`res://assets/character/asterrahuman.glb`

No separate training humanoid is allowed. The imported GLB provides:

- visible mesh and skin;
- complete deform skeleton;
- rest-pose joint axes;
- body proportions;
- mocap retarget target;
- landmarks used to generate the 19-body physical proxy.

The full skin skeleton is preserved. **The visible skeleton does not need to contain exactly 19 bones.** The locomotion system generates a 19-rigid-body control/physics proxy around selected landmarks while all extra deform/helper/future hand or facial bones can remain available to the character system.

This also prevents the earlier class of forward-axis errors: character forward is measured from the imported rest frame and explicitly converted to Asterra's `-Z` runtime convention instead of assuming both coordinate frames match.

## Canonical-character extraction pipeline

Run this once whenever `asterrahuman.glb` or its rig changes:

```bash
godot --headless --path . --script res://experiments/locomotion_19body/scripts/export_character_manifest.gd -- \
  --output=res://experiments/locomotion_19body/generated/asterrahuman_skeleton_manifest.json

python experiments/locomotion_19body/scripts/derive_character_physics_proxy.py
python experiments/locomotion_19body/scripts/validate_experiment.py --require-character-manifest
```

The pipeline is:

```text
assets/character/asterrahuman.glb
        ↓
Godot-imported Skeleton3D
        ↓
asterrahuman_skeleton_manifest.json
  exact names / parents / rest transforms / bounds
        ↓
derive_character_physics_proxy.py
        ↓
asterrahuman_physics_proxy_map.json
  19 unique skin landmarks
        ↓
19-body USD physics articulation
        ↓
CMU retarget to canonical proportions/rest frame
        ↓
Isaac Lab training
        ↓
ONNX actor
        ↓
Asterra Jolt runtime + the same asterrahuman.glb
```

The proxy resolver deliberately fails on missing or ambiguous limbs instead of guessing.

## Goal

Train one physically simulated **19-rigid-body** proxy of the canonical Asterra humanoid to produce a stable natural walk from reference motion, then export the actor policy to ONNX for runtime inference.

The first policy is **walking only**. Standing transitions, starts/stops, turning and running are added after basic walking is physically stable.

## Training stack

- **Simulator:** NVIDIA Isaac Lab with GPU PhysX.
- **RL:** PPO with motion imitation / AMP-style reference features.
- **Training physics:** 240 Hz.
- **Policy:** 60 Hz, held/interpolated across 4 physics substeps.
- **Runtime Asterra physics:** 120 Hz Jolt; ONNX inference at 60 Hz.
- **Action:** bounded normalized joint-position targets executed by physical PD/impedance motors.
- **Initial parallelism:** 2048 environments; target 4096 when VRAM allows.

The animation is only a training reference. It never teleports or directly drives the physical character.

## 19-body contract

`config/rig_19body.json` defines these physical/control bodies:

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

**Body count is not action count.** The neural action dimension is derived from the actual controllable joint DOFs of the generated articulation.

Once the articulation reports `N` controlled DOFs:

```bash
python experiments/locomotion_19body/scripts/derive_policy_dimensions.py DOF_COUNT
```

For the current v0 observation contract:

- actions = `N`;
- observations = `35 + 3*N`.

## Motion set

Use the CMU Graphics Lab Motion Capture Database for v0.

Core walking set:

- Subject 7, trials 1-12: normal, slow and brisk walking.
- Subject 8, trials 1-11: normal, slow, stride and exaggerated-stride walking.
- Subject 17, trials 1-9: initially held out for style/generalization evaluation.
- Subject 16: reserved for later walk/run transitions.
- Subject 69: reserved for later walking and turning.

CMU source data is kept at 120 fps outside Git. `config/motion_sources.json` is the auditable manifest.

## Reference retargeting

`config/cmu_retarget.json` retargets CMU **directly onto the canonical Character Editor character**, using the generated rest-pose manifest and physics-proxy map.

Examples of source collapse:

- `lfemur/rfemur` -> thighs;
- `ltibia/rtibia` -> shins;
- `lhumerus/rhumerus` -> upper arms;
- `lradius/rradius` -> forearms;
- `lowerback + upperback` -> Asterra spine;
- `upperback + thorax` -> Asterra chest;
- toe trajectories -> contact inference, not extra physical bodies.

Preserve per reference frame:

- root position/orientation;
- joint position and velocity for every controllable DOF;
- key-body position/orientation for head, hands and feet;
- root linear/angular velocity;
- left/right foot contact state.

Infer contacts and velocities at source 120 Hz, then resample the retargeted training reference to 60 Hz.

## Curriculum

`curriculum.py` defines four phases:

- **0-20%:** strong imitation, 0.8-1.35 m/s, flat ground, no pushes.
- **20-50%:** imitation weakens, task-velocity reward grows, mild pushes/randomization.
- **50-80%:** balance/task performance becomes dominant, broader speeds, slopes and physical variation.
- **80-100%:** imitation remains as a style prior while robustness and commanded velocity dominate.

Episodes initialize from random reference frames. We reduce the **reward weight for exact imitation**, never an imaginary animation force.

## Reward structure

The v0 contract includes:

- pose imitation;
- joint-velocity imitation;
- root-velocity imitation;
- head/hand/foot tracking;
- foot-contact timing;
- commanded forward velocity;
- upright/balance reward;
- foot-slip penalty;
- joint-power penalty;
- action-rate penalty;
- joint-limit penalty.

## Perturbations

Late curriculum adds:

- ±10% body mass;
- ±15% joint strength;
- friction variation;
- 0-1 policy-step latency;
- random lateral/forward pushes;
- ±8° slope.

Uneven terrain, moving supports and partial-foot contacts come after flat-ground walking is reliable.

## ONNX runtime contract

`config/policy_io.json` defines the inference interface. Runtime must:

1. construct observations in exactly the training order;
2. apply the exported observation normalization;
3. infer at 60 Hz;
4. interpolate/hold targets through Asterra's 120 Hz Jolt simulation;
5. apply those targets through bounded physical joint motors.

The exported ONNX actor contains neither the critic nor any training-only discriminator.

## Local layout

```text
experiments/locomotion_19body/
  config/
  generated/               # skeleton/proxy manifests; may be committed for reproducibility
  data/cmu/                 # downloaded ASF/AMC; local only
  data/retargeted/          # generated motion data; local only
  assets/asterra_humanoid.usd
  logs/
  checkpoints/
  artifacts/
```

## Validation

Static configuration only:

```bash
python experiments/locomotion_19body/scripts/validate_experiment.py
```

After extracting the canonical GLB and resolving the 19-body proxy:

```bash
python experiments/locomotion_19body/scripts/validate_experiment.py --require-character-manifest
```

After generating/freezing the final USD/runtime articulation and filling `runtime_name_map`:

```bash
python experiments/locomotion_19body/scripts/validate_experiment.py \
  --require-character-manifest --require-runtime-map
```

## Isaac Lab integration point

Use Isaac Lab's humanoid AMP environment architecture as the reference implementation, but register an Asterra-specific task. Replace its stock humanoid, fixed observation sizes and fixed action sizes with the generated Asterra articulation and its resolved DOFs.

Do not freeze the ONNX interface until the canonical 19-body articulation is frozen; changing DOFs afterward invalidates the policy contract.

## First convergence gate

Do not move to running until walking passes all of these:

- 60 s simulated flat-ground survival without falling;
- command tracking from 0.5 to 2.0 m/s;
- no persistent stance-foot skating;
- survives a 0.8 m/s random pelvis impulse from front/back/left/right;
- remains stable with ±5% body mass and friction 0.7-1.2;
- can initialize from random phases of every core walking clip.
