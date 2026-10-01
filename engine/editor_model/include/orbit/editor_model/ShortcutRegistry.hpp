#pragma once

#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/platform/Window.hpp>

#include <functional>
#include <vector>

namespace orbit::editor_model
{
struct ShortcutChord
{
    platform::Key key{platform::Key::Enter};
    bool control{false};
    bool shift{false};
    bool alt{false};

    [[nodiscard]] bool operator==(
        const ShortcutChord&) const noexcept = default;
};

class ShortcutRegistry
{
public:
    ShortcutRegistry() noexcept;
    ~ShortcutRegistry();

    ShortcutRegistry(const ShortcutRegistry&) = delete;
    ShortcutRegistry& operator=(const ShortcutRegistry&) = delete;

    // Orbit Studio owns one process-wide shortcut registry. Presentation
    // modules use this discovery seam to contribute bindings without threading
    // the registry through every UI constructor or creating parallel handlers.
    [[nodiscard]] static ShortcutRegistry* Active() noexcept;

    void Register(
        ShortcutChord chord,
        commands::CommandId command);

    void RegisterCallback(
        ShortcutChord chord,
        std::function<void()> callback,
        bool allowWhenKeyboardCaptured = false);

    // suppressInvocation blocks shortcuts while the UI owns the keyboard
    // (bindings registered with allowWhenKeyboardCaptured still fire).
    // blockAll blocks every shortcut, for gestures that own the keyboard, such
    // as flying the viewport camera: WASD/Q/E movement must never double as a
    // letter shortcut (on AZERTY the Q position types A). Key state is still
    // tracked, so releasing the gesture while a key is held does not fire it.
    void Update(
        platform::Window& window,
        const commands::CommandRegistry& commandRegistry,
        bool suppressInvocation,
        bool blockAll = false);

private:
    struct Binding
    {
        ShortcutChord chord;
        commands::CommandId command{};
        std::function<void()> callback;
        bool allowWhenKeyboardCaptured{false};
        bool wasDown{false};
    };

    std::vector<Binding> bindings_;
};
} // namespace orbit::editor_model
