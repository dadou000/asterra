# Orbit V0.0.6 Celestial Research Baseline

Status: **Normative research starting point**

This document records the initial external references and the engineering conclusions that shape Orbit V0.0.6. It is not a claim that Orbit will copy another engine's implementation.

Each major V0.0.6 subsystem must add a focused research note before acceptance when the model goes beyond this baseline.

---

# 1. Research policy

For each physical or rendering subsystem:

1. prefer primary scientific/technical literature or authoritative engineering documentation;
2. use other engines as feature/visual references, not as undocumented algorithm authorities;
3. write down assumptions and validity range;
4. separate physical authority from artistic presentation overrides;
5. build deterministic numerical tests where a reference result exists;
6. build visual comparison scenes where perception matters;
7. record performance and residency tradeoffs;
8. keep third-party APIs behind Orbit-owned adapters.

---

# 2. SpaceEngine — feature and architecture reference

Official references:

- Planet authoring manual: https://spaceengine.org/manual/making-addons/creating-a-planet
- Star authoring manual: https://spaceengine.org/manual/making-addons/creating-a-star/
- User manual / landscape controls: https://spaceengine.org/manual/user-manual
- Historical GPU procedural terrain article: https://spaceengine.org/news/blog100824/
- 0.990 release notes: https://spaceengine.org/news/blog190611/

Useful documented observations:

- celestial objects form parent/child hierarchies and complex star systems can nest;
- planet definitions compose optional Surface, Ocean, Clouds, Atmosphere, Aurora, Rings, AccretionDisk, CometTail, Corona and Orbit sections;
- unspecified data may be defaulted, derived or generated procedurally;
- orbital motion is described through classical Kepler/conic parameters in the documented planet format;
- distant stars use a point-like/far representation distinct from close stellar appearance;
- landscape work is budgeted and LOD/detail can be controlled by projected/display needs;
- multiple cloud layers, ring lighting differences, eclipse shadows and planet shine are explicit features.

Orbit conclusion:

- preserve capability composition and explicit hierarchy;
- improve authoring by storing value provenance instead of silently hiding procedural/derived sources;
- retain Orbit's toroidal terrain path for the ground rather than copying SpaceEngine's terrain structure;
- build a dedicated orbital globe/impostor ladder;
- keep generation work explicitly budgeted.

---

# 3. Reference frames and ephemerides — NASA NAIF SPICE

Primary references:

- SPICE overview: https://naif.jpl.nasa.gov/naif/documentation.html
- Frames required reading: https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/req/frames.html
- SPK ephemeris required reading: https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/req/spk.html
- SPICE time subsystem: https://naif.jpl.nasa.gov/pub/naif/toolkit_docs/C/req/time.html

Relevant principles:

- state vectors are meaningful only with a named reference frame and time;
- inertial, body-fixed and dynamic frames are distinct concepts;
- ephemeris data and frame transformations are separable;
- sampled/imported ephemerides can be queried through a stable state interface;
- barycenters and non-renderable ephemeris objects are ordinary useful entities.

Orbit conclusion:

- the existing FrameGraph direction is correct;
- V0.0.6 should add explicit barycenter/reference semantic nodes rather than fake bodies;
- orbit models should expose one Orbit-owned state-query interface;
- a future SPICE/CSPICE adapter is optional and must remain behind that interface.

---

# 4. Atmospheric scattering — Bruneton / Neyret lineage

Primary practical reference:

- Eric Bruneton, Precomputed Atmospheric Scattering: A New Implementation:
  https://ebruneton.github.io/precomputed_atmospheric_scattering/

Important engineering lessons from the documented implementation:

- avoid Earth-specific hard-coded texture-coordinate constants;
- support configurable density profiles;
- validate GPU results against reference computations;
- distinguish spectral radiance and RGB/luminance approximations explicitly;
- include ozone/absorption and aerosol choices in the model rather than tinting the final image arbitrarily.

Orbit conclusion:

- V0.0.6 atmosphere must be parameterized by arbitrary planetary radius/profile;
- LUTs are derived GPU data;
- atmosphere composition/profile is semantic authority;
- presentation gain/tint, if provided, is explicitly labeled as an override;
- acceptance requires numerical/reference validation plus ground/orbit visual cases.

---

# 5. Orbital mechanics baseline

Implementation references should include a standard astrodynamics text or equivalent primary engineering source for:

