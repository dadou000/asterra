#include <orbit/surface_model/SurfaceComposition.hpp>

#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <stdexcept>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace orbit::surface_model
{
namespace
{
template <typename Value>
[[nodiscard]] Value PropertyOr(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    Value fallback)
{
    const auto stored = objects.GetProperty(object, property);

    if (!stored.has_value())
    {
        return fallback;
    }

    const auto* value = std::get_if<Value>(&*stored);

    if (value == nullptr)
    {
        throw std::runtime_error(
            "Terrain surface property has an unexpected persisted type on object " +
            object.ToString() + ".");
    }

    return *value;
}

[[nodiscard]] u32 U32ProcessPropertyOr(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    const u32 fallback)
{
    const i64 stored =
        PropertyOr<i64>(
            objects,
            object,
            property,
            static_cast<i64>(fallback));

    if (stored < 0 ||
        static_cast<u64>(stored) >
            static_cast<u64>(
                std::numeric_limits<u32>::max()))
    {
        throw std::runtime_error(
            "Terrain process integer property is outside uint32 range.");
    }

    return static_cast<u32>(stored);
}

[[nodiscard]] TerrainProcessService ProcessDescription(
    const scene::ObjectStore& objects,
    const scene::ObjectId object)
{
    TerrainProcessService result{};

    result.streamPowerEnabled =
        PropertyOr<bool>(
            objects, object,
            world_model::kProcessStreamPowerEnabled,
            true);
    result.streamPower.iterations =
        U32ProcessPropertyOr(
            objects, object,
            world_model::kProcessStreamPowerIterations,
            result.streamPower.iterations);
    result.streamPower.incisionCoefficientMetersPerIteration =
        PropertyOr<f64>(
            objects, object,
            world_model::kProcessStreamPowerIncision,
            result.streamPower.incisionCoefficientMetersPerIteration);

    result.hydraulicEnabled =
        PropertyOr<bool>(
            objects, object,
            world_model::kProcessHydraulicEnabled,
            true);
    result.hydraulic.iterations =
        U32ProcessPropertyOr(
            objects, object,
            world_model::kProcessHydraulicIterations,
            result.hydraulic.iterations);
    result.hydraulic.rainfallMetersPerSecond =
        PropertyOr<f64>(
            objects, object,
            world_model::kProcessHydraulicRainfall,
            result.hydraulic.rainfallMetersPerSecond);
    result.hydraulic.timeStepSeconds =
        PropertyOr<f64>(
            objects, object,
            world_model::kProcessHydraulicTimeStep,
            result.hydraulic.timeStepSeconds);

    result.thermalEnabled =
        PropertyOr<bool>(
            objects, object,
            world_model::kProcessThermalEnabled,
            true);
    result.thermal.maximumIterations =
        U32ProcessPropertyOr(
            objects, object,
            world_model::kProcessThermalIterations,
            result.thermal.maximumIterations);
    result.thermal.relaxation =
        PropertyOr<f64>(
            objects, object,
            world_model::kProcessThermalRelaxation,
            result.thermal.relaxation);

    result.aeolianEnabled =
        PropertyOr<bool>(
            objects, object,
            world_model::kProcessAeolianEnabled,
            true);
    result.aeolian.iterations =
        U32ProcessPropertyOr(
            objects, object,
            world_model::kProcessAeolianIterations,
            result.aeolian.iterations);
    result.aeolian.capacityCoefficient =
        PropertyOr<f64>(
            objects, object,
            world_model::kProcessAeolianCapacity,
            result.aeolian.capacityCoefficient);
    result.aeolian.timeStepSeconds =
        PropertyOr<f64>(
            objects, object,
            world_model::kProcessAeolianTimeStep,
            result.aeolian.timeStepSeconds);

    result.glacialEnabled =
        PropertyOr<bool>(
            objects, object,
            world_model::kProcessGlacialEnabled,
            true);
    result.glacial.iterations =
        U32ProcessPropertyOr(
            objects, object,
            world_model::kProcessGlacialIterations,
            result.glacial.iterations);
    result.glacial.maximumGlacierTemperatureC =
        PropertyOr<f64>(
            objects, object,
            world_model::kProcessGlacialMaximumTemperature,
            result.glacial.maximumGlacierTemperatureC);
    result.glacial.timeStepYears =
        PropertyOr<f64>(
            objects, object,
            world_model::kProcessGlacialTimeStepYears,
            result.glacial.timeStepYears);

    result.riversEnabled =
        PropertyOr<bool>(
            objects, object,
            world_model::kProcessRiversEnabled,
            true);
    result.rivers.enableMeanders =
        PropertyOr<bool>(
            objects, object,
            world_model::kProcessRiverMeandersEnabled,
            result.rivers.enableMeanders);
    result.rivers.meanderIterations =
        U32ProcessPropertyOr(
            objects, object,
            world_model::kProcessRiverMeanderIterations,
            result.rivers.meanderIterations);
    result.rivers.enableCutoffs =
        PropertyOr<bool>(
            objects, object,
            world_model::kProcessRiverCutoffsEnabled,
            result.rivers.enableCutoffs);
    result.rivers.minimumDrainageAreaSquareMeters =
        PropertyOr<f64>(
            objects, object,
            world_model::kProcessRiverMinimumDrainageArea,
            result.rivers.minimumDrainageAreaSquareMeters);

    result.coastal.enabled =
        PropertyOr<bool>(
            objects, object,
            world_model::kProcessCoastalEnabled,
            result.coastal.enabled);
    result.coastal.hydrodynamicSteps =
        U32ProcessPropertyOr(
            objects, object,
            world_model::kProcessCoastalHydrodynamicSteps,
            result.coastal.hydrodynamicSteps);
    result.coastal.water.cflNumber =
        PropertyOr<f64>(
            objects, object,
            world_model::kProcessCoastalCflNumber,
            result.coastal.water.cflNumber);
    result.coastal.water.maximumTimeStepSeconds =
        PropertyOr<f64>(
            objects, object,
            world_model::kProcessCoastalMaximumTimeStep,
            result.coastal.water.maximumTimeStepSeconds);

    if (!result.IsValid())
    {
        throw std::runtime_error(
            "Terrain Process Settings failed physical solver validation.");
    }

    return result;
}

[[nodiscard]] terrain_biome::BiomeAuthoredMaskId BiomeMaskIdFor(
    const scene::ObjectId object) noexcept
{
    terrain_biome::BiomeAuthoredMaskId id{
        .high =
            object.high ^
            0x4d32304d41534b49ULL,
        .low =
            object.low ^
            0x444f524249540001ULL
    };

    if (!id.IsValid())
    {
        id.low = 1U;
    }

    return id;
}

[[nodiscard]] terrain_biome::BiomePlacementMode PlacementModeFor(
    const i64 value)
{
    if (value < 0 ||
        value > 2)
    {
        throw std::runtime_error(
            "Biome placement mode must be Automatic, Authored, or Automatic+Authored.");
    }

    return
        static_cast<
            terrain_biome::
                BiomePlacementMode>(
                    value);
}

[[nodiscard]] terrain_biome::BiomeSelectorField SelectorFieldFor(
    const i64 value)
{
    if (value < 0 ||
        value > 16)
    {
        throw std::runtime_error(
            "Biome selector field is outside the M20 field catalog.");
    }

    return
        static_cast<
            terrain_biome::
                BiomeSelectorField>(
                    value);
}

[[nodiscard]] terrain_biome::BiomeAuthoredWeightOperation MaskOperationFor(
    const i64 value)
{
    if (value < 0 ||
        value > 5)
    {
        throw std::runtime_error(
            "Biome authored mask operation is invalid.");
    }

    return
        static_cast<
            terrain_biome::
                BiomeAuthoredWeightOperation>(
                    value);
}

[[nodiscard]] terrain_biome::BiomeAutomaticSelector BiomeSelectorDescription(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& object)
{
    terrain_biome::BiomeAutomaticSelector selector{
        .field =
            SelectorFieldFor(
                PropertyOr<i64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSelectorField,
                    i64{0})),
        .minimum =
            PropertyOr<f64>(
                objects,
                object.id,
                world_model::
                    kBiomeSelectorMinimum,
                0.0),
        .maximum =
            PropertyOr<f64>(
                objects,
                object.id,
                world_model::
                    kBiomeSelectorMaximum,
                1.0),
        .lowerFalloff =
            PropertyOr<f64>(
                objects,
                object.id,
                world_model::
                    kBiomeSelectorLowerFalloff,
                0.0),
        .upperFalloff =
            PropertyOr<f64>(
                objects,
                object.id,
                world_model::
                    kBiomeSelectorUpperFalloff,
                0.0),
        .invert =
            PropertyOr<bool>(
                objects,
                object.id,
                world_model::
                    kBiomeSelectorInvert,
                false),
        .enabled =
            PropertyOr<bool>(
                objects,
                object.id,
                world_model::
                    kBiomeSelectorEnabled,
                true)
    };

    if (selector.field ==
        terrain_biome::
            BiomeSelectorField::
                GeologyMaterial)
    {
        const std::string text =
            PropertyOr<std::string>(
                objects,
                object.id,
                world_model::
                    kBiomeSelectorMaterial,
                {});

        const auto parsed =
            terrain_geology::
                RockTypeId::Parse(
                    text);

        if (!parsed.has_value())
        {
            throw std::runtime_error(
                "Biome geology selector requires a valid RockTypeId string.");
        }

        selector.material =
            *parsed;
    }

    if (selector.field ==
        terrain_biome::
            BiomeSelectorField::
                UserField)
    {
        const std::string name =
            PropertyOr<std::string>(
                objects,
                object.id,
                world_model::
                    kBiomeSelectorUserField,
                {});

        if (name.empty())
        {
            throw std::runtime_error(
                "Biome user-field selector requires a non-empty field name.");
        }

        selector.userField =
            terrain_biome::
                BiomeUserFieldIdFromName(
                    name);
    }

    if (!selector.IsValid())
    {
        throw std::runtime_error(
            "Biome automatic selector contains invalid M20 parameters on object " +
            object.id.ToString() +
            ".");
    }

    return selector;
}

[[nodiscard]] terrain_biome::BiomeAuthoredMask BiomeMaskDescription(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& object)
{
    terrain_biome::BiomeAuthoredMask mask{
        .id =
            BiomeMaskIdFor(
                object.id),
        .operation =
            MaskOperationFor(
                PropertyOr<i64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeMaskOperation,
                    i64{0})),
        .centerUnitDirection =
            PropertyOr<math::Double3>(
                objects,
                object.id,
                world_model::
                    kBiomeMaskCenter,
                {0.0, 1.0, 0.0}),
        .innerRadiusMeters =
            PropertyOr<f64>(
                objects,
                object.id,
                world_model::
                    kBiomeMaskInnerRadius,
                0.0),
        .outerRadiusMeters =
            PropertyOr<f64>(
                objects,
                object.id,
                world_model::
                    kBiomeMaskOuterRadius,
                1'000.0),
        .global =
            PropertyOr<bool>(
                objects,
                object.id,
                world_model::
                    kBiomeMaskGlobal,
                false),
        .value =
            PropertyOr<f64>(
                objects,
                object.id,
                world_model::
                    kBiomeMaskValue,
                1.0),
        .opacity =
            PropertyOr<f64>(
                objects,
                object.id,
                world_model::
                    kBiomeMaskOpacity,
                1.0),
        .enabled =
            PropertyOr<bool>(
                objects,
                object.id,
                world_model::
                    kBiomeMaskEnabled,
                true)
    };

    if (!mask.IsValid())
    {
        throw std::runtime_error(
            "Biome authored mask contains invalid M20 parameters on object " +
            object.id.ToString() +
            ".");
    }

    return mask;
}

[[nodiscard]] terrain_biome::BiomeSurfaceLayerKind SurfaceLayerKindFor(
    const i64 value)
{
    if (value < 0 ||
        value > 3)
    {
        throw std::runtime_error(
            "Biome surface layer kind is outside the M21 layer catalog.");
    }

    return
        static_cast<
            terrain_biome::
                BiomeSurfaceLayerKind>(
                    value);
}

[[nodiscard]] terrain_biome::BiomeSurfaceLayerRule BiomeSurfaceLayerDescription(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& object)
{
    const i64 compatibility =
        PropertyOr<i64>(
            objects,
            object.id,
            world_model::
                kBiomeSurfaceLayerCompatibility,
            i64{31});

    if (compatibility < 1 ||
        compatibility > 31)
    {
        throw std::runtime_error(
            "Biome surface layer compatibility mask must be in [1,31].");
    }

    terrain_biome::BiomeSurfaceLayerRule layer{
        .kind =
            SurfaceLayerKindFor(
                PropertyOr<i64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSurfaceLayerKind,
                    i64{3})),
        .strength =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSurfaceLayerStrength,
                    1.0)),
        .compatibleExposed =
            static_cast<
                terrain_biome::
                    BiomeExposedMaterialMask>(
                        static_cast<u32>(
                            compatibility)),
        .minimumSlopeDegrees =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSurfaceLayerSlopeMin,
                    0.0)),
        .maximumSlopeDegrees =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSurfaceLayerSlopeMax,
                    90.0)),
        .slopeFalloffDegrees =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSurfaceLayerSlopeFalloff,
                    0.0)),
        .minimumCurvature =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSurfaceLayerCurvatureMin,
                    -1.0)),
        .maximumCurvature =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSurfaceLayerCurvatureMax,
                    1.0)),
        .curvatureFalloff =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSurfaceLayerCurvatureFalloff,
                    0.0)),
        .minimumMoisture =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSurfaceLayerMoistureMin,
                    0.0)),
        .maximumMoisture =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSurfaceLayerMoistureMax,
                    1.0)),
        .moistureFalloff =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::
                        kBiomeSurfaceLayerMoistureFalloff,
                    0.0)),
        .enabled =
            PropertyOr<bool>(
                objects,
                object.id,
                world_model::
                    kBiomeSurfaceLayerEnabled,
                true)
    };

    if (!layer.IsValid())
    {
        throw std::runtime_error(
            "Biome surface layer contains invalid M21 parameters on object " +
            object.id.ToString() +
            ".");
    }

    return layer;
}

