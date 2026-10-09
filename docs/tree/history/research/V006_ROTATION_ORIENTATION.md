# Orbit V0.0.6 — Rotation / Orientation Capability Research Note

Status: M06 implementation baseline.

## Scope

M06 separates body-fixed orientation from orbital translation.

The permanent runtime composition is now:

```
parentFromBody(t) =
    orientationProvider(t)
    + orbitStateProvider(t).position
```

Both providers remain independent and are combined only when the FrameGraph evaluates the body's parent transform.

## Provider contract

`Orbit::CelestialRotation` defines:

- Fixed orientation;
- Uniform spin;
- Synchronous orientation.

The shared `OrientationProvider` interface is the extension seam for later precession, nutation, libration and higher-order rotational models.

## Pole convention

The authored pole vector defines the body's +Z axis in the parent frame.

For uniform spin:

1. normalize the pole;
2. construct a stable orthonormal equatorial X/Y basis perpendicular to it;
3. rotate X/Y around the pole by phase;
4. preserve +Z exactly as the authored pole.

This gives axial tilt a direct geometric interpretation rather than treating tilt as a generic axis-angle around the parent identity basis.

## Uniform spin

```
phase(t) = phase_epoch + omega (t - epoch)
omega = 2 pi / rotation_period
```

Legacy body rotation-period, tilt and phase fields are migrated through the same provider when no enabled Rotation capability exists.

## Synchronous lock

Synchronous orientation uses the current orbit-state provider.

The local +X axis points toward the parent/reference origin after projection into the equatorial plane defined by the pole. The +Z axis is the requested pole.

If the authored pole is degenerate with the radial direction, the provider derives a usable pole from orbital angular momentum when possible.

An authored synchronous phase offset rotates the locked longitude around the pole without changing the orbit.

## Fixed orientation

Fixed orientation returns a time-invariant body basis.

The current semantic Fixed model uses identity orientation. A richer authored fixed-basis representation can extend the provider without changing FrameGraph or BodyRegistry contracts.

## Independence from orbit

Changing orbit parameters does not mutate rotation parameters.

Changing rotation parameters does not mutate orbital state.

Synchronous rotation is the only current model that deliberately queries orbit state, and does so through the public `OrbitStateProvider` interface.

## Validation

Tests cover:

- uniform-spin phase propagation;
- pole-axis / axial-tilt semantics;
- synchronous facing at epoch;
- synchronous facing after quarter orbit;
- end-to-end semantic Orbit + Rotation capability composition through FrameGraph;
- switching a body from synchronous lock to independent uniform spin;
- legacy rotation-field compatibility.

## Intentional extension seams

Not yet implemented:

- precession;
- nutation;
- forced/free libration;
- differential rotation;
- IAU pole/prime-meridian polynomial models;
- tidal evolution.

These should implement or compose with `OrientationProvider` rather than create a second orientation hierarchy.
