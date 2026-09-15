#include <orbit/editor_model/CommandSurfaces.hpp>

#include <functional>
#include <stdexcept>
#include <utility>

namespace orbit::editor_model
{
std::size_t
CommandSurfaceRegistry::KeyHash::operator()(
    const Key& key) const noexcept
{
    const std::size_t first =
        std::hash<std::string>{}(
            key.surface);

    const std::size_t second =
        std::hash<u8>{}(
            static_cast<u8>(
                key.kind));

    return first ^
        (second +
         static_cast<std::size_t>(
             0x9e3779b97f4a7c15ULL) +
         (first << 6U) +
         (first >> 2U));
}

void CommandSurfaceRegistry::Set(
    const std::string_view surface,
    const CommandSurfaceKind kind,
    std::vector<commands::CommandId> commandIds)
{
    if (surface.empty())
    {
        throw std::invalid_argument(
            "Command surface name must not be empty.");
    }

    surfaces_.insert_or_assign(
        Key{
            .surface =
                std::string(surface),
            .kind = kind
        },
        std::move(commandIds));
}

std::vector<commands::CommandId>
CommandSurfaceRegistry::Commands(
    const std::string_view surface,
    const CommandSurfaceKind kind) const
{
    const auto found =
        surfaces_.find(
            Key{
                .surface =
                    std::string(surface),
                .kind = kind
            });

    return found == surfaces_.end()
        ? std::vector<commands::CommandId>{}
        : found->second;
}

std::vector<PresentedCommand>
CommandSurfaceRegistry::Present(
    const std::string_view surface,
    const CommandSurfaceKind kind,
    const commands::CommandRegistry& registry) const
{
    std::vector<PresentedCommand> result;

    for (const commands::CommandId id :
         Commands(surface, kind))
    {
        const commands::CommandDescriptor* descriptor =
            registry.Find(id);

        if (descriptor == nullptr)
        {
            continue;
        }

        const commands::CommandEnablement enablement =
            registry.Enablement(id);

        result.push_back({
            .id = id,
            .label = descriptor->name,
            .enabled = enablement.enabled,
            .disabledReason = enablement.reason
        });
    }

    return result;
}
} // namespace orbit::editor_model