[[nodiscard]] terrain_biome::BiomeScatterRuleId BiomeScatterRuleIdFor(
    const scene::ObjectId object) noexcept
{
    terrain_biome::BiomeScatterRuleId id{
        .high =
            object.high ^
            0x4d32325343415449ULL,
        .low =
            object.low ^
            0x4f52424954000001ULL
    };

    if (!id.IsValid())
    {
        id.low = 1U;
    }

    return id;
}

[[nodiscard]] terrain_biome::BiomeScatterKind ScatterKindFor(
    const i64 value)
{
    if (value < 0 ||
        value > 5)
    {
        throw std::runtime_error(
            "Biome scatter kind is outside the M22 scatter catalog.");
    }

    return static_cast<terrain_biome::BiomeScatterKind>(value);
}

[[nodiscard]] terrain_biome::BiomeScatterLayerRule BiomeScatterRuleDescription(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& object)
{
    const i64 compatibility =
        PropertyOr<i64>(
            objects,
            object.id,
            world_model::kBiomeScatterCompatibility,
            i64{31});

    const i64 seedSalt =
        PropertyOr<i64>(
            objects,
            object.id,
            world_model::kBiomeScatterSeedSalt,
            i64{0});

    if (compatibility < 1 ||
        compatibility > 31 ||
        seedSalt < 0 ||
        seedSalt >
            static_cast<i64>(
                (std::numeric_limits<u32>::max)()))
    {
        throw std::runtime_error(
            "Biome scatter rule has invalid compatibility or seed salt.");
    }

    terrain_biome::BiomeScatterLayerRule rule{
        .id = BiomeScatterRuleIdFor(object.id),
        .kind =
            ScatterKindFor(
                PropertyOr<i64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterKind,
                    i64{2})),
        .densityPerSquareMeter =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterDensity,
                    0.01)),
        .minimumSpacingMeters =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterSpacing,
                    1.0)),
        .seedSalt = static_cast<u32>(seedSalt),
        .compatibleExposed =
            static_cast<terrain_biome::BiomeExposedMaterialMask>(
                static_cast<u32>(compatibility)),
        .requiresSoil =
            PropertyOr<bool>(
                objects,
                object.id,
                world_model::kBiomeScatterRequiresSoil,
                false),
        .minimumSoilDepthMeters =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterMinimumSoilDepth,
                    0.0)),
        .minimumSlopeDegrees =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterSlopeMin,
                    0.0)),
        .maximumSlopeDegrees =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterSlopeMax,
                    90.0)),
        .slopeFalloffDegrees =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterSlopeFalloff,
                    0.0)),
        .minimumMoisture =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterMoistureMin,
                    0.0)),
        .maximumMoisture =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterMoistureMax,
                    1.0)),
        .moistureFalloff =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterMoistureFalloff,
                    0.0)),
        .minimumScale =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterScaleMin,
                    1.0)),
        .maximumScale =
            static_cast<f32>(
                PropertyOr<f64>(
                    objects,
                    object.id,
                    world_model::kBiomeScatterScaleMax,
                    1.0)),
        .enabled =
            PropertyOr<bool>(
                objects,
                object.id,
                world_model::kBiomeScatterEnabled,
                true)
    };

    if (!rule.IsValid())
    {
        throw std::runtime_error(
            "Biome scatter rule contains invalid M22 parameters on object " +
            object.id.ToString() +
            ".");
    }

    return rule;
}

