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
  "engine/rhi/vulkan/src/VulkanGpuProgress.hpp",
  "engine/rhi/vulkan/src/VulkanGpuProgress.cpp",
  "engine/rhi/vulkan/src/VulkanQueue.cpp",
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
  "The graphics queue is never idled after a submit. Every Queue::Submit signals one serial on a per-device timeline (GpuProgress, VulkanGpuProgress.hpp), and anything the CPU touches that the GPU may still use is protected precisely: host-visible and readback Buffer::Map waits for the last submit that referenced that buffer, CommandAllocator::Reset waits for the last submit recorded from it, and destroying a buffer, texture, pipeline, acceleration structure, timestamp pool or command pool is deferred until the next submit completes and never blocks the CPU.",
  "A host-visible buffer the CPU rewrites every frame must be ring-buffered per frame in flight (EditorUi keeps three vertex/index pairs). Map's wait keeps a single-buffered one correct but serialises that frame against the previous one, so a vk.host_hazard_wait is a performance bug and not a correctness bug.",
  "A buffer is registered for Map tracking only through VulkanCommandList::UseBuffer (stamped with the real submit serial at Submit). Any new command that binds, copies or barriers a buffer must go through it, and a host write that bypasses Buffer::Map (vmaMapMemory directly) has no guard.",
  "GPU timing: Device::CalibrateGpuClock reads the GPU timestamp clock and the CPU counter at the same instant so GPU timestamps can be placed on the CPU profiler timeline; it returns false when the backend cannot calibrate.",
  "Optional hardware features (ray query, acceleration structures, mesh shaders, variable-rate shading, calibrated timestamps, shader model) are reported by DeviceCapabilities and features gate on them; for example the proxy sun shadow runs only when ray queries are supported.",
]
related = ["/rendering/render-graph", "/rendering/lighting/proxy-sun-shadow", "/legacy/orbit-profiler"]
depends_on = ["/foundation/core", "/foundation/math"]
used_by = ["/apps/player", "/celestial/appearance", "/celestial/compact-render", "/celestial/far-render", "/celestial/globe", "/celestial/magnetosphere-render", "/celestial/rings", "/editor/ui-toolkit", "/foundation/runtime-session", "/rendering/debug-overlay", "/rendering/lighting/radiance-cache", "/rendering/planet-map", "/rendering/post-process", "/rendering/render-graph", "/rendering/render-view", "/rendering/shading", "/rendering/terrain/debug-fields", "/rendering/terrain/gpu-passes", "/rendering/terrain/material-column", "/rendering/terrain/regions", "/rendering/volumes/fields", "/rendering/volumes/render", "/rendering/volumes/solver", "/rendering/water", "/tools/eye-adaptation-module"]
verify = [
  "ctest -R Orbit.RhiCompute",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "rendering corruption (garbled UI text, flicker, stale geometry) that might be a CPU/GPU lifetime race"
steps = [
  "Set ORBIT_RHI_SYNC_SUBMIT=1: Queue::Submit then idles the queue after every submit like the old behaviour. If the corruption disappears it is a lifetime race.",
  "Run with ORBIT_VK_SYNC_VALIDATION=1 (enables the Khronos layer plus synchronization validation) and look for objects in use, resets while pending or host hazards.",
  "Check that the buffer is only written through Buffer::Map and that every command that references it goes through VulkanCommandList::UseBuffer.",
]

[[diagnose]]
symptom = "frame time is roughly CPU time plus GPU time, or the profiler shows vk.host_hazard_wait or a long vk.queue_submit"
steps = [
  "vk.queue_submit carrying the whole GPU time means ORBIT_RHI_SYNC_SUBMIT=1 is set in the environment.",
  "vk.host_hazard_wait is a Buffer::Map (or CommandAllocator::Reset) waiting for a submit still in flight. Run with ORBIT_RHI_LOG_HAZARDS=1: stderr prints the size, usage and memory type of every buffer whose Map had to wait.",
  "Fix the writer, not the guard: give that buffer a per-frame ring like EditorUi's upload slots. Never remove the wait.",
]
+++

The only backend today is Vulkan (`rhi::Backend::Vulkan`).
