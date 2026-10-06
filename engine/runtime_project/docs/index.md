+++
path = "/authoring/cooked-project"
title = "Cooked projects (runtime side of build)"
kind = "reference"
status = "stable"
summary = "The runtime-side model of a cooked project: CookedProject with CookedAssetRecord and CookedScriptRecord entries, CookedBuildProfile and CookedProjectManifest, plus ScriptRuntime."
owner_module = "OrbitRuntimeProject"
keywords = ["cooked project", "cook", "runtime project", "build output", "player", "manifest", "script runtime"]
sources = [
  "engine/runtime_project/include/orbit/runtime_project/CookedProject.hpp",
  "engine/runtime_project/CMakeLists.txt",
]
symbols = ["CookedAssetRecord"]
related = ["/legacy/v0-0-3-spec/21-build-cook-package-system", "/authoring/content"]
depends_on = ["/authoring/documents", "/authoring/scene", "/foundation/core"]
verify = [
  "ctest -R Orbit.RuntimeProject",
]
verified = "b0a0de7f"
+++


