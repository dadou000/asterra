#pragma once

#include <orbit/schema/SchemaRegistry.hpp>

namespace orbit::world_model
{
// V0.0.6 M01: semantic capability records. These are ordinary scene-object
// schemas attached beneath a Celestial Body. They intentionally do not create
// a Planet/Star subclass hierarchy; recipes and later runtime systems compose
// these records.

inline constexpr schema::TypeId kReferenceShapeCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x5348504341500001ULL};
inline constexpr schema::TypeId kMassPropertiesCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x4d41534341500001ULL};
inline constexpr schema::TypeId kOrbitCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x4f52424341500001ULL};
inline constexpr schema::TypeId kRotationCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x524f544341500001ULL};
inline constexpr schema::TypeId kGravityCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x4752415643415001ULL};
inline constexpr schema::TypeId kSurfaceCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x5355524341500001ULL};
inline constexpr schema::TypeId kAtmosphereCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x41544d4341500001ULL};
inline constexpr schema::TypeId kOceanCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x4f434e4341500001ULL};
inline constexpr schema::TypeId kCloudLayerCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x434c444341500001ULL};
inline constexpr schema::TypeId kRingSystemCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x524e474341500001ULL};
inline constexpr schema::TypeId kRingBandType{
    .high = 0x4f5242495443454cULL, .low = 0x524e4742414e4401ULL};
inline constexpr schema::TypeId kRadiativeEmitterCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x454d544341500001ULL};
inline constexpr schema::TypeId kPhotosphereCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x50484f4341500001ULL};
inline constexpr schema::TypeId kMagnetosphereCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x4d41474341500001ULL};
inline constexpr schema::TypeId kCometTailCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x434f4d4341500001ULL};
inline constexpr schema::TypeId kCompactObjectCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x434d504341500001ULL};
inline constexpr schema::TypeId kAccretionFlowCapabilityType{
    .high = 0x4f5242495443454cULL, .low = 0x4143434341500001ULL};

inline constexpr schema::TypeId kEphemerisAssetType{
    .high = 0x4f52424954455048ULL, .low = 0x4153534554000001ULL};
inline constexpr schema::TypeId kEphemerisSampleType{
    .high = 0x4f52424954455048ULL, .low = 0x53414d504c450001ULL};

inline constexpr schema::PropertyId kCapabilityEnabled{
    .high = 0x4f5242495443454cULL, .low = 0x50524f50454e0001ULL};
inline constexpr schema::PropertyId kCapabilityModel{
    .high = 0x4f5242495443454cULL, .low = 0x50524f504d4f0001ULL};
inline constexpr schema::PropertyId kCapabilitySourceObject{
    .high = 0x4f5242495443454cULL, .low = 0x50524f5053520001ULL};

inline constexpr schema::PropertyId kOrbitSemiMajorAxisMeters{
    .high = 0x4f524249544f5242ULL, .low = 0x53454d494d414a01ULL};
inline constexpr schema::PropertyId kOrbitPeriapsisDistanceMeters{
    .high = 0x4f524249544f5242ULL, .low = 0x5045524941505301ULL};
inline constexpr schema::PropertyId kOrbitEccentricity{
    .high = 0x4f524249544f5242ULL, .low = 0x454343454e545201ULL};
inline constexpr schema::PropertyId kOrbitInclinationDegrees{
    .high = 0x4f524249544f5242ULL, .low = 0x494e434c494e4501ULL};
inline constexpr schema::PropertyId kOrbitAscendingNodeDegrees{
    .high = 0x4f524249544f5242ULL, .low = 0x4153434e4f444501ULL};
inline constexpr schema::PropertyId kOrbitArgumentPeriapsisDegrees{
    .high = 0x4f524249544f5242ULL, .low = 0x4152475045524901ULL};
inline constexpr schema::PropertyId kOrbitMeanAnomalyEpochDegrees{
    .high = 0x4f524249544f5242ULL, .low = 0x4d45414e414e4f01ULL};
