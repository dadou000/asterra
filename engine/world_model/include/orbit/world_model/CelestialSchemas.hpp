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

inline constexpr schema::PropertyId kCapabilityEnabled{
    .high = 0x4f5242495443454cULL, .low = 0x50524f50454e0001ULL};
inline constexpr schema::PropertyId kCapabilityModel{
    .high = 0x4f5242495443454cULL, .low = 0x50524f504d4f0001ULL};
inline constexpr schema::PropertyId kCapabilitySourceObject{
    .high = 0x4f5242495443454cULL, .low = 0x50524f5053520001ULL};

void RegisterCelestialCapabilitySchemas(schema::SchemaRegistry& schemas);
} // namespace orbit::world_model
