+++
path = "/world/weather-lab/global-system-status-p10-persistence"
title = "Storm domain: the placed storm and the lightning settings survive a restart"
kind = "concept"
status = "experimental"
summary = """
The storm placed with storm.place, the scheduled-storm setting and every lightning setting are saved in the project (Config/storm_domain.json) on each change and restored at startup, so a restart of Studio or the automatic handoff to a new generation of it brings back the same storm."""
keywords = ["persistence", "storm_domain.json", "StormPersistentState", "RestoreStormState", "hot handoff", "restart"]
sources = [
  "engine/studio_ui/src/StormDomain.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StormDomain.hpp",
  "apps/editor/src/StudioApplication.cpp",
]
symbols = ["StormPersistentState", "StormStateToValue", "RestoreStormState"]
invariants = [
  "The state is saved by the thread that changed it, after every successful Place, Clear, SetAuto and SetLightning (a rejected request is not saved), and restored through the same validating setters, so a hand-edited or stale file cannot put the mailbox in an invalid state; problems are logged and the rest is applied.",
  "A storm restored at startup spins up again from its beginning (spin_minutes), at its original latitude and longitude: the file keeps the request, not the state of the solver, which is recomputed. A scheduled storm needs no saving beyond the setting: it is a function of the clock.",
  "The simulation clock, its rate and the camera are not part of this state.",
]
related = ["/world/weather-lab/global-system-status-p10-ground", "/world/weather-lab/gpu-port"]
depends_on = ["/world/weather-lab/global-system-status-p10-ground"]
verify = ["ctest -R \"Orbit.StormDomain\"", "storm.place, restart Studio on the same project: storm.status is active again, mode manual, same latitude"]
verified = "e8a963b"
+++

**Why (reported by the user):** the storm was gone after Studio restarted. Studio replaces itself with a new generation when sources change (the automatic hot handoff, `docs/ORBIT_HOT_ITERATION.md`), and a new generation starts from nothing: everything held only in memory, the placed storm and the lightning settings among it, was lost. The hot-iteration rules ask for authoritative state to survive such handoffs; for the storm that state is small, so it is saved in the project.

**What is saved** (`Config/storm_domain.json` in the project, JSON):
- `manual`: the request of the storm placed with `storm.place` (latitude, longitude, cells, spacing, bubble, spin minutes, ice, sponge, nested); absent after `storm.clear`;
- `auto`: the `storm.auto` setting (kept when enabled);
- `lightning`: every `storm.lightning` setting, including the toggles of the bottom bar (strike pause, frame strike, E-grid) and the testing levers.

**How:** `StormDomainMailbox` keeps the manual request and calls a change handler (set by Studio after the restore) outside its lock whenever the state changes; the handler writes the file under a mutex. At startup Studio reads the file and applies it with `RestoreStormState`, which uses the mailbox setters. A file that cannot be read or a value that does not validate is logged, and what is valid is still applied.

**Checked:** with Studio started on a project without the file, `storm.place` and a set of lightning settings wrote a 507-byte file; after killing Studio and starting it again, `storm.status` was active, mode manual, latitude 5.55, 128 cells, and the lightning settings were the ones saved (test: the round trip of the JSON through a fresh mailbox, rejected requests not saved, invalid saved values reported).

**Not saved:** the simulation time and rate (a restart starts at the project's time, not at dusk), the camera pose, and the state of the storm itself (a restored storm spins up again, a few seconds, from its beginning).