inline constexpr schema::PropertyId kOrbitBarkerParameterEpoch{
    .high = 0x4f524249544f5242ULL, .low = 0x4241524b45520001ULL};
inline constexpr schema::PropertyId kOrbitGravitationalParameter{
    .high = 0x4f524249544f5242ULL, .low = 0x4752415650415201ULL};
inline constexpr schema::PropertyId kOrbitEpochMicroseconds{
    .high = 0x4f524249544f5242ULL, .low = 0x45504f4348555301ULL};
inline constexpr schema::PropertyId kOrbitDynamicPromotionEnabled{
    .high = 0x4f524249544f5242ULL, .low = 0x4e424f4459454e01ULL};
inline constexpr schema::PropertyId kOrbitDynamicStepSeconds{
    .high = 0x4f524249544f5242ULL, .low = 0x4e42535445500001ULL};
inline constexpr schema::PropertyId kOrbitDynamicSofteningMeters{
    .high = 0x4f524249544f5242ULL, .low = 0x4e42534f46540001ULL};

inline constexpr schema::PropertyId kRotationAxis{
    .high = 0x4f52424954524f54ULL, .low = 0x4158495300000001ULL};
inline constexpr schema::PropertyId kRotationPeriodSeconds{
    .high = 0x4f52424954524f54ULL, .low = 0x504552494f440001ULL};
inline constexpr schema::PropertyId kRotationPhaseDegrees{
    .high = 0x4f52424954524f54ULL, .low = 0x5048415345000001ULL};
inline constexpr schema::PropertyId kRotationEpochMicroseconds{
    .high = 0x4f52424954524f54ULL, .low = 0x45504f4348000001ULL};
inline constexpr schema::PropertyId kRotationSynchronousPhaseOffsetDegrees{
    .high = 0x4f52424954524f54ULL, .low = 0x53594e434f464601ULL};

inline constexpr schema::PropertyId kGravityDeriveMuFromMass{
    .high = 0x4f52424954475241ULL, .low = 0x4445524956450001ULL};
inline constexpr schema::PropertyId kGravityMuM3PerS2{
    .high = 0x4f52424954475241ULL, .low = 0x4d55000000000001ULL};
inline constexpr schema::PropertyId kGravitySofteningMeters{
    .high = 0x4f52424954475241ULL, .low = 0x534f4654454e0001ULL};

inline constexpr schema::PropertyId kEmitterLuminosityWatts{
    .high = 0x4f52424954454d49ULL, .low = 0x4c554d494e000001ULL};
inline constexpr schema::PropertyId kEmitterEffectiveTemperatureKelvin{
    .high = 0x4f52424954454d49ULL, .low = 0x54454d504b000001ULL};
inline constexpr schema::PropertyId kEmitterEmissivity{
    .high = 0x4f52424954454d49ULL, .low = 0x454d495353000001ULL};
inline constexpr schema::PropertyId kEmitterDeriveLuminosity{
    .high = 0x4f52424954454d49ULL, .low = 0x4445524956450001ULL};

inline constexpr schema::PropertyId kPhotosphereRadiusMeters{
    .high = 0x4f5242495450484fULL, .low = 0x5241444955530001ULL};
inline constexpr schema::PropertyId kPhotosphereTemperatureKelvin{
    .high = 0x4f5242495450484fULL, .low = 0x54454d504b000001ULL};

inline constexpr schema::PropertyId kPhotosphereLimbDarkening{
    .high = 0x4f5242495450484fULL, .low = 0x4c494d4244415201ULL};
inline constexpr schema::PropertyId kPhotosphereGranulationStrength{
    .high = 0x4f5242495450484fULL, .low = 0x4752414e53545201ULL};
inline constexpr schema::PropertyId kPhotosphereGranulationScale{
    .high = 0x4f5242495450484fULL, .low = 0x4752414e53434c01ULL};
