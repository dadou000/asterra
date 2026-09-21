#include <orbit/world_model/PropertyProvenanceSchema.hpp>

#include <string>

namespace orbit::world_model
{
void RegisterPropertyProvenanceSchema(
    schema::SchemaRegistry& schemas)
{
    schemas.RegisterType({
        .id = kPropertyProvenanceType,
        .displayName = "Property Provenance",
        .category = "Celestial/Property Metadata",
        .properties = {
            {.id = kProvenanceTargetProperty,
             .name = "Target Property",
             .kind = schema::PropertyKind::String,
             .defaultValue = std::string{}},
            {.id = kProvenanceSourceMode,
             .name = "Source Mode",
             .kind = schema::PropertyKind::Integer,
             .defaultValue = i64{static_cast<i64>(PropertySourceMode::Default)},
             .range = {.minimum = 0.0, .maximum = 5.0}},
            {.id = kProvenanceSolveState,
             .name = "Solve State",
             .kind = schema::PropertyKind::Integer,
             .defaultValue = i64{static_cast<i64>(PropertySolveState::Free)},
             .range = {.minimum = 0.0, .maximum = 3.0}},
            {.id = kProvenanceSourceObject,
             .name = "Source Object",
             .kind = schema::PropertyKind::ObjectReference,
             .defaultValue = schema::ObjectReferenceValue{},
             .advanced = true},
            {.id = kProvenanceSourceAsset,
             .name = "Source Asset",
             .kind = schema::PropertyKind::String,
             .defaultValue = std::string{},
             .advanced = true},
            {.id = kProvenanceSourceProperty,
             .name = "Source Property",
             .kind = schema::PropertyKind::String,
             .defaultValue = std::string{},
             .advanced = true},
            {.id = kProvenanceDiagnostic,
             .name = "Diagnostic",
             .kind = schema::PropertyKind::String,
             .defaultValue = std::string{},
             .readOnly = true},
            {.id = kProvenanceUncertainty,
             .name = "Uncertainty",
             .kind = schema::PropertyKind::Float,
             .defaultValue = 0.0,
             .range = {.minimum = 0.0},
             .advanced = true}
        }
    });
}
} // namespace orbit::world_model
