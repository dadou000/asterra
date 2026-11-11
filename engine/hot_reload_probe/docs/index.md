+++
path = "/tools/hot-reload-probe"
title = "Hot reload probe module"
kind = "subsystem"
status = "stable"
summary = "A minimal reloadable module used to prove the hot-reload path end to end: on each activation it checks the host ABI version, restores its state (an activation counter) when the previous generation's schema and size match, and logs the new count."
owner_module = "OrbitHotReloadProbe"
keywords = ["hot reload probe", "probe", "activation count", "test module", "abi check"]
sources = [
  "engine/hot_reload_probe/src/HotReloadProbe.cpp",
  "engine/hot_reload_probe/CMakeLists.txt",
]
symbols = ["OnLoad", "ProbeState"]
invariants = [
  "State migration is explicit: the probe restores previous state only when schema == 1 and the size equals its state struct, otherwise it starts fresh; OnLoad returns false for a host with a different ABI version.",
]
related = ["/foundation/hot-reload", "/rules/hot-iteration"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


