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
    Escape
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

    [[nodiscard]] virtual void* NativeHandle() const = 0;
    [[nodiscard]] virtual u32 Width() const = 0;
    [[nodiscard]] virtual u32 Height() const = 0;

protected:
    Window() = default;
};

[[nodiscard]] std::unique_ptr<Window> MakeWindow(
    const WindowDesc& desc);
} // namespace orbit::platform
