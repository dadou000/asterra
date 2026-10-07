+++
path = "/foundation/time"
title = "Simulation time"
kind = "subsystem"
status = "stable"
summary = "SimulationTime is the authoritative simulation timeline (header-only). The running, paused and stepped clock that advances it for Studio lives in studio_session (SimulationClock)."
owner_module = "OrbitTime"
keywords = ["time", "simulation time", "epoch"]
sources = [
  "engine/time/include/orbit/time/SimulationTime.hpp",
  "engine/time/CMakeLists.txt"]
symbols = ["SimulationTime"]
invariants = [
  "OrbitTime is an interface library: SimulationTime is the only type and it is header-only. The clock that advances it for Studio is engine/studio_session SimulationClock (/editor/studio-session)."]
related = ["/foundation/frames", "/world/universe"]
depends_on = ["/foundation/core"]
used_by = ["/celestial/gravity", "/celestial/orbits", "/celestial/rotation", "/foundation/frames", "/rendering/volumes/representation", "/world/path-geometry", "/world/path-routing", "/world/universe", "/world/world-model"]
verify = [
  "ctest -R Orbit.TimeAndIds"]
verified = "b0a0de7f"
+++

Orbital and rotation providers evaluate at an explicit SimulationTime; frames resolve bodies at an explicit time (`/foundation/frames`, `/world/universe`).
