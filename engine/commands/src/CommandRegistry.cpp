#include <orbit/commands/CommandRegistry.hpp>

#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace orbit::commands
{
CommandValueKind CommandRegistry::KindOf(
    const CommandValue& value) noexcept
{
    return std::visit(
        [](const auto& item)
        {
            using Value =
                std::decay_t<
                    decltype(item)>;

            if constexpr (
                std::is_same_v<Value, bool>)
            {
                return CommandValueKind::Boolean;
            }
            else if constexpr (
                std::is_same_v<Value, i64>)
            {
                return CommandValueKind::Integer;
            }
            else if constexpr (
                std::is_same_v<Value, f64>)
            {
                return CommandValueKind::Float;
            }
            else if constexpr (
                std::is_same_v<
                    Value,
                    std::string>)
            {
                return CommandValueKind::String;
            }
            else if constexpr (
                std::is_same_v<
                    Value,
                    math::Double3>)
            {
                return CommandValueKind::Vector3;
            }
            else if constexpr (
                std::is_same_v<
                    Value,
                    scene::ObjectId>)
            {
                return CommandValueKind::ObjectId;
            }
            else
            {
                return CommandValueKind::PropertyId;
            }
        },
        value);
}

void CommandRegistry::Register(
    CommandDescriptor descriptor)
{
    if (!descriptor.id ||
        descriptor.name.empty() ||
        !descriptor.invoke)
    {
        throw std::invalid_argument(
            "Command requires stable ID, name and invocation callback.");
    }

    if (commands_.contains(
            descriptor.id))
    {
        throw std::invalid_argument(
            "Command ID is already registered.");
    }

    std::unordered_set<std::string>
        parameterNames;

    for (const CommandParameter&
             parameter :
         descriptor.parameters)
    {
        if (parameter.name.empty() ||
            !parameterNames.insert(
                parameter.name).second)
        {
            throw std::invalid_argument(
                "Command parameters require unique non-empty names.");
        }
    }

    commands_.emplace(
        descriptor.id,
        std::move(descriptor));
}

bool CommandRegistry::Unregister(
    const CommandId id) noexcept
{
    return commands_.erase(id) != 0U;
}

const CommandDescriptor*
CommandRegistry::Find(
    const CommandId id) const noexcept
{
    const auto found =
        commands_.find(id);

    return found == commands_.end()
        ? nullptr
        : &found->second;
}

std::vector<CommandCatalogEntry>
CommandRegistry::Catalog() const
{
    std::vector<CommandCatalogEntry>
        result;

    result.reserve(
        commands_.size());

    for (const auto& [id, command] :
         commands_)
    {
        result.push_back({
            .id = id,
            .name = command.name,
            .category = command.category,
            .description =
                command.description,
            .parameters =
                command.parameters,
            .automationVisible =
                command.automationVisible
        });
    }

    return result;
}

CommandEnablement CommandRegistry::Enablement(
    const CommandId id) const
{
    const CommandDescriptor* command =
        Find(id);

    if (command == nullptr)
    {
        return {
            .enabled = false,
            .reason =
                "Command is not registered."
        };
    }

    if (!command->enablement)
    {
        return {};
    }

    return command->enablement();
}

void CommandRegistry::Invoke(
    const CommandId id,
    const CommandArguments& arguments)
    const
{
    const CommandDescriptor* command =
        Find(id);

    if (command == nullptr)
    {
        throw std::invalid_argument(
            "Cannot invoke unknown command.");
    }

    const CommandEnablement enabled =
        Enablement(id);

    if (!enabled.enabled)
    {
        throw std::logic_error(
            enabled.reason.empty()
                ? "Command is disabled."
                : enabled.reason);
    }

    for (const CommandParameter&
             parameter :
         command->parameters)
    {
        const auto found =
            arguments.find(
                parameter.name);

        if (found == arguments.end())
        {
            if (parameter.required)
            {
                throw std::invalid_argument(
                    "Missing required command argument: " +
                    parameter.name);
            }

            continue;
        }

        if (KindOf(found->second) !=
            parameter.kind)
        {
            throw std::invalid_argument(
                "Command argument has wrong type: " +
                parameter.name);
        }
    }

    for (const auto& [name, value] :
         arguments)
    {
        static_cast<void>(value);

        bool declared = false;

        for (const CommandParameter&
                 parameter :
             command->parameters)
        {
            if (parameter.name == name)
            {
                declared = true;
                break;
            }
        }

        if (!declared)
        {
            throw std::invalid_argument(
                "Unknown command argument: " +
                name);
        }
    }

    command->invoke(arguments);
}
} // namespace orbit::commands
