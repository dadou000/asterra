#pragma once

#include <orbit/commands/CommandRegistry.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orbit::editor_model
{
enum class CommandSurfaceKind : u8
{
    Toolbar,
    ContextMenu,
    Radial
};

struct PresentedCommand
{
    commands::CommandId id{};
    std::string label;
    bool enabled{false};
    std::string disabledReason;
};

class CommandSurfaceRegistry
{
public:
    void Set(
        std::string_view surface,
        CommandSurfaceKind kind,
        std::vector<commands::CommandId> commandIds);

    void Add(
        std::string_view surface,
        CommandSurfaceKind kind,
        commands::CommandId commandId);

    [[nodiscard]] bool Remove(
        std::string_view surface,
        CommandSurfaceKind kind,
        commands::CommandId commandId);

    [[nodiscard]] std::vector<commands::CommandId>
    Commands(
        std::string_view surface,
        CommandSurfaceKind kind) const;

    [[nodiscard]] std::vector<PresentedCommand>
    Present(
        std::string_view surface,
        CommandSurfaceKind kind,
        const commands::CommandRegistry& registry) const;

private:
    struct Key
    {
        std::string surface;
        CommandSurfaceKind kind{};

        [[nodiscard]] bool operator==(
            const Key&) const noexcept = default;
    };

    struct KeyHash
    {
        [[nodiscard]] std::size_t operator()(
            const Key& key) const noexcept;
    };

    std::unordered_map<
        Key,
        std::vector<commands::CommandId>,
        KeyHash>
        surfaces_;
};
} // namespace orbit::editor_model
