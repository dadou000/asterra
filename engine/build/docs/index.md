+++
path = "/apps/build-service"
title = "Build service (validate, cook, package)"
kind = "subsystem"
status = "stable"
summary = "BuildService validates a project against a build profile, cooks deterministic project products (assets, scripts, manifest) through the content pipeline and shader compiler, and can assemble a player package."
owner_module = "OrbitBuildService"
keywords = ["build", "cook", "package", "build profile", "validate", "cooked project", "build manifest", "build request"]
sources = [
  "engine/build/include/orbit/build/BuildService.hpp",
  "engine/build/CMakeLists.txt",
]
symbols = ["BuildIssue"]
invariants = [
  "Cooking produces deterministic cooked project products; runtime executable assembly and package signing are separate stages that cooking does not imply.",
  "Editor imports and headless cooks share DDC key semantics because the cook path preserves each asset's canonical import settings (/authoring/content).",
]
related = ["/apps/build-cli", "/authoring/cooked-project", "/authoring/content", "/legacy/v0-0-3-spec/21-build-cook-package-system"]
depends_on = ["/authoring/content", "/authoring/content-wic", "/authoring/documents", "/foundation/core", "/foundation/platform-services"]
used_by = ["/apps/build-cli", "/apps/studio", "/authoring/cooked-project"]
verify = [
  "ctest -R Orbit.BuildService",
]
verified = "b0a0de7f"
+++


