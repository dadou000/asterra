#pragma once

#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/platform/Window.hpp>

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
    void Register(
        ShortcutChord chord,
        commands::CommandId command);

    void Update(
        platform::Window& window,
        const commands::CommandRegistry& commandRegistry,
        bool suppressInvocation);

private:
    struct Binding
    {
        ShortcutChord chord;
        commands::CommandId command{};
        bool wasDown{false};
    };

    std::vector<Binding> bindings_;
};
} // namespace orbit::editor_model
