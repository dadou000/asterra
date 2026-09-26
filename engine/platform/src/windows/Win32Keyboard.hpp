#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <orbit/platform/Window.hpp>

// Translation of Orbit's layout-independent Key names to Win32 virtual keys for
// a given keyboard layout. Split out of Win32Window so it can be tested against
// real layouts (QWERTY, AZERTY, QWERTZ) without a window.
namespace orbit::platform
{
// True for the movement cluster (W A S D Q E): these name a *physical position*
// on the keyboard (the QWERTY reference layout), so they follow the player's
// hands on every layout. Every other letter key names the *letter*.
[[nodiscard]] bool IsPositionalKey(Key key) noexcept;

// The virtual-key code that `key` currently corresponds to under `layout`.
// Throws std::invalid_argument for an unknown Key.
[[nodiscard]] int VirtualKeyFor(Key key, HKL layout);
} // namespace orbit::platform
