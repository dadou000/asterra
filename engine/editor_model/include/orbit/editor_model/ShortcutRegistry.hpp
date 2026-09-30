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

    void Update(
        platform::Window& window,
        const commands::CommandRegistry& commandRegistry,
        bool suppressInvocation);

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
