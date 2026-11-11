+++
path = "/apps/player"
title = "Orbit Player"
kind = "subsystem"
status = "stable"
summary = "The runtime player executable: opens a cooked project (the path argument or the current directory), creates the platform runtime, opens the startup world read-only, runs the cooked script entry points and then renders through a RuntimeSession (1600x900, three swapchain buffers); --validate-only stops before any graphics device exists."
keywords = ["player", "runtime app", "validate only", "steam", "game executable", "cooked project", "startup world"]
sources = [
  "apps/player/src/Main.cpp",
  "apps/player/CMakeLists.txt",
]
symbols = []
invariants = [
  "--validate-only opens and validates the cooked project, loads the world and runs the script entry points, then exits 0 without creating a graphics device; unknown options throw.",
  "The startup world is opened read-only: the player never writes authoritative world state.",
  "Script entry points run before the render loop starts and use cooked bytecode only (/authoring/cooked-project).",
]
related = ["/authoring/cooked-project", "/foundation/runtime-session", "/foundation/platform-services", "/apps/build-cli"]
depends_on = ["/authoring/cooked-project", "/authoring/documents", "/authoring/scene", "/foundation/core", "/foundation/platform", "/foundation/platform-services", "/foundation/runtime-session", "/rendering/rhi"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


