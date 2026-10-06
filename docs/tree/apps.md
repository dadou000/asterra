+++
path = "/apps"
title = "Applications"
kind = "section"
status = "stable"
summary = """
The executables built from the repository: Orbit Studio (the one unified editor, Orbit.exe), the runtime Player, the engine Sandbox test application and the build CLI \
with the BuildService behind it. Applications are composition roots only: engine logic lives in modules."""
keywords = ["apps", "executable", "studio", "player", "sandbox", "build cli", "orbit.exe", "composition root"]
related = ["/rules/architecture", "/rules/ui"]

[routes]
"Studio / Orbit.exe, startup, relaunch, RPC port" = "studio"
"runtime player, cooked project, validate-only" = "player"
"sandbox test app, dev server on 4319" = "sandbox"
"build/cook/package from the command line" = "build-cli"
"what validate, cook and package do" = "build-service"
+++

| App | Directory | Notes |
| --- | --- | --- |
| Orbit Studio | `apps/editor` | target `OrbitStudio`, published as `<repo-root>/Orbit.exe` |
| Player | `apps/player` | `OrbitPlayer`, opens a cooked project |
| Sandbox | `apps/sandbox` | `OrbitSandbox`, engine test application |
| Build CLI | `apps/build` | `OrbitBuild` over `engine/build` |
