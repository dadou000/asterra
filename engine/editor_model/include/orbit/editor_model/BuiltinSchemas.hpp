#pragma once

#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

namespace orbit::editor_model::builtin
{
// Compatibility aliases keep existing editor/plugin code stable while the
// permanent ownership of world semantic IDs lives in Orbit::WorldModel.
inline constexpr schema::TypeId kWorldType =
    world_model::kWorldType;
inline constexpr schema::TypeId kCelestialSystemType =
    world_model::kCelestialSystemType;
inline constexpr schema::TypeId kCelestialBodyType =
    world_model::kCelestialBodyType;
inline constexpr schema::TypeId kSurfaceDecalType =
    world_model::kSurfaceDecalType;

inline constexpr schema::PropertyId kSystemEpochMicroseconds =
    world_model::kSystemEpochMicroseconds;
inline constexpr schema::PropertyId kBodyEllipsoidEnabled =
    world_model::kBodyEllipsoidEnabled;
inline constexpr schema::PropertyId kBodyRadius =
    world_model::kBodyRadius;
inline constexpr schema::PropertyId kBodyPolarRadius =
    world_model::kBodyPolarRadius;
inline constexpr schema::PropertyId kBodyMass =
    world_model::kBodyMass;
inline constexpr schema::PropertyId kBodyParentPositionMeters =
    world_model::kBodyParentPositionMeters;
inline constexpr schema::PropertyId kBodyRotationPeriodSeconds =
    world_model::kBodyRotationPeriodSeconds;
inline constexpr schema::PropertyId kBodyAxialTiltDegrees =
    world_model::kBodyAxialTiltDegrees;
inline constexpr schema::PropertyId kBodyRotationPhaseDegrees =
    world_model::kBodyRotationPhaseDegrees;
inline constexpr schema::PropertyId kBodyMaterialAsset =
    world_model::kBodyMaterialAsset;
inline constexpr schema::PropertyId kDecalAsset =
    world_model::kDecalAsset;
inline constexpr schema::PropertyId kDecalLatitudeRadians =
    world_model::kDecalLatitudeRadians;
inline constexpr schema::PropertyId kDecalLongitudeRadians =
    world_model::kDecalLongitudeRadians;
inline constexpr schema::PropertyId kDecalWidthMeters =
    world_model::kDecalWidthMeters;
inline constexpr schema::PropertyId kDecalHeightMeters =
    world_model::kDecalHeightMeters;
inline constexpr schema::PropertyId kDecalRotationDegrees =
    world_model::kDecalRotationDegrees;
inline constexpr schema::PropertyId kDecalOpacity =
    world_model::kDecalOpacity;

void RegisterSchemas(
    schema::SchemaRegistry& schemas);
} // namespace orbit::editor_model::builtin
