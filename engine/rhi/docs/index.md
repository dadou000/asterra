+++
path = "/rendering/rhi"
title = "RHI (render hardware interface) and Vulkan backend"
kind = "subsystem"
status = "stable"
summary = "The device-neutral GPU interface (Device, Queue, Swapchain, Fence, CommandList, Pipeline, Resource buffers/textures, timestamp queries, acceleration structures) plus the Vulkan backend in engine/rhi/vulkan (VulkanBackend, RenderDoc capture). Everything above renders through this interface; the persistent world never holds GPU handles."
owner_module = "OrbitRHI"
keywords = ["rhi", "vulkan", "device", "command list", "queue", "swapchain", "fence", "barrier", "pipeline", "texture", "buffer", "ray query", "acceleration structure", "timestamp", "renderdoc"]
sources = [
  "engine/rhi/include/orbit/rhi/AccelerationStructure.hpp",
  "engine/rhi/include/orbit/rhi/Command.hpp",
  "engine/rhi/include/orbit/rhi/Device.hpp",
  "engine/rhi/include/orbit/rhi/Fence.hpp",
  "engine/rhi/include/orbit/rhi/Pipeline.hpp",
  "engine/rhi/include/orbit/rhi/Query.hpp",
  "engine/rhi/include/orbit/rhi/Queue.hpp",
  "engine/rhi/include/orbit/rhi/Resource.hpp",
  "engine/rhi/vulkan/include/orbit/rhi/vulkan/VulkanBackend.hpp",
  "engine/rhi/vulkan/include/orbit/rhi/vulkan/RenderDocCapture.hpp",
  "engine/rhi/vulkan/CMakeLists.txt",
  "engine/rhi/CMakeLists.txt",
]
symbols = ["AccelerationAabb", "ClearColor", "DeviceCapabilities", "Fence", "VertexAttribute", "TimestampQueryPool", "Queue", "BufferDesc"]
invariants = [
  "Render code depends on the RHI interface, never on Vulkan types; the Vulkan backend is an implementation detail behind it (render --> RHI interface --> backend).",
  "The public RHI deliberately exposes AABB acceleration structures, not Vulkan-specific BLAS/TLAS handles; triangle/mesh AS creation can be added as a sibling path later.",
  "Transition early-outs when before == after; a same-state UnorderedAccess hazard (ping-pong compute passes) needs the always-emitting barrier call.",
  "Buffer and texture upload helpers issue no barrier of their own: the caller transitions the destination to CopyDestination first and to ShaderResource afterwards.",
  "Storage-image usage on a texture is opt-in (it signals real UAV intent and is not free on every format), unlike the blanket transfer/sampled usage flags.",
  "Mapping a readback buffer invalidates non-coherent Vulkan memory before the pointer is returned.",
  "GPU timing: Device::CalibrateGpuClock reads the GPU timestamp clock and the CPU counter at the same instant so GPU timestamps can be placed on the CPU profiler timeline; it returns false when the backend cannot calibrate.",
  "Optional hardware features (ray query, acceleration structures, mesh shaders, variable-rate shading, calibrated timestamps, shader model) are reported by DeviceCapabilities and features gate on them; for example the proxy sun shadow runs only when ray queries are supported.",
]
related = ["/rendering/render-graph", "/rendering/lighting/proxy-sun-shadow", "/legacy/orbit-profiler"]
depends_on = ["/foundation/core", "/foundation/math"]
used_by = ["/apps/player", "/celestial/appearance", "/celestial/compact-render", "/celestial/far-render", "/celestial/globe", "/celestial/magnetosphere-render", "/celestial/rings", "/editor/ui-toolkit", "/foundation/runtime-session", "/rendering/debug-overlay", "/rendering/planet-map", "/rendering/post-process", "/rendering/render-graph", "/rendering/render-view", "/rendering/shading", "/rendering/terrain/debug-fields", "/rendering/terrain/material-column", "/rendering/terrain/regions", "/rendering/volumes/fields", "/rendering/volumes/render", "/rendering/volumes/solver", "/rendering/water", "/tools/eye-adaptation-module"]
verify = [
  "ctest -R Orbit.RhiCompute",
]
verified = "b0a0de7f"
+++

The only backend today is Vulkan (`rhi::Backend::Vulkan`).
