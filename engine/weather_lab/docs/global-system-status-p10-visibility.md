+++
path = "/world/weather-lab/global-system-status-p10-visibility"
title = "Storm domain: seeing the lightning, one time control, strike pause"
kind = "concept"
status = "experimental"
summary = """
Phase 10 follow-ups from the user's testing: the lightning channel drawn as jagged arcs with a brightness gain, the flash clock following the simulation rate (slow motion, pause, fast forward), a rate slider in the bottom bar, and a toggle that pauses just before an in-view flash so it can be played in slow motion."""
keywords = ["lightning arcs", "ChannelLines", "flash clock", "time rate slider", "strike pause", "pause_on_strike", "UpcomingFlashes", "slow motion"]
sources = [
  "engine/studio_ui/src/StudioViewportRenderer.cpp",
  "engine/studio_ui/src/SimulationControlsUi.cpp",
  "engine/lighting/src/TransientFlashRegistry.cpp",
]
symbols = ["WatchStrikes", "TickFlashClock", "FlashArcLines", "ChannelLines", "UpcomingFlashes"]
invariants = [
  "The flash clock follows the simulation clock up to real time (rate 1) and stops when paused; above 1 the weather speeds up and a flash stays at real time, because a stroke lasts milliseconds and a faster clock would step over it between frames.",
  "The arcs are drawn without depth testing, over the clouds, from the registry's lit channel segments (a pulse in progress or fading after it), each segment broken into four with a displacement hashed from its end points so it is the same every frame.",
  "The strike pause fires once per flash, before the flash starts (or within 0.1 s after), and only for a flash whose channel sphere intersects the view; flashes of one electrification job are queued up to 10 s ahead, which is the lead time.",
  "Results are single runs of one storm on one machine; the arcs were checked in randomly timed screenshots, the rest by RPC.",
]
related = ["/world/weather-lab/global-system-status-p10", "/rendering/unified-transport/storm-electrification"]
depends_on = ["/world/weather-lab/global-system-status-p10"]
verify = ["storm.lightning with pause_on_strike true while time plays: time.get playing turns false before a flash lights; time.set rate 0.05 and playing true then plays it in slow motion", "ctest -R \"Orbit.LightingTransientFlashRegistry|Orbit.StormDomain\""]
verified = "94291df"
+++

**3. Seeing the flash (reported by the user: too dim, the light on the clouds slow, no arc).** There was no drawing of the channel at all: a flash only existed as up to 16 point lights. Now `TransientFlashRegistry::ChannelLines` returns the lit segments of the live flashes (a pulse in progress, or fading exponentially after it) and a `LightningArcs` pass after the cloud composite draws them with the gizmo line renderer, each channel segment broken into four with a displacement hashed from its own end points (the electrification's channel follows the 500 m grid and was a lattice of straight axis-aligned lines before), on a logarithmic HDR scale, with a soft edge of two half-pixel lines. `storm.lightning` gained `brightness` (energy of every flash, lights and arc alike) and `arcs`; `storm.status` reports `lightning_arc_segments`. A frame caught during a flash shows the cloud lit from inside and the jagged channel network (about 200 px across at 50 km); the earlier "slow" look came from a slow motion of 100, which spreads a flash over seconds at the same power. Seen only in screenshots taken at random, so how it looks in motion is for the user to judge.

**3b. The flashes were 4x too high (found by the user from the arcs).** Once the arcs were visible they floated far above the storm. The charge grid has one spacing for all three axes, and the feed gave it the solver's horizontal spacing (2 km by default) while the vertical cells are 500 m, so every channel was 4x too tall (and the fields were read at the wrong levels). The lightning now runs on the nested child grid, which is 500 m in all three directions, and a single grid is resampled to cubic cells of its vertical spacing; the window's place is kept in metres from the grid's west and south edges (`NestedGpuSolver::ChildWestSouthEdge`). Checked in a frame caught during a flash: the arcs sit on the cloud's flash glow inside the anvil.

**4. One time control (requested by the user).** The flashes ran on the wall clock, so the simulation rate scaled the weather and the storm but not the lightning. The flash clock (`StudioViewportRenderer::TickFlashClock`) now follows the simulation clock up to real time: `time.set rate` below 1 plays a flash in slow motion with the weather, pausing holds a flash in the middle of its stroke (so it can be looked at or captured), and above 1 the storm and sky speed up while a flash stays at real time (a stroke lasts milliseconds; a faster clock would step over it between frames), the flashes of one electrification job being spread over the real time they cover. The rate selector gained 0.01x, 0.05x, 0.25x, 0.5x, 2x and 5x. Checked live: at rate 1 a flash was lit about 5 % of the time, paused after a flash it stayed lit for 4 s, and at rate 0.1 it stayed lit 34 % of 4 s. `slow_motion` of `storm.lightning` still stretches the flashes on top of that.

**5. Strike pause (requested by the user).** A toggle "Strike pause" in the bottom bar (`storm.lightning pause_on_strike`, MCP `orbit_storm_lightning`) pauses the simulation about 0.3 s of flash time before a flash that is in view starts (`WatchStrikes`: the registry's `UpcomingFlashes` gives each queued flash's start and a sphere around its channel, the view test is the camera frustum with the sphere's size as margin), once per flash; the flashes of an electrification job are queued up to 10 s ahead, so there is lead time. Then play it with the rate slider or `time.set` below 1x. Checked live: with it on the simulation paused 5.8 s after the toggle, before the flash lit (arc segments 0, 20 flashes queued); resumed at 0.05x the flash was lit for 67 % of an 8 s window. A flash that begins immediately (no lead) pauses with its first pulses already past.

**6. Two more helpers (requested by the user).** Both are toggles in the bottom bar and `storm.lightning` / `orbit_storm_lightning` parameters.
- **Frame strike** (`frame_strike`): when a flash is about to start (the same queue the strike pause watches, any flash, in view or not), the renderer asks Studio to turn the camera to it. The request is stored and applied between frames, not during the compose: the camera moves along the line from the flash, at the same altitude (the same distance from the planet's centre), until the sphere around the channel fills about 60 % of the narrower field of view, and aims at its centre (`viewport` pose restore with a fresh surface frame). With the strike pause on as well it then pauses. Checked live: the camera moved 12.7 km at unchanged altitude and the flash was centred; a flash that is still growing from its centre looks small because the sphere is that of the finished channel.
- **E-grid** (`show_grid`): draws the electrified window as a box with a grid line every 4 km on the ground, the four faces and every 4 km of height, and the charge in it as crosses on 2 km blocks (4 x 4 x 4 cells; red positive, blue negative, size with the square root of the charge, blocks under 5 % of the largest skipped). The worker publishes the block sums after each job (`StormChargeBlocks`), so the picture follows the charge as it builds and discharges. Checked live: the box and a layered pattern of positive and negative blocks with the larger ones in the upper part of the window.

