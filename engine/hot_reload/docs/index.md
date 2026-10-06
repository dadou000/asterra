+++
path = "/foundation/hot-reload"
title = "Hot reload host and iteration service"
kind = "subsystem"
status = "stable"
summary = "The machinery behind save-to-reflect: ChangeClassifier maps a saved file to a reflection path, HotIterationService watches and dispatches, HotReloadHost loads versioned reloadable modules through a stable ModuleApi (HostApi, StateView, ModuleRegistration) and hands the previous generation's state to the new one."
owner_module = "OrbitHotReloadApi"
keywords = ["hot reload", "hot iteration", "change classifier", "module api", "host api", "generation", "state view", "watcher", "dll swap", "abi version"]
sources = [
  "engine/hot_reload/include/orbit/hot_reload/ChangeClassifier.hpp",
  "engine/hot_reload/include/orbit/hot_reload/HotIterationService.hpp",
  "engine/hot_reload/include/orbit/hot_reload/HotReloadHost.hpp",
  "engine/hot_reload/include/orbit/hot_reload/ModuleApi.hpp",
  "engine/hot_reload/CMakeLists.txt",
]
symbols = ["HotIterationEvent", "ModuleRegistration", "HostApi"]
invariants = [
  "The authoritative rules are /rules/hot-iteration; this module is their implementation.",
  "A module's OnLoad receives the host API and the previous generation's StateView; it must reject an ABI version it does not understand (host->abiVersion != kHostAbiVersion) and migrate state only when the schema and size match.",
  "ChangeClassifier is the single place where file kinds are routed to reflection paths: new source or resource extensions are added there and in its tests in the same change.",
]
related = ["/rules/hot-iteration", "/tools/hot-reload-probe", "/tools/eye-adaptation-module"]
depends_on = ["/foundation/core"]
used_by = ["/apps/studio", "/authoring/content"]
verify = [
  "ctest -R Orbit.HotReload",
]
verified = "b0a0de7f"
+++


