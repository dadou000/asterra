# Asterra volumetric-cloud adaptation

This note records how the `pre/0.1.0` renderer maps the vendored Kerbin
Raymarched Volumetrics/EVE reference into Asterra's own Godot renderer.

The files under this reference directory remain reference-only. Runtime code does
not load or ship the paid mod's planet maps, shader bundles, DLLs, audio, or other
proprietary assets.

## Runtime ownership

Asterra keeps its meteorology authoritative:

| WeatherSystem channel | Cloud renderer use |
| --- | --- |
| R | physical cloud fraction / coverage |
| G | organised convection; selects cumulus, congestus and deep Cb development |
| B | precipitation; strengthens storm cores and low rain haze |
| A | lower-atmosphere pressure anomaly; organizes broad pressure-system/anvil structure |

The live global field and moving local nest replace Kerbin's static
`coverageMap`, `cloudTypeMap`, and pressure-system painter tiles.

## Kerbin parameters carried over

The base-layer reference supplies the visual model:

- spherical Worley base noise, persistence `0.57`;
- edge-only detail erosion, `erosionDepth = 0.65`;
- Stratus/Cumulus/Congestus vertical bands and `noiseEdgeHardness` range
  `0.90 .. 0.97`;
- four-step local light march;
- adaptive view march (`0.006`) and max/base-step relationship;
- dual single-scattering phase lobes `(0.95, 0.10)` and `(0.8, 0.20)`;
- multiple-scattering lobes `(0.2, 3.0)` and `(-0.4, 0.3)`;
- Beer-Lambert extinction plus powder edge brightening;
- `upwardsCloudSpeed = 5 m/s`.

The cumulonimbus reference supplies three structural scales: broad soft
`trail/anvil`, harder `edge`, and dense `core`.

## Asterra scaling

Asterra's canonical radius is 1000 km, versus Kerbin's 600 km. Horizontal
cloud-cell dimensions and march distances are therefore multiplied by `5/3` to
preserve similar angular scale:

- 2300 m base cells -> about 3830 m;
- 3000 m Cb core -> about 5000 m;
- 4000 m Cb edge -> about 6670 m;
- 50000 m Cb trail -> about 83 km;
- 75 m base march -> 125 m reference scale;
- 1500 m max march -> 2500 m;
- 1200 m light march -> 2000 m.

Vertical altitudes are deliberately **not** multiplied by planet radius.
Asterra's weather model already represents an Earth-like troposphere, so the
adapted renderer uses roughly 1.0-3.5 km Stratus, 1.0-4.5 km Cumulus, progressively
taller Congestus, and organized Cb/anvils extending to about 14.5 km.

## Implementation boundaries

Implemented in this pass:

- depth-aware full-scene cloud compositing;
- WeatherSystem global/local coverage coupling;
- Worley mass, type-dependent height, hardness and erosion;
- separate fair-weather and deep-convective structures;
- Kerbin-derived light march, phase functions and powder response;
- matching terrain/ocean optical-depth shadows with finite Helion-disc penumbra;
- matching procedural sky fallback if the compositor is unavailable.

Still intentionally deferred:

- EVE's ~224^3 time-sliced multiple-scattering light volume (the current pass
  uses an analytic approximation with the same MS phase pair);
- the 200-260 km raymarch-to-scaled-space 2D cloud handoff;
- precipitation particles, canopy droplets, wet-surface accumulation and lightning;
- screen-space cloud god-rays.

Those are separate downstream systems and can be added without changing the
WeatherSystem-to-density contract established here.