inline constexpr schema::PropertyId kPhotosphereActivityLevel{
    .high = 0x4f5242495450484fULL, .low = 0x4143544956495401ULL};
inline constexpr schema::PropertyId kPhotosphereActivitySeed{
    .high = 0x4f5242495450484fULL, .low = 0x4143545345454401ULL};
inline constexpr schema::PropertyId kPhotosphereChromosphereStrength{
    .high = 0x4f5242495450484fULL, .low = 0x4348524f4d535401ULL};
inline constexpr schema::PropertyId kPhotosphereChromosphereExtent{
    .high = 0x4f5242495450484fULL, .low = 0x4348524f4d455801ULL};
inline constexpr schema::PropertyId kPhotosphereCoronaStrength{
    .high = 0x4f5242495450484fULL, .low = 0x434f524f4e535401ULL};
inline constexpr schema::PropertyId kPhotosphereCoronaExtent{
    .high = 0x4f5242495450484fULL, .low = 0x434f524f4e455801ULL};
inline constexpr schema::PropertyId kPhotosphereGlareStrength{
    .high = 0x4f5242495450484fULL, .low = 0x474c415245535401ULL};
inline constexpr schema::PropertyId kPhotosphereGlareRadiusPixels{
    .high = 0x4f5242495450484fULL, .low = 0x474c415245525001ULL};

inline constexpr schema::PropertyId kAtmosphereTopRadiusMeters{
    .high = 0x4f5242495441544dULL, .low = 0x544f505241440001ULL};
inline constexpr schema::PropertyId kAtmosphereRayleighScatteringPerMeter{
    .high = 0x4f5242495441544dULL, .low = 0x5241594c53434101ULL};
inline constexpr schema::PropertyId kAtmosphereRayleighScaleHeightMeters{
    .high = 0x4f5242495441544dULL, .low = 0x5241594c53434801ULL};
inline constexpr schema::PropertyId kAtmosphereMieScatteringPerMeter{
    .high = 0x4f5242495441544dULL, .low = 0x4d49455343415401ULL};
inline constexpr schema::PropertyId kAtmosphereMieExtinctionPerMeter{
    .high = 0x4f5242495441544dULL, .low = 0x4d49454558544901ULL};
inline constexpr schema::PropertyId kAtmosphereMieScaleHeightMeters{
    .high = 0x4f5242495441544dULL, .low = 0x4d49455343484801ULL};
inline constexpr schema::PropertyId kAtmosphereMieAnisotropy{
    .high = 0x4f5242495441544dULL, .low = 0x4d4945414e495301ULL};
inline constexpr schema::PropertyId kAtmosphereAbsorptionExtinctionPerMeter{
    .high = 0x4f5242495441544dULL, .low = 0x4142534558544901ULL};
inline constexpr schema::PropertyId kAtmosphereAbsorptionCenterHeightMeters{
    .high = 0x4f5242495441544dULL, .low = 0x41425343454e5401ULL};
inline constexpr schema::PropertyId kAtmosphereAbsorptionHalfWidthMeters{
    .high = 0x4f5242495441544dULL, .low = 0x41425348414c4601ULL};
inline constexpr schema::PropertyId kAtmosphereGroundAlbedo{
    .high = 0x4f5242495441544dULL, .low = 0x47524e44414c4201ULL};

inline constexpr schema::PropertyId kAtmosphereAuthoringMode{
    .high = 0x4f5242495441544dULL, .low = 0x415554484d4f4401ULL};
inline constexpr schema::PropertyId kAtmospherePreset{
    .high = 0x4f5242495441544dULL, .low = 0x5052455345540001ULL};
inline constexpr schema::PropertyId kAtmosphereSurfacePressurePascals{
    .high = 0x4f5242495441544dULL, .low = 0x5052455353555201ULL};
