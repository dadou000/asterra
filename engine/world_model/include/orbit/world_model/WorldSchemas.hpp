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
