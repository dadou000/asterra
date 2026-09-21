#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <vector>

namespace orbit::celestial_magnetosphere
{
struct MagnetosphereParameters
{
    // Body-fixed magnetic north axis. It is intentionally independent from the
    // spin axis so tilted/off-axis fields remain authorable.
    math::Double3 dipoleAxis{0.0, 0.0, 1.0};

    // Magnetic field magnitude at the magnetic equator at one reference radius.
    f64 equatorialFieldTesla{3.12e-5};

    // Solar-wind-facing +X direction in body-fixed presentation space. A later
    // field service may supply a fully time-dependent incident wind direction.
    math::Double3 solarWindDirection{-1.0, 0.0, 0.0};

    // Magnetopause baseline. r0 is expressed in reference-body radii so the
    // capability is portable across planets and moons.
    f64 subsolarStandoffBodyRadii{10.2};
    f64 magnetopauseFlaringAlpha{0.58};
    f64 maximumTailBodyRadii{80.0};

    // Environmental driver values remain explicit semantic inputs rather than
    // hidden Earth constants. The baseline only maps them onto presentation.
    f64 solarWindDynamicPressurePascals{2.0e-9};
    f64 interplanetaryFieldBzTesla{0.0};
    f64 activity{0.35};

    // Auroral presentation seam. These values describe precipitation geometry,
    // not atmospheric composition/emission chemistry.
    f64 auroralOvalLatitudeDegrees{67.0};
    f64 auroralOvalWidthDegrees{7.0};
    f64 auroralMinimumAltitudeMeters{100000.0};
    f64 auroralMaximumAltitudeMeters{300000.0};
    f64 auroralIntensity{1.0};
    math::Double3 auroralColorLinear{0.12, 1.8, 0.42};
    f64 auroralStructure{0.65};
    u64 auroralSeed{1};
};

struct MagnetosphereConfig
{
    u32 ovalSamples{256};
};

struct AuroraVertex
{
    math::Float3 positionNormalized{};
    math::Float3 emissionLinear{};
    math::Float2 presentation{};
};

struct AuroraMeshProduct
{
    f64 referenceRadiusMeters{1.0};
    u32 angularSegments{0};
    u64 fingerprint{0};
    std::vector<AuroraVertex> vertices;
    std::vector<u32> indices;
};

struct MagnetosphereProduct
{
    u64 fingerprint{0};
    f64 referenceRadiusMeters{1.0};
    f64 subsolarStandoffMeters{0.0};
    f64 tailExtentMeters{0.0};
    f64 auroralCenterLatitudeDegrees{0.0};
    f64 auroralHalfWidthDegrees{0.0};
    std::vector<math::Double3> northAuroralRing;
    std::vector<math::Double3> southAuroralRing;
};

[[nodiscard]] u64 MagnetosphereFingerprint(
    const MagnetosphereParameters& parameters,
    f64 referenceRadiusMeters,
    const MagnetosphereConfig& config = {});

[[nodiscard]] math::Double3 EvaluateDipoleFieldTesla(
    const MagnetosphereParameters& parameters,
    f64 referenceRadiusMeters,
    math::Double3 positionBodyMeters) noexcept;

[[nodiscard]] f64 MagnetopauseRadiusMeters(
    const MagnetosphereParameters& parameters,
    f64 referenceRadiusMeters,
    math::Double3 unitDirectionBody) noexcept;

[[nodiscard]] f64 AuroralOvalWeight(
    const MagnetosphereParameters& parameters,
    math::Double3 unitDirectionBody) noexcept;

[[nodiscard]] MagnetosphereProduct BuildMagnetosphereProduct(
    const MagnetosphereParameters& parameters,
    f64 referenceRadiusMeters,
    const MagnetosphereConfig& config = {});

[[nodiscard]] AuroraMeshProduct BuildAuroraCurtainMesh(
    const MagnetosphereParameters& parameters,
    f64 referenceRadiusMeters,
    u32 angularSegments = 256U);

} // namespace orbit::celestial_magnetosphere
