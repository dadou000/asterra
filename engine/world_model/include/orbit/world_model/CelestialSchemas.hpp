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
