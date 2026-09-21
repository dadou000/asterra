#include <orbit/world_model/PropertyProvenanceStore.hpp>

#include <orbit/world_model/PropertyProvenanceSchema.hpp>

#include <stdexcept>
#include <string>
#include <variant>

namespace orbit::world_model
{
namespace
{
template <typename T>
[[nodiscard]] std::optional<T> GetTyped(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property)
{
    const auto value =
        objects.GetProperty(
            object,
            property);

    if (!value.has_value())
    {
        return std::nullopt;
    }

    const auto* typed =
        std::get_if<T>(
            &*value);

    if (typed == nullptr)
    {
        throw std::runtime_error(
            "Property provenance record contains an unexpected property type.");
    }

    return *typed;
}
} // namespace

std::optional<scene::ObjectId>
FindPropertyProvenanceRecord(
    const scene::ObjectStore& objects,
    const scene::ObjectId owner,
    const schema::PropertyId targetProperty)
{
    const std::string target =
        targetProperty.ToString();

    for (const auto& child :
         objects.Children(owner))
    {
        if (child.type !=
            kPropertyProvenanceType)
        {
            continue;
        }

        const auto storedTarget =
            GetTyped<std::string>(
                objects,
                child.id,
                kProvenanceTargetProperty);

        if (storedTarget.has_value() &&
            *storedTarget == target)
        {
            return child.id;
        }
    }

    return std::nullopt;
}

std::optional<PropertyProvenance>
ReadStoredPropertyProvenance(
    const scene::ObjectStore& objects,
    const scene::ObjectId owner,
    const schema::PropertyId targetProperty)
{
    const auto record =
        FindPropertyProvenanceRecord(
            objects,
            owner,
            targetProperty);

    if (!record.has_value())
    {
        return std::nullopt;
    }

    PropertyProvenance result;

    if (const auto value =
            GetTyped<i64>(
                objects,
                *record,
                kProvenanceSourceMode);
        value.has_value())
    {
        if (*value < 0 ||
            *value >
                static_cast<i64>(
                    PropertySourceMode::Linked))
        {
            throw std::runtime_error(
                "Property provenance source mode is out of range.");
        }

        result.sourceMode =
            static_cast<PropertySourceMode>(
                *value);
    }

    if (const auto value =
            GetTyped<i64>(
                objects,
                *record,
                kProvenanceSolveState);
        value.has_value())
    {
        if (*value < 0 ||
            *value >
                static_cast<i64>(
                    PropertySolveState::Conflict))
        {
            throw std::runtime_error(
                "Property provenance solve state is out of range.");
        }

        result.solveState =
            static_cast<PropertySolveState>(
                *value);
    }

    if (const auto value =
            GetTyped<
                schema::ObjectReferenceValue>(
                    objects,
                    *record,
                    kProvenanceSourceObject);
        value.has_value() &&
        (value->high != 0U ||
         value->low != 0U))
    {
        result.sourceObject =
            *value;
    }

    if (const auto value =
            GetTyped<std::string>(
                objects,
                *record,
                kProvenanceSourceAsset);
        value.has_value())
    {
        result.sourceAsset =
            *value;
    }

    if (const auto value =
            GetTyped<std::string>(
                objects,
                *record,
                kProvenanceSourceProperty);
        value.has_value())
    {
        result.sourceProperty =
            *value;
    }

    if (const auto value =
            GetTyped<f64>(
                objects,
                *record,
                kProvenanceUncertainty);
        value.has_value())
    {
        result.uncertainty =
            *value;
    }

    return result;
}

PropertyProvenance
EffectivePropertyProvenance(
    const scene::ObjectStore& objects,
    const scene::ObjectId owner,
    const schema::PropertyId targetProperty)
{
    if (const auto stored =
            ReadStoredPropertyProvenance(
                objects,
                owner,
                targetProperty);
        stored.has_value())
    {
        return *stored;
    }

    if (objects.GetProperty(
            owner,
            targetProperty).
        has_value())
    {
        return {
            .sourceMode =
                PropertySourceMode::Explicit,
            .solveState =
                PropertySolveState::Locked
        };
    }

    return {};
}

scene::ObjectId WritePropertyProvenance(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    const scene::ObjectId owner,
    const schema::PropertyId targetProperty,
    const PropertyProvenance& provenance)
{
    const auto errors =
        ValidateProvenance(
            provenance);

    if (!errors.empty())
    {
        throw std::invalid_argument(
            errors.front());
    }

    scene::ObjectId record{};

    if (const auto existing =
            FindPropertyProvenanceRecord(
                objects,
                owner,
                targetProperty);
        existing.has_value())
    {
        record =
            *existing;
    }
    else
    {
        record =
            commands.CreateObject(
                kPropertyProvenanceType,
                "Property Provenance",
                owner);

        commands.SetProperty(
            record,
            kProvenanceTargetProperty,
            targetProperty.ToString());
    }

    commands.SetProperty(
        record,
        kProvenanceSourceMode,
        static_cast<i64>(
            provenance.sourceMode));

    commands.SetProperty(
        record,
        kProvenanceSolveState,
        static_cast<i64>(
            provenance.solveState));

    commands.SetProperty(
        record,
        kProvenanceSourceAsset,
        provenance.sourceAsset);

    commands.SetProperty(
        record,
        kProvenanceSourceProperty,
        provenance.sourceProperty);

    commands.SetProperty(
        record,
        kProvenanceUncertainty,
        provenance.uncertainty.value_or(
            0.0));

    if (provenance.sourceObject.has_value())
    {
        commands.SetProperty(
            record,
            kProvenanceSourceObject,
            *provenance.sourceObject);
    }

    return record;
}
} // namespace orbit::world_model
