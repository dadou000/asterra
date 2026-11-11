# Orbit V0.0.6 — Celestial Representation Resolver

Status: M14 implementation baseline.

## Purpose

A celestial body has one semantic authority but may have several render representations.

M14 implements only the representation-selection policy. It does not introduce a second body hierarchy and it does not make any derived render proxy authoritative.

The required ladder is:

```
Production Surface / Toroidal Clipmaps
    -> Macro-displaced Orbital Globe
    -> Smooth Globe
    -> Analytic or Cached Disc Impostor
    -> Point / Stellar Point Proxy
```

## Selection inputs

The resolver is driven by measurable view/error inputs rather than raw altitude:

- body reference radius;
- camera distance to body center;
- vertical field of view;
- viewport height;
- maximum production-detail amplitude;
- maximum macro-displacement amplitude;
- feature availability;
- quality policy;
- previous representation for hysteresis.

This lets a telephoto view retain a richer representation farther away than a wide-angle view when projected error justifies it.

## Projected radius

For a camera outside the body:

```
angularRadius = asin(radius / distance)
projectedRadiusPx = angularRadius / verticalFov * viewportHeight
```

For an inside/intersecting camera the projected radius is saturated to half the viewport height for policy purposes.

## Projected geometric error

For a feature with physical amplitude `h`:

```
projectedErrorPx =
    projectedRadiusPx * h / bodyRadius
```

The production surface is selected while unresolved production-detail error exceeds its configured threshold.

The macro globe remains selected while macro displacement still produces visible geometric error.

When geometric displacement becomes sub-threshold, the resolver moves to smooth geometry and ultimately impostor/point representations based on apparent radius.

## Quality policy

`qualityScale > 1` retains richer representations longer by lowering transition thresholds.

The policy currently exposes:

- production-surface projected-error threshold;
- macro-displacement projected-error threshold;
- minimum smooth-globe apparent radius;
- minimum disc-impostor apparent radius;
- quality scale;
- hysteresis fraction.

No fixed authoring limit or semantic property is changed by these values.

## Feature requirements

The resolver accepts explicit feature requirements:

- production surface available;
- macro displacement available;
- complex far appearance;
- radiative emitter.

Complex far appearance chooses the cached-disc path instead of the simpler analytic disc path.

Radiative emitters use a stellar point proxy instead of an ordinary point proxy when sub-pixel.

These flags are runtime/render capability requirements, not body classifications.

## Hysteresis and overlap

Representation transitions have two separate mechanisms:

1. hysteresis may retain the previous representation inside a configured threshold band;
2. `blendToLower` reports an overlap blend weight toward the next lower-fidelity representation.

This allows render implementations to overlap representations rather than hard-switch them.

A `RepresentationTracker` owns previous representation state per stable subject ID. The stateless resolver remains directly testable.

## Diagnostics

Each decision exposes:

- selected representation;
- lower-fidelity neighbor;
- overlap blend weight;
- projected body radius in pixels;
- projected production-detail error;
- projected macro-displacement error;
- whether hysteresis retained the previous representation.

These values are intended for M32 diagnostics and render debugging.

## Separation from semantic authority

The resolver consumes scalar/runtime inputs and returns a render-policy decision.

It never creates, edits or persists a Celestial Body, Surface, Orbit or other semantic record.

Later milestones bind concrete render products to the decision:

- M15 macro globe;
- M17 surface/globe transition;
- M18 smooth globe, impostor and point implementations.

## Validation

Deterministic tests cover:

- close-range production surface;
- macro-displaced globe;
- smooth globe;
- analytic disc impostor;
- cached disc impostor for complex appearance;
- ordinary point proxy;
- stellar point proxy;
- hysteresis retention across a threshold;
- overlap blend range;
- quality-scale direction;
- persistent per-subject tracker state.

## Intentional limits

M14 does not yet:

- allocate GPU resources;
- build a globe mesh;
- generate planetary appearance textures;
- render impostors;
- integrate a performance-budget scheduler;
- perform occlusion or visibility culling.

Those are consumers of this policy and do not change its semantic contract.
