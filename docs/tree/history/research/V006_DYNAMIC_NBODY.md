# Orbit V0.0.6 — Dynamic N-Body Promotion Research Note

Status: M08 implementation baseline.

## Scope

M08 adds dynamic gravitational promotion as a simulation-LOD layer on top of existing authored orbit providers.

A body's authored motion authority remains one of the ordinary OrbitStateProvider sources, such as:

- Fixed;
- Analytic Conic;
- Imported Ephemeris.

Dynamic N-body promotion snapshots that provider's state at a chosen epoch, places selected sibling bodies into one shared integration domain, and exposes the integrated result through the same OrbitStateProvider interface.

No new celestial body identity or transform hierarchy is created.

## Promotion

Each promoted member provides:

- stable NBodyMemberId;
- mass in kilograms;
- source OrbitStateProvider.

At domain construction, the source provider is evaluated at the domain epoch and its position/velocity become the initial integration state.

This makes promotion independent of whether the source state came from fixed authoring, analytic conics, or imported samples.

## Integration

M08 uses fixed-step velocity-Verlet integration.

For each step:

1. evaluate all pairwise gravitational accelerations;
2. advance positions using current velocity and acceleration;
3. recompute acceleration at the new positions;
4. advance velocity using the average acceleration.

The implementation uses:

```
G = 6.67430e-11 m3 kg-1 s-2
```

and optional Plummer-style distance softening through `r^2 + epsilon^2`.

Velocity-Verlet was chosen as the v1 baseline because it is deterministic, time-reversible for fixed steps, second-order accurate, and significantly better suited to orbital dynamics than a simple explicit Euler integrator.

## Determinism

- members are sorted by stable ID before integration;
- pair evaluation order is stable;
- timestep is explicit and fixed;
- no random state is used;
- queries integrate from the promotion epoch rather than mutating hidden global state.

A final fractional step is used when the requested time is not an integer multiple of the configured step.

Backward-time queries use the same integrator with negative timestep.

## Simulation LOD semantics

N-body promotion is not a new Orbit model string.

The authored provider remains intact. The Orbit capability contains an independent `Dynamic N-Body Promotion` flag plus step and softening controls.

This preserves the V0.0.6 rule that simulation LOD is independent from authored orbital representation.

## Domain membership

The current semantic composition groups promoted sibling bodies sharing one inertial/reference parent frame into one domain.

At least two promoted siblings are required.

All siblings in one domain must currently use the same integration step and softening settings.

Dynamic integration is rejected beneath a rotating body-fixed frame, including nested reference nodes below such a frame. Supporting those coordinates correctly would require explicit non-inertial fictitious-force terms and is intentionally not approximated.

## State-provider bridge

Each promoted body receives an `NBodyOrbitStateProvider`.

BodyRegistry, OrientationProvider and FrameGraph consume it exactly as they consume fixed, analytic or imported providers.

`OrbitStateQuality::DynamicIntegrated` identifies integrated states without leaking N-body implementation types into downstream systems.

## Demotion

`NBodyDomain::DemotionState(member, time)` exports the integrated position and velocity for explicit handoff to another state authority.

Automatic runtime scheduling of promotion/demotion is not part of M08; later simulation scheduling can use this explicit state-transfer API without changing the provider contract.

## Transactional composition

UniverseComposition still constructs candidate FrameGraph/BodyRegistry state first.

If domain setup fails—for example due to inconsistent sibling integration settings—the existing live composition is not replaced.

Stable semantic BodyId and FrameId derivation is preserved across promotion-setting rebuilds.

## Validation

Tests cover:

- two-body Sun/Earth-scale promotion;
- integrated state quality;
- approximate orbital-radius preservation;
- forward and backward time queries;
- explicit demotion-state export;
- semantic Dynamic N-Body promotion controls;
- end-to-end promoted provider composition into BodyRegistry;
- stable body identity after rebuild;
- sibling integration-setting mismatch rejection;
- inertial/reference-frame enforcement.

## Intentional limits

M08 does not yet implement:

- adaptive timesteps;
- hierarchical/barnes-hut acceleration;
- collision/merger handling;
- close-encounter regularization;
- post-Newtonian corrections;
- persistent integration caches/checkpoints;
- automatic promotion/demotion scheduling;
- cross-parent-frame gravity domains.

Those extend the same NBodyDomain and OrbitStateProvider seams rather than changing semantic body authority.
