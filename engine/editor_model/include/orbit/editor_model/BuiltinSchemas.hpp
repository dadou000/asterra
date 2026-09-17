#pragma once

#include <orbit/schema/SchemaRegistry.hpp>

namespace orbit::editor_model::builtin
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

void RegisterSchemas(
    schema::SchemaRegistry& schemas);
} // namespace orbit::editor_model::builtin
