#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace orbit::platform
{
// Letter keys come in two kinds, on purpose:
//  - W A S D Q E are *positions* (the QWERTY reference cluster), for movement.
//    They stay under the player's fingers on AZERTY, QWERTZ, Dvorak...
//  - C G L M V X Y Z (and LetterA) name the *letter* that key types on the
//    active layout, for mnemonic and text-editing shortcuts (Ctrl+Z is the key
//    labelled Z, wherever it is).
// `A` is the movement position; text shortcuts that mean the letter A (select
// all) use LetterA.
enum class Key : u8
{
    W,
    A,
    S,
    D,
    Q,
    E,
    C,
    G,
    L,
    M,
    V,
    X,
    Y,
    Z,
    LeftShift,
    LeftControl,
    LeftAlt,
    Escape,
    Tab,
    Enter,
    Space,
    Backspace,
    Delete,
    Insert,
    Home,
    End,
    PageUp,
    PageDown,
    F2,
    F3,
    F4,
    ArrowLeft,
    ArrowRight,
    ArrowUp,
    ArrowDown,
    // The key that types the letter A on the active layout (Ctrl+A).
    LetterA,
    // Right Alt / AltGr. On layouts with AltGr (French, German, ...) it is how
    // braces, brackets, pipe, backslash, at, hash and tilde are typed, and
    // Windows reports it as Left-Ctrl plus Right-Alt, so text input must see it
    // to accept those characters.
    RightAlt
};

enum class MouseButton : u8
{
    Left,
    Right,
    Middle
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
    bool startMaximized{false};
};

class Window
{
public:
    virtual ~Window() = default;

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    virtual bool PumpEvents() = 0;

    virtual void SetTitle(std::string_view title) = 0;

    [[nodiscard]] virtual bool KeyDown(
        Key key) const = 0;

    [[nodiscard]] virtual bool MouseButtonDown(
        MouseButton button) const = 0;

    virtual void SetRelativeMouseMode(
        bool enabled) = 0;

    [[nodiscard]] virtual bool RelativeMouseMode() const noexcept = 0;

    [[nodiscard]] virtual MouseDelta ConsumeMouseDelta() = 0;

    // Accumulated wheel movement since the previous consume, measured
    // in conventional 120-delta wheel notches.
    [[nodiscard]] virtual f32 ConsumeMouseWheelDelta() = 0;

    // UTF-16 text entered through the native window message stream since
    // the previous consume. Kept separate from key state so IME/text input
    // never has to be reconstructed from virtual keys.
    [[nodiscard]] virtual std::u16string ConsumeTextInputUtf16() = 0;

    [[nodiscard]] bool LeftMouseButtonDown() const
    {
        return MouseButtonDown(
            MouseButton::Left);
    }

    [[nodiscard]] virtual math::Double2 CursorPositionPixels() const = 0;

    [[nodiscard]] virtual void* NativeHandle() const = 0;
    [[nodiscard]] virtual u32 Width() const = 0;
    [[nodiscard]] virtual u32 Height() const = 0;

    [[nodiscard]] virtual bool CaptureScreenshotBmp(
        std::string_view path) const = 0;

    virtual void RaiseToTop() const = 0;

protected:
    Window() = default;
};

[[nodiscard]] std::unique_ptr<Window> MakeWindow(
    const WindowDesc& desc);
} // namespace orbit::platform
