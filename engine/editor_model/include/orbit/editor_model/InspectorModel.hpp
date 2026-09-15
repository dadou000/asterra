#pragma once

#include <orbit/commands/CommandService.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/selection/SelectionService.hpp>

#include <optional>
#include <vector>

namespace orbit::editor_model
{
struct InspectedProperty
{
    schema::PropertySchema schema;
    schema::PropertyValue value;
    bool mixed{false};
};

class InspectorModel
{
public:
    InspectorModel(
        scene::ObjectStore& objects,
        const schema::SchemaRegistry& schemas,
        commands::CommandService& commands,
        const selection::SelectionService& selection);

    [[nodiscard]] std::vector<scene::ObjectRecord>
    SelectedObjects() const;

    [[nodiscard]] std::vector<InspectedProperty>
    CommonProperties() const;

    // Applies one schema-compatible property value to every selected
    // object as one semantic transaction/undo record.
    void SetForSelection(
        schema::PropertyId property,
        schema::PropertyValue value);

private:
    [[nodiscard]] schema::PropertyValue
    EffectiveValue(
        const scene::ObjectRecord& object,
        const schema::PropertySchema& property) const;

    scene::ObjectStore& objects_;
    const schema::SchemaRegistry& schemas_;
    commands::CommandService& commands_;
    const selection::SelectionService& selection_;
};
} // namespace orbit::editor_model
