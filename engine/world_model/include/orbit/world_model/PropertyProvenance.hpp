#pragma once

#include <orbit/schema/SchemaRegistry.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::world_model
{
enum class PropertySourceMode : u8
{
    Default = 0,
    Explicit = 1,
    Derived = 2,
    Procedural = 3,
    Imported = 4,
    Linked = 5
};

enum class PropertySolveState : u8
{
    Free = 0,
    Locked = 1,
    Solved = 2,
    Conflict = 3
};

struct PropertyProvenance
{
    PropertySourceMode sourceMode{PropertySourceMode::Default};
    PropertySolveState solveState{PropertySolveState::Free};
    std::optional<schema::ObjectReferenceValue> sourceObject;
    std::string sourceAsset;
    std::string sourceProperty;
    std::string diagnostic;
    std::optional<f64> uncertainty;
};

[[nodiscard]] std::string_view ToString(PropertySourceMode mode) noexcept;
[[nodiscard]] std::string_view ToString(PropertySolveState state) noexcept;

[[nodiscard]] bool CanSolverWrite(
    const PropertyProvenance& provenance) noexcept;

[[nodiscard]] bool HasConflict(
    const PropertyProvenance& provenance) noexcept;

[[nodiscard]] std::vector<std::string>
ValidateProvenance(const PropertyProvenance& provenance);
} // namespace orbit::world_model
