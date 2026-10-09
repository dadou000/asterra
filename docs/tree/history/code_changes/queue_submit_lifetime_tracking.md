# Queue submit lifetime tracking

## Behavior
Frames no longer stall on a full GPU idle at every `Queue::Submit`. Before: `VulkanQueue::Submit` called `vkQueueWaitIdle` so that host-visible buffers could be rewritten and resources destroyed right after a submit. After: the queue signals a per-device timeline semaphore, and only the operations that really need a GPU result wait for it. Static Atrium scene: about 51 -> 90 fps, with zero hazard waits and an identical validation-message set.

## Existing owner
- Module: `engine/rhi/vulkan`
- Class/service: `VulkanQueue`, `VulkanBuffer`, `VulkanCommandAllocator`, `VulkanCommandList`, the device's deferred-destruction path
- Canonical state: one timeline semaphore per device (`GpuProgress`); every submit gets a serial, every resource remembers the serial of its last GPU use

## Primary insertion point
- File: `engine/rhi/vulkan/src/VulkanGpuProgress.{hpp,cpp}` (new), `VulkanQueue.cpp`
- Symbol/function: `GpuProgress::BeginSubmit/IsComplete/Wait/Retire/Collect/Flush`, `Buffer::Map` (waits on the buffer's last-use serial), `CommandAllocator::Reset` (waits on its last submit), `VulkanCommandList::UseBuffer` + `FinalizeSubmit`
- Reason: the stall was the idle in `Submit`; the information needed to remove it (what the GPU still reads) is only known per command list, so it is recorded where buffers are bound.

## Secondary touch points
- `engine/editor_ui/src/EditorUi.cpp`: 3-slot upload ring for the ImGui vertex/index buffers so the CPU never rewrites a buffer a pending frame still reads.
- Destruction of buffers, textures, acceleration structures, query pools and pipelines goes through `Retire`, so Vulkan objects outlive the GPU work that references them.
- Debug switches: `ORBIT_RHI_SYNC_SUBMIT=1` restores the old per-submit idle, `ORBIT_RHI_LOG_HAZARDS=1` logs every host wait.
- `engine/rhi/docs/index.md` records the lifetime invariants and two diagnose recipes.

## Must not be implemented in
- Per-call-site `vkDeviceWaitIdle`/fences in renderers: the lifetime rules belong to the RHI so callers cannot get them wrong.
- A second submission queue: Orbit still has one `VkQueue`.

## Data/control flow
`CommandList::Bind*` -> `UseBuffer` -> `Queue::Submit` (serial, `FinalizeSubmit`) -> timeline signal -> `Buffer::Map` / `CommandAllocator::Reset` / `Retire` consult `GpuProgress`

## Validation
- [x] Existing authority remains canonical (timeline semaphore, no second tracker)
- [x] No duplicate state/controller was introduced
- [x] `tests/RhiHostHazardTests.cpp` (back-to-back host copies, destroy while in flight, submit does not block) passes and fails under `ORBIT_RHI_SYNC_SUBMIT=1`
- [x] GPU readback tests and a 2,949-panel-switch UI stress run clean; validation-layer message sets are identical in sync and async mode
- [x] Atrium FPS A/B with the same binary (recipe in the Atrium FPS note of the agent memory)
