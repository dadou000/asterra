+++
path = "/rendering/volumes"
title = "Volumes (universal volumetrics)"
kind = "section"
status = "stable"
summary = """
Generic authored volumes (fog, clouds-like media, liquids, particles) as fields: tiled GPU field storage, a representation policy that picks how each \
volume is represented, caches and transport-neutral outputs (particle spawns, surface deposits), a surface volume solver and the ray-marching and GPU particle \
renderers. Atmosphere clouds have their own subsystem (/rendering/clouds)."""
keywords = ["volume", "volumetrics", "fog", "particles", "volume field", "representation", "universal volume"]
related = ["/rendering/clouds", "/rendering/lighting"]

[routes]
"volume tiles, channels, residency" = "fields"
"representation mode, volume cache, bake, outputs, surface deposits" = "representation"
"surface volume solve, slice debug views" = "solver"
"ray-marched volumes, GPU particles, OIT" = "render"
"design baseline" = "/legacy/tree-history-research-v007-universal-volumetrics"
+++

The V0.0.7 milestones M31-M38 introduced this stack: M31 field storage, M35 renderer, M36 representation policy, M37 caches, M38 outputs and GPU particles
(`/legacy/v0-0-7-spec`).
