#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <orbit/core/Assert.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/platform/AppResources.hpp>
#include <orbit/platform/Window.hpp>

#include "Win32Keyboard.hpp"

#include <mutex>
#include <stdexcept>
#include <string>

namespace orbit::platform
{
namespace
{
constexpr const wchar_t* kWindowClassName =
    L"OrbitWindowClass";

// Orbit renders directly into a Vulkan swapchain whose extent is expressed
// in physical pixels. If Windows is allowed to treat the process as
// DPI-unaware, GetClientRect()/cursor coordinates are virtualized to 96-DPI
// logical pixels while Vulkan still presents to the monitor's real pixel
// grid. The editor then sizes its embedded RenderViews from the smaller
// logical dimensions and DWM stretches the result, which is visibly soft on
// 4K/high-DPI displays.
//
// Establish PMv2 before creating any HWND so Win32 client coordinates,
// ImGui's display size and the Vulkan surface all describe the same physical
// pixel space. ERROR_ACCESS_DENIED is benign: it means a manifest or an
// earlier startup path already selected a DPI-awareness mode.
void EnsureProcessDpiAwareness()
{
    static std::once_flag once;

    std::call_once(
        once,
        []
        {
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
            if (SetProcessDpiAwarenessContext(
                    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE)
            {
                log::Info(
                    "Win32 per-monitor DPI awareness V2 enabled.");
                return;
            }

            if (GetLastError() == ERROR_ACCESS_DENIED)
            {
                log::Info(
                    "Win32 DPI awareness was already configured before window creation.");
                return;
            }
#endif

            // Compatibility fallback for older Windows SDK/runtime pairs.
            // This is not as capable as PMv2, but it still prevents the
            // DPI-unaware bitmap virtualization that causes the 4K blur.
            static_cast<void>(
                SetProcessDPIAware());
            log::Info(
                "Win32 system DPI awareness fallback enabled.");
        });
}

// The window is a Unicode (UTF-16) window: with the ANSI API, WM_CHAR delivers
// characters in the system ANSI code page, which mangles anything outside it
// (and disagrees with UTF-16 for 0x80-0x9F, for example the euro sign).
[[nodiscard]] std::wstring Widen(const std::string_view utf8)
{
    if (utf8.empty())
    {
        return {};
    }

    const int length = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);

    if (length <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
        wide.data(), length);
    return wide;
}

[[nodiscard]] bool WriteWindowBmp(
    const HWND hwnd,
    const std::string_view path)
{
    if (hwnd == nullptr ||
        IsWindow(hwnd) == FALSE)
    {
        return false;
    }

    RECT clientRect{};

    if (GetClientRect(
            hwnd,
            &clientRect) == FALSE)
    {
        return false;
    }

    const LONG width =
        clientRect.right -
        clientRect.left;

    const LONG height =
        clientRect.bottom -
        clientRect.top;

    if (width <= 0 ||
        height <= 0)
    {
        return false;
    }

    // Orbit presents through a Vulkan swapchain, which never draws
    // through GDI -- BitBlt'ing from GetDC(hwnd) reads
    // whatever stale/foreign content happens to sit in the window's
    // own (unused) GDI surface, not what's on screen. Capturing from
    // the desktop DC at the window's screen position instead reads
    // the real, DWM-composited pixels.
    POINT topLeft{
        clientRect.left,
        clientRect.top
    };

    if (ClientToScreen(
            hwnd,
            &topLeft) == FALSE)
    {
        return false;
    }

    const HDC windowDc =
        GetDC(nullptr);

    if (windowDc == nullptr)
    {
        return false;
    }

    const HDC memoryDc =
        CreateCompatibleDC(
            windowDc);

    const HBITMAP bitmap =
        CreateCompatibleBitmap(
            windowDc,
            width,
            height);

    bool succeeded = false;

    if (memoryDc != nullptr &&
        bitmap != nullptr)
    {
        const HGDIOBJ previousObject =
            SelectObject(
                memoryDc,
                bitmap);

        if (BitBlt(
                memoryDc,
                0,
                0,
                width,
                height,
                windowDc,
                topLeft.x,
                topLeft.y,
                SRCCOPY) != FALSE)
        {
            BITMAPINFOHEADER infoHeader{};
            infoHeader.biSize =
                sizeof(infoHeader);
            infoHeader.biWidth = width;
            infoHeader.biHeight = height;
            infoHeader.biPlanes = 1;
            infoHeader.biBitCount = 24;
            infoHeader.biCompression = BI_RGB;

            const DWORD rowStrideBytes =
                (static_cast<DWORD>(
                     width) *
                     3U +
                 3U) &
                ~3U;

            const DWORD pixelBytes =
                rowStrideBytes *
                static_cast<DWORD>(
                    height);

            std::string pixels(
                pixelBytes,
                '\0');

            if (GetDIBits(
                    memoryDc,
                    bitmap,
                    0,
                    static_cast<UINT>(
                        height),
                    pixels.data(),
                    reinterpret_cast<
                        BITMAPINFO*>(
                        &infoHeader),
                    DIB_RGB_COLORS) != 0)
            {
                BITMAPFILEHEADER
                    fileHeader{};
                fileHeader.bfType =
                    0x4D42;
                fileHeader.bfOffBits =
                    sizeof(fileHeader) +
                    sizeof(infoHeader);
                fileHeader.bfSize =
                    fileHeader.bfOffBits +
                    pixelBytes;

                const HANDLE file =
                    CreateFileA(
                        std::string(
                            path)
                            .c_str(),
                        GENERIC_WRITE,
                        0,
                        nullptr,
                        CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL,
                        nullptr);

                if (file !=
                    INVALID_HANDLE_VALUE)
                {
                    DWORD written = 0;

                    const bool wroteFileHeader =
                        WriteFile(
                            file,
                            &fileHeader,
                            sizeof(
                                fileHeader),
                            &written,
                            nullptr) !=
                        FALSE;

                    const bool wroteInfoHeader =
                        WriteFile(
                            file,
                            &infoHeader,
                            sizeof(
                                infoHeader),
                            &written,
                            nullptr) !=
                        FALSE;

                    const bool wrotePixels =
                        WriteFile(
                            file,
                            pixels.data(),
                            pixelBytes,
                            &written,
                            nullptr) !=
                        FALSE;

                    succeeded =
                        wroteFileHeader &&
                        wroteInfoHeader &&
                        wrotePixels;

                    CloseHandle(file);
                }
            }
        }

        SelectObject(
            memoryDc,
            previousObject);
    }

    if (bitmap != nullptr)
    {
        DeleteObject(bitmap);
    }

    if (memoryDc != nullptr)
    {
        DeleteDC(memoryDc);
    }

    ReleaseDC(nullptr, windowDc);

    return succeeded;
}

struct WindowEventState
{
    f32 wheelDelta{0.0F};
    std::u16string textInput;
};

LRESULT CALLBACK OrbitWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        const auto* create =
            reinterpret_cast<
                const CREATESTRUCTW*>(
                    lParam);

