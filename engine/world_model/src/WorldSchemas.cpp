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

    schemas.RegisterType({
        .id = kTerrainSurfaceType,
        .displayName = "Terrain Surface",
        .category = "World / Surface",
        .properties = {
            schema::PropertySchema{
                .id = kTerrainSeed,
                .name = "Seed",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{0x41535445525241LL},
                .range = {
                    .minimum = 0.0
                }
            },
            schema::PropertySchema{
                .id = kTerrainMacroAmplitudeMeters,
                .name = "Macro Amplitude",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 1'200.0,
                .range = {
                    .minimum = 0.0
                }
            },
            schema::PropertySchema{
                .id = kTerrainMacroWavelengthMeters,
                .name = "Macro Wavelength",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 800'000.0,
                .range = {
                    .minimum = 1.0
                }
            },
            schema::PropertySchema{
                .id = kTerrainDetailAmplitudeMeters,
                .name = "Detail Amplitude",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 320.0,
                .range = {
                    .minimum = 0.0
                }
            },
            schema::PropertySchema{
                .id = kTerrainDetailWavelengthMeters,
                .name = "Detail Wavelength",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 40'000.0,
                .range = {
                    .minimum = 1.0
                }
            },
            schema::PropertySchema{
                .id = kTerrainDetailOctaves,
                .name = "Detail Octaves",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{10},
                .range = {
                    .minimum = 1.0,
                    .maximum = 16.0
                }
            },
            schema::PropertySchema{
                .id = kTerrainMaximumElevationMeters,
                .name = "Maximum Elevation Above Sea Level",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 8'000.0,
                .range = {
                    .minimum = 1.0
                }
            }
        }
    });

    // These V0.0.4 surface assets intentionally have no placeholder process
    // properties yet. Their stable schema identities are real persistence and
    // command-layer contracts now; each milestone adds only the properties
    // whose runtime semantics are implemented at that point.
    schemas.RegisterType({
        .id = kGeologyAssetType,
        .displayName = "Geology Asset",
        .category = "World / Surface / Geology"
    });

    schemas.RegisterType({
        .id = kTerrainProcessAssetType,
        .displayName = "Terrain Process Asset",
        .category = "World / Surface / Processes"
    });

    schemas.RegisterType({
        .id = kBiomeAssetType,
        .displayName = "Biome Asset",
        .category = "World / Surface / Biomes",
        .properties = {
            schema::PropertySchema{
                .id = kBiomeMinimumResolvedWeight,
                .name = "Minimum Resolved Weight",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 0.05,
                .range = {
                    .minimum = 0.0,
                    .maximum = 1.0
                }
            },
            schema::PropertySchema{
                .id = kBiomeMaterialInfluence,
                .name = "Surface Material Influence",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {
                    .minimum = 0.0
                }
            },
            schema::PropertySchema{
                .id = kBiomeScatterDensityMultiplier,
                .name = "Scatter Density Multiplier",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {
                    .minimum = 0.0
                }
            },
            schema::PropertySchema{
                .id = kBiomeHydraulicErosionMultiplier,
                .name = "Hydraulic Erosion Multiplier",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {.minimum = 0.0},
                .advanced = true
            },
            schema::PropertySchema{
                .id = kBiomeThermalTransportMultiplier,
                .name = "Thermal Transport Multiplier",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {.minimum = 0.0},
                .advanced = true
            },
            schema::PropertySchema{
                .id = kBiomeAeolianTransportMultiplier,
                .name = "Aeolian Transport Multiplier",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {.minimum = 0.0},
                .advanced = true
            },
            schema::PropertySchema{
                .id = kBiomeGlacialErosionMultiplier,
                .name = "Glacial Erosion Multiplier",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {.minimum = 0.0},
                .advanced = true
            },
            schema::PropertySchema{
                .id = kBiomeCoastalErosionMultiplier,
                .name = "Coastal Erosion Multiplier",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {.minimum = 0.0},
                .advanced = true
            },
            schema::PropertySchema{
                .id = kBiomeChemicalWeatheringMultiplier,
                .name = "Chemical Weathering Multiplier",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {.minimum = 0.0},
                .advanced = true
            }
        }
    });

    schemas.RegisterType({
        .id = kSurfaceDecalType,
        .displayName = "Surface Decal",
        .category = "Material",
        .properties = {
            schema::PropertySchema{
                .id = kDecalAsset,
                .name = "Decal Asset",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{}
            },
            schema::PropertySchema{
                .id = kDecalLatitudeRadians,
                .name = "Latitude",
                .kind = schema::PropertyKind::Float,
                .unit = "rad",
                .defaultValue = 0.0,
                .range = {
                    .minimum = -1.5707963267948966,
                    .maximum = 1.5707963267948966
                }
            },
            schema::PropertySchema{
                .id = kDecalLongitudeRadians,
                .name = "Longitude",
                .kind = schema::PropertyKind::Float,
                .unit = "rad",
                .defaultValue = 0.0,
                .range = {
                    .minimum = -3.141592653589793,
                    .maximum = 3.141592653589793
                }
            },
            schema::PropertySchema{
                .id = kDecalWidthMeters,
                .name = "Width",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 1.0,
                .range = {
                    .minimum = 0.001
                }
            },
            schema::PropertySchema{
                .id = kDecalHeightMeters,
                .name = "Height",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 1.0,
                .range = {
                    .minimum = 0.001
                }
            },
            schema::PropertySchema{
                .id = kDecalRotationDegrees,
                .name = "Rotation",
                .kind = schema::PropertyKind::Float,
                .unit = "deg",
                .defaultValue = 0.0
            },
            schema::PropertySchema{
                .id = kDecalOpacity,
                .name = "Opacity",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {
                    .minimum = 0.0,
                    .maximum = 1.0
                }
            }
        }
    });
}
} // namespace orbit::world_model
