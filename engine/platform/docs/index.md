+++
path = "/foundation/platform"
title = "Platform layer (window, input, dialogs, crash handler)"
kind = "subsystem"
status = "stable"
summary = "The native OS layer: Window (keyboard, mouse, text, DPI, idle wake), file dialogs and the file browser, the per-user writable data root, crash handler and shared Win32 resource IDs."
owner_module = "OrbitPlatform"
keywords = ["platform", "window", "keyboard", "input", "dpi", "file dialog", "crash handler", "layout", "azerty", "altgr", "paths"]
sources = [
  "engine/platform/include/orbit/platform/AppResources.hpp",
  "engine/platform/include/orbit/platform/CrashHandler.hpp",
  "engine/platform/include/orbit/platform/FileBrowser.hpp",
  "engine/platform/include/orbit/platform/FileDialog.hpp",
  "engine/platform/include/orbit/platform/Paths.hpp",
  "engine/platform/include/orbit/platform/Window.hpp",
  "engine/platform/CMakeLists.txt",
]
symbols = ["CrashHandlerConfig", "FolderDialogOptions", "MouseDelta"]
invariants = [
  "Letter keys come in two kinds on purpose: W A S D Q E are positions (the QWERTY reference cluster) for movement and stay under the player's fingers on AZERTY, QWERTZ and Dvorak; other letters (C G L M P V X Y Z and LetterA) name the letter the key types on the active layout, for mnemonic and text-editing shortcuts.",
  "The key that means 'A' for text shortcuts (select all) is LetterA; A is the movement position.",
  "AltGr arrives from Windows as Left-Ctrl plus Right-Alt; text input must see it to accept characters typed through AltGr on layouts that have it.",
  "Text input is delivered separately (UTF-16 from the native message stream) from key state, so IME and text never need reconstructing from virtual keys.",
  "Width()/Height() are physical pixels; the DPI scale exists only for UI layers that draw at a fixed logical size.",
  "The per-user data root resolves beneath LOCALAPPDATA on Windows and never points inside a source project.",
  "Window can sleep up to a time but wakes immediately when any window message arrives, so a throttled loop still reacts to input at once.",
]
related = ["/rules/ui", "/foundation/runtime-session"]
depends_on = ["/foundation/core", "/foundation/math"]
used_by = ["/foundation/runtime-session"]
verify = [
  "ctest -R Orbit.KeyboardLayout",
]
verified = "b0a0de7f"
+++