[[nodiscard]] terrain_biome::BiomeId BiomeIdFor(
    const scene::ObjectId object) noexcept
{
    terrain_biome::BiomeId id{
        .high =
            object.high ^
            0x42494f4d45415353ULL,
        .low =
            object.low ^
            0x455456303030344dULL
    };

    if (!id.IsValid())
    {
        id.low = 1U;
    }

    return id;
}

[[nodiscard]] terrain_biome::BiomeDefinition BiomeDescription(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& object)
{
    terrain_biome::BiomeDefinition result{
        .id = BiomeIdFor(
            object.id),
        .name = object.name,
        .placement = {
            .minimumResolvedWeight =
                static_cast<f32>(
                    PropertyOr<f64>(
                        objects,
                        object.id,
                        world_model::kBiomeMinimumResolvedWeight,
                        0.05)),
            .enabled = true,
            .mode =
                PlacementModeFor(
                    PropertyOr<i64>(
                        objects,
                        object.id,
                        world_model::
                            kBiomePlacementMode,
                        i64{0}))
        },
        .surface = {
            .materialInfluence =
                static_cast<f32>(
                    PropertyOr<f64>(
                        objects,
                        object.id,
                        world_model::kBiomeMaterialInfluence,
                        1.0))
        },
        .scatter = {
            .densityMultiplier =
                static_cast<f32>(
                    PropertyOr<f64>(
                        objects,
                        object.id,
                        world_model::kBiomeScatterDensityMultiplier,
                        1.0))
        },
        .processModifiers = {
            .hydraulicErosion =
                static_cast<f32>(
                    PropertyOr<f64>(
                        objects,
                        object.id,
                        world_model::kBiomeHydraulicErosionMultiplier,
                        1.0)),
            .thermalTransport =
                static_cast<f32>(
                    PropertyOr<f64>(
                        objects,
                        object.id,
                        world_model::kBiomeThermalTransportMultiplier,
                        1.0)),
            .aeolianTransport =
                static_cast<f32>(
                    PropertyOr<f64>(
                        objects,
                        object.id,
                        world_model::kBiomeAeolianTransportMultiplier,
                        1.0)),
            .glacialErosion =
                static_cast<f32>(
                    PropertyOr<f64>(
                        objects,
                        object.id,
                        world_model::kBiomeGlacialErosionMultiplier,
                        1.0)),
            .coastalErosion =
                static_cast<f32>(
                    PropertyOr<f64>(
                        objects,
                        object.id,
                        world_model::kBiomeCoastalErosionMultiplier,
                        1.0)),
            .chemicalWeathering =
                static_cast<f32>(
                    PropertyOr<f64>(
                        objects,
                        object.id,
                        world_model::kBiomeChemicalWeatheringMultiplier,
                        1.0))
        }
    };

    for (const auto& child :
         objects.Children(
             object.id))
    {
        if (child.type ==
            world_model::
                kBiomeSelectorType)
        {
            result.placement.
                selectors.push_back(
                    BiomeSelectorDescription(
                        objects,
                        child));
        }
        else if (child.type ==
            world_model::
                kBiomeAuthoredMaskType)
        {
            result.placement.
                authoredMasks.push_back(
                    BiomeMaskDescription(
                        objects,
                        child));
        }
        else if (child.type ==
            world_model::
                kBiomeSurfaceLayerType)
        {
            result.surface.
                layers.push_back(
                    BiomeSurfaceLayerDescription(
                        objects,
                        child));
        }
        else if (child.type ==
            world_model::
                kBiomeScatterRuleType)
        {
            result.scatter.
                layers.push_back(
                    BiomeScatterRuleDescription(
                        objects,
                        child));
        }
    }

    if (!result.IsValid())
    {
        throw std::runtime_error(
            "Biome Asset contains invalid M19/M20 rules on object " +
            object.id.ToString() +
            ".");
    }

    return result;
}