- conversion between orbital elements and Cartesian state;
- robust solution of Kepler's equation;
- elliptic/parabolic/hyperbolic conics;
- gravitational parameter relationships;
- reference-frame conventions.

For dynamic N-body work, select and document an integrator based on required gameplay timescale and error behavior.

Orbit validation requirements:

- circular orbit;
- high-eccentricity ellipse;
- near-parabolic case;
- hyperbolic escape;
- binary barycenter;
- long-duration energy/angular-momentum drift for the chosen N-body integrator.

Do not accept a visually plausible orbit solver without numerical tests.

---

# 6. Stellar radiation baseline

The stellar emitter model should start from explicit radiometric quantities rather than arbitrary RGB light colours.

Initial physically motivated relationships may use:

- effective temperature;
- radius;
- luminosity;
- blackbody/spectral approximation where appropriate;
- inverse-square irradiance.

Any colour conversion from spectrum/temperature to the renderer's working colour space must be documented.

The appearance model (granulation, spots, corona) is separate from the emitter's physical energy authority.

---

# 7. Planetary representation research questions

V0.0.6 does not assume one mesh is ideal at all distances.

The renderer must test and document thresholds for:

- production toroidal clipmap;
- macro-displaced globe;
- smooth globe;
- analytic disc;
- cached disc;
- point proxy.

Selection metric should be based primarily on projected error/angular size and feature requirements, not arbitrary altitude.

Required comparison scenes:

1. orbiting rocky planet with strong macro relief;
2. airless cratered moon at grazing illumination;
3. thick-atmosphere planet;
4. ringed giant;
5. moon shrinking from hundreds of pixels to sub-pixel;
6. ground-to-orbit continuous ascent.

Measured outputs:

- GPU time;
- draw calls;
- generated/uploaded bytes;
- transition error/pop metric where practical;
- silhouette error;
- representation cache churn.

---

# 8. Eclipse and reflected-light baseline

Celestial occlusion should begin from analytic geometry:

- apparent/angular radii;
- emitter/occluder/receiver geometry;
- partial/total/annular overlap;
- multiple occluders.

Atmospheric response, ring attenuation and soft extended-source effects can layer on top.

Planet/moon shine should use a bounded approximation whose energy assumptions are documented.

A scripted eclipse event is not the primary implementation.

---

# 9. Rings baseline

Research before implementation should cover:

- optical depth;
- single/multiple scattering approximation choice;
- forward/back scattering;
- particle-size influence;
- self-shadowing and planet shadow;
- apparent thickness and LOD.

SpaceEngine's documented front/back ring appearance is a useful visual feature reference, but Orbit should prefer a physically parameterized phase/optical model where real-time cost permits.

---

# 10. Clouds baseline

Cloud implementation must explicitly decide which scales are:

- climate/weather authority;
- procedural unresolved detail;
- rendering-only noise.

Ground and orbital views may use different representations, but they must be recognizable samples of the same weather state.

Research notes should cover:

- participating-media approximation;
- phase function;
- multiple scattering approximation;
- shadowing;
- temporal reprojection;
- orbital-scale coverage representation;
- volumetric residency/performance.

---

# 11. Gas giants baseline

Research should treat a giant as an atmospheric/volumetric body rather than a rocky sphere with a texture.

Important targets:

- oblateness;
- zonal jets/bands;
- cloud decks;
- storms/cyclones;
- depth-dependent opacity/color;
- limb behavior;
- transition to far impostor.

Orbit's generic field system should own dense pressure/density/velocity fields if simulation later requires them.

---

# 12. Airless bodies baseline

Visual acceptance should reproduce the characteristics that make airless bodies convincing:

- macro impact basins;
- hierarchical craters;
- low-angle terminator relief;
- strong but plausible roughness variation;
- nearly black unlit side unless another real illumination source exists;
- macro displacement affecting silhouette.

Do not compensate for missing lighting with a permanent ambient brightness term.

---

# 13. Research-note template

Every focused V0.0.6 research file should contain:

~~~
# Problem
# Target visual/physical behavior
# Sources
# Chosen model
# Equations / algorithm
# Assumptions
# Validity range
# Orbit-specific customization
# CPU/GPU residency
# Cache/streaming plan
# Numerical tests
# Visual benchmark
# Performance measurements
# Known deviations / future work
~~~

A milestone that introduces a major model is not accepted until its research note and tests agree with the implementation.
