+++
path = "/foundation/time"
title = "Simulation time and clock"
kind = "subsystem"
status = "stable"
summary = "SimulationTime is the authoritative simulation timeline; SimulationClock advances it in running, paused or manually stepped modes and accumulates interpolation time separately."
owner_module = "OrbitTime"
keywords = ["time", "simulation clock", "simulation time", "step", "pause", "fixed step"]
sources = [
  "engine/time/include/orbit/time/SimulationClock.hpp",
  "engine/time/include/orbit/time/SimulationTime.hpp",
  "engine/time/CMakeLists.txt",
]
symbols = ["SimulationClockDesc", "SimulationTime"]
invariants = [
  "Manual fixed stepping is valid in either paused or running mode: it advances authoritative simulation time immediately and leaves interpolation accumulation unchanged.",
]
related = ["/foundation/frames", "/world/universe", "/legacy/v0-0-3-spec/6-universe-frames-and-time"]
depends_on = ["/foundation/core"]
used_by = ["/celestial/gravity", "/celestial/orbits", "/celestial/rotation", "/foundation/frames", "/rendering/volumes/representation", "/world/path-geometry", "/world/path-routing", "/world/universe", "/world/world-model"]
verify = [
  "ctest -R Orbit.TimeAndIds",
]
verified = "b0a0de7f"
+++

Orbital and rotation providers evaluate at an explicit SimulationTime; frames resolve bodies at an explicit time (`/foundation/frames`, `/world/universe`).