        SetWindowLongPtrW(
            hwnd,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(
                create->lpCreateParams));
    }

    auto* events =
        reinterpret_cast<WindowEventState*>(
            GetWindowLongPtrW(
                hwnd,
                GWLP_USERDATA));

    switch (message)
    {
    case WM_MOUSEWHEEL:
        if (events != nullptr)
        {
            events->wheelDelta +=
                static_cast<f32>(
                    GET_WHEEL_DELTA_WPARAM(
                        wParam)) /
                static_cast<f32>(
                    WHEEL_DELTA);
        }
        return 0;

    case WM_CHAR:
        if (events != nullptr &&
            wParam <= 0xFFFFU)
        {
            events->textInput.push_back(
                static_cast<char16_t>(
                    wParam));
        }
        return 0;

    case WM_DPICHANGED:
    {
        // PMv2 supplies a monitor-appropriate outer rect in lParam. Applying
        // it keeps the client area's physical-pixel size coherent when the
        // window crosses monitors with different scale factors. The regular
        // Width()/Height() polling then drives the Vulkan swapchain resize on
        // the next frame.
        const auto* suggested =
            reinterpret_cast<const RECT*>(
                lParam);

        if (suggested != nullptr)
        {
            SetWindowPos(
                hwnd,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right -
                    suggested->left,
                suggested->bottom -
                    suggested->top,
                SWP_NOZORDER |
                    SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(
            hwnd,
            message,
            wParam,
            lParam);
    }
}

void EnsureWindowClassRegistered()
{
    static std::once_flag once;

    std::call_once(
        once,
        []
        {
            WNDCLASSEXW windowClass{};
            windowClass.cbSize =
                sizeof(windowClass);
            windowClass.style =
                CS_HREDRAW |
                CS_VREDRAW;
            windowClass.lpfnWndProc =
                OrbitWindowProc;
            windowClass.hInstance =
                GetModuleHandleW(nullptr);
            windowClass.hCursor =
                LoadCursorW(
                    nullptr,
                    MAKEINTRESOURCEW(32512)); // IDC_ARROW
            windowClass.lpszClassName =
                kWindowClassName;

            windowClass.hIcon =
                LoadIconW(
                    windowClass.hInstance,
                    MAKEINTRESOURCEW(
                        ORBIT_RESOURCE_ICON));

            windowClass.hIconSm =
                windowClass.hIcon;

            if (RegisterClassExW(
                    &windowClass) == 0)
            {
                throw std::runtime_error(
                    "Orbit failed to register the Win32 window class.");
            }
        });
}

class Win32Window final : public Window
{
public:
    explicit Win32Window(
        const WindowDesc& desc)
        : width_(desc.width),
          height_(desc.height)
    {
        ORBIT_ASSERT(width_ > 0);
        ORBIT_ASSERT(height_ > 0);

        EnsureWindowClassRegistered();

        DWORD windowStyle =
            WS_OVERLAPPEDWINDOW;
        int windowX =
            CW_USEDEFAULT;
        int windowY =
            CW_USEDEFAULT;
        int windowWidth =
            static_cast<int>(width_);
        int windowHeight =
            static_cast<int>(height_);

        if (desc.startMaximized)
        {
            // Orbit Studio requests startMaximized today. Treat that startup
            // mode as borderless fullscreen: cover the primary monitor's full
            // physical pixel rectangle without changing the desktop display
            // mode. This keeps Vulkan/DWM/Alt-Tab behavior predictable while
            // giving Studio true edge-to-edge native-resolution output.
            MONITORINFO monitorInfo{};
            monitorInfo.cbSize =
                sizeof(monitorInfo);

            const POINT primaryPoint{
                0,
                0
            };

            const HMONITOR monitor =
                MonitorFromPoint(
                    primaryPoint,
                    MONITOR_DEFAULTTOPRIMARY);

            if (monitor != nullptr &&
                GetMonitorInfoA(
                    monitor,
                    &monitorInfo) != FALSE)
            {
                windowStyle =
                    WS_POPUP;
                windowX =
                    monitorInfo.rcMonitor.left;
                windowY =
                    monitorInfo.rcMonitor.top;
                windowWidth =
                    monitorInfo.rcMonitor.right -
                    monitorInfo.rcMonitor.left;
                windowHeight =
                    monitorInfo.rcMonitor.bottom -
                    monitorInfo.rcMonitor.top;

                width_ =
                    static_cast<u32>(
                        windowWidth);
                height_ =
                    static_cast<u32>(
                        windowHeight);
            }
            else
            {
                // If monitor discovery ever fails, retain the old maximized
                // window behavior rather than failing Studio startup.
                RECT rect{
                    0,
                    0,
                    static_cast<LONG>(width_),
                    static_cast<LONG>(height_)
                };

                AdjustWindowRect(
                    &rect,
                    WS_OVERLAPPEDWINDOW,
                    FALSE);

                windowWidth =
                    rect.right - rect.left;
                windowHeight =
                    rect.bottom - rect.top;
            }
        }
        else
        {
            RECT rect{
                0,
                0,
                static_cast<LONG>(width_),
                static_cast<LONG>(height_)
            };

            AdjustWindowRect(
                &rect,
                windowStyle,
                FALSE);

            windowWidth =
                rect.right - rect.left;
            windowHeight =
                rect.bottom - rect.top;
        }

        const std::wstring title =
            Widen(desc.title);

        hwnd_ = CreateWindowExW(
            0,
            kWindowClassName,
            title.c_str(),
            windowStyle,
            windowX,
            windowY,
            windowWidth,
            windowHeight,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            &eventState_);

        if (hwnd_ == nullptr)
        {
            throw std::runtime_error(
                "Orbit failed to create a Win32 window.");
        }

        if (desc.startMaximized &&
            windowStyle != WS_POPUP)
        {
            ShowWindow(
                hwnd_,
                SW_SHOWMAXIMIZED);
        }
        else
        {
            ShowWindow(
                hwnd_,
                SW_SHOW);
        }

        UpdateWindow(hwnd_);

        log::Info(
            windowStyle == WS_POPUP
                ? "Win32 borderless fullscreen window created."
                : "Win32 window created.");
    }

    ~Win32Window() override
    {
        SetRelativeMouseMode(false);

        if (hwnd_ != nullptr &&
            IsWindow(hwnd_) != FALSE)
        {
            DestroyWindow(hwnd_);
        }
    }

    bool PumpEvents() override
    {
        MSG message{};

        while (PeekMessageW(
                   &message,
                   nullptr,
                   0,
                   0,
                   PM_REMOVE) != FALSE)
        {
            if (message.message ==
                WM_QUIT)
            {
                return false;
            }

            if ((message.message >= WM_KEYFIRST &&
                 message.message <= WM_KEYLAST) ||
                (message.message >= WM_MOUSEFIRST &&
                 message.message <= WM_MOUSELAST) ||
                message.message == WM_INPUT ||
                message.message == WM_SIZE ||
                message.message == WM_SETFOCUS ||
                message.message == WM_KILLFOCUS ||
                message.message == WM_DPICHANGED)
            {
                inputActivity_ = true;
            }

            TranslateMessage(
                &message);

            DispatchMessageW(
                &message);
        }

        UpdateRelativeMouseCapture();
        return true;
    }

    [[nodiscard]] bool ConsumeInputActivity() override
    {
        // Held buttons keep the editor interactive even when no new message
        // arrives (for example a right-drag in relative mouse mode).
        const bool buttonHeld =
            (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
            (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;

        const bool activity = inputActivity_ || buttonHeld;
        inputActivity_ = false;
        return activity;
    }

    void WaitForActivity(const u32 milliseconds) override
    {
        if (milliseconds == 0U)
        {
            return;
        }

        MsgWaitForMultipleObjectsEx(
            0,
            nullptr,
            milliseconds,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);
    }

    void WakeForActivity() override
    {
        if (hwnd_ != nullptr)
        {
            PostMessageW(hwnd_, WM_NULL, 0, 0);
        }
    }

    [[nodiscard]] bool KeyDown(
        const Key key) const override
    {
        if (GetForegroundWindow() !=
            hwnd_)
        {
            return false;
        }

        const DWORD windowThreadId =
            GetWindowThreadProcessId(
                hwnd_,
                nullptr);

        const HKL keyboardLayout =
            GetKeyboardLayout(
                windowThreadId);

        return (
            GetAsyncKeyState(
                VirtualKeyFor(
                    key,
                    keyboardLayout)) &
            0x8000) != 0;
    }

    [[nodiscard]] bool
    MouseButtonDown(
        const MouseButton button) const override
    {
        if (GetForegroundWindow() !=
            hwnd_)
        {
            return false;
        }

        int virtualKey = 0;

        switch (button)
        {
        case MouseButton::Left:
            virtualKey = VK_LBUTTON;
            break;
        case MouseButton::Right:
            virtualKey = VK_RBUTTON;
            break;
        case MouseButton::Middle:
            virtualKey = VK_MBUTTON;
            break;
        }

        return (
            GetAsyncKeyState(
                virtualKey) &
            0x8000) != 0;
    }

    [[nodiscard]] math::Double2
    CursorPositionPixels() const override
    {
        POINT cursor{};

        if (GetCursorPos(
                &cursor) == FALSE)
        {
            return {};
        }

        if (ScreenToClient(
                hwnd_,
                &cursor) == FALSE)
        {
            return {};
        }

        return {
            static_cast<f64>(
                cursor.x),
            static_cast<f64>(
                cursor.y)
        };
    }

    void SetTitle(
        const std::string_view title) override
    {
        if (hwnd_ != nullptr)
        {
            SetWindowTextW(
                hwnd_,
                Widen(title).c_str());
        }
    }

    void SetRelativeMouseMode(
        const bool enabled) override
    {
        relativeMouseMode_ =
            enabled;

        UpdateRelativeMouseCapture();
    }

    [[nodiscard]] bool
    RelativeMouseMode() const noexcept override
    {
        return relativeMouseMode_;
    }

    [[nodiscard]] MouseDelta
    ConsumeMouseDelta() override
    {
        UpdateRelativeMouseCapture();

        if (!relativeMouseCaptured_)
        {
            return {};
        }

        RECT clientRect{};

        if (GetClientRect(
                hwnd_,
                &clientRect) == FALSE)
        {
            return {};
        }

        POINT center{
            (clientRect.right -
             clientRect.left) /
                2,
            (clientRect.bottom -
             clientRect.top) /
                2
        };

        if (ClientToScreen(
                hwnd_,
                &center) == FALSE)
        {
            return {};
        }

        POINT cursor{};

        if (GetCursorPos(
                &cursor) == FALSE)
        {
            return {};
        }

        const MouseDelta delta{
            .x =
                static_cast<i32>(
                    cursor.x -
                    center.x),
            .y =
                static_cast<i32>(
                    cursor.y -
                    center.y)
        };

        CenterAndClipCursor();

        return delta;
    }

    [[nodiscard]] f32
    ConsumeMouseWheelDelta() override
    {
        const f32 result =
            eventState_.wheelDelta;
        eventState_.wheelDelta = 0.0F;
        return result;
    }

    [[nodiscard]] std::u16string
    ConsumeTextInputUtf16() override
    {
        std::u16string result =
            std::move(
                eventState_.textInput);
        eventState_.textInput.clear();
        return result;
    }

    [[nodiscard]] void*
    NativeHandle() const override
    {
        return hwnd_;
    }

    [[nodiscard]] u32
    Width() const override
    {
        RefreshCachedSize();
        return width_;
    }

    [[nodiscard]] u32
    Height() const override
    {
        RefreshCachedSize();
        return height_;
    }

    [[nodiscard]] f32
    DpiScale() const override
    {
        // Per-monitor: reads the DPI of whichever monitor the window
        // currently sits on, not just the one it was created on.
        const UINT dpi = GetDpiForWindow(hwnd_);
        return dpi > 0
            ? static_cast<f32>(dpi) /
                  static_cast<f32>(USER_DEFAULT_SCREEN_DPI)
            : 1.0F;
    }

    [[nodiscard]] bool
    CaptureScreenshotBmp(
        const std::string_view path)
        const override
    {
        return WriteWindowBmp(
            hwnd_,
            path);
    }

    void RaiseToTop() const override
    {
        // Screen-capture reads real desktop pixels, so anything
        // this window doesn't sit above on screen wins the shot.
        // SetForegroundWindow cannot help here -- Windows' foreground
        // lock timeout blocks it even for a window's own owning
        // process when that process didn't just receive user input.
        // Raising the Z-order does not need that permission, so use
        // that instead: briefly toggle topmost to guarantee it beats
        // even another already-topmost window, then release
        // topmost so it behaves like a normal window afterwards.
        //
        // This cannot itself force a fresh frame onto the screen --
        // presenting to an occluded swapchain is typically skipped
        // by the compositor -- so the caller must let the render
        // loop actually draw and present at least once more (from a
        // *later* frame, not synchronously here: blocking this
        // thread would block that same render loop) before it reads
        // pixels back.
        if (IsIconic(hwnd_) != FALSE)
        {
            ShowWindow(
                hwnd_,
                SW_RESTORE);
        }

        SetWindowPos(
            hwnd_,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE |
                SWP_NOSIZE |
                SWP_NOACTIVATE);

        SetWindowPos(
            hwnd_,
            HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE |
                SWP_NOSIZE |
                SWP_NOACTIVATE);
    }

private:
    // Width()/Height() must reflect the window's *current* client
    // size (not the size it was created with) so Main.cpp can detect
    // a resize by comparing them against the swapchain's own cached
    // size each frame -- there is no WM_SIZE handling to push updates
    // the other way, so this pulls them live instead.
    void RefreshCachedSize() const
    {
        if (hwnd_ == nullptr ||
            IsWindow(hwnd_) == FALSE)
        {
            return;
        }

        RECT clientRect{};

        if (GetClientRect(
                hwnd_,
                &clientRect) == FALSE)
        {
            return;
        }

        width_ = static_cast<u32>(
            clientRect.right -
            clientRect.left);

        height_ = static_cast<u32>(
            clientRect.bottom -
            clientRect.top);
    }

    void UpdateRelativeMouseCapture()
    {
        const bool shouldCapture =
            relativeMouseMode_ &&
            hwnd_ != nullptr &&
            IsWindow(hwnd_) != FALSE &&
            GetForegroundWindow() ==
                hwnd_;

        if (shouldCapture ==
            relativeMouseCaptured_)
        {
            return;
        }

        relativeMouseCaptured_ =
            shouldCapture;

        if (relativeMouseCaptured_)
        {
            while (ShowCursor(FALSE) >= 0)
            {
            }

            CenterAndClipCursor();
        }
        else
        {
            ClipCursor(nullptr);

            while (ShowCursor(TRUE) < 0)
            {
            }
        }
    }

    void CenterAndClipCursor()
    {
        if (hwnd_ == nullptr ||
            IsWindow(hwnd_) == FALSE)
        {
            return;
        }

        RECT clientRect{};

        if (GetClientRect(
                hwnd_,
                &clientRect) == FALSE)
        {
            return;
        }

        POINT topLeft{
            clientRect.left,
            clientRect.top
        };

        POINT bottomRight{
            clientRect.right,
            clientRect.bottom
        };

        if (ClientToScreen(
                hwnd_,
                &topLeft) == FALSE ||
            ClientToScreen(
                hwnd_,
                &bottomRight) == FALSE)
        {
            return;
        }

        const RECT clipRect{
            topLeft.x,
            topLeft.y,
            bottomRight.x,
            bottomRight.y
        };

        static_cast<void>(
            ClipCursor(
                &clipRect));

        const int centerX =
            topLeft.x +
            (bottomRight.x -
             topLeft.x) /
                2;

        const int centerY =
            topLeft.y +
            (bottomRight.y -
             topLeft.y) /
                2;

        static_cast<void>(
            SetCursorPos(
                centerX,
                centerY));
    }

    WindowEventState eventState_{};
    HWND hwnd_{nullptr};
    mutable u32 width_{};
    mutable u32 height_{};
    bool relativeMouseMode_{false};
    bool inputActivity_{true};
    bool relativeMouseCaptured_{false};
};
} // namespace

std::unique_ptr<Window> MakeWindow(
    const WindowDesc& desc)
{
    EnsureProcessDpiAwareness();

    return std::make_unique<
        Win32Window>(desc);
}
} // namespace orbit::platform
