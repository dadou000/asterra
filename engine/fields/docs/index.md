+++
path = "/world/fields"
title = "Generic field system"
kind = "subsystem"
status = "stable"
summary = "FieldRegistry describes spatial fields (scalar, vector, categorical, material, tensor) over domains (surface, volume, atmospheric shell, local region, body interior, free space) with CPU/GPU/hybrid residency and a resolution policy, sampled at surface, volume or free-space locations."
owner_module = "OrbitFields"
keywords = ["field", "field registry", "scalar field", "residency", "resolution", "surface field", "volume field", "domain"]
sources = [
  "engine/fields/include/orbit/fields/FieldRegistry.hpp",
  "engine/fields/CMakeLists.txt",
]
symbols = ["FieldResolutionPolicy"]
invariants = [
  "Material field values are semantic class IDs plus blend weights, not renderer material asset handles.",
]
related = ["/legacy/v0-0-3-spec/8-generic-field-system", "/legacy/v0-0-3-spec/5-cpu-gpu-residency-rules"]
depends_on = ["/foundation/core", "/foundation/frames", "/foundation/math", "/world/universe"]
used_by = ["/world/path-routing"]
verify = [
  "ctest -R Orbit.FieldRegistry",
]
verified = "b0a0de7f"
+++


