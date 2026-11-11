+++
path = "/celestial"
title = "Celestial bodies"
kind = "section"
status = "stable"
summary = """
Everything about bodies as astronomical objects: orbits (fixed, analytic conic, imported ephemeris, N-body), rotation, gravity, stars (radiometry and \
appearance), eclipses and reflected light, planets from orbit (appearance, representation ladder, macro globe, far impostors), rings, giants, compact objects, \
magnetospheres, orbital oceans, small bodies and the budgeted work scheduler. A body has one semantic authority and several derived, disposable representations; \
atmosphere and clouds live under /rendering."""
keywords = ["celestial", "body", "planet", "star", "orbit", "rotation", "gravity", "space", "solar system", "v0.0.6"]
related = ["/world/universe", "/rendering/atmosphere", "/rendering/clouds"]

[routes]
"orbital motion, ephemeris, N-body" = "orbits"
"rotation, axial tilt, tidal locking" = "rotation"
"gravity queries" = "gravity"
"star brightness, SI units, exposure scale" = "radiometry"
"star appearance, corona, glare" = "stellar"
"eclipse, transit, moonlight, reflected light" = "lighting"
"planet looks wrong from orbit, far view colours" = "appearance"
"which representation is drawn at which distance" = "representation"
"orbital globe patches, macro globe" = "globe"
"disc impostors, point proxies" = "far-render"
"ring systems" = "rings"
"gas and ice giants" = "giants"
"black holes, accretion" = "compact-objects"
"aurora, magnetic field" = "magnetosphere"
"ocean seen from orbit" = "ocean"
"asteroids, comets" = "small-bodies"
"derived work budget, stale completions" = "scheduler"
"sky, scattering, limb" = "/rendering/atmosphere"
"clouds" = "/rendering/clouds"
"all V0.0.6 research notes" = "/legacy/tree-history-research-v006-celestial-research-baseline"
+++

The V0.0.6 milestones M01-M31 built this layer; each module's card links the research note that records its baseline and intentional limits. The shared rules:
**authority is semantic** (capabilities on a body), **products are derived and disposable**, **FrameGraph is the only spatial hierarchy**, and **no representation becomes
authoritative** (`/celestial/representation`).
