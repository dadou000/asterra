+++
path = "/world/weather-lab/global-system-status-p10-ground"
title = "Storm domain: cloud-to-ground strikes (descending leaders)"
kind = "concept"
status = "experimental"
summary = """
A share of the simulated storm's flashes are descending leaders that seek the ground and get the full return-stroke sequence; how the growth is biased, the setting, the measured share and what was not checked in a picture."""
keywords = ["cloud to ground", "descending leader", "groundSeekChance", "ground_strike_chance", "return stroke"]
sources = [
  "engine/storm_electrification/src/LeaderSimulation.cpp",
  "engine/studio_ui/src/StudioViewportStorm.cpp",
]
symbols = ["LeaderParams", "TryFlash"]
invariants = [
  "A flash that seeks the ground does not stop for the charge budget or for crossing air with no charge, only when it connects or reaches the cell cap.",
  "Studio caps the flashes per electrification step at 2: each costs a potential solve over the whole window.",
]
related = ["/world/weather-lab/global-system-status-p10-emission"]
depends_on = ["/world/weather-lab/global-system-status-p10-emission"]
verify = ["ctest -R \"Orbit.StormElectrification\"", "storm.lightning ground_strike_chance, then storm.status lightning_ground_flashes against lightning_flashes"]
verified = "08a50d4"
+++

**Cloud-to-ground strikes (requested next step).** The leader reached the ground in only 2.5 % of the flashes (6 of 240), because a channel grown toward strong field in air without charge has no reason to descend. `LeaderParams::groundSeekChance` makes that share of the flashes descending leaders: their steps going down are multiplied by 1 + `groundSeekBias` (12) times the downward component, and they do not stop for the charge budget or for crossing air with no charge (the stagnation rule), only when they connect or reach the cell cap. A grounded flash gets the full sequence of the emission model: the return stroke up the whole path, the dim dart leaders and later strokes, continuing current. Studio: `ground_strike_chance` (0 to 1, default 0.25) in `storm.lightning` and `orbit_storm_lightning`; the flashes per electrification step are capped at 2 (each costs a potential solve over the window), so the worker keeps up at 1x (9 s of work per 10 storm seconds). Measured at 1x for 120 s: 48 flashes, 18 reaching the ground (37 %, more than the 25 % asked for because the plain growth also grounds now and then); test: 2 of 3 seeking flashes ground, 0 of 3 plain ones. Not verified in a picture: with every flash seeking the ground the screenshots show several flashes at once and no clean single bolt, so how a ground strike looks is for the user to judge; frame strike and strike pause treat all flashes alike.

