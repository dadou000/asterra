#include <orbit/world_model/PropertyProvenance.hpp>
#include <orbit/world_model/PropertyProvenanceSchema.hpp>
#include <orbit/schema/SchemaRegistry.hpp>

#include <string>

int main()
{
    using namespace orbit::world_model;

    PropertyProvenance explicitValue{
        .sourceMode = PropertySourceMode::Explicit,
        .solveState = PropertySolveState::Locked
    };

    if (CanSolverWrite(explicitValue))
    {
        return 1;
    }

    PropertyProvenance derivedValue{
        .sourceMode = PropertySourceMode::Derived,
        .solveState = PropertySolveState::Solved
    };

    if (!CanSolverWrite(derivedValue))
    {
        return 2;
    }

    PropertyProvenance conflict{
        .sourceMode = PropertySourceMode::Derived,
        .solveState = PropertySolveState::Conflict,
        .diagnostic = "Mass and density both locked; radius is inconsistent."
    };

    if (!HasConflict(conflict) ||
        CanSolverWrite(conflict) ||
        !ValidateProvenance(conflict).empty())
    {
        return 3;
    }

    PropertyProvenance invalidImported{
        .sourceMode = PropertySourceMode::Imported
    };

    if (ValidateProvenance(invalidImported).empty())
    {
        return 4;
    }

    PropertyProvenance invalidLinked{
        .sourceMode = PropertySourceMode::Linked
    };

    if (ValidateProvenance(invalidLinked).empty())
    {
        return 5;
    }

    orbit::schema::SchemaRegistry schemas;
    RegisterPropertyProvenanceSchema(schemas);

    const auto* type = schemas.FindType(kPropertyProvenanceType);
    if (type == nullptr ||
        type->category != "Celestial/Property Metadata")
    {
        return 6;
    }

    const auto* source = schemas.FindProperty(
        kPropertyProvenanceType,
        kProvenanceSourceMode);

    const auto* diagnostic = schemas.FindProperty(
        kPropertyProvenanceType,
        kProvenanceDiagnostic);

    if (source == nullptr ||
        diagnostic == nullptr ||
        !diagnostic->readOnly)
    {
        return 7;
    }

    return 0;
}
