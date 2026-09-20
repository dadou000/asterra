#pragma once

#include <orbit/schema/SchemaRegistry.hpp>

namespace orbit::world_model
{
inline constexpr schema::TypeId kWorldType{
    .high = 0x4f52424954574f52ULL,
    .low = 0x4c44545950450001ULL
};

inline constexpr schema::TypeId kCelestialSystemType{
    .high = 0x4f52424954535953ULL,
    .low = 0x54454d5459500001ULL
};

inline constexpr schema::TypeId kCelestialBodyType{
    .high = 0x4f52424954424f44ULL,
    .low = 0x5954595045000001ULL
};

// Real body capability record. The semantic child is CPU authority; the
// surface_model composition layer reconstructs SurfaceRegistry +
// AnalyticTerrainSource from these properties.
inline constexpr schema::TypeId kTerrainSurfaceType{
    .high = 0x4f52424954544552ULL,
    .low = 0x5241494e00000001ULL
};

// V0.0.4 permanent authored surface asset identities. These are schema type
// IDs, not runtime/cache handles: instances live in the semantic document and
// therefore inherit the V0.0.3 command/undo/persistence authority path.
inline constexpr schema::TypeId kGeologyAssetType{
    .high = 0x4f5242495447454fULL,
    .low = 0x4c4f475941535431ULL
};

inline constexpr schema::TypeId kTerrainProcessAssetType{
    .high = 0x4f52424954505243ULL,
    .low = 0x4553534153543031ULL
};

inline constexpr schema::TypeId kWaterServiceType{
    .high = 0x4f52424954574154ULL,
    .low = 0x4552534552563031ULL
};

inline constexpr schema::TypeId kWaterSourceType{
    .high = 0x4f52424954574154ULL,
    .low = 0x4552534f55523031ULL
};

inline constexpr schema::TypeId kWaterBarrierType{
    .high = 0x4f52424954574154ULL,
    .low = 0x4552424152523031ULL
};

inline constexpr schema::TypeId kWaterDomainType{
    .high = 0x4f52424954574154ULL,
    .low = 0x4552444f4d413031ULL
};

inline constexpr schema::TypeId kWaterEmitterType{
    .high = 0x4f52424954574154ULL,
    .low = 0x4552454d49543031ULL
};

inline constexpr schema::TypeId kBiomeAssetType{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45415353455431ULL
};

inline constexpr schema::TypeId kBiomeSelectorType{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d4553454c454331ULL
};

inline constexpr schema::TypeId kBiomeAuthoredMaskType{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d454d41534b3031ULL
};

inline constexpr schema::TypeId kBiomeSurfaceLayerType{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45535552463031ULL
};

inline constexpr schema::TypeId kBiomeScatterRuleType{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534341543031ULL
};

inline constexpr schema::TypeId kSurfaceDecalType{
    .high = 0x4f52424954444543ULL,
    .low = 0x414c545950450001ULL
};

inline constexpr schema::PropertyId kSystemEpochMicroseconds{
    .high = 0x4f5242495450524fULL,
    .low = 0x505345504f434801ULL
};

inline constexpr schema::PropertyId kBodyEllipsoidEnabled{
    .high = 0x4f5242495450524fULL,
    .low = 0x50454c4c49505301ULL
};

inline constexpr schema::PropertyId kBodyRadius{
    .high = 0x4f5242495450524fULL,
    .low = 0x5052414449555301ULL
};

inline constexpr schema::PropertyId kBodyPolarRadius{
    .high = 0x4f5242495450524fULL,
    .low = 0x50504f4c41520001ULL
};

inline constexpr schema::PropertyId kBodyMass{
    .high = 0x4f5242495450524fULL,
    .low = 0x504d415353000001ULL
};

inline constexpr schema::PropertyId kBodyParentPositionMeters{
    .high = 0x4f5242495450524fULL,
    .low = 0x50504152454e5401ULL
};

inline constexpr schema::PropertyId kBodyRotationPeriodSeconds{
    .high = 0x4f5242495450524fULL,
    .low = 0x50524f5450455201ULL
};

inline constexpr schema::PropertyId kBodyAxialTiltDegrees{
    .high = 0x4f5242495450524fULL,
    .low = 0x5054494c54444501ULL
};

inline constexpr schema::PropertyId kBodyRotationPhaseDegrees{
    .high = 0x4f5242495450524fULL,
    .low = 0x5050484153450001ULL
};

inline constexpr schema::PropertyId kBodyMaterialAsset{
    .high = 0x4f5242495450524fULL,
    .low = 0x504d41544c000001ULL
};

inline constexpr schema::PropertyId kTerrainSeed{
    .high = 0x4f52424954544552ULL,
    .low = 0x5241494e53454544ULL
};

inline constexpr schema::PropertyId kTerrainMacroAmplitudeMeters{
    .high = 0x4f52424954544552ULL,
    .low = 0x4d4143524f414d50ULL
};

inline constexpr schema::PropertyId kTerrainMacroWavelengthMeters{
    .high = 0x4f52424954544552ULL,
    .low = 0x4d4143524f574156ULL
};

inline constexpr schema::PropertyId kTerrainDetailAmplitudeMeters{
    .high = 0x4f52424954544552ULL,
    .low = 0x44455441494c414dULL
};

inline constexpr schema::PropertyId kTerrainDetailWavelengthMeters{
    .high = 0x4f52424954544552ULL,
    .low = 0x44455441494c5741ULL
};

inline constexpr schema::PropertyId kTerrainDetailOctaves{
    .high = 0x4f52424954544552ULL,
    .low = 0x44455441494c4f43ULL
};

inline constexpr schema::PropertyId kTerrainMaximumElevationMeters{
    .high = 0x4f52424954544552ULL,
    .low = 0x4d4158454c455641ULL
};

inline constexpr schema::PropertyId kWaterOceanEnabled{
    .high = 0x4f52424954574154ULL,
    .low = 0x45524f43454e4142ULL
};

inline constexpr schema::PropertyId kWaterOceanDatumMeters{
    .high = 0x4f52424954574154ULL,
    .low = 0x45524f4344415455ULL
};

inline constexpr schema::PropertyId kWaterFluidAsset{
    .high = 0x4f52424954574154ULL,
    .low = 0x4552464c55494431ULL
};

inline constexpr schema::PropertyId kWaterEnabled{
    .high = 0x4f52424954574154ULL,
    .low = 0x4552454e41424c45ULL
};

inline constexpr schema::PropertyId kBiomePlacementMode{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45504c41434501ULL
};

inline constexpr schema::PropertyId kBiomeSelectorField{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534649454c01ULL
};

inline constexpr schema::PropertyId kBiomeSelectorMinimum{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534d494e0001ULL
};

inline constexpr schema::PropertyId kBiomeSelectorMaximum{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534d41580001ULL
};

inline constexpr schema::PropertyId kBiomeSelectorLowerFalloff{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c46414c01ULL
};

inline constexpr schema::PropertyId kBiomeSelectorUpperFalloff{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45535546414c01ULL
};

inline constexpr schema::PropertyId kBiomeSelectorMaterial{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534d41544c01ULL
};

inline constexpr schema::PropertyId kBiomeSelectorUserField{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45535553455201ULL
};

inline constexpr schema::PropertyId kBiomeSelectorInvert{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d4553494e564501ULL
};

inline constexpr schema::PropertyId kBiomeSelectorEnabled{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d4553454e414201ULL
};

inline constexpr schema::PropertyId kBiomeMaskOperation{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d454d4f50455201ULL
};

inline constexpr schema::PropertyId kBiomeMaskCenter{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d454d43454e5401ULL
};

inline constexpr schema::PropertyId kBiomeMaskInnerRadius{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d454d494e4e5201ULL
};

inline constexpr schema::PropertyId kBiomeMaskOuterRadius{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d454d4f55544501ULL
};

inline constexpr schema::PropertyId kBiomeMaskGlobal{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d454d474c4f4201ULL
};

inline constexpr schema::PropertyId kBiomeMaskValue{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d454d56414c5501ULL
};

inline constexpr schema::PropertyId kBiomeMaskOpacity{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d454d4f50414301ULL
};

inline constexpr schema::PropertyId kBiomeMaskEnabled{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d454d454e414201ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerKind{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c4b494e01ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerStrength{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c53545201ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerCompatibility{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c434f4d01ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerSlopeMin{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c534d4901ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerSlopeMax{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c534d4101ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerSlopeFalloff{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c53464101ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerCurvatureMin{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c434d4901ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerCurvatureMax{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c434d4101ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerCurvatureFalloff{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c43464101ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerMoistureMin{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c4d4d4901ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerMoistureMax{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c4d4d4101ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerMoistureFalloff{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c4d464101ULL
};

inline constexpr schema::PropertyId kBiomeSurfaceLayerEnabled{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534c454e4101ULL
};

inline constexpr schema::PropertyId kBiomeScatterKind{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d4553434b494e01ULL
};

inline constexpr schema::PropertyId kBiomeScatterDensity{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534344454e01ULL
};

inline constexpr schema::PropertyId kBiomeScatterSpacing{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534353504101ULL
};

inline constexpr schema::PropertyId kBiomeScatterSeedSalt{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534353454501ULL
};

inline constexpr schema::PropertyId kBiomeScatterCompatibility{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d455343434f4d01ULL
};

inline constexpr schema::PropertyId kBiomeScatterRequiresSoil{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534352535101ULL
};

inline constexpr schema::PropertyId kBiomeScatterMinimumSoilDepth{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d455343534f4901ULL
};

inline constexpr schema::PropertyId kBiomeScatterSlopeMin{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d455343534d4901ULL
};

inline constexpr schema::PropertyId kBiomeScatterSlopeMax{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d455343534d4101ULL
};

inline constexpr schema::PropertyId kBiomeScatterSlopeFalloff{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534353464101ULL
};

inline constexpr schema::PropertyId kBiomeScatterMoistureMin{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d4553434d4d4901ULL
};

inline constexpr schema::PropertyId kBiomeScatterMoistureMax{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d4553434d4d4101ULL
};

inline constexpr schema::PropertyId kBiomeScatterMoistureFalloff{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d4553434d464101ULL
};

inline constexpr schema::PropertyId kBiomeScatterScaleMin{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d4553435a4d4901ULL
};

inline constexpr schema::PropertyId kBiomeScatterScaleMax{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d4553435a4d4101ULL
};

inline constexpr schema::PropertyId kBiomeScatterEnabled{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d455343454e4101ULL
};

inline constexpr schema::PropertyId kBiomeMinimumResolvedWeight{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45544852455301ULL
};

inline constexpr schema::PropertyId kBiomeMaterialInfluence{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d454d4154494e01ULL
};

inline constexpr schema::PropertyId kBiomeScatterDensityMultiplier{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45534341545401ULL
};

inline constexpr schema::PropertyId kBiomeHydraulicErosionMultiplier{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45485944524f01ULL
};

inline constexpr schema::PropertyId kBiomeThermalTransportMultiplier{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45544845524d01ULL
};

inline constexpr schema::PropertyId kBiomeAeolianTransportMultiplier{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d4541454f4c4901ULL
};

inline constexpr schema::PropertyId kBiomeGlacialErosionMultiplier{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45474c41434901ULL
};

inline constexpr schema::PropertyId kBiomeCoastalErosionMultiplier{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d45434f41535401ULL
};

inline constexpr schema::PropertyId kBiomeChemicalWeatheringMultiplier{
    .high = 0x4f5242495442494fULL,
    .low = 0x4d454348454d5701ULL
};

inline constexpr schema::PropertyId kDecalAsset{
    .high = 0x4f52424954444543ULL,
    .low = 0x414c415353455401ULL
};

inline constexpr schema::PropertyId kDecalLatitudeRadians{
    .high = 0x4f52424954444543ULL,
    .low = 0x414c4c4154495401ULL
};

inline constexpr schema::PropertyId kDecalLongitudeRadians{
    .high = 0x4f52424954444543ULL,
    .low = 0x414c4c4f4e470001ULL
};

inline constexpr schema::PropertyId kDecalWidthMeters{
    .high = 0x4f52424954444543ULL,
    .low = 0x414c574944544801ULL
};

inline constexpr schema::PropertyId kDecalHeightMeters{
    .high = 0x4f52424954444543ULL,
    .low = 0x414c484549474801ULL
};

inline constexpr schema::PropertyId kDecalRotationDegrees{
    .high = 0x4f52424954444543ULL,
    .low = 0x414c524f54415401ULL
};

inline constexpr schema::PropertyId kDecalOpacity{
    .high = 0x4f52424954444543ULL,
    .low = 0x414c4f5041434901ULL
};

void RegisterSchemas(
    schema::SchemaRegistry& schemas);
} // namespace orbit::world_model
