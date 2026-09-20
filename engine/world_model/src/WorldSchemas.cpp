#include <orbit/world_model/WorldSchemas.hpp>

#include <string>
#include <utility>

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
        .id = kWaterServiceType,
        .displayName = "Water Service",
        .category = "World / Surface / Water",
        .properties = {
            schema::PropertySchema{
                .id = kWaterOceanEnabled,
                .name = "Ocean Enabled",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = false
            },
            schema::PropertySchema{
                .id = kWaterOceanDatumMeters,
                .name = "Ocean Datum",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 0.0
            },
            schema::PropertySchema{
                .id = kWaterFluidAsset,
                .name = "Fluid Asset",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{}
            }
        }
    });

    for (const auto& [type, name] : {
             std::pair{kWaterSourceType, "Water Source"},
             std::pair{kWaterBarrierType, "Water Barrier / Gate / Spillway"},
             std::pair{kWaterDomainType, "Interior Water Domain"},
             std::pair{kWaterEmitterType, "Water Force / Wave Emitter"}})
    {
        schemas.RegisterType({
            .id = type,
            .displayName = name,
            .category = "World / Surface / Water",
            .properties = {
                schema::PropertySchema{
                    .id = kWaterEnabled,
                    .name = "Enabled",
                    .kind = schema::PropertyKind::Boolean,
                    .defaultValue = true
                }
            }
        });
    }

    schemas.RegisterType({
        .id = kBiomeAssetType,
        .displayName = "Biome Asset",
        .category = "World / Surface / Biomes",
        .properties = {
            schema::PropertySchema{
                .id = kBiomePlacementMode,
                .name = "Placement Mode",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{0},
                .range = {
                    .minimum = 0.0,
                    .maximum = 2.0
                }
            },
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
        .id = kBiomeSelectorType,
        .displayName = "Biome Automatic Selector",
        .category = "World / Surface / Biomes",
        .properties = {
            schema::PropertySchema{
                .id = kBiomeSelectorField,
                .name = "Field",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{0},
                .range = {
                    .minimum = 0.0,
                    .maximum = 16.0
                }
            },
            schema::PropertySchema{
                .id = kBiomeSelectorMinimum,
                .name = "Minimum",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 0.0
            },
            schema::PropertySchema{
                .id = kBiomeSelectorMaximum,
                .name = "Maximum",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0
            },
            schema::PropertySchema{
                .id = kBiomeSelectorLowerFalloff,
                .name = "Lower Falloff",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 0.0,
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeSelectorUpperFalloff,
                .name = "Upper Falloff",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 0.0,
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeSelectorMaterial,
                .name = "Geological Material ID",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{},
                .advanced = true
            },
            schema::PropertySchema{
                .id = kBiomeSelectorUserField,
                .name = "User Field",
                .kind = schema::PropertyKind::String,
                .defaultValue = std::string{},
                .advanced = true
            },
            schema::PropertySchema{
                .id = kBiomeSelectorInvert,
                .name = "Invert",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = false
            },
            schema::PropertySchema{
                .id = kBiomeSelectorEnabled,
                .name = "Enabled",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = true
            }
        }
    });

    schemas.RegisterType({
        .id = kBiomeAuthoredMaskType,
        .displayName = "Biome Authored Mask",
        .category = "World / Surface / Biomes",
        .properties = {
            schema::PropertySchema{
                .id = kBiomeMaskOperation,
                .name = "Operation",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{0},
                .range = {
                    .minimum = 0.0,
                    .maximum = 5.0
                }
            },
            schema::PropertySchema{
                .id = kBiomeMaskCenter,
                .name = "Center Unit Direction",
                .kind = schema::PropertyKind::Vector3,
                .defaultValue = math::Double3{0.0, 1.0, 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeMaskInnerRadius,
                .name = "Inner Radius",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 0.0,
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeMaskOuterRadius,
                .name = "Outer Radius",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 1000.0,
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeMaskGlobal,
                .name = "Global",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = false
            },
            schema::PropertySchema{
                .id = kBiomeMaskValue,
                .name = "Weight Value",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {
                    .minimum = 0.0,
                    .maximum = 1.0
                }
            },
            schema::PropertySchema{
                .id = kBiomeMaskOpacity,
                .name = "Opacity",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {
                    .minimum = 0.0,
                    .maximum = 1.0
                }
            },
            schema::PropertySchema{
                .id = kBiomeMaskEnabled,
                .name = "Enabled",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = true
            }
        }
    });

    schemas.RegisterType({
        .id = kBiomeSurfaceLayerType,
        .displayName = "Biome Surface Layer",
        .category = "World / Surface / Biomes",
        .properties = {
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerKind,
                .name = "Layer Kind",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{3},
                .range = {.minimum = 0.0, .maximum = 3.0}
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerStrength,
                .name = "Strength",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {.minimum = 0.0, .maximum = 1.0}
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerCompatibility,
                .name = "Compatible Exposed Materials",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{31},
                .range = {.minimum = 1.0, .maximum = 31.0}
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerSlopeMin,
                .name = "Slope Minimum",
                .kind = schema::PropertyKind::Float,
                .unit = "deg",
                .defaultValue = 0.0,
                .range = {.minimum = 0.0, .maximum = 90.0}
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerSlopeMax,
                .name = "Slope Maximum",
                .kind = schema::PropertyKind::Float,
                .unit = "deg",
                .defaultValue = 90.0,
                .range = {.minimum = 0.0, .maximum = 90.0}
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerSlopeFalloff,
                .name = "Slope Falloff",
                .kind = schema::PropertyKind::Float,
                .unit = "deg",
                .defaultValue = 0.0,
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerCurvatureMin,
                .name = "Curvature Minimum",
                .kind = schema::PropertyKind::Float,
                .defaultValue = -1.0
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerCurvatureMax,
                .name = "Curvature Maximum",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerCurvatureFalloff,
                .name = "Curvature Falloff",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 0.0,
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerMoistureMin,
                .name = "Moisture Minimum",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 0.0,
                .range = {.minimum = 0.0, .maximum = 1.0}
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerMoistureMax,
                .name = "Moisture Maximum",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {.minimum = 0.0, .maximum = 1.0}
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerMoistureFalloff,
                .name = "Moisture Falloff",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 0.0,
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeSurfaceLayerEnabled,
                .name = "Enabled",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = true
            }
        }
    });

    schemas.RegisterType({
        .id = kBiomeScatterRuleType,
        .displayName = "Biome Scatter Rule",
        .category = "World / Surface / Biomes",
        .properties = {
            schema::PropertySchema{
                .id = kBiomeScatterKind,
                .name = "Scatter Kind",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{2},
                .range = {.minimum = 0.0, .maximum = 5.0}
            },
            schema::PropertySchema{
                .id = kBiomeScatterDensity,
                .name = "Density",
                .kind = schema::PropertyKind::Float,
                .unit = "1/m2",
                .defaultValue = 0.01,
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeScatterSpacing,
                .name = "Minimum Spacing",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 1.0,
                .range = {.minimum = 0.001}
            },
            schema::PropertySchema{
                .id = kBiomeScatterSeedSalt,
                .name = "Seed Salt",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{0},
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeScatterCompatibility,
                .name = "Compatible Exposed Materials",
                .kind = schema::PropertyKind::Integer,
                .defaultValue = i64{31},
                .range = {.minimum = 1.0, .maximum = 31.0}
            },
            schema::PropertySchema{
                .id = kBiomeScatterRequiresSoil,
                .name = "Requires Soil",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = false
            },
            schema::PropertySchema{
                .id = kBiomeScatterMinimumSoilDepth,
                .name = "Minimum Soil Depth",
                .kind = schema::PropertyKind::Float,
                .unit = "m",
                .defaultValue = 0.0,
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeScatterSlopeMin,
                .name = "Slope Minimum",
                .kind = schema::PropertyKind::Float,
                .unit = "deg",
                .defaultValue = 0.0,
                .range = {.minimum = 0.0, .maximum = 90.0}
            },
            schema::PropertySchema{
                .id = kBiomeScatterSlopeMax,
                .name = "Slope Maximum",
                .kind = schema::PropertyKind::Float,
                .unit = "deg",
                .defaultValue = 90.0,
                .range = {.minimum = 0.0, .maximum = 90.0}
            },
            schema::PropertySchema{
                .id = kBiomeScatterSlopeFalloff,
                .name = "Slope Falloff",
                .kind = schema::PropertyKind::Float,
                .unit = "deg",
                .defaultValue = 0.0,
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeScatterMoistureMin,
                .name = "Moisture Minimum",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 0.0,
                .range = {.minimum = 0.0, .maximum = 1.0}
            },
            schema::PropertySchema{
                .id = kBiomeScatterMoistureMax,
                .name = "Moisture Maximum",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {.minimum = 0.0, .maximum = 1.0}
            },
            schema::PropertySchema{
                .id = kBiomeScatterMoistureFalloff,
                .name = "Moisture Falloff",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 0.0,
                .range = {.minimum = 0.0}
            },
            schema::PropertySchema{
                .id = kBiomeScatterScaleMin,
                .name = "Scale Minimum",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {.minimum = 0.001}
            },
            schema::PropertySchema{
                .id = kBiomeScatterScaleMax,
                .name = "Scale Maximum",
                .kind = schema::PropertyKind::Float,
                .defaultValue = 1.0,
                .range = {.minimum = 0.001}
            },
            schema::PropertySchema{
                .id = kBiomeScatterEnabled,
                .name = "Enabled",
                .kind = schema::PropertyKind::Boolean,
                .defaultValue = true
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
