# Derived hydraulic readouts for the river graph

- Existing owner: M16 `RiverNetwork` derives its directed graph from M09
  drainage, with M14 waterborne sediment as an optional page input.
- Primary insertion points: `RiverNetworkNode` and `RiverNetworkSegment`,
  physical-page graph construction, Planet Hydrology inspection, and the
  read-only `terrain.rivers_nearby` RPC/MCP operation.
- Canonical state: none added. Water level, Manning velocity, slope, wetted
  cross-section and segment summaries are derived for inspection and rendering.
- Sediment values report M14 waterborne inventory mass in kg, not concentration
  or transport rate.
- Native changes use the central Studio generation handoff. Failed generation
  leaves the active Studio generation available.
