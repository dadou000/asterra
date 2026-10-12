# Meridian

A showcase world that uses the authored (non-simulated) engine features. Built by `python tools/build_meridian_world.py`
(fresh project, driven over RPC; see `tools/meridian_lib.py`, `tools/meridian_site.py`).

- System: Meridian Star; Meridian (physical atmosphere, two cloud layers, ocean, aurora, tectonics, hydrology, rivers, erosion
  processes, stratigraphy, impact history, authored constraints, planet bake) with moons Selene (airless crater history) and Ember;
  Halcyon (gas giant, ring bands, aurora, ice moon Aegis); comet Wanderer; compact object Umbra with accretion flow.
- Plaza at 35N 5E on Meridian: opaque/mirror/glass/emissive primitives, visibility proxies, point and spot lights, a path network,
  fog/smoke/fire volumes.
- Deliberately absent: the CM1-derived storm domain, Weather Lab and the lightning simulator.

Terrain bakes are generated on first launch and are not tracked.
