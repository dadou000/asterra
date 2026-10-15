+++
path = "/world/weather-lab/global-system-status-p10-shape"
title = "Storm domain: the lightning channel's shape, lattice and timing"
kind = "concept"
status = "experimental"
summary = """
Phase 10 follow-ups from the user's screenshots: the lights and arcs reading the same pulses, the grid lattice hidden with diagonal growth and node jitter, and a main channel that crosses the cloud (screening, direction persistence, one cell per round) with an arc brightness that follows the flash."""
keywords = ["diagonalGrowth", "screening", "directionPersistence", "ChannelLine", "flashPeakWatts", "lattice", "main channel", "arc timing", "trunk"]
sources = [
  "engine/storm_electrification/src/LeaderSimulation.cpp",
  "engine/lighting/src/TransientFlashRegistry.cpp",
  "engine/studio_ui/src/StudioViewportRenderer.cpp",
  "engine/studio_ui/src/StudioViewportStorm.cpp",
]
symbols = ["LeaderParams", "TryFlash", "ChannelLines", "FlashArcLines"]
invariants = [
  "The lights on the clouds and the arc overlay read the same pulses of the flash (the channel glow is a pulse like any other), so their timing and duration agree; the overlay adds only a 2 ms fade per unit of slow motion.",
  "The channel steps between cell centres of the electrification grid; the overlay moves each node by a hash of its own position (a quarter of a cell) so the lattice is not recognisable and connected segments still meet.",
  "LeaderParams defaults are the plain mesh growth; diagonalGrowth, screening and directionPersistence are opt-in and Studio sets them. A tip is a cell without a child in the tree.",
  "The channel grows in a potential field that does not respond to the channel; screening stands in for the weakening of the field around a conducting channel.",
]
related = ["/world/weather-lab/global-system-status-p10-solver", "/rendering/unified-transport/storm-electrification"]
depends_on = ["/world/weather-lab/global-system-status-p10-solver"]
verify = ["ctest -R \"Orbit.StormElectrification\"", "storm.lightning frame_strike and pause_on_strike on, then time.set rate 0.05: the arcs and the light fade out together, a bright main channel with thin branches"]
verified = "4cf7f96"
+++

**9. The light on the clouds did not match the arc (reported by the user).** The arc overlay kept every formed segment visible with a persistent glow added in the registry, while the lights on the clouds only reacted to the flash's energy pulses, so the light was a brief spike at the bright stage and the arc stayed far longer. The glow is now part of the flash: when a flash is queued every segment gets a pulse from its formation to a little after the last pulse (3 x 0.02 s of flash time, at most 3 s), at a fraction (0.5 %, at least 20 kW) of the strongest pulse it carries, and the registry's `ChannelLines` reads pulses only. Lights and arcs now read the same data, so their timing and duration agree. Checked: at a rate of 0.2, the frame's mean brightness followed the number of lit arc segments (39 with 406 segments, 4.3 with 858, 0.2 with 104, 0 with none). The bright stage is still a spike, so the light peaks when the whole channel flashes while the arc stays lit a little after it.

**10. The 3D lattice was obvious (reported by the user).** The channel steps between cell centres, and with the 6 face neighbours every segment was along an axis, with right-angle turns and many parallel vertical runs. Two changes. `LeaderParams::diagonalGrowth` lets the channel step to the 26 neighbours (the potential step is divided by the real step length); the first version of the "only tips extend" rule counted channel neighbours, which with diagonals is almost every cell, so flashes stopped after about 9 cells; a tip is now a cell with no child in the tree (`hasChild`). And the overlay moves every node (a cell centre) by a hash of its own position, up to a quarter of a cell, so segments that meet in a node still meet, before breaking each edge with the jagged midpoints. Tests: the face-neighbour channel is all axis-aligned, the 26-neighbour one has at most 70 % axis-aligned segments and the same full-size tree. Seen in captures: curved, diagonal channels with no visible grid. The worker takes about 12.7 s per 10 storm seconds now (400-cell flashes over 26 neighbours), a little behind real time at rate 1, so flashes lag slightly; frames are unaffected.

**11. No main channel, lights and arcs out of step (reported by the user, from a screenshot).** Three causes, three changes. (a) *The channel packed space*: with the channel at a fixed potential the field is strongest at one fixed region of the cloud, so the leader flew there and filled its neighbourhood (400 cells inside a box 6 km wide). `LeaderParams::screening` multiplies a candidate by screening^(channel cells touching it - 1), the weakening of the field at cells that hug a conducting channel, and `directionPersistence` favours a step that continues the heading of the segment that reached its parent; Studio uses 0.15 and 6, one new cell per round (`picksDivisor` huge), growth exponent 4 and a 0.4 % chance for an inner cell to branch. Test: the same 400 cells reach more than 1.4 times farther with screening and persistence (7 km to 12-16 km on the 24 km test grid); in Studio a flash is a bright main channel with long thin branches. (b) *The arc brightness* was a log of the power with a narrow range, so trunk and twig looked alike and the arc did not flare with the light: the flash's brightness now follows its peak power (about 0.9 in the glow, 7 at the bright stage) and each segment shows its share of the strongest segment of the same flash (`ChannelLine::flashPeakWatts`), the trunk full, the twigs a fifth, with thinner and fainter lines. (c) *The arc outlasted the light*: the overlay faded each segment exponentially (20 ms) until under a threshold, about nine time constants, while the lights read pulses that end sharply; the fade is now 2 ms per unit of slow motion and the glow pulses end 30 ms after the last pulse, and the leader runs at 4 km/s x 1000 so a 400-cell channel grows in about 50 ms. Measured at a rate of 0.05: the light and the arcs now both fade out between 3 and 3.3 s, where the arcs used to stay lit about 4 s after the light.

