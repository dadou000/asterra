#pragma once

#include <memory>
#include <string>

namespace orbit::rhi::vulkan
{
// Loads renderdoc.dll (if available) and exposes RenderDoc's in-application
// API so a capture can be triggered on demand -- from a dev-server command,
// say -- without needing to launch the app through the RenderDoc UI or
// alt-tab to it first.
//
// Must be constructed BEFORE the Vulkan instance: RenderDoc's Vulkan
// capture layer only activates for instances created after renderdoc.dll
// is loaded into the process (see TryLoad's doc comment for how it's
// found).
class RenderDocCapture
{
public:
    // Returns null if RenderDoc isn't available: neither already loaded
    // (the app was launched under the RenderDoc UI or with its Vulkan
    // layer globally enabled) nor found at RenderDoc's default install
    // path or the ORBIT_RENDERDOC_DLL environment variable.
    [[nodiscard]] static std::unique_ptr<RenderDocCapture> TryLoad();

    ~RenderDocCapture();

    RenderDocCapture(const RenderDocCapture&) = delete;
    RenderDocCapture& operator=(const RenderDocCapture&) = delete;

    // Tells RenderDoc which device/window pair a later TriggerCapture()
    // applies to and lets its in-app overlay respond to keypresses on
    // this window. Call once the VkInstance and HWND both exist.
    void SetActiveWindow(void* vkInstance, void* hwnd);

    // Captures the next frame -- the next present marks the boundary.
    // Safe to call from any thread per the RenderDoc API.
    void TriggerCapture();

    [[nodiscard]] bool IsCapturing() const;

    // Absolute path of the most recently completed capture, or empty if
    // none has been made yet. Capture files don't appear here until the
    // frame they were triggered for has actually finished presenting.
    [[nodiscard]] std::string LastCapturePath() const;

private:
    RenderDocCapture(void* module, void* api, bool ownsModule) noexcept;

    void* module_{nullptr};
    void* api_{nullptr};
    bool ownsModule_{false};
};
} // namespace orbit::rhi::vulkan
