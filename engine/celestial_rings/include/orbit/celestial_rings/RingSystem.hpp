#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <vector>

namespace orbit::celestial_rings
{
struct RingBand
{
    u64 semanticIdHigh{0};
    u64 semanticIdLow{0};
    f64 innerRadiusMeters{0.0};
    f64 outerRadiusMeters{0.0};
    f64 normalOpticalDepth{0.0};
    f64 singleScatteringAlbedo{0.65};
    f64 anisotropy{0.35};
    math::Double3 colorLinear{0.72, 0.66, 0.56};
    f64 thicknessMeters{100.0};
};

struct RingSystem
{
    math::Double3 planeNormalBody{0.0, 1.0, 0.0};
    bool castRingShadows{true};
    bool receiveBodyShadow{true};
    std::vector<RingBand> bands;
    u64 fingerprint{0};
};

struct RingVertex
{
    math::Float3 positionNormalized{};
    math::Float3 colorLinear{};
    f32 opticalDepth{0.0F};
    f32 singleScatteringAlbedo{0.0F};
    f32 anisotropy{0.0F};
};

struct RingMeshProduct
{
    f64 referenceRadiusMeters{1.0};
    u32 angularSegments{0};
    u64 fingerprint{0};
    std::vector<RingVertex> vertices;
    std::vector<u32> indices;
};

struct FarRingSample
{
    f32 radiusNormalized{0.0F};
    f32 opticalDepth{0.0F};
    math::Float3 colorLinear{};
    f32 singleScatteringAlbedo{0.0F};
    f32 anisotropy{0.0F};
};

struct FarRingProfile
{
    f64 referenceRadiusMeters{1.0};
    u32 radialSamples{0};
    f64 innerRadiusMeters{0.0};
    f64 outerRadiusMeters{0.0};
    u64 fingerprint{0};
    std::vector<FarRingSample> samples;
};

[[nodiscard]] u64 RingSystemFingerprint(
    const RingSystem& system);

void ValidateRingSystem(
    const RingSystem& system);

[[nodiscard]] f64 HenyeyGreensteinPhase(
    f64 cosineTheta,
    f64 anisotropy);

[[nodiscard]] f64 RingTransmission(
    f64 normalOpticalDepth,
    f64 cosineToPlaneNormal);

[[nodiscard]] f64 RingShadowTransmittanceAtSurface(
    const RingSystem& system,
    f64 bodyRadiusMeters,
    math::Double3 surfaceUnitDirection,
    math::Double3 directionToLightBody);

[[nodiscard]] f64 BodyShadowTransmittanceAtRingPoint(
    const RingSystem& system,
    f64 bodyRadiusMeters,
    math::Double3 ringPointBodyMeters,
    math::Double3 directionToLightBody);

[[nodiscard]] RingMeshProduct BuildRingMesh(
    const RingSystem& system,
    f64 referenceRadiusMeters,
    u32 angularSegments = 256U);

[[nodiscard]] FarRingProfile BuildFarRingProfile(
    const RingSystem& system,
    f64 referenceRadiusMeters,
    u32 radialSamples = 256U);
} // namespace orbit::celestial_rings
