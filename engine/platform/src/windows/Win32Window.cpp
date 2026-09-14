#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <orbit/core/Assert.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/platform/Window.hpp>

#include <mutex>
#include <stdexcept>
#include <string>

namespace orbit::platform
{
namespace
{
constexpr const char* kWindowClassName =
    "OrbitWindowClass";

[[nodiscard]] int ToVirtualKey(
    const Key key)
{
    switch (key)
    {
    case Key::W:
        return 'W';
    case Key::A:
        return 'A';
    case Key::S:
        return 'S';
    case Key::D:
        return 'D';
    case Key::Q:
        return 'Q';
    case Key::E:
        return 'E';
    case Key::LeftShift:
        return VK_LSHIFT;
    case Key::Escape:
        return VK_ESCAPE;
    case Key::F3:
        return VK_F3;
    case Key::F4:
        return VK_F4;
    case Key::ArrowLeft:
        return VK_LEFT;
    case Key::ArrowRight:
        return VK_RIGHT;
    case Key::Enter:
        return VK_RETURN;
    }

    throw std::invalid_argument(
        "Orbit received an invalid platform key.");
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

LRESULT CALLBACK OrbitWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(
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
            WNDCLASSEXA windowClass{};
            windowClass.cbSize =
                sizeof(windowClass);
            windowClass.style =
                CS_HREDRAW |
                CS_VREDRAW;
            windowClass.lpfnWndProc =
                OrbitWindowProc;
            windowClass.hInstance =
                GetModuleHandleA(nullptr);
            windowClass.hCursor =
                LoadCursorA(
                    nullptr,
                    IDC_ARROW);
            windowClass.lpszClassName =
                kWindowClassName;

            if (RegisterClassExA(
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

        const std::string title(
            desc.title);

        hwnd_ = CreateWindowExA(
            0,
            kWindowClassName,
            title.c_str(),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr,
            nullptr,
            GetModuleHandleA(nullptr),
            nullptr);

        if (hwnd_ == nullptr)
        {
            throw std::runtime_error(
                "Orbit failed to create a Win32 window.");
        }

        ShowWindow(
            hwnd_,
            SW_SHOW);

        UpdateWindow(hwnd_);

        log::Info(
            "Win32 window created.");
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

        while (PeekMessageA(
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

            TranslateMessage(
                &message);

            DispatchMessageA(
                &message);
        }

        UpdateRelativeMouseCapture();
        return true;
    }

    [[nodiscard]] bool KeyDown(
        const Key key) const override
    {
        if (GetForegroundWindow() !=
            hwnd_)
        {
            return false;
        }

        return (
            GetAsyncKeyState(
                ToVirtualKey(key)) &
            0x8000) != 0;
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

    HWND hwnd_{nullptr};
    mutable u32 width_{};
    mutable u32 height_{};
    bool relativeMouseMode_{false};
    bool relativeMouseCaptured_{false};
};
} // namespace

std::unique_ptr<Window> MakeWindow(
    const WindowDesc& desc)
{
    return std::make_unique<
        Win32Window>(desc);
}
} // namespace orbit::platform
