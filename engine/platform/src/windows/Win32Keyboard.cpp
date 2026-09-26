#include "Win32Keyboard.hpp"

#include <stdexcept>

namespace orbit::platform
{
namespace
{
// Scan codes (set 1 make codes) of the movement cluster on the QWERTY
// reference layout. A scan code identifies a physical key, so translating it
// through the active layout yields whatever that key types there: the key at
// QWERTY's W position is Z on AZERTY, and so on.
[[nodiscard]] UINT PositionalScanCode(const Key key) noexcept
{
    switch (key)
    {
    case Key::Q:
        return 0x10;
    case Key::W:
        return 0x11;
    case Key::E:
        return 0x12;
    case Key::A:
        return 0x1E;
    case Key::S:
        return 0x1F;
    case Key::D:
        return 0x20;
    default:
        return 0;
    }
}

// The letter a key names (used for the letter keys and as the fallback for
// positional ones).
[[nodiscard]] wchar_t LogicalCharacter(const Key key) noexcept
{
    switch (key)
    {
    case Key::W:
        return L'W';
    case Key::A:
    case Key::LetterA:
        return L'A';
    case Key::S:
        return L'S';
    case Key::D:
        return L'D';
    case Key::Q:
        return L'Q';
    case Key::E:
        return L'E';
    case Key::C:
        return L'C';
    case Key::G:
        return L'G';
    case Key::L:
        return L'L';
    case Key::M:
        return L'M';
    case Key::V:
        return L'V';
    case Key::X:
        return L'X';
    case Key::Y:
        return L'Y';
    case Key::Z:
        return L'Z';
    default:
        return L'\0';
    }
}
} // namespace

bool IsPositionalKey(const Key key) noexcept
{
    return PositionalScanCode(key) != 0U;
}

int VirtualKeyFor(const Key key, const HKL layout)
{
    if (const UINT scan = PositionalScanCode(key); scan != 0U)
    {
        // Windows' virtual-key codes for letters name the letter printed on the
        // key, NOT the physical position, so VkKeyScan('W') on AZERTY is the
        // key labelled W (bottom-left), not where a WASD player's finger is.
        // Going through the scan code keeps the physical position.
        if (const UINT mapped =
                MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK, layout);
            mapped != 0U)
        {
            return static_cast<int>(mapped);
        }

        return static_cast<int>(LogicalCharacter(key));
    }

    if (const wchar_t character = LogicalCharacter(key);
        character != L'\0')
    {
        // A letter key: resolve the character through the active layout, so
        // the Z of "Ctrl+Z" is the key that types Z (Y on QWERTZ is a
        // different key from Z). Only the low byte is a virtual key; any
        // Shift/AltGr state is a text-input concern.
        const SHORT mapped = VkKeyScanExW(character, layout);

        if (mapped != -1)
        {
            return LOBYTE(mapped);
        }

        return static_cast<int>(character);
    }

    switch (key)
    {
    case Key::LeftShift:
        return VK_LSHIFT;
    case Key::LeftControl:
        return VK_LCONTROL;
    case Key::LeftAlt:
        return VK_LMENU;
    case Key::RightAlt:
        // AltGr. On layouts that have it, Windows reports AltGr as Left-Ctrl
        // plus Right-Alt, so this is how a text field learns that Ctrl is not
        // a shortcut modifier here.
        return VK_RMENU;
    case Key::Escape:
        return VK_ESCAPE;
    case Key::Tab:
        return VK_TAB;
    case Key::Enter:
        return VK_RETURN;
    case Key::Space:
        return VK_SPACE;
    case Key::Backspace:
        return VK_BACK;
    case Key::Delete:
        return VK_DELETE;
    case Key::Insert:
        return VK_INSERT;
    case Key::Home:
        return VK_HOME;
    case Key::End:
        return VK_END;
    case Key::PageUp:
        return VK_PRIOR;
    case Key::PageDown:
        return VK_NEXT;
    case Key::F2:
        return VK_F2;
    case Key::F3:
        return VK_F3;
    case Key::F4:
        return VK_F4;
    case Key::ArrowLeft:
        return VK_LEFT;
    case Key::ArrowRight:
        return VK_RIGHT;
    case Key::ArrowUp:
        return VK_UP;
    case Key::ArrowDown:
        return VK_DOWN;
    default:
        break;
    }

    throw std::invalid_argument("Orbit received an invalid platform key.");
}
} // namespace orbit::platform
