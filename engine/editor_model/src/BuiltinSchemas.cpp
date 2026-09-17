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
        .category = "World",
        .properties = {
            schema::PropertySchema{
                .id = kWorldEpochMicroseconds,
                .name = "Simulation Epoch",
                .kind = schema::PropertyKind::Integer,
                .unit = "us",
                .defaultValue = i64{0},
                .advanced = true
            },
            schema::PropertySchema{
                .id = kWorldTimeScale,
                .name = "Time Scale",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {
                    .minimum = 0.0
                }
            }
        }
    });

    schemas.RegisterType({
        .id = kCelestialSystemType,
        .displayName = "Celestial System",
        .category = "World",
        .properties = {
            schema::PropertySchema{
                .id = kSystemEpochMicroseconds,
                .name = "System Epoch",
                .kind = schema::PropertyKind::Integer,
                .unit = "us",
                .defaultValue = i64{0},
                .advanced = true
            }
        }
    });

    schemas.RegisterType({
        .id = kCelestialBodyType,
        .displayName = "Celestial Body",
        .category = "World",
        .properties = {
            schema::PropertySchema{
                .id = kBodyShapeMode,
                .name = "Shape Mode",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{"Sphere"}
            },
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
                .id = kBodyPolarRadius,
                .name = "Polar Radius",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 6'000'000.0,
                .range = {
                    .minimum = 1.0
                },
                .advanced = true
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
                .id = kBodyRotationPeriodSeconds,
                .name = "Rotation Period",
                .kind = schema::PropertyKind::Float,
                .unit = "s",
                .defaultValue = 86'400.0,
                .range = {
                    .minimum = 0.001
                }
            },
            schema::PropertySchema{
                .id = kBodyAxialTiltDegrees,
                .name = "Axial Tilt",
                .kind = schema::PropertyKind::Float,
                .unit = "deg",
                .defaultValue = 0.0,
                .range = {
                    .minimum = -180.0,
                    .maximum = 180.0
                }
            },
            schema::PropertySchema{
                .id = kBodySurfaceEnabled,
                .name = "Surface Enabled",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = true
            },
            schema::PropertySchema{
                .id = kBodyAtmosphereEnabled,
                .name = "Atmosphere Enabled",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = false
            },
            schema::PropertySchema{
                .id = kBodyHydrosphereEnabled,
                .name = "Hydrosphere Enabled",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = false
            },
            schema::PropertySchema{
                .id = kBodyTerrainSeed,
                .name = "Terrain Seed",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{0}
            },
            schema::PropertySchema{
                .id = kBodyOceanLevelMeters,
                .name = "Ocean Level",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 0.0,
                .advanced = true
            },
            schema::PropertySchema{
                .id = kBodyMaximumElevationMeters,
                .name = "Maximum Elevation",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 8'000.0,
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
