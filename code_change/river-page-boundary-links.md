# Inspectable river page continuations

- Existing owner: `RiverNetwork` derives M16 boundary links from M09 flow;
  `StudioTerrainPhysicalPageService` owns immutable built-page snapshots.
- Primary insertion points: `terrain.rivers_nearby` RPC/MCP and the Planet
  Hydrology menu, which already inspect generated M16 nodes.
- Canonical state: derived `RiverNetwork::boundaryLinks`; do not persist them
  or infer synthetic downstream nodes from an unloaded page.
- Expose the upstream node, stable basin identity, source page, flow vector and
  target edge coordinate. Pending destination pages remain explicitly pending.
- Iteration: the existing C++ changes follow the central Studio generation
  handoff; MCP wrapper/doc edits reflect immediately on server reload.
