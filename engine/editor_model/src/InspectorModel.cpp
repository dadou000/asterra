#include <orbit/editor_model/InspectorModel.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::editor_model
{
InspectorModel::InspectorModel(
    scene::ObjectStore& objects,
    const schema::SchemaRegistry& schemas,
    commands::CommandService& commands,
    const selection::SelectionService& selection)
    : objects_(objects),
      schemas_(schemas),
      commands_(commands),
      selection_(selection)
{
}

std::vector<scene::ObjectRecord>
InspectorModel::SelectedObjects() const
{
    std::vector<scene::ObjectRecord> result;

    for (const scene::ObjectId id :
         selection_.Ordered())
    {
        const auto object =
            objects_.Find(id);

        if (object.has_value())
        {
            result.push_back(*object);
        }
    }

    return result;
}

schema::PropertyValue
InspectorModel::EffectiveValue(
    const scene::ObjectRecord& object,
    const schema::PropertySchema& property) const
{
    const auto stored =
        objects_.GetProperty(
            object.id,
            property.id);

    return stored.has_value()
        ? *stored
        : property.defaultValue;
}

std::vector<InspectedProperty>
InspectorModel::CommonProperties() const
{
    const auto selected =
        SelectedObjects();

    if (selected.empty())
    {
        return {};
    }

    const schema::TypeSchema* firstType =
        schemas_.FindType(
            selected.front().type);

    if (firstType == nullptr)
    {
        return {};
    }

    std::vector<InspectedProperty> result;

    for (const schema::PropertySchema&
             candidate :
         firstType->properties)
    {
        bool compatible = true;

        for (std::size_t index = 1;
             index < selected.size();
             ++index)
        {
            const schema::PropertySchema*
                other =
                    schemas_.FindProperty(
                        selected[index].type,
                        candidate.id);

            if (other == nullptr ||
                other->kind != candidate.kind ||
                other->unit != candidate.unit ||
                other->readOnly !=
                    candidate.readOnly)
            {
                compatible = false;
                break;
            }
        }

        if (!compatible)
        {
            continue;
        }

        const schema::PropertyValue
            firstValue =
                EffectiveValue(
                    selected.front(),
                    candidate);

        bool mixed = false;

        for (std::size_t index = 1;
             index < selected.size();
             ++index)
        {
            const schema::PropertySchema*
                other =
                    schemas_.FindProperty(
                        selected[index].type,
                        candidate.id);

            if (EffectiveValue(
                    selected[index],
                    *other) != firstValue)
            {
                mixed = true;
                break;
            }
        }

        result.push_back({
            .schema = candidate,
            .value = firstValue,
            .mixed = mixed
        });
    }

    return result;
}

void InspectorModel::SetForSelection(
    const schema::PropertyId property,
    schema::PropertyValue value)
{
    const auto selected =
        SelectedObjects();

    if (selected.empty())
    {
        return;
    }

    for (const scene::ObjectRecord& object :
         selected)
    {
        const schema::PropertySchema*
            schema =
                schemas_.FindProperty(
                    object.type,
                    property);

        if (schema == nullptr ||
            schema->readOnly ||
            !schemas_.ValidateValue(
                *schema,
                value))
        {
            throw std::invalid_argument(
                "Selected objects do not share a writable compatible property.");
        }
    }

    commands_.BeginTransaction(
        "Edit Property");

    try
    {
        for (const scene::ObjectRecord& object :
             selected)
        {
            commands_.SetProperty(
                object.id,
                property,
                value);
        }

        commands_.CommitTransaction();
    }
    catch (...)
    {
        commands_.RollbackTransaction();
        throw;
    }
}
} // namespace orbit::editor_model
