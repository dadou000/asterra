+++
path = "/apps/player"
title = "Orbit Player"
kind = "reference"
status = "stable"
summary = "The runtime player executable: opens a cooked project (the path argument, or the current directory), runs it through the runtime session with platform services (including the Steam provider), and supports --validate-only."
keywords = ["player", "runtime app", "validate only", "steam", "game executable"]
sources = [
  "apps/player/src/Main.cpp",
  "apps/player/CMakeLists.txt",
]
symbols = []
related = ["/authoring/cooked-project", "/foundation/runtime-session", "/foundation/platform-services"]
depends_on = ["/authoring/cooked-project", "/authoring/documents", "/authoring/scene", "/foundation/core", "/foundation/platform", "/foundation/platform-services", "/foundation/runtime-session", "/rendering/rhi"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


