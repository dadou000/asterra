#pragma once

#include <orbit/commands/CommandService.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::editor_model
{
enum class CelestialDiagnosticSeverity : u8
{
    Info = 0,
    Warning = 1,
    Error = 2
};

struct CelestialDiagnostic
{
    CelestialDiagnosticSeverity severity{CelestialDiagnosticSeverity::Info};
    std::string message;
    std::optional<scene::ObjectId> object;
};

struct CelestialCapabilityDescriptor
{
    schema::TypeId type{};
    std::string_view label;
    bool singleton{true};
};

class CelestialAuthoringModel
{
public:
    CelestialAuthoringModel(
        scene::ObjectStore& objects,
        const schema::SchemaRegistry& schemas,
        commands::CommandService& commands,
        selection::SelectionService& selection);

    [[nodiscard]] std::optional<scene::ObjectRecord>
    PrimarySelection() const;

    [[nodiscard]] std::optional<scene::ObjectRecord>
    SelectedBody() const;

    [[nodiscard]] scene::ObjectId CreateSystem(
        std::string_view name = "Celestial System");

    [[nodiscard]] scene::ObjectId CreateBody(
        std::string_view name = "Celestial Body");

    [[nodiscard]] scene::ObjectId CreateReferenceNode(
        std::string_view name = "Barycenter");

    [[nodiscard]] scene::ObjectId AddCapability(
        schema::TypeId capabilityType,
        std::string_view name);

    [[nodiscard]] scene::ObjectId AddRingBand(
        std::string_view name = "Ring Band");

    void RemoveSelectedCapability();
    void RemoveSelectedRingBand();

    [[nodiscard]] std::vector<CelestialCapabilityDescriptor>
    AvailableCapabilities() const;

    [[nodiscard]] std::vector<CelestialDiagnostic>
    Validate() const;

    [[nodiscard]] bool IsCapabilityType(
        schema::TypeId type) const noexcept;

private:
    [[nodiscard]] std::optional<scene::ObjectRecord>
    NearestBody(scene::ObjectId start) const;

    [[nodiscard]] std::optional<scene::ObjectRecord>
    FindWorldRoot() const;

    [[nodiscard]] std::optional<scene::ObjectRecord>
    FindSystemAncestor(scene::ObjectId start) const;

    void SelectOnly(scene::ObjectId object);

    scene::ObjectStore& objects_;
    const schema::SchemaRegistry& schemas_;
    commands::CommandService& commands_;
    selection::SelectionService& selection_;
};
} // namespace orbit::editor_model
