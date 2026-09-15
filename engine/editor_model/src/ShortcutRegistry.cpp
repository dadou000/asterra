#include <orbit/editor_model/ShortcutRegistry.hpp>

#include <stdexcept>

namespace orbit::editor_model
{
void ShortcutRegistry::Register(
    const ShortcutChord chord,
    const commands::CommandId command)
{
    if (!command)
    {
        throw std::invalid_argument(
            "Shortcut requires a valid command ID.");
    }

    for (const Binding& binding :
         bindings_)
    {
        if (binding.chord == chord)
        {
            throw std::invalid_argument(
                "Shortcut chord is already registered.");
        }
    }

    bindings_.push_back({
        .chord = chord,
        .command = command
    });
}

void ShortcutRegistry::Update(
    platform::Window& window,
    const commands::CommandRegistry& commandRegistry,
    const bool suppressInvocation)
{
    const bool control =
        window.KeyDown(
            platform::Key::LeftControl);

    const bool shift =
        window.KeyDown(
            platform::Key::LeftShift);

    const bool alt =
        window.KeyDown(
            platform::Key::LeftAlt);

    for (Binding& binding :
         bindings_)
    {
        const bool down =
            window.KeyDown(
                binding.chord.key) &&
            control == binding.chord.control &&
            shift == binding.chord.shift &&
            alt == binding.chord.alt;

        if (!suppressInvocation &&
            down &&
            !binding.wasDown)
        {
            const auto enablement =
                commandRegistry.Enablement(
                    binding.command);

            if (enablement.enabled)
            {
                commandRegistry.Invoke(
                    binding.command);
            }
        }

        binding.wasDown = down;
    }
}
} // namespace orbit::editor_model
