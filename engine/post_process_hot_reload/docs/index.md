+++
path = "/tools/eye-adaptation-module"
title = "Eye adaptation hot-reload module"
kind = "subsystem"
status = "stable"
summary = "The reloadable DLL OrbitEyeAdaptationHotReload: it exposes the eye-adaptation update through the versioned interface 'orbit.post_process.human_eye_adaptation' so saving HumanEyeAdaptation.cpp swaps the implementation in-process."
owner_module = "OrbitEyeAdaptationHotReload"
keywords = ["eye adaptation module", "hot reload module", "dll", "queryInterface", "interface version", "reloadable"]
sources = [
  "engine/post_process_hot_reload/src/EyeAdaptationHotReloadModule.cpp",
  "engine/post_process_hot_reload/CMakeLists.txt",
]
symbols = ["QueryInterface", "kEyeInterface"]
invariants = [
  "The module's interface name and version (kHumanEyeAdaptationHotReloadInterfaceName, kHumanEyeAdaptationHotReloadInterfaceVersion) are the contract with the host; QueryInterface answers only the matching name and version.",
  "New state fields of the eye update must default to 'no limit' so the running state survives a swap.",
]
related = ["/rendering/lighting/eye-adaptation", "/foundation/hot-reload", "/rendering/post-process"]
depends_on = ["/foundation/core", "/rendering/rhi", "/rendering/shader-compiler"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


