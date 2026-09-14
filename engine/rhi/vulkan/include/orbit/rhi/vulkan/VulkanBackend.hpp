#pragma once

#include <orbit/rhi/Device.hpp>

#include <memory>
#include <string>

namespace orbit::rhi::vulkan
{
struct DeviceDesc
{
    bool enableValidation{false};

    // Extra VK_LAYER_KHRONOS_validation features, each independently
    // toggleable since they trade off against each other for
    // performance-analysis work: best-practices flags actual
    // performance anti-patterns with negligible overhead, synchronization
    // validation catches missing/incorrect barriers (real bugs, but this
    // is exactly the kind of session where you toggle it off once you
    // trust the barriers and want accurate timing again), and
    // GPU-assisted validation catches shader-side out-of-bounds access
    // at a real, sometimes large runtime cost while it's on. None of
    // these do anything unless enableValidation is also true.
    bool enableBestPracticesValidation{false};
    bool enableSynchronizationValidation{false};
    bool enableGpuAssistedValidation{false};

    // Loads renderdoc.dll (if present) before creating the Vulkan
    // instance so its capture layer activates for this process even
    // when launched directly rather than through the RenderDoc UI.
    // See orbit::rhi::vulkan::RenderDocCapture.
    bool enableRenderDoc{false};
};

[[nodiscard]] std::unique_ptr<Device> CreateDevice(const DeviceDesc& desc = {});

// RenderDoc capture control. All are safe to call regardless of whether
// the device was created with DeviceDesc::enableRenderDoc or RenderDoc
// was actually found (see RenderDocCapture::TryLoad) -- they're no-ops
// (or report "unavailable"/empty) in that case rather than throwing, so
// call sites like a dev-server command don't need to track availability
// themselves.

// Associates the given native window (HWND) with this device's Vulkan
// instance so a later TriggerRenderDocCapture applies to it, and so
// RenderDoc's in-app overlay can respond to keypresses on it. Call once
// both the device and window exist.
void SetRenderDocActiveWindow(Device& device, void* nativeWindow);

// Captures the next frame (the next present marks the boundary).
void TriggerRenderDocCapture(Device& device);

[[nodiscard]] bool IsRenderDocAvailable(const Device& device) noexcept;
[[nodiscard]] bool IsRenderDocCapturing(const Device& device) noexcept;

// Absolute path of the most recently completed capture, or empty if
// none has been made yet (including if RenderDoc isn't available).
[[nodiscard]] std::string LastRenderDocCapturePath(const Device& device);
} // namespace orbit::rhi::vulkan
