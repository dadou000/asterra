#include <orbit/editor_model/BuiltinSchemas.hpp>

#include <orbit/paths/PathNetwork.hpp>

#include <string>

namespace orbit::editor_model::builtin
{
void RegisterSchemas(
    schema::SchemaRegistry& schemas)
{
    schemas.RegisterType({
        .id = kWorldType,
        .displayName = "World",
        .category = "World"
    });

    schemas.RegisterType({
        .id = kCelestialBodyType,
        .displayName = "Celestial Body",
        .category = "World",
        .properties = {
            schema::PropertySchema{
                .id = kBodyRadius,
                .name = "Reference Radius",
                .kind =
                    schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue =
                    6'000'000.0,
                .range = {
                    .minimum = 1.0
                }
            },
            schema::PropertySchema{
                .id = kBodyMass,
                .name = "Mass",
                .kind =
                    schema::PropertyKind::Float,
                .unit = "kg",
                .defaultValue =
                    5.0e24,
                .range = {
                    .minimum = 0.0
                },
                .advanced = true
            },
            schema::PropertySchema{
                .id = kBodyMaterialAsset,
                .name = "Material Asset",
                .kind =
                    schema::PropertyKind::String,
                .defaultValue =
                    std::string{}
            }
        }
    });

    // Path objects are ordinary semantic scene objects. Registering them in
    // the shared catalog makes Explorer, Properties, plugins and MCP discover
    // the same production schemas without an editor-only parallel model.
    paths::RegisterSchemas(schemas);
}
} // namespace orbit::editor_model::builtin