inline constexpr schema::PropertyId kAtmosphereSurfaceTemperatureKelvin{
    .high = 0x4f5242495441544dULL, .low = 0x54454d5045520001ULL};
inline constexpr schema::PropertyId kAtmosphereSurfaceGravityMetersPerSecondSquared{
    .high = 0x4f5242495441544dULL, .low = 0x4752415649545901ULL};
inline constexpr schema::PropertyId kAtmosphereNitrogenFraction{
    .high = 0x4f5242495441544dULL, .low = 0x4e32465241435401ULL};
inline constexpr schema::PropertyId kAtmosphereOxygenFraction{
    .high = 0x4f5242495441544dULL, .low = 0x4f32465241435401ULL};
inline constexpr schema::PropertyId kAtmosphereArgonFraction{
    .high = 0x4f5242495441544dULL, .low = 0x4152465241435401ULL};
inline constexpr schema::PropertyId kAtmosphereCarbonDioxideFraction{
    .high = 0x4f5242495441544dULL, .low = 0x434f324652414301ULL};
inline constexpr schema::PropertyId kAtmosphereAerosolOpticalDepth550{
    .high = 0x4f5242495441544dULL, .low = 0x414f443535300001ULL};
inline constexpr schema::PropertyId kAtmosphereAerosolSingleScatteringAlbedo{
    .high = 0x4f5242495441544dULL, .low = 0x4145525353410001ULL};
inline constexpr schema::PropertyId kAtmosphereAerosolAngstromExponent{
    .high = 0x4f5242495441544dULL, .low = 0x414e475354520001ULL};
inline constexpr schema::PropertyId kAtmosphereAerosolScaleHeightMeters{
    .high = 0x4f5242495441544dULL, .low = 0x4145525343480001ULL};
inline constexpr schema::PropertyId kAtmosphereAbsorberScale{
    .high = 0x4f5242495441544dULL, .low = 0x4142535343414c01ULL};

inline constexpr schema::PropertyId kOceanRefractiveIndex{
    .high = 0x4f524249544f434eULL, .low = 0x52454652494e4401ULL};
inline constexpr schema::PropertyId kOceanOrbitalRoughness{
    .high = 0x4f524249544f434eULL, .low = 0x524f5547484e5301ULL};
inline constexpr schema::PropertyId kOceanAbsorptionPerMeter{
    .high = 0x4f524249544f434eULL, .low = 0x4142534f52505401ULL};
inline constexpr schema::PropertyId kOceanDeepWaterColor{
    .high = 0x4f524249544f434eULL, .low = 0x44454550434f4c01ULL};
inline constexpr schema::PropertyId kOceanGlintStrength{
    .high = 0x4f524249544f434eULL, .low = 0x474c494e54535401ULL};
inline constexpr schema::PropertyId kOceanMinimumDepthForDeepColorMeters{
    .high = 0x4f524249544f434eULL, .low = 0x4445455044455001ULL};

inline constexpr schema::PropertyId kRingPlaneNormalBody{
    .high = 0x4f5242495452494eULL, .low = 0x504c414e454e4f01ULL};
inline constexpr schema::PropertyId kRingShadowParticipation{
    .high = 0x4f5242495452494eULL, .low = 0x5348445750415201ULL};
inline constexpr schema::PropertyId kRingBodyShadowEnabled{
    .high = 0x4f5242495452494eULL, .low = 0x424f445953484401ULL};
inline constexpr schema::PropertyId kRingBandInnerRadiusMeters{
    .high = 0x4f52424954524244ULL, .low = 0x494e4e4552524101ULL};
inline constexpr schema::PropertyId kRingBandOuterRadiusMeters{
    .high = 0x4f52424954524244ULL, .low = 0x4f55544552524101ULL};
inline constexpr schema::PropertyId kRingBandOpticalDepth{
    .high = 0x4f52424954524244ULL, .low = 0x4f50544445505401ULL};
