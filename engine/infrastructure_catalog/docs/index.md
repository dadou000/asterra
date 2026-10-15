+++
path = "/infrastructure/catalog"
title = "Infrastructure catalog: typed road/rail rules"
kind = "subsystem"
status = "experimental"
summary = "Lightweight versioned road/rail catalog rule definitions and per-segment overrides; Orbit Paths remains the graph authority."
owner_module = "OrbitInfrastructureCatalog"
keywords = ["road", "rail", "catalog", "rules", "segment", "banking"]
sources = ["engine/infrastructure_catalog/include/orbit/infrastructure_catalog/InfrastructureCatalog.hpp"]
symbols = ["Catalog", "SegmentIntent", "DesignRules"]
invariants = [
  "Catalog definitions are authored metadata, never generated mesh/collision authority.",
  "Existing Orbit Paths remains the network graph authority.",
  "Catalog version mismatches fail resolution explicitly.",
  "Per-segment overrides do not mutate shared catalog entries.",
]
depends_on = ["/infrastructure"]
verify = ["Orbit.InfrastructureCatalog CTest (Windows build)", "python tools/orbit_docs_cli.py check"]
+++

This first M1 slice defines infrastructure presets and typed segment intent over existing Orbit Paths profiles. It is not yet persisted or wired to Studio/RPC/MCP. Next: audit PathNetwork persistence and CommandService to attach catalog IDs and validated mutations to existing path edges. Supported versus terrain mode is independent of road/rail type. Design speed and transition preferences are intents, not claims that an engineering alignment solver has already enforced them.
