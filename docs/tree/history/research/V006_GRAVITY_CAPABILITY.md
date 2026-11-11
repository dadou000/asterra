# Orbit V0.0.6 — Gravity Capability / Service Research Note

Status: M09 implementation baseline.

## Scope

M09 introduces gravity as a reusable query service separate from orbital propagation and N-body integration.

The runtime service answers:

```
AccelerationFrom(source, point, time)
TotalAcceleration(point, time)
```

using existing FrameGraph coordinates and semantic gravity sources.

Gravity does not own transforms, body identities or integration state.

## Gravity model contract

`Orbit::CelestialGravity` defines a `GravityModel` interface.

A model evaluates acceleration in its own source/body frame:

```
AccelerationLocal(sourceToPointMeters)
```

The GravityService transforms the query point into the gravity source frame, evaluates the model there, then rotates the acceleration vector back into the caller's frame.

This detail is required for later non-spherical models. J2 and spherical-harmonic fields depend on the body's axis/orientation and cannot be evaluated correctly from a vector expressed in an arbitrary query frame.

## Point-mass baseline

The M09 baseline model is:

```
a = -mu r / |r|^3
mu = G M
G = 6.67430e-11 m3 kg-1 s-2
```

Optional distance softening evaluates the denominator using:

```
(|r|^2 + epsilon^2)^(3/2)
```

Softening is explicit and defaults to zero.

At an unsoftened point-mass singularity, the service returns zero only for the exact source origin instead of generating NaN/Inf. Collision/interior-body gravity is outside M09.

## Semantic capability

A body becomes a runtime gravity source only when it has an enabled `Gravity` capability.

Current model:

- `Point Mass`.

Authoring controls:

- enabled;
- model;
- derive gravitational parameter from body mass;
- explicit gravitational parameter in m3/s2;
- optional softening distance.

When derivation is enabled, the source uses the body's authoritative mass and the physical constant G.

When disabled, the explicitly authored gravitational parameter becomes authority.

## Source identity

Gravity sources use stable IDs deterministically derived from the semantic body object ID.

Rebuilding or switching between mass-derived and explicit mu therefore does not change source identity.

Disabling the capability removes the body from the active gravity service without deleting the semantic body.

## FrameGraph integration

Each gravity source is attached to the existing body frame.

Queries may be made from any FrameGraph frame connected to that source.

The GravityService uses FrameGraph transformation rather than flattening positions into an astronomical root frame, preserving the existing local-precision architecture.

Moving analytic, imported-ephemeris and N-body bodies therefore automatically move their gravity source because the source follows the same body frame.

## Separation from N-body

M08's N-body integrator currently has its own pairwise acceleration implementation.

M09 intentionally does not make NBodyDomain call GravityService.

Reasons:

- GravityService is a general spatial query service.
- NBodyDomain operates on a tightly packed deterministic integration state.
- coupling the hot integration loop to FrameGraph would introduce avoidable transform work and ordering dependencies.

Future gravity models can share physical parameters/model code where appropriate without forcing the runtime query service into the integrator.

## Extension seam

Future models can implement `GravityModel` for:

- J2 oblateness;
- zonal/tesseral spherical harmonics;
- irregular-body polyhedral fields;
- authored/local anomalous fields.

Callers continue using GravityService and do not need model-specific branches.

## Validation

Tests cover:

- point-mass acceleration magnitude and direction;
- source offset through FrameGraph;
- total acceleration query;
- gravitational parameter derivation from mass;
- semantic Gravity capability registration;
- mass-derived mu;
- explicit mu;
- stable gravity-source identity across rebuild;
- capability enable/disable behavior.

## Intentional limits

M09 does not yet implement:

- J2 coefficients;
- spherical-harmonic datasets;
- interior-body density fields;
- collision response;
- relativistic gravity;
- automatic coupling of GravityService into NBodyDomain.

Those are extensions of the existing model/service contract rather than changes to semantic or spatial authority.