inline constexpr schema::PropertyId kRingBandSingleScatteringAlbedo{
    .high = 0x4f52424954524244ULL, .low = 0x53434154414c4201ULL};
inline constexpr schema::PropertyId kRingBandAnisotropy{
    .high = 0x4f52424954524244ULL, .low = 0x414e49534f545201ULL};
inline constexpr schema::PropertyId kRingBandColorLinear{
    .high = 0x4f52424954524244ULL, .low = 0x434f4c4f524c4901ULL};
inline constexpr schema::PropertyId kRingBandThicknessMeters{
    .high = 0x4f52424954524244ULL, .low = 0x544849434b4e5301ULL};

inline constexpr schema::PropertyId kCloudSourceModel{
    .high = 0x4f52424954434c44ULL, .low = 0x5352434d4f444501ULL};
inline constexpr schema::PropertyId kCloudBaseAltitudeMeters{
    .high = 0x4f52424954434c44ULL, .low = 0x42415345414c5401ULL};
inline constexpr schema::PropertyId kCloudTopAltitudeMeters{
    .high = 0x4f52424954434c44ULL, .low = 0x544f50414c540001ULL};
inline constexpr schema::PropertyId kCloudCoverageBias{
    .high = 0x4f52424954434c44ULL, .low = 0x434f564249415301ULL};
inline constexpr schema::PropertyId kCloudOpticalDepth{
    .high = 0x4f52424954434c44ULL, .low = 0x4f50544445505401ULL};
inline constexpr schema::PropertyId kCloudSingleScatteringAlbedo{
    .high = 0x4f52424954434c44ULL, .low = 0x5353414c42454401ULL};
inline constexpr schema::PropertyId kCloudAnisotropy{
    .high = 0x4f52424954434c44ULL, .low = 0x414e49534f545201ULL};
inline constexpr schema::PropertyId kCloudDensityExponent{
    .high = 0x4f52424954434c44ULL, .low = 0x44454e5345585001ULL};
inline constexpr schema::PropertyId kCloudWeatherScale{
    .high = 0x4f52424954434c44ULL, .low = 0x5745415448534301ULL};
inline constexpr schema::PropertyId kCloudDetailScale{
    .high = 0x4f52424954434c44ULL, .low = 0x44455441494c5301ULL};
inline constexpr schema::PropertyId kCloudSeed{
    .high = 0x4f52424954434c44ULL, .low = 0x5345454400000001ULL};
inline constexpr schema::PropertyId kCloudWindAngularRadiansPerSecond{
    .high = 0x4f52424954434c44ULL, .low = 0x57494e44414e4701ULL};
inline constexpr schema::PropertyId kCloudPrecipitationPhase{
    .high = 0x4f52424954434c44ULL, .low = 0x5052454350485301ULL};
inline constexpr schema::PropertyId kCloudShadowParticipation{
    .high = 0x4f52424954434c44ULL, .low = 0x534841444f570001ULL};
inline constexpr schema::PropertyId kCloudOrbitalRepresentation{
    .high = 0x4f52424954434c44ULL, .low = 0x4f52424954414c01ULL};

inline constexpr schema::PropertyId kEphemerisSourceLabel{
    .high = 0x4f52424954455048ULL, .low = 0x534f555243450001ULL};
inline constexpr schema::PropertyId kEphemerisSampleTimeMicroseconds{
    .high = 0x4f52424954455048ULL, .low = 0x54494d4555530001ULL};
inline constexpr schema::PropertyId kEphemerisSamplePositionMeters{
    .high = 0x4f52424954455048ULL, .low = 0x504f534954494f01ULL};
inline constexpr schema::PropertyId kEphemerisSampleVelocityMetersPerSecond{
    .high = 0x4f52424954455048ULL, .low = 0x56454c4f43495401ULL};

void RegisterCelestialCapabilitySchemas(schema::SchemaRegistry& schemas);
} // namespace orbit::world_model
