#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace orbit::commands
{
struct CommandIdTag;
using CommandId = core::StrongId<CommandIdTag>;

enum class CommandValueKind : u8
{
    Boolean,
    Integer,
    Float,
    String,
    Vector3,
    ObjectId,
    PropertyId
};

using CommandValue =
    std::variant<
        bool,
        i64,
        f64,
        std::string,
        math::Double3,
        scene::ObjectId,
        schema::PropertyId>;

struct CommandParameter
{
    std::string name;
    CommandValueKind kind{
        CommandValueKind::String};
    bool required{true};
};

struct CommandEnablement
{
    bool enabled{true};
    std::string reason;
};

using CommandArguments =
    std::unordered_map<
        std::string,
        CommandValue>;

struct CommandDescriptor
{
    CommandId id{};
    std::string name;
    std::string category;
    std::string description;
    std::vector<CommandParameter> parameters;
    bool automationVisible{true};
    std::function<CommandEnablement()>
        enablement;
    std::function<void(
        const CommandArguments&)>
        invoke;
};

struct CommandCatalogEntry
{
    CommandId id{};
    std::string name;
    std::string category;
    std::string description;
    std::vector<CommandParameter> parameters;
    bool automationVisible{true};
};

class CommandRegistry
{
public:
    void Register(
        CommandDescriptor descriptor);

    [[nodiscard]] bool Unregister(
        CommandId id) noexcept;

    [[nodiscard]] const CommandDescriptor*
    Find(CommandId id) const noexcept;

    [[nodiscard]] std::vector<
        CommandCatalogEntry>
    Catalog() const;

    [[nodiscard]] CommandEnablement
    Enablement(CommandId id) const;

    void Invoke(
        CommandId id,
        const CommandArguments& arguments = {})
        const;

private:
    [[nodiscard]] static CommandValueKind
    KindOf(const CommandValue& value) noexcept;

    std::unordered_map<
        CommandId,
        CommandDescriptor>
        commands_;
};
} // namespace orbit::commands
