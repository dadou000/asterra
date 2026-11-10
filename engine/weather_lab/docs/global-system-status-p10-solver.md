+++
path = "/world/weather-lab/global-system-status-p10-solver"
title = "Storm domain: lightning window, flash size and the channel as a tree"
kind = "concept"
status = "experimental"
summary = """
Phase 10 follow-ups on the electrification itself, from the user's testing: the electrified window widened to the whole nested grid, flash size no longer shrunk by the testing charge boost, and the channel grown as a tree with a trunk that carries more current than its twigs instead of a mesh with equal current everywhere."""
keywords = ["flash size", "maxFlashCharge", "growthExponent", "sideBranchChance", "currentWeighted", "leader tree", "trunk", "TryFlash", "dielectric breakdown"]
sources = [
  "engine/storm_electrification/src/LeaderSimulation.cpp",
  "engine/storm_electrification/include/orbit/storm_electrification/LeaderSimulation.hpp",
  "engine/studio_ui/src/StudioViewportStorm.cpp",
]
symbols = ["LeaderParams", "TryFlash", "StormLightningFeed"]
invariants = [
  "The LeaderParams defaults grow the channel as a random mesh with equal energy in every segment; growthExponent, sideBranchChance, picksDivisor and currentWeighted are opt-in, and Studio sets them.",
  "With currentWeighted the flash's intracloud energy is shared in proportion to the segments downstream of each, with the total unchanged; a grounded flash's return stroke already runs along one path.",
  "A flash's charge budget scales with the testing charge boost: the boost changes how often the storm flashes, flash_size how large a flash is.",
  "The channel grows in a potential field that does not respond to the channel (no conductor boundary condition), so its direction comes from the charge layers, not from its own tip fields.",
]
related = ["/world/weather-lab/global-system-status-p10-visibility", "/rendering/unified-transport/storm-electrification"]
depends_on = ["/world/weather-lab/global-system-status-p10-visibility"]
verify = ["ctest -R \"Orbit.StormElectrification\"", "storm.status lightning_flash_cells and lightning_flash_radius_m; show a flash with frame_strike and a rate of 0.2"]
verified = "895ec73"
+++

**7. Flashes and window too small (reported by the user).** Two causes. The electrified window was 64 cells (32 km) around the ice while the storm and its nested grid are 64 km; it is now up to 128 cells, the whole nested grid (the worker needs about 9 s per 10 storm seconds, on its own thread, so it keeps up at 1x; `storm.status` reports `lightning_job_ms`). And the flashes were tiny (radius 1 to 2 km, measured with the new `lightning_flash_radius_m` and `lightning_flash_radius_max_m`) whatever `flash_size` was, because a flash stops once it could neutralise `maxFlashCharge` and the testing boost of the charging rate made every cell hold that many times more charge, so the budget was reached in a few cells. The budget now scales with the boost (and with `flash_size`, default 4): the boost changes how often the storm flashes, not how big a flash is. After the change the flashes reach 7 to 9.5 km from their centre (about 15 to 19 km across) at flash sizes 1 and 4; they are taller and wider than the charge layers, and the worker takes about 18 s per 30 storm seconds at rate 3.

**8. Every branch carried the same current (reported by the user, from a screenshot).** The leader grew as a random mesh and every segment radiated the same energy. Causes in `TryFlash`: any cell of the channel could extend, each candidate with a chance linear in the field excess, and a number of new cells per round proportional to the channel size, so in the smooth potential field the channel filled space; and the intracloud flash lit every segment with the same pulse. `LeaderParams` gained opt-in switches (the defaults keep the old behaviour and its tests): `growthExponent` (chance goes with excess^n, so growth concentrates where the field is strongest), `sideBranchChance` (only tips extend, a cell in the middle rarely does), `picksDivisor`, and `currentWeighted` (the flash's energy is shared over the segments in proportion to the segments downstream of each, the charge it carries back to the root, so the trunk is bright and the twigs dim; the total is unchanged). Studio uses 3, 0.03, 24 and on, and caps a flash at 400 cells (about 200 km of channel; the default 6000 let one fill the volume). The overlay's persistent glow of a formed segment is now a fraction of the strongest pulse it carries, so the trunk stays bright and thick and the twigs thin and dim through the whole flash. Test: the trunk radiates several times more than the twigs, the default sharing is equal. Seen in captures: a vertical main channel with thin side branches; many simultaneous flashes (a boosted storm throws 25 per job) still overlap into a mesh, and the leader-time glow of a growing tree is the same for all segments (only the bright stage has the trunk). The channel still grows in a fixed potential field (the channel does not distort it as a conductor), so it is directed by the charge layers and not by its own tip fields.

The light and arc timing (item 9), the lattice (item 10) and the main channel (item 11) are in `global-system-status-p10-shape`.
