+++
path = "/authoring/cooked-project"
title = "Cooked projects (runtime side of build)"
kind = "subsystem"
status = "stable"
summary = "The runtime-side model of a cooked project: CookedProject opens a package directory or manifest and validates it (format version, engine compatibility, every packaged file), exposing assets, script bytecode records, build profile and the startup world; ScriptRuntime executes the cooked Luau bytecode of every script entry point."
owner_module = "OrbitRuntimeProject"
keywords = ["cooked project", "cook", "runtime project", "build output", "player", "manifest", "script runtime", "package", "luau bytecode", "engine compatibility"]
sources = [
  "engine/runtime_project/include/orbit/runtime_project/CookedProject.hpp",
  "engine/runtime_project/CMakeLists.txt"]
symbols = ["CookedAssetRecord"]
invariants = [
  "The manifest format version must be exactly 1 and its engineCompatibilityVersion must equal the running engine's version, otherwise Open throws ('Cooked project requires Orbit X, current runtime is Y').",
  "All packaged paths must be non-empty, package-relative and resolve inside the package root: a path that escapes the root (for example through '..') or is not an existing regular file is rejected.",
  "Open validates the whole package up front: every asset artifact and every script bytecode file must exist, and every script entry point must have packaged bytecode.",
  "ScriptRuntime loads and executes only cooked bytecode for the project's entry points: source compilation never occurs at runtime, and the Luau state opens only the safe libraries.",
  "The player opens the startup world read-only (apps/player)."]
related = ["/apps/build-service", "/apps/player", "/authoring/content"]
depends_on = ["/apps/build-service", "/authoring/documents", "/authoring/scene", "/foundation/core"]
used_by = ["/apps/player"]
verify = [
  "ctest -R Orbit.RuntimeProject"]
verified = "b0a0de7f"
+++


