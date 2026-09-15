#include <orbit/schema/SchemaRegistry.hpp>

#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace orbit::schema
{
namespace
{
[[nodiscard]] PropertyKind KindOf(
    const PropertyValue& value) noexcept
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
                return PropertyKind::Boolean;
            }
            else if constexpr (
                std::is_same_v<Value, i64>)
            {
                return PropertyKind::Integer;
            }
            else if constexpr (
                std::is_same_v<Value, f64>)
            {
                return PropertyKind::Float;
            }
            else if constexpr (
                std::is_same_v<
                    Value,
                    std::string>)
            {
                return PropertyKind::String;
            }
            else if constexpr (
                std::is_same_v<
                    Value,
                    math::Double3>)
            {
                return PropertyKind::Vector3;
            }
            else
            {
                return PropertyKind::ObjectReference;
            }
        },
        value);
}
} // namespace

void SchemaRegistry::RegisterType(
    TypeSchema schema)
{
    if (!schema.id)
    {
        throw std::invalid_argument(
            "Schema type ID must be valid.");
    }

    if (schema.displayName.empty())
    {
        throw std::invalid_argument(
            "Schema type display name must not be empty.");
    }

    if (types_.contains(schema.id))
    {
        throw std::invalid_argument(
            "Schema type ID already registered.");
    }

    std::unordered_set<PropertyId>
        propertyIds;

    for (const PropertySchema& property :
         schema.properties)
    {
        if (!property.id ||
            property.name.empty())
        {
            throw std::invalid_argument(
                "Schema property requires stable ID and name.");
        }

        if (!propertyIds.insert(
                property.id).second)
        {
            throw std::invalid_argument(
                "Schema contains duplicate property ID.");
        }

        if (KindOf(property.defaultValue) !=
            property.kind)
        {
            throw std::invalid_argument(
                "Schema property default value has wrong type.");
        }

        if (property.range.minimum.has_value() &&
            property.range.maximum.has_value() &&
            *property.range.minimum >
                *property.range.maximum)
        {
            throw std::invalid_argument(
                "Schema numeric range minimum exceeds maximum.");
        }

        if (!ValidateValue(
                property,
                property.defaultValue))
        {
            throw std::invalid_argument(
                "Schema property default value violates its range.");
        }
    }

    types_.emplace(
        schema.id,
        std::move(schema));
}

const TypeSchema*
SchemaRegistry::FindType(
    const TypeId id) const noexcept
{
    const auto found = types_.find(id);

    return found == types_.end()
        ? nullptr
        : &found->second;
}

const PropertySchema*
SchemaRegistry::FindProperty(
    const TypeId type,
    const PropertyId property) const noexcept
{
    const TypeSchema* schema =
        FindType(type);

    if (schema == nullptr)
    {
        return nullptr;
    }

    for (const PropertySchema& candidate :
         schema->properties)
    {
        if (candidate.id == property)
        {
            return &candidate;
        }
    }

    return nullptr;
}

bool SchemaRegistry::ValidateValue(
    const PropertySchema& property,
    const PropertyValue& value) const noexcept
{
    if (KindOf(value) != property.kind)
    {
        return false;
    }

    if (property.kind ==
            PropertyKind::Float)
    {
        const f64 number =
            std::get<f64>(value);

        if (!std::isfinite(number))
        {
            return false;
        }

        if (property.range.minimum.has_value() &&
            number <
                *property.range.minimum)
        {
            return false;
        }

        if (property.range.maximum.has_value() &&
            number >
                *property.range.maximum)
        {
            return false;
        }
    }
    else if (property.kind ==
             PropertyKind::Integer)
    {
        const f64 number =
            static_cast<f64>(
                std::get<i64>(value));

        if (property.range.minimum.has_value() &&
            number <
                *property.range.minimum)
        {
            return false;
        }

        if (property.range.maximum.has_value() &&
            number >
                *property.range.maximum)
        {
            return false;
        }
    }

    return true;
}
} // namespace orbit::schema
