# Orbit V0.0.6 — Imported Ephemeris Adapter Research Note

Status: M07 implementation baseline.

## Architecture

Imported trajectories implement the same runtime contract introduced by M05:

```
OrbitStateProvider::EvaluateState(time)
```

No third-party ephemeris, astronomy-library, SPICE-kernel, Horizons, or file-format type crosses that runtime boundary.

Importers are responsible for converting external data into Orbit-native samples:

- simulation time in integer microseconds;
- position in meters;
- velocity in meters per second;
- coordinates already expressed in the semantic parent/reference frame.

The runtime therefore remains independent from whichever external provider produced the data.

## Semantic source model

Imported data is persistent scene authority rather than an opaque runtime cache.

An `Ephemeris Asset` owns zero or more `Ephemeris Sample` child records.

Each sample stores:

- time;
- position;
- velocity.

An Orbit / Ephemeris capability using model `Imported Ephemeris` binds to the asset through the existing schema `Source Object` reference.

The binding, asset identity and samples therefore use the normal command, undo, persistence, plugin and RPC paths.

## Interpolation

M07 uses cubic Hermite interpolation between adjacent samples because both position and velocity are available.

For normalized interval `u` and sample spacing `dt`:

```
r(u) =
    h00 r0 +
    h10 dt v0 +
    h01 r1 +
    h11 dt v1
```

Velocity is evaluated analytically from the derivative of the same cubic.

This gives:

- exact endpoint positions;
- exact endpoint velocities;
- continuous position;
- continuous first derivative within each interval;
- deterministic results without hidden numerical integration.

Samples are sorted by timestamp when the provider is constructed. Duplicate timestamps and non-finite states are rejected.

## Validity range

Imported ephemerides are authoritative only inside their sampled interval.

Orbit deliberately does not extrapolate or silently clamp outside that range.

An out-of-range query raises a diagnostic containing:

- requested time;
- first valid sample time;
- last valid sample time;
- source label.

This is important for editor/system-time controls: an invalid imported time range must be visible rather than producing plausible-but-untrusted motion.

## State quality

OrbitStateQuality now distinguishes:

- ExactAnalytic;
- Fixed;
- SampledExact;
- SampledInterpolated.

This allows diagnostics and later simulation/render systems to know whether a state came from an exact stored sample or interpolation without depending on provider-specific types.

## Source adapters

Future importers may consume, for example:

- SPICE-derived states;
- JPL Horizons exports;
- mission telemetry;
- custom CSV/binary state histories;
- user-generated trajectory tools.

Those adapters terminate at Orbit-native `EphemerisSample` records. They do not become permanent engine dependencies of FrameGraph, BodyRegistry or WorldModel.

## Validation

Tests cover:

- unsorted imported samples;
- deterministic sample sorting;
- exact-sample state queries;
- Hermite midpoint position and velocity;
- duplicate-time rejection;
- explicit range rejection;
- range diagnostics containing query/range/source information;
- semantic Source Object binding;
- end-to-end Ephemeris Asset -> OrbitStateProvider -> BodyRegistry -> FrameGraph motion;
- save/reopen preservation of source identity.

## Intentional limits

M07 does not yet provide a parser for a specific vendor/file format.

That is intentional: the permanent runtime contract and semantic storage are implemented first. Format-specific import services can be added independently without changing celestial runtime architecture.
