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

// V0.0.6 M04: a non-renderable semantic reference/barycenter node. It owns
// a FrameGraph frame but never becomes a CelestialBody or renderable surface.
inline constexpr schema::TypeId kCelestialReferenceNodeType{
    .high = 0x4f52424954524546ULL,
    .low = 0x4e4f444500000001ULL
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

// M09 semantic bridge for the canonical M04 TerrainConstraintSet.
inline constexpr schema::TypeId kTerrainConstraintType{
    .high = 0x4f52424954434f4eULL,
    .low = 0x53545241494e5431ULL
};

inline constexpr schema::TypeId kTerrainConstraintControlPointType{
    .high = 0x4f52424954434f4eULL,
    .low = 0x5452504f494e5431ULL
};

inline constexpr schema::TypeId kSurfaceDecalType{
    .high = 0x4f52424954444543ULL,
    .low = 0x414c545950450001ULL
};

inline constexpr schema::TypeId kPointLightType{
    .high = 0x4f524249544c4954ULL,
    .low = 0x504f494e54000001ULL
};

inline constexpr schema::TypeId kSpotLightType{
    .high = 0x4f524249544c4954ULL,
    .low = 0x53504f5400000001ULL
};

inline constexpr schema::TypeId kVisibilityProxyType{
    .high = 0x4f52424954564953ULL,
    .low = 0x50524f5859000001ULL
};

inline constexpr schema::TypeId kMaterialAssignmentType{
    .high = 0x4f524249544d4154ULL,
    .low = 0x41535349474e0001ULL
};

inline constexpr schema::PropertyId kSystemEpochMicroseconds{
    .high = 0x4f5242495450524fULL,
    .low = 0x505345504f434801ULL
};

inline constexpr schema::PropertyId kReferenceNodePositionMeters{
    .high = 0x4f5242495450524fULL,
    .low = 0x505245464e4f4401ULL
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

inline constexpr schema::PropertyId kTerrainCratersEnabled{
    .high = 0x4f52424954544552ULL,
    .low = 0x435241544552454eULL
};

inline constexpr schema::PropertyId kTerrainCraterCount{
    .high = 0x4f52424954544552ULL,
    .low = 0x435241544552434eULL
};

inline constexpr schema::PropertyId kTerrainCraterMinimumRadiusMeters{
    .high = 0x4f52424954544552ULL,
    .low = 0x4352415445524d49ULL
};

inline constexpr schema::PropertyId kTerrainCraterMaximumRadiusMeters{
    .high = 0x4f52424954544552ULL,
    .low = 0x4352415445524d41ULL
};

inline constexpr schema::PropertyId kTerrainCraterCumulativeExponent{
    .high = 0x4f52424954544552ULL,
    .low = 0x4352415445524558ULL
};

inline constexpr schema::PropertyId kTerrainComplexCraterRadiusMeters{
    .high = 0x4f52424954544552ULL,
    .low = 0x4352415445524358ULL
};

// M11 authored terrain-process configuration. The semantic record owns
// service policy; physical pages/products remain derived runtime state.
inline constexpr schema::PropertyId kProcessStreamPowerEnabled{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000101ULL};
inline constexpr schema::PropertyId kProcessStreamPowerIterations{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000102ULL};
inline constexpr schema::PropertyId kProcessStreamPowerIncision{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000103ULL};

inline constexpr schema::PropertyId kProcessHydraulicEnabled{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000201ULL};
inline constexpr schema::PropertyId kProcessHydraulicIterations{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000202ULL};
inline constexpr schema::PropertyId kProcessHydraulicRainfall{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000203ULL};
inline constexpr schema::PropertyId kProcessHydraulicTimeStep{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000204ULL};

inline constexpr schema::PropertyId kProcessThermalEnabled{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000301ULL};
inline constexpr schema::PropertyId kProcessThermalIterations{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000302ULL};
inline constexpr schema::PropertyId kProcessThermalRelaxation{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000303ULL};

inline constexpr schema::PropertyId kProcessAeolianEnabled{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000401ULL};
inline constexpr schema::PropertyId kProcessAeolianIterations{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000402ULL};
inline constexpr schema::PropertyId kProcessAeolianCapacity{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000403ULL};
inline constexpr schema::PropertyId kProcessAeolianTimeStep{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000404ULL};

inline constexpr schema::PropertyId kProcessGlacialEnabled{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000501ULL};
inline constexpr schema::PropertyId kProcessGlacialIterations{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000502ULL};
inline constexpr schema::PropertyId kProcessGlacialMaximumTemperature{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000503ULL};
inline constexpr schema::PropertyId kProcessGlacialTimeStepYears{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000504ULL};

inline constexpr schema::PropertyId kProcessRiversEnabled{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000601ULL};
inline constexpr schema::PropertyId kProcessRiverMeandersEnabled{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000602ULL};
inline constexpr schema::PropertyId kProcessRiverMeanderIterations{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000603ULL};
inline constexpr schema::PropertyId kProcessRiverCutoffsEnabled{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000604ULL};
inline constexpr schema::PropertyId kProcessRiverMinimumDrainageArea{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000605ULL};

inline constexpr schema::PropertyId kProcessCoastalEnabled{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000701ULL};
inline constexpr schema::PropertyId kProcessCoastalHydrodynamicSteps{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000702ULL};
inline constexpr schema::PropertyId kProcessCoastalCflNumber{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000703ULL};
inline constexpr schema::PropertyId kProcessCoastalMaximumTimeStep{
    .high = 0x4f52424954505243ULL, .low = 0x4d31310000000704ULL};

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

inline constexpr schema::PropertyId kTerrainConstraintChannel{
    .high = 0x4f52424954434e53ULL,
    .low = 0x4348414e4e454c31ULL
};
inline constexpr schema::PropertyId kTerrainConstraintShape{
    .high = 0x4f52424954434e53ULL,
    .low = 0x5348415045303031ULL
};
inline constexpr schema::PropertyId kTerrainConstraintMode{
    .high = 0x4f52424954434e53ULL,
    .low = 0x4d4f444530303031ULL
};
inline constexpr schema::PropertyId kTerrainConstraintCenter{
    .high = 0x4f52424954434e53ULL,
    .low = 0x43454e5445523031ULL
};
inline constexpr schema::PropertyId kTerrainConstraintInnerRadius{
    .high = 0x4f52424954434e53ULL,
    .low = 0x494e4e4552303031ULL
};
inline constexpr schema::PropertyId kTerrainConstraintOuterRadius{
    .high = 0x4f52424954434e53ULL,
    .low = 0x4f55544552303031ULL
};
inline constexpr schema::PropertyId kTerrainConstraintHalfWidth{
    .high = 0x4f52424954434e53ULL,
    .low = 0x48414c4657494431ULL
};
inline constexpr schema::PropertyId kTerrainConstraintFalloff{
    .high = 0x4f52424954434e53ULL,
    .low = 0x46414c4c4f464631ULL
};
inline constexpr schema::PropertyId kTerrainConstraintValue{
    .high = 0x4f52424954434e53ULL,
    .low = 0x56414c5545303031ULL
};
inline constexpr schema::PropertyId kTerrainConstraintOpacity{
    .high = 0x4f52424954434e53ULL,
    .low = 0x4f50414349545931ULL
};
inline constexpr schema::PropertyId kTerrainConstraintMaterial{
    .high = 0x4f52424954434e53ULL,
    .low = 0x4d4154455249414cULL
};
inline constexpr schema::PropertyId kTerrainConstraintEnabled{
    .high = 0x4f52424954434e53ULL,
    .low = 0x454e41424c454431ULL
};
inline constexpr schema::PropertyId kTerrainConstraintPointDirection{
    .high = 0x4f52424954434e53ULL,
    .low = 0x504f494e54444952ULL
};

inline constexpr schema::PropertyId kLightPositionMeters{
    .high = 0x4f524249544c5052ULL,
    .low = 0x504f534954494f4eULL
};

inline constexpr schema::PropertyId kLightDirection{
    .high = 0x4f524249544c5052ULL,
    .low = 0x444952454354494fULL
};

inline constexpr schema::PropertyId kLightColorLinear{
    .high = 0x4f524249544c5052ULL,
    .low = 0x434f4c4f52000001ULL
};

inline constexpr schema::PropertyId kLightIntensityLumens{
    .high = 0x4f524249544c5052ULL,
    .low = 0x4c554d454e530001ULL
};

inline constexpr schema::PropertyId kLightRangeMeters{
    .high = 0x4f524249544c5052ULL,
    .low = 0x52414e4745000001ULL
};

inline constexpr schema::PropertyId kLightInnerConeDegrees{
    .high = 0x4f524249544c5052ULL,
    .low = 0x494e4e4552434f4eULL
};

inline constexpr schema::PropertyId kLightOuterConeDegrees{
    .high = 0x4f524249544c5052ULL,
    .low = 0x4f55544552434f4eULL
};

inline constexpr schema::PropertyId kLightEnabled{
    .high = 0x4f524249544c5052ULL,
    .low = 0x454e41424c454401ULL
};

inline constexpr schema::PropertyId kVisibilityProxyEnabled{
    .high = 0x4f52424954565052ULL,
    .low = 0x454e41424c454401ULL
};

inline constexpr schema::PropertyId kVisibilityProxyShape{
    .high = 0x4f52424954565052ULL,
    .low = 0x5348415045000001ULL
};

inline constexpr schema::PropertyId kVisibilityProxyPositionMeters{
    .high = 0x4f52424954565052ULL,
    .low = 0x504f534954494f4eULL
};

inline constexpr schema::PropertyId kVisibilityProxyEulerDegrees{
    .high = 0x4f52424954565052ULL,
    .low = 0x45554c4552000001ULL
};

inline constexpr schema::PropertyId kVisibilityProxyRadiusMeters{
    .high = 0x4f52424954565052ULL,
    .low = 0x5241444955530001ULL
};

inline constexpr schema::PropertyId kVisibilityProxyHalfExtentsMeters{
    .high = 0x4f52424954565052ULL,
    .low = 0x455854454e545301ULL
};

inline constexpr schema::PropertyId kVisibilityProxyMaterialId{
    .high = 0x4f52424954565052ULL,
    .low = 0x4d4154455249414cULL
};

inline constexpr schema::PropertyId kVisibilityProxyInstanceId{
    .high = 0x4f52424954565052ULL,
    .low = 0x494e5354414e4345ULL
};

inline constexpr schema::PropertyId kVisibilityProxyErrorMeters{
    .high = 0x4f52424954565052ULL,
    .low = 0x4552524f52000001ULL
};

inline constexpr schema::PropertyId kVisibilityProxyDynamic{
    .high = 0x4f52424954565052ULL,
    .low = 0x44594e414d494301ULL
};

inline constexpr schema::PropertyId kMaterialAssignmentAsset{
    .high = 0x4f524249544d4154ULL,
    .low = 0x4153534554000001ULL
};

inline constexpr schema::PropertyId kMaterialAssignmentSlot{
    .high = 0x4f524249544d4154ULL,
    .low = 0x534c4f5400000001ULL
};

inline constexpr schema::PropertyId kMaterialAssignmentEnabled{
    .high = 0x4f524249544d4154ULL,
    .low = 0x454e41424c454401ULL
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