[[nodiscard]] surface_authoring::ConstraintCompositionMode
ConstraintModeFor(const i64 value)
{
    if (value < 0 || value > 5)
        throw std::runtime_error("Terrain constraint composition mode is invalid.");
    return static_cast<surface_authoring::ConstraintCompositionMode>(value);
}

[[nodiscard]] surface_authoring::TerrainConstraintPrimitive
ConstraintPrimitiveFor(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& object)
{
    const i64 shape=PropertyOr<i64>(
        objects,object.id,world_model::kTerrainConstraintShape,i64{0});

    if(shape==0)
    {
        auto center=PropertyOr<math::Double3>(
            objects,object.id,world_model::kTerrainConstraintCenter,{0.0,1.0,0.0});
        if(math::LengthSquared(center)<=1.0e-20)
            throw std::runtime_error("Terrain brush has an invalid center direction.");
        center=math::Normalize(center);
        return surface_authoring::BrushConstraintPrimitive{
            .centerUnitDirection=center,
            .innerRadiusMeters=PropertyOr<f64>(
                objects,object.id,world_model::kTerrainConstraintInnerRadius,250.0),
            .outerRadiusMeters=PropertyOr<f64>(
                objects,object.id,world_model::kTerrainConstraintOuterRadius,1'000.0)
        };
    }

    if(shape==1)
    {
        auto children=objects.Children(object.id);
        std::sort(children.begin(),children.end(),
            [](const scene::ObjectRecord& a,const scene::ObjectRecord& b){
                return a.sortOrder<b.sortOrder;
            });

        std::vector<math::Double3> points;
        for(const auto& child:children)
        {
            if(child.type!=world_model::kTerrainConstraintControlPointType) continue;
            auto direction=PropertyOr<math::Double3>(
                objects,child.id,world_model::kTerrainConstraintPointDirection,{0.0,1.0,0.0});
            if(math::LengthSquared(direction)<=1.0e-20)
                throw std::runtime_error("Terrain spline control point has an invalid direction.");
            points.push_back(math::Normalize(direction));
        }

        return surface_authoring::SplineConstraintPrimitive{
            .controlUnitDirections=std::move(points),
            .halfWidthMeters=PropertyOr<f64>(
                objects,object.id,world_model::kTerrainConstraintHalfWidth,500.0),
            .falloffMeters=PropertyOr<f64>(
                objects,object.id,world_model::kTerrainConstraintFalloff,500.0)
        };
    }

    throw std::runtime_error("Terrain constraint primitive is outside the M09 catalog.");
}

[[nodiscard]] surface_authoring::TerrainConstraintSet
ConstraintSetFor(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& terrainObject,
    const world::PlanetId planet)
{
    surface_authoring::TerrainConstraintSet result{
        .id={.high=terrainObject.id.high,.low=terrainObject.id.low},
        .planet=planet,
        .name=terrainObject.name+" Constraints"
    };

    for(const auto& child:objects.Children(terrainObject.id))
    {
        if(child.type!=world_model::kTerrainConstraintType) continue;

        const i64 channel=PropertyOr<i64>(
            objects,child.id,world_model::kTerrainConstraintChannel,i64{0});
        if(channel<0||channel>3)
            throw std::runtime_error("Terrain constraint channel is outside the M09 catalog.");

        const auto id=surface_authoring::TerrainConstraintId{
            .high=child.id.high,.low=child.id.low};
        const auto mode=ConstraintModeFor(PropertyOr<i64>(
            objects,child.id,world_model::kTerrainConstraintMode,i64{0}));
        auto primitive=ConstraintPrimitiveFor(objects,child);
        const f64 value=PropertyOr<f64>(
            objects,child.id,world_model::kTerrainConstraintValue,0.0);
        const f64 opacity=PropertyOr<f64>(
            objects,child.id,world_model::kTerrainConstraintOpacity,1.0);
        const bool enabled=PropertyOr<bool>(
            objects,child.id,world_model::kTerrainConstraintEnabled,true);

        if(channel==3)
        {
            const auto material=terrain_geology::RockTypeId::Parse(
                PropertyOr<std::string>(
                    objects,child.id,world_model::kTerrainConstraintMaterial,{}));
            if(!material.has_value())
                throw std::runtime_error("Terrain geology override requires a valid RockTypeId.");
            result.material.constraints.push_back({
                .id=id,.mode=mode,.primitive=std::move(primitive),
                .material=*material,.weight=value,.opacity=opacity,.enabled=enabled
            });
            continue;
        }

        surface_authoring::ScalarTerrainConstraint scalar{
            .id=id,.mode=mode,.primitive=std::move(primitive),
            .value=value,.opacity=opacity,.enabled=enabled
        };
        if(channel==0) result.height.constraints.push_back(std::move(scalar));
        else if(channel==1) result.protection.constraints.push_back(std::move(scalar));
        else result.drainage.constraints.push_back(std::move(scalar));
    }

    if(!result.IsValid())
        throw std::runtime_error("Semantic terrain constraints did not compose into a valid M04 authority set.");
    return result;
}

[[nodiscard]] u32 ConstraintCount(
    const surface_authoring::TerrainConstraintSet& set) noexcept
{
    return static_cast<u32>(
        set.height.constraints.size()+set.gradient.constraints.size()+
        set.uplift.constraints.size()+set.material.constraints.size()+
        set.protection.constraints.size()+set.drainage.constraints.size());
}

[[nodiscard]] terrain::AnalyticTerrainDesc TerrainDescription(
    const scene::ObjectStore& objects,
    const scene::ObjectId object)
{
    terrain::AnalyticTerrainDesc result;

    const i64 seed = PropertyOr<i64>(
        objects,
        object,
        world_model::kTerrainSeed,
        i64{0x41535445525241LL});
    const i64 octaves = PropertyOr<i64>(
        objects,
        object,
        world_model::kTerrainDetailOctaves,
        i64{10});

    if (seed < 0)
    {
        throw std::runtime_error(
            "Terrain seed must be non-negative on object " +
            object.ToString() + ".");
    }

    if (octaves < 1 || octaves > 16)
    {
        throw std::runtime_error(
            "Terrain detail octave count must be between 1 and 16 on object " +
            object.ToString() + ".");
    }

    result.seed = static_cast<u64>(seed);
    result.macroAmplitudeMeters = PropertyOr<f64>(
        objects,
        object,
        world_model::kTerrainMacroAmplitudeMeters,
        1'200.0);
    result.macroWavelengthMeters = PropertyOr<f64>(
        objects,
        object,
        world_model::kTerrainMacroWavelengthMeters,
        800'000.0);
    result.detailAmplitudeMeters = PropertyOr<f64>(
        objects,
        object,
        world_model::kTerrainDetailAmplitudeMeters,
        320.0);
    result.detailWavelengthMeters = PropertyOr<f64>(
        objects,
        object,
        world_model::kTerrainDetailWavelengthMeters,
        40'000.0);
    result.detailOctaves = static_cast<u32>(octaves);
    result.maximumElevationAboveSeaLevelMeters = PropertyOr<f64>(
        objects,
        object,
        world_model::kTerrainMaximumElevationMeters,
        8'000.0);
    result.craters.enabled = PropertyOr<bool>(
        objects, object, world_model::kTerrainCratersEnabled, true);
    const i64 craterCount = PropertyOr<i64>(
        objects, object, world_model::kTerrainCraterCount, i64{96});
    if (craterCount < 0 || craterCount > 4096)
    {
        throw std::runtime_error(
            "Terrain crater count must be between 0 and 4096 on object " +
            object.ToString() + ".");
    }
    result.craters.count = static_cast<u32>(craterCount);
    result.craters.minimumRadiusMeters = PropertyOr<f64>(
        objects, object, world_model::kTerrainCraterMinimumRadiusMeters,
        4'000.0);
    result.craters.maximumRadiusMeters = PropertyOr<f64>(
        objects, object, world_model::kTerrainCraterMaximumRadiusMeters,
        280'000.0);
    result.craters.cumulativeExponent = PropertyOr<f64>(
        objects, object, world_model::kTerrainCraterCumulativeExponent,
        1.8);
    result.craters.complexTransitionRadiusMeters = PropertyOr<f64>(
        objects, object, world_model::kTerrainComplexCraterRadiusMeters,
        18'000.0);

    if (result.macroAmplitudeMeters < 0.0 ||
        result.detailAmplitudeMeters < 0.0 ||
        result.macroWavelengthMeters <= 0.0 ||
        result.detailWavelengthMeters <= 0.0 ||
        result.maximumElevationAboveSeaLevelMeters <= 0.0)
    {
        throw std::runtime_error(
            "Terrain surface recipe contains invalid dimensions on object " +
            object.ToString() + ".");
    }

    return result;
}
} // namespace

SurfaceCompositionStats SurfaceComposition::Rebuild(
    const scene::ObjectStore& objects,
    const world_model::UniverseComposition& universe)
{
    // UniverseComposition replaces BodyRegistry storage during a rebuild.
    // Drop every reference to the previous registry before validating the
    // replacement capability graph; a failed terrain recipe therefore leaves
    // the surface layer unavailable rather than dangling into freed bodies.
    registry_.reset();
    bodyByTerrainObject_.clear();
    terrainObjectByBody_.clear();
    biomeByObject_.clear();

    // Derived per-body services keep stable lifetime across semantic
    // recomposition for a stable BodyId. This preserves M26 residency and gives
    // M27 a durable cache target while authored policy below is rebuilt.
    auto previousServices =
        std::move(servicesByBody_);
    servicesByBody_.clear();

    constraintsByBody_.clear();
    sourceRevision_ = ~u64{0};

    auto candidate =
        std::make_unique<surface::SurfaceRegistry>(
            universe.Bodies());
    std::unordered_map<scene::ObjectId, universe::BodyId>
        candidateBodyByObject;
    std::unordered_map<universe::BodyId, scene::ObjectId>
        candidateObjectByBody;
    std::unordered_map<
        scene::ObjectId,
        terrain_biome::BiomeId>
        candidateBiomeByObject;
    std::unordered_map<
        universe::BodyId,
        std::unique_ptr<TerrainBodyServices>>
        candidateServices;
    std::unordered_map<
        universe::BodyId,
        surface_authoring::TerrainConstraintSet>
        candidateConstraints;

    std::vector<scene::ObjectRecord> pending = objects.Roots();
    u32 terrainCount = 0;
    u32 biomeDefinitionCount = 0;
    u32 terrainConstraintCount = 0;

    while (!pending.empty())
    {
        const auto object = pending.back();
        pending.pop_back();

        auto children = objects.Children(object.id);
        pending.insert(
            pending.end(),
            children.begin(),
            children.end());

        if (object.type != world_model::kTerrainSurfaceType)
        {
            continue;
        }

        if (!object.parent.has_value())
        {
            throw std::runtime_error(
                "Terrain Surface must be parented to a Celestial Body.");
        }

        const auto parent = objects.Find(*object.parent);

        if (!parent.has_value() ||
            parent->type != world_model::kCelestialBodyType)
        {
            throw std::runtime_error(
                "Terrain Surface parent must be a Celestial Body.");
        }

        const auto bodyId = universe.BodyForObject(parent->id);

        if (!bodyId.has_value())
        {
            throw std::runtime_error(
                "Terrain Surface parent is not present in UniverseComposition.");
        }

        if (candidateObjectByBody.contains(*bodyId))
        {
            throw std::runtime_error(
                "A Celestial Body may own only one Terrain Surface capability.");
        }

        const auto planet = candidate->SphericalPlanetDefinition(*bodyId);

        if (!planet.has_value())
        {
            throw std::runtime_error(
                "Analytic terrain currently requires a spherical Celestial Body; ellipsoid terrain must use a future ellipsoid-aware terrain source.");
        }

        auto source =
            std::make_shared<terrain::AnalyticTerrainSource>(
                *planet,
                TerrainDescription(objects, object.id));

        candidate->AttachTerrain(*bodyId, std::move(source));
        candidateBodyByObject.emplace(object.id, *bodyId);
        candidateObjectByBody.emplace(*bodyId, object.id);

        std::unique_ptr<TerrainBodyServices>
            services;

        if (auto previous =
                previousServices.find(
                    *bodyId);
            previous !=
                previousServices.end())
        {
            services =
                std::move(
                    previous->second);
            previousServices.erase(
                previous);
        }
        else
        {
            services =
                std::make_unique<
                    TerrainBodyServices>(
                        *bodyId);
        }

        // Semantic policy is reconstructed from ObjectStore authority while
        // runtime-only cache/water state remains owned by the stable service.
        services->Processes() =
            TerrainProcessService{};
        services->Biomes() =
            terrain_biome::BiomeService(
                *bodyId);

        std::optional<scene::ObjectId>
            processObject;

        for (const auto& child :
             objects.Children(object.id))
        {
            if (child.type !=
                world_model::kTerrainProcessAssetType)
            {
                continue;
            }

            if (processObject.has_value())
            {
                throw std::runtime_error(
                    "Terrain Surface may own only one Terrain Process Settings record.");
            }

            processObject = child.id;
        }

        if (processObject.has_value())
        {
            services->Processes() =
                ProcessDescription(
                    objects,
                    *processObject);
        }

        ++biomeDefinitionCount; // implicit, non-removable BaseBiome

        for (const auto& child : objects.Children(object.id))
        {
            if (child.type != world_model::kBiomeAssetType)
            {
                continue;
            }

            auto biome =
                BiomeDescription(
                    objects,
                    child);

            candidateBiomeByObject.emplace(
                child.id,
                biome.id);

            services->Biomes().UpsertBiome(
                std::move(biome));

            ++biomeDefinitionCount;
        }

        if (!services->IsValid())
        {
            throw std::runtime_error(
                "Rocky terrain body services failed default validation.");
        }

        auto constraints=
            ConstraintSetFor(
                objects,
                object,
                planet->id);
        terrainConstraintCount +=
            ConstraintCount(constraints);
        candidateConstraints.emplace(
            *bodyId,
            std::move(constraints));

        candidateServices.emplace(
            *bodyId,
            std::move(
                services));

        ++terrainCount;
    }

    registry_ = std::move(candidate);
    bodyByTerrainObject_ = std::move(candidateBodyByObject);
    terrainObjectByBody_ = std::move(candidateObjectByBody);
    biomeByObject_ = std::move(candidateBiomeByObject);
    servicesByBody_ = std::move(candidateServices);
    constraintsByBody_ =
        std::move(candidateConstraints);
    sourceRevision_ = objects.Revision();

    return {
        .terrainSurfaces = terrainCount,
        .biomeServices =
            static_cast<u32>(
                servicesByBody_.size()),
        .biomeDefinitions =
            biomeDefinitionCount,
        .terrainConstraints =
            terrainConstraintCount,
        .sourceRevision = sourceRevision_
    };
}

surface::SurfaceRegistry& SurfaceComposition::Registry()
{
    if (registry_ == nullptr)
    {
        throw std::logic_error(
            "SurfaceComposition has not been built for an active universe.");
    }

    return *registry_;
}

const surface::SurfaceRegistry& SurfaceComposition::Registry() const
{
    if (registry_ == nullptr)
    {
        throw std::logic_error(
            "SurfaceComposition has not been built for an active universe.");
    }

    return *registry_;
}

std::optional<universe::BodyId>
SurfaceComposition::BodyForTerrainObject(
    const scene::ObjectId object) const noexcept
{
    const auto found = bodyByTerrainObject_.find(object);
    return found != bodyByTerrainObject_.end()
        ? std::optional<universe::BodyId>(found->second)
        : std::nullopt;
}

std::optional<scene::ObjectId>
SurfaceComposition::TerrainObjectForBody(
    const universe::BodyId body) const noexcept
{
    const auto found = terrainObjectByBody_.find(body);
    return found != terrainObjectByBody_.end()
        ? std::optional<scene::ObjectId>(found->second)
        : std::nullopt;
}

std::optional<terrain_biome::BiomeId>
SurfaceComposition::BiomeForObject(
    const scene::ObjectId object) const noexcept
{
    const auto found =
        biomeByObject_.find(object);

    return found != biomeByObject_.end()
        ? std::optional<terrain_biome::BiomeId>(
              found->second)
        : std::nullopt;
}

TerrainBodyServices*
SurfaceComposition::ServicesForBody(
    const universe::BodyId body) noexcept
{
    const auto found =
        servicesByBody_.find(
            body);

    return
        found != servicesByBody_.end()
            ? found->second.get()
            : nullptr;
}

const TerrainBodyServices*
SurfaceComposition::ServicesForBody(
    const universe::BodyId body) const noexcept
{
    const auto found =
        servicesByBody_.find(
            body);

    return
        found != servicesByBody_.end()
            ? found->second.get()
            : nullptr;
}

terrain_geology::GeologicalMaterialLibrary*
SurfaceComposition::GeologyForBody(
    const universe::BodyId body) noexcept
{
    auto* services =
        ServicesForBody(body);

    return
        services != nullptr
            ? &services->Geology()
            : nullptr;
}

const terrain_geology::GeologicalMaterialLibrary*
SurfaceComposition::GeologyForBody(
    const universe::BodyId body) const noexcept
{
    const auto* services =
        ServicesForBody(body);

    return
        services != nullptr
            ? &services->Geology()
            : nullptr;
}

TerrainProcessService*
SurfaceComposition::ProcessesForBody(
    const universe::BodyId body) noexcept
{
    auto* services =
        ServicesForBody(body);

    return
        services != nullptr
            ? &services->Processes()
            : nullptr;
}

const TerrainProcessService*
SurfaceComposition::ProcessesForBody(
    const universe::BodyId body) const noexcept
{
    const auto* services =
        ServicesForBody(body);

    return
        services != nullptr
            ? &services->Processes()
            : nullptr;
}

terrain_biome::BiomeService*
SurfaceComposition::BiomesForBody(
    const universe::BodyId body) noexcept
{
    auto* services =
        ServicesForBody(body);

    return
        services != nullptr
            ? &services->Biomes()
            : nullptr;
}

const terrain_biome::BiomeService*
SurfaceComposition::BiomesForBody(
    const universe::BodyId body) const noexcept
{
    const auto* services =
        ServicesForBody(body);

    return
        services != nullptr
            ? &services->Biomes()
            : nullptr;
}

terrain_water::WaterService*
SurfaceComposition::WaterForBody(
    const universe::BodyId body) noexcept
{
    auto* services = ServicesForBody(body);
    return services != nullptr ? &services->Water() : nullptr;
}

const terrain_water::WaterService*
SurfaceComposition::WaterForBody(
    const universe::BodyId body) const noexcept
{
    const auto* services = ServicesForBody(body);
    return services != nullptr ? &services->Water() : nullptr;
}

const surface_authoring::TerrainConstraintSet*
SurfaceComposition::ConstraintsForBody(
    const universe::BodyId body) const noexcept
{
    const auto found=constraintsByBody_.find(body);
    return found!=constraintsByBody_.end()
        ? &found->second
        : nullptr;
}

terrain_gpu::PersistentGpuTerrainCache*
SurfaceComposition::CacheForBody(
    const universe::BodyId body) noexcept
{
    auto* services =
        ServicesForBody(body);

    return
        services != nullptr
            ? &services->Cache()
            : nullptr;
}

const terrain_gpu::PersistentGpuTerrainCache*
SurfaceComposition::CacheForBody(
    const universe::BodyId body) const noexcept
{
    const auto* services =
        ServicesForBody(body);

    return
        services != nullptr
            ? &services->Cache()
            : nullptr;
}

u64 SurfaceComposition::SourceRevision() const noexcept
{
    return sourceRevision_;
}
} // namespace orbit::surface_model
