#include <orbit/world_model/WorldSchemas.hpp>

#include <string>

namespace orbit::world_model
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
                .id = kBodyEllipsoidEnabled,
                .name = "Ellipsoid Shape",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = false
            },
            schema::PropertySchema{
                .id = kBodyRadius,
                .name = "Equatorial / Reference Radius",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 6'000'000.0,
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
                .kind = schema::PropertyKind::Float,
                .unit = "kg",
                .defaultValue = 5.0e24,
                .range = {
                    .minimum = 0.0
                },
                .advanced = true
            },
            schema::PropertySchema{
                .id = kBodyParentPositionMeters,
                .name = "Parent-frame Position",
                .kind = schema::PropertyKind::Vector3,
                .unit = "m",
                .defaultValue = math::Double3{}
            },
            schema::PropertySchema{
                .id = kBodyRotationPeriodSeconds,
                .name = "Rotation Period",
                .kind = schema::PropertyKind::Float,
                .unit = "s",
                .defaultValue = 86'400.0,
                .range = {
                    .minimum = 0.0
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
                .id = kBodyRotationPhaseDegrees,
                .name = "Rotation Phase At Epoch",
                .kind = schema::PropertyKind::Float,
                .unit = "deg",
                .defaultValue = 0.0,
                .advanced = true
            },
            schema::PropertySchema{
                .id = kBodyMaterialAsset,
                .name = "Material Asset",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{}
            }
        }
    });
}
} // namespace orbit::world_model
