#include "Win32Keyboard.hpp"

#include <cstdlib>
#include <iostream>
#include <source_location>

namespace
{
using orbit::platform::Key;

void Check(
    const bool condition,
    const std::source_location location = std::source_location::current())
{
    if (!condition)
    {
        std::cerr << "Keyboard layout test failed at " << location.file_name()
                  << ':' << location.line() << '\n';
        std::exit(1);
    }
}

[[nodiscard]] int Vk(const Key key, const HKL layout)
{
    return orbit::platform::VirtualKeyFor(key, layout);
}

// Loads a system keyboard layout without activating it for this process.
[[nodiscard]] HKL Load(const wchar_t* klid)
{
    return LoadKeyboardLayoutW(klid, KLF_NOTELLSHELL);
}
} // namespace

int main()
{
    const HKL us = Load(L"00000409");
    const HKL french = Load(L"0000040C");
    const HKL german = Load(L"00000407");

    if (us == nullptr)
    {
        std::cerr << "US layout unavailable; nothing to test.\n";
        return 1;
    }

    // Which keys are positions and which are letters.
    for (const Key key : {Key::W, Key::A, Key::S, Key::D, Key::Q, Key::E})
    {
        Check(orbit::platform::IsPositionalKey(key));
    }
    for (const Key key : {Key::C, Key::G, Key::L, Key::M, Key::V, Key::X,
                          Key::Y, Key::Z, Key::LetterA, Key::Space})
    {
        Check(!orbit::platform::IsPositionalKey(key));
    }

    // QWERTY: positions and letters coincide.
    Check(Vk(Key::W, us) == 'W');
    Check(Vk(Key::A, us) == 'A');
    Check(Vk(Key::S, us) == 'S');
    Check(Vk(Key::D, us) == 'D');
    Check(Vk(Key::Q, us) == 'Q');
    Check(Vk(Key::E, us) == 'E');
    Check(Vk(Key::LetterA, us) == 'A');
    Check(Vk(Key::Z, us) == 'Z');

    if (french != nullptr)
    {
        // AZERTY: the movement cluster follows the physical positions, so it
        // is ZQSD under the fingers; the shortcut letters follow the letters.
        Check(Vk(Key::W, french) == 'Z');
        Check(Vk(Key::A, french) == 'Q');
        Check(Vk(Key::S, french) == 'S');
        Check(Vk(Key::D, french) == 'D');
        Check(Vk(Key::Q, french) == 'A');
        Check(Vk(Key::E, french) == 'E');

        // Ctrl+A / Ctrl+Z are the keys that type A and Z, not the positions.
        Check(Vk(Key::LetterA, french) == 'A');
        Check(Vk(Key::Z, french) == 'Z');
        Check(Vk(Key::C, french) == 'C');
        Check(Vk(Key::M, french) == 'M');

        // The two meanings of "A" really are different physical keys here.
        Check(Vk(Key::A, french) != Vk(Key::LetterA, french));
    }
    else
    {
        std::cerr << "French layout unavailable; AZERTY checks skipped.\n";
    }

    if (german != nullptr)
    {
        // QWERTZ swaps Y and Z: the letter keys must follow the letters.
        Check(Vk(Key::Z, german) == 'Z');
        Check(Vk(Key::Y, german) == 'Y');
        Check(Vk(Key::Z, german) != Vk(Key::Z, us) ||
              Vk(Key::Y, german) == 'Y');
        // The movement cluster is unchanged on QWERTZ.
        Check(Vk(Key::W, german) == 'W');
        Check(Vk(Key::A, german) == 'A');
    }
    else
    {
        std::cerr << "German layout unavailable; QWERTZ checks skipped.\n";
    }

    // Non-letter keys and the modifiers, including AltGr (Right Alt), which is
    // what lets a text field accept { } [ ] | \ @ # ~ on AZERTY/QWERTZ.
    Check(Vk(Key::LeftAlt, us) == VK_LMENU);
    Check(Vk(Key::RightAlt, us) == VK_RMENU);
    Check(Vk(Key::LeftControl, us) == VK_LCONTROL);
    Check(Vk(Key::Enter, us) == VK_RETURN);
    Check(Vk(Key::ArrowLeft, us) == VK_LEFT);

    bool rejected = false;
    try
    {
        (void)orbit::platform::VirtualKeyFor(static_cast<Key>(200), us);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    Check(rejected);

    return 0;
}
