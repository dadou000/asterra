+++
path = "/foundation/runtime-session"
title = "Runtime session (window + device + swapchain)"
kind = "subsystem"
status = "stable"
summary = "RuntimeSession ties a platform Window to an RHI Device, Queue and Swapchain for runtime applications."
owner_module = "OrbitRuntime"
keywords = ["runtime session", "swapchain", "resize", "device", "window", "present"]
sources = [
  "engine/runtime/include/orbit/runtime/RuntimeSession.hpp",
  "engine/runtime/CMakeLists.txt",
]
symbols = ["RuntimeSessionDesc"]
invariants = [
  "Resize returns true only when a resize actually happened; swapchain-sized resources owned by higher-level render systems must be recreated by those systems after a true result.",
]
related = ["/foundation/platform", "/rendering/rhi"]
depends_on = ["/foundation/core", "/foundation/platform", "/rendering/rhi"]
used_by = ["/apps/player", "/apps/studio"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


