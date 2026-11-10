# Orbit V0.0.6 — Analytic Orbit State Research Note

Status: M05 implementation baseline.

## Scope

M05 introduces the time-dependent orbital state contract used by Orbit runtime composition:

```
EvaluateState(time) -> position, velocity, quality
```

The implementation lives in `Orbit::CelestialOrbits`. WorldModel and FrameGraph consume the contract but do not own orbital mathematics.

Initial providers:

- Fixed;
- Analytic Conic.

The provider interface is intentionally open so M07 imported ephemerides and M08 dynamic N-body state can implement the same query seam.

## Coordinate convention

Analytic conics are evaluated first in the standard perifocal plane and then rotated into the parent/reference frame using:

1. argument of periapsis;
2. inclination;
3. longitude of ascending node.

The resulting state is relative to the semantic parent frame selected by the World/System/Body/Reference hierarchy.

FrameGraph remains the only spatial hierarchy.

## Elliptic trajectories

For `0 <= e < 1`:

```
n = sqrt(mu / a^3)
M(t) = M0 + n (t - epoch)
M = E - e sin(E)
x = a (cos(E) - e)
y = a sqrt(1 - e^2) sin(E)
```

The eccentric anomaly is solved deterministically with Newton iteration.

## Hyperbolic trajectories

For `e > 1`, Orbit stores the positive magnitude of the conventional negative semi-major axis.

```
n = sqrt(mu / |a|^3)
M(t) = M0 + n (t - epoch)
M = e sinh(H) - H
x = |a| (e - cosh(H))
y = |a| sqrt(e^2 - 1) sinh(H)
```

The hyperbolic anomaly is solved with Newton iteration.

## Parabolic trajectories

For `e = 1`, semi-major axis is undefined. Orbit therefore uses periapsis distance `q` and Barker's variable `D = tan(nu/2)`.

```
B = D + D^3/3
B(t) = B0 + sqrt(mu / (2 q^3)) (t - epoch)
x = q (1 - D^2)
y = 2 q D
```

This avoids forcing parabolic trajectories into an artificial very-large ellipse/hyperbola representation.

## Fixed compatibility path

Bodies without an enabled orbit capability continue to use the legacy authored parent-frame position.

That value is now wrapped in `FixedOrbitStateProvider`, so fixed and time-dependent bodies flow through the same orbital translation seam.

This migration preserves existing semantic files while removing the need for later systems to special-case legacy body translation.

## Numerical policy

- SI units internally.
- Double-precision state evaluation.
- 64-iteration maximum for anomaly solves.
- Deterministic convergence tolerance.
- Elliptic mean anomaly is reduced with `remainder` to avoid unnecessary loss of precision over long times.
- Non-finite or invalid conic parameters are rejected rather than silently repaired.

## Validation

Reference tests cover:

- circular orbit position and velocity;
- quarter-period propagation;
- elliptic periapsis;
- parabolic periapsis and escape-speed relation;
- hyperbolic periapsis;
- orbital-plane orientation rotation;
- fixed-state behavior;
- end-to-end semantic Orbit capability -> provider -> BodyRegistry -> FrameGraph integration;
- disabled capability fallback to the legacy fixed-position authority.

## Intentional limits

M05 does not implement:

- perturbations;
- J2 precession;
- relativistic corrections;
- osculating-element updates;
- imported ephemerides;
- N-body integration;
- rotation/orientation separation.

Those extend the same state-provider and FrameGraph contracts in later milestones.
