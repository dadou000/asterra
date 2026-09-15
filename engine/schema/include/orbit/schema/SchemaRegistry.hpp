#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/math/Vector.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace orbit::schema
{
struct TypeIdTag;
struct PropertyIdTag;

using TypeId = core::StrongId<TypeIdTag>;
using PropertyId = core::StrongId<PropertyIdTag>;

enum class PropertyKind : u8
{
    Boolean,
    Integer,
    Float,
    String,
    Vector3,
    ObjectReference
};

struct ObjectReferenceValue
{
    // Kept schema-owned to avoid a schema -> scene dependency. Scene
    // ObjectId uses the same opaque 128-bit representation and command
    // adapters convert explicitly.
    u64 high{0};
    u64 low{0};

    [[nodiscard]] constexpr bool operator==(
        const ObjectReferenceValue&) const noexcept = default;
};

using PropertyValue =
    std::variant<
        bool,
        i64,
        f64,
        std::string,
        math::Double3,
        ObjectReferenceValue>;

struct NumericRange
{
    std::optional<f64> minimum;
    std::optional<f64> maximum;
};

struct PropertySchema
{
    PropertyId id{};
    std::string name;
    PropertyKind kind{PropertyKind::String};
    std::string unit;
    PropertyValue defaultValue{std::string{}};
    NumericRange range{};
    bool advanced{false};
    bool readOnly{false};
};

struct TypeSchema
{
    TypeId id{};
    std::string displayName;
    std::string category;
    std::vector<PropertySchema> properties;
};

class SchemaRegistry
{
public:
    void RegisterType(TypeSchema schema);

    [[nodiscard]] const TypeSchema*
    FindType(TypeId id) const noexcept;

    [[nodiscard]] const PropertySchema*
    FindProperty(
        TypeId type,
        PropertyId property) const noexcept;

    [[nodiscard]] bool ValidateValue(
        const PropertySchema& property,
        const PropertyValue& value) const noexcept;

private:
    std::unordered_map<TypeId, TypeSchema>
        types_;
};
} // namespace orbit::schema
