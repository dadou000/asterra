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
constexpr const char* kWindowClassName = "OrbitWindowClass";

LRESULT CALLBACK OrbitWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
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
        return DefWindowProcA(hwnd, message, wParam, lParam);
    }
}

void EnsureWindowClassRegistered()
{
    static std::once_flag once;
    std::call_once(once, []
    {
        WNDCLASSEXA windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = OrbitWindowProc;
        windowClass.hInstance = GetModuleHandleA(nullptr);
        windowClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kWindowClassName;

        if (RegisterClassExA(&windowClass) == 0)
        {
            throw std::runtime_error("Orbit failed to register the Win32 window class.");
        }
    });
}

class Win32Window final : public Window
{
public:
    explicit Win32Window(const WindowDesc& desc)
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

        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

        const std::string title(desc.title);
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
            nullptr
        );

        if (hwnd_ == nullptr)
        {
            throw std::runtime_error("Orbit failed to create a Win32 window.");
        }

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        log::Info("Win32 window created.");
    }

    ~Win32Window() override
    {
        if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE)
        {
            DestroyWindow(hwnd_);
        }
    }

    bool PumpEvents() override
    {
        MSG message{};

        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
        {
            if (message.message == WM_QUIT)
            {
                return false;
            }

            TranslateMessage(&message);
            DispatchMessageA(&message);
        }

        return true;
    }

    [[nodiscard]] void* NativeHandle() const override
    {
        return hwnd_;
    }

    [[nodiscard]] u32 Width() const override
    {
        return width_;
    }

    [[nodiscard]] u32 Height() const override
    {
        return height_;
    }

private:
    HWND hwnd_{nullptr};
    u32 width_{};
    u32 height_{};
};
} // namespace

std::unique_ptr<Window> CreateWindow(const WindowDesc& desc)
{
    return std::make_unique<Win32Window>(desc);
}
} // namespace orbit::platform
