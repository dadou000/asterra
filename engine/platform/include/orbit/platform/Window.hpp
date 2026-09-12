#pragma once

#include <orbit/core/Types.hpp>

#include <memory>
#include <string_view>

namespace orbit::platform
{
enum class Key : u8
{
    W,
    A,
    S,
    D,
    Q,
    E,
    LeftShift,
    Escape,
    F3
};

struct MouseDelta
{
    i32 x{0};
    i32 y{0};
};

struct WindowDesc
{
    std::string_view title{"Orbit"};
    u32 width{1600};
    u32 height{900};
};

class Window
{
public:
    virtual ~Window() = default;

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    virtual bool PumpEvents() = 0;

    [[nodiscard]] virtual bool KeyDown(
        Key key) const = 0;

    virtual void SetRelativeMouseMode(
        bool enabled) = 0;

    [[nodiscard]] virtual bool RelativeMouseMode() const noexcept = 0;

    [[nodiscard]] virtual MouseDelta ConsumeMouseDelta() = 0;

    [[nodiscard]] virtual void* NativeHandle() const = 0;
    [[nodiscard]] virtual u32 Width() const = 0;
    [[nodiscard]] virtual u32 Height() const = 0;

    // Captures the current window client area to an uncompressed
    // BMP file. Used by the dev server to hand screenshots to
    // external tooling (e.g. an MCP client) for visual testing.
    // Pure screen readback -- does not change window Z-order or
    // wait for anything, so call it only once the window is known
    // to be unoccluded and to have actually presented a fresh
    // frame (see RaiseToTop).
    [[nodiscard]] virtual bool CaptureScreenshotBmp(
        std::string_view path) const = 0;

    // Raises the window above other normal (non-topmost) windows
    // without taking input focus. Screen capture only sees what is
    // actually on top on screen; since presenting to an occluded
    // D3D swapchain is typically skipped by the compositor, the
    // caller must let at least one more frame render and present
    // *after* calling this before capturing, or the capture will
    // read a stale pre-occlusion frame.
    virtual void RaiseToTop() const = 0;

protected:
    Window() = default;
};

[[nodiscard]] std::unique_ptr<Window> MakeWindow(
    const WindowDesc& desc);
} // namespace orbit::platform
