# M09-derived lake basins in Studio

- Existing owners: M09 `DrainagePage` owns conditioned terrain and depression
  fill; `terrain_water::LakeWaterField` owns sampled presentation data; the
  physical-page service publishes derived products and the viewport builds the
  water-depth upload.
- Primary insertion points: an M09-backed lake-field builder, immutable
  physical-page snapshot, existing water-depth texture path, Planet Hydrology
  inspector, and `terrain.lakes_nearby` RPC/MCP. The M16 association is a
  derived join from each spill outlet along M09 flow to the first existing
  downstream river node on the same page.
- Canonical state remains M09's derived depression fill. Do not persist lakes
  or treat generated lake surfaces as terrain elevation authority.
- Keep this first bridge page-local and report page exits explicitly; do not
  infer global lake boundaries or downstream pages that are not built.
- Do not create a new river path at the lake edge: M09 remains the routing
  source and M16 remains the graph authority; report when no qualifying channel
  is present downstream.
- Iteration uses the existing Studio generation handoff. RPC/MCP queries are
  read-only views of the same published snapshot used by the viewport.
