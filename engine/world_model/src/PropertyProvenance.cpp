#include <orbit/world_model/PropertyProvenance.hpp>

namespace orbit::world_model
{
std::string_view ToString(
    const PropertySourceMode mode) noexcept
{
    switch (mode)
    {
        case PropertySourceMode::Default: return "Default";
        case PropertySourceMode::Explicit: return "Explicit";
        case PropertySourceMode::Derived: return "Derived";
        case PropertySourceMode::Procedural: return "Procedural";
        case PropertySourceMode::Imported: return "Imported";
        case PropertySourceMode::Linked: return "Linked";
    }
    return "Unknown";
}

std::string_view ToString(
    const PropertySolveState state) noexcept
{
    switch (state)
    {
        case PropertySolveState::Free: return "Free";
        case PropertySolveState::Locked: return "Locked";
        case PropertySolveState::Solved: return "Solved";
        case PropertySolveState::Conflict: return "Conflict";
    }
    return "Unknown";
}

bool CanSolverWrite(
    const PropertyProvenance& provenance) noexcept
{
    if (provenance.solveState == PropertySolveState::Locked ||
        provenance.solveState == PropertySolveState::Conflict)
    {
        return false;
    }

    return provenance.sourceMode != PropertySourceMode::Explicit &&
           provenance.sourceMode != PropertySourceMode::Imported;
}

bool HasConflict(
    const PropertyProvenance& provenance) noexcept
{
    return provenance.solveState == PropertySolveState::Conflict ||
           !provenance.diagnostic.empty();
}

std::vector<std::string>
ValidateProvenance(
    const PropertyProvenance& provenance)
{
    std::vector<std::string> errors;

    if (provenance.uncertainty.has_value() &&
        *provenance.uncertainty < 0.0)
    {
        errors.emplace_back(
            "Property uncertainty must be non-negative.");
    }

    if (provenance.sourceMode == PropertySourceMode::Imported &&
        provenance.sourceAsset.empty() &&
        !provenance.sourceObject.has_value())
    {
        errors.emplace_back(
            "Imported property requires a source asset or source object.");
    }

    if (provenance.sourceMode == PropertySourceMode::Linked &&
        provenance.sourceProperty.empty())
    {
        errors.emplace_back(
            "Linked property requires a source property.");
    }

    if (provenance.solveState == PropertySolveState::Conflict &&
        provenance.diagnostic.empty())
    {
        errors.emplace_back(
            "Conflict state requires a diagnostic.");
    }

    return errors;
}
} // namespace orbit::world_model
