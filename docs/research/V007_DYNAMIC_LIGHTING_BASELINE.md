# Orbit V0.0.7 — Dynamic Lighting / GI / Eye Response Research Baseline

Status: **Architecture baseline for M00**

This note records the research conclusions that motivated V0.0.7. The release specification is normative; this file explains why the architecture is shaped that way.

## 1. Hybrid rendering conclusion

The useful lesson from modern hybrid renderers and the Threat Interactive optimization demonstration discussed during V0.0.7 planning is not "software RT replaces hardware RT" and not "trace everything".

The useful rule is:

> Use the cheapest representation that can answer the lighting visibility query with sufficient accuracy.

Orbit has an unusual advantage over a generic engine because important world classes are already structured:

- planets have analytic reference shapes;
- production terrain is a hierarchical heightfield/clipmap system;
- future virtual heightfield detail can expose its own intersection;
- roads/paths are procedural;
- water has explicit world semantics;
- local rigid geometry can use proxy or triangle acceleration structures.

A universal triangle BVH is therefore not the only reasonable visibility representation.

## 2. Vulkan RT policy

Orbit uses Vulkan and must remain cross-vendor.

The lighting architecture distinguishes support for:

- acceleration structures;
- ray queries;
- full ray-tracing pipelines.

The high-value V0.0.7 integration is ray query from ordinary compute/graphics work, because it can accelerate selected visibility tests without forcing the renderer into a separate RT pipeline.

Hardware support is optional.

The lighting caller never requests a vendor API. It submits a visibility query; the scheduler chooses an available backend.

## 3. Budget policy

A common failure mode in graphics settings is:

~~~
RT unsupported -> cheap renderer
RT supported   -> enable many more rays/effects -> very heavy renderer
~~~

V0.0.7 rejects that policy.

Balanced/Auto instead keeps an approximate GPU-time budget and uses faster hardware traversal to improve the number/quality of useful exact hits within that budget.

Explicit Ultra/Cinematic settings may enlarge budgets. Capability alone may not.

## 4. GI architecture

The target is cache-driven dynamic diffuse GI:

- short screen traces recover high-frequency visible detail;
- body-stable radiance clipmaps hold lower-frequency diffuse radiance;
- sparse visibility queries update the cache;
- temporal/spatial reuse reduces query count;
- far-scale lighting collapses to broad planetary/sky irradiance rather than kilometre-scale per-pixel rays.

This is compatible with both non-RT and RT hardware because only the visibility backend changes.

## 5. Emissive GI

An emissive material is an emitter.

For a large display, it is incorrect to immediately replace the surface with one average point light because spatial color matters nearby.

It is also impractical to create one Light object per display texel.

The proposed compromise is a hierarchical emissive field:

- preserve fine texel/tile detail nearby;
- importance-sample bright regions;
- explicitly promote tiny bright emitters when stochastic GI would miss them;
- collapse to larger tiles/one emitter as projected importance falls;
- aggregate further into regional/planetary emission fields.

This allows an LED wall to cast blue/red/green bounce locally while a distant city becomes a coarse glow source.

## 6. Dynamic emissive content

Video screens and animated signs require event-aware GI refresh.

Uniformly refreshing the entire GI cache every video frame wastes work.

Track meaningful emitter-tile radiance change, dirty nearby cache regions and temporarily prioritize them.

Static/stable emissive regions can remain heavily cached.

## 7. Orbit-to-ground scale

Asterra's render representation already changes with scale.

Lighting must not change identity at the same boundary.

Near-field:
- detailed surface data;
- dense cache;
- screen final gather;
- fine emissive hierarchy.

Far/orbital:
- broad direct illumination;
- coarse reflected irradiance;
- atmosphere/sky;
- regional/planetary emission.

The exact representation may change while common energy/source identity remains.

## 8. Godot Asterra eye-adaptation findings

The previous Godot implementation in:

pre/0.1.0:scripts/rendering/human_eye_exposure.gd

already contained several good ideas:

