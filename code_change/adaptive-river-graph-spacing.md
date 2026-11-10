# Adaptive M16 river graph spacing

- Existing owner: `BuildRiverNetwork` derives M16 graph nodes from authoritative
  M09 drainage cells; the resulting segments already drive carving and display.
- Primary insertion point: graph extraction, retaining junctions, sharp bends,
  headwaters and page exits while sampling straight reaches at an authored
  maximum physical spacing.
- Canonical state: maximum reach spacing on the existing Terrain Process
  Settings object. No separate river graph or renderer-only topology.
- Keep node IDs tied to source cells. Sparse segments must follow the drainage
  field, preserve tributary joins, and carry page exits from the last sampled
  node to the exact page edge for overlays and MCP inspection.
- Iteration: native changes use the central Studio generation handoff; the
  toolbar and MCP call the same persisted process settings.
