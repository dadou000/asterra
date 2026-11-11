+++
path = "/rendering/clouds/cloud-lab"
title = "Cloud lab (one isolated cloud)"
kind = "playbook"
status = "stable"
owner_module = "OrbitCelestialClouds"
summary = """
The cloud lab replaces the weather with ONE isolated cloud of a chosen type so its vertical development, shape and \
self-shadowing can be inspected: type, coverage, cirrus, precipitation, radius, height scale, maturity, organisation, density, \
seed, an optional sun and an optional cirrus sheet to see clouds shadowing cirrus. Set it with view.terrain_layers_set { cloud_lab }."""
keywords = ["cloud lab", "isolated cloud", "cumulonimbus", "anvil", "maturity", "organisation", "cirrus sheet", "lab", "single cloud", "inspect cloud"]
sources = [
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudRenderer.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioTerrainLayerOptions.hpp",
  "engine/studio_ui/src/StudioRenderViewRpc.cpp",
]
symbols = ["CloudLab", "StudioCloudLab", "cirrusSheet", "heightScale", "organisation", "maturity"]
invariants = [
  "While the lab is enabled the weather is replaced by one cloud; everything else about the march and shadows is the production path, so lab findings apply to real weather.",
  "Maturity is a life cycle: 0 towering cumulus, 0.3 growing cumulonimbus (cauliflower, no anvil), 0.6 mature (glaciated top, anvil spreading), 0.9 dissipating (tower collapsing, broad ragged anvil with mammatus).",
  "Organisation: 0 single cell, 0.5 multicell cluster of cells of different ages, 1 organised (large coherent updraft, wide anvil).",
  "height_scale multiplies the vertical extent of the shell (0.25-4) - it exaggerates development for inspection; it is not a physical setting.",
  "`place: true` (re)places the cloud ahead of the camera at distance_meters; `get` returns the stored values plus place_serial.",
]
related = ["/rendering/clouds/raymarch"]
depends_on = ["/rendering/clouds"]
verify = ["Set the lab, place it, capture at several distances, then disable it: the weather returns unchanged."]
verified = "b0a0de7f"

[[diagnose]]
symptom = "I need to see how one cloud type looks and shades in isolation"
steps = [
  "orbit_view_terrain_layers_set(view_id, cloud_lab={enabled: true, type: 1.0, maturity: 0.6, radius_meters: 6000, distance_meters: ..., place: true}).",
  "Vary maturity (0 to 0.9) and organisation (0, 0.5, 1) one at a time and capture; use density (0.2-6) to see self-shadowing.",
  "Add cirrus_sheet (0-1) to put a thin cirrus layer on the anti-sun side and watch the storm shadow it; override the sun with sun_override, sun_elevation_degrees and sun_azimuth_degrees.",
  "Disable the lab when done (cloud_lab.enabled = false).",
]
docs = ["/rendering/clouds/shadows-and-light-volume"]
+++

## Fields (`CloudLab` / `StudioCloudLab`)

| Field | Meaning |
| --- | --- |
| `type` | cloud type axis: 0.05 stratus, 0.2 stratocumulus, 0.32 nimbostratus, 0.5 cumulus, 0.72 congestus, 1.0 cumulonimbus |
| `coverage`, `cirrus`, `precipitation` | coverage of the cell, high-cloud/anvil coverage, rain amount |
| `radius_meters` | horizontal radius (default 6000 m) |
| `height_scale` | vertical exaggeration of the shell |
| `maturity`, `organisation`, `density` | life cycle, cell organisation, extinction multiplier |
| `cirrus_sheet` | patchy thin-cirrus sheet on the anti-sun side (0 = none) |
| `seed` | noise seed |
| `distance_meters`, `place` | how far ahead of the camera to place it; `place: true` places it |
| `sun_override`, `sun_elevation_degrees`, `sun_azimuth_degrees` | light the cloud from a chosen sun |
