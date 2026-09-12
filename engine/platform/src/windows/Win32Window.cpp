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
    }

    throw std::invalid_argument(
        "Orbit received an invalid platform key.");
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
        return width_;
    }

    [[nodiscard]] u32
    Height() const override
    {
        return height_;
    }

private:
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
    u32 width_{};
    u32 height_{};
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