- asymmetric light/dark adaptation;
- logarithmic adaptation;
- hysteresis;
- cone/rod dark-adaptation stages;
- a long rod-onset delay;
- special highlight protection for a sunlit planet against dark space;
- explicit solar-disc protection.

Its main limitation was coupling ordinary exposure state too strongly to extreme scene brightness.

It also used a highlight ceiling tied to the AgX white point, mixing display/tone-map concerns with adaptation physiology.

V0.0.7 keeps the useful timing/state ideas but splits:

1. normal photopic adaptation;
2. dark sensitivity adaptation;
3. transient overload/glare;
4. optional sustained bleaching.

## 9. Bright clouds and highlight saturation

The desired behavior is:

- normal daylight/cloud levels influence exposure up to a calibrated photopic adaptation range;
- brighter sources keep their real HDR radiance;
- exposure does not continue reducing without bound;
- excess energy becomes highlight compression/bloom/glare;
- when the extreme source leaves view, transient overload can recover quickly because base exposure never followed it arbitrarily far.

This is a rendering model, not a claim of exact retinal biology.

The implementation should be calibrated perceptually against real camera/eye references and remain configurable.

## 10. Histogram metering

A raw arithmetic average is a poor global exposure statistic for:

- black sky + bright planet;
- tiny solar disc;
- dark room + bright LED wall;
- high-contrast cloud edges.

Use a log-luminance histogram and derive separate robust statistics.

Do not force one statistic to solve exposure, overload and bloom simultaneously.

## 11. Bloom vs glare vs flare

Bloom:
- local optical spread;
- soft knee;
- common bright-region response.

Glare:
- broader contrast reduction/veil for strong overload.

Flare:
- compact/extreme source optics;
- should not appear just because a normal highlight crosses display white slightly.

All are downstream presentation effects.

## 12. LUT/color correction

V0.0.7 begins with a packed-2D 3D LUT implementation because the current RHI exposes 2D textures.

This is acceptable long-term as an abstraction: native 3D texture support can replace the storage representation later without changing the grading contract.

A display-referred LUT can safely operate after tone mapping in a bounded domain.

A scene-referred HDR grade requires an explicit shaper/log encoding before a bounded 3D LUT. Clamping HDR into [0,1] before the LUT is prohibited.

## 13. Validation philosophy

Lighting validation needs both deterministic numerical tests and visual scenarios.

Numerical:
- radiometry;
- cache address stability;
- histogram percentiles;
- adaptation differential equations/time constants;
- LUT identity/interpolation;
- output transforms;
- capability/budget decisions.

Visual/integration:
- LED room;
- cloud glare;
- dark-to-light;
- city emission;
- ground-to-orbit;
- RT on/off.

Performance results must name the tested GPU and settings rather than becoming universal CI FPS claims.

## 14. Research references / implementation anchors

Primary internal anchors:

- docs/V0.0.6_SPEC.md
- docs/V0.0.6_PROGRESS.md
- docs/research/V006_RADIOMETRY_HDR_EXPOSURE.md
- engine/celestial_radiometry
- engine/celestial_representation
- engine/celestial_globe
- engine/celestial_far_render
- engine/terrain_view
- engine/terrain_render
- engine/render_graph
- engine/render_view
- engine/post_process
- Godot reference branch pre/0.1.0, scripts/rendering/human_eye_exposure.gd

External implementation families to compare during milestone research:

- Vulkan VK_KHR_acceleration_structure;
- Vulkan VK_KHR_ray_query;
- hybrid screen/cache/ray-query GI literature and production presentations;
- radiance caching / irradiance-field literature;
- HDR display/tone-mapping/color-management references;
- psychophysical light/dark adaptation references.

Universal volumetric simulation/rendering is specified separately in [V007_UNIVERSAL_VOLUMETRICS.md](V007_UNIVERSAL_VOLUMETRICS.md). Volume lighting must consume the same V0.0.7 lighting authority described here rather than creating a parallel light model.

Each milestone that selects a concrete algorithm beyond this baseline should add a focused research note with assumptions, alternatives and measured tradeoffs before freezing its constants.
