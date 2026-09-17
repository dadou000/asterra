#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainPosition.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/world/Planet.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::terrain_geology
{
struct StratigraphyProfileIdTag;
using StratigraphyProfileId =
    core::StrongId<StratigraphyProfileIdTag>;

// One finite conceptual rock layer, ordered from youngest/topmost to oldest.
// The transition band occupies the bottom portion of this layer and blends
// toward the next layer (or basement) without storing a mixed voxel volume.
struct StratigraphyLayer
{
    RockTypeId material{};
    f64 thicknessMeters{1.0};
    f64 transitionBandMeters{0.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Continuous deformation of the virtual layer coordinate. The anchor is a
// canonical body-space direction rather than a cube face/tile location.
// Tilt slopes are rise/run in anchor tangent east/north. Warp displaces the
// tangent coordinates before tilt/fold evaluation; fold then offsets the
// virtual top boundary. All values are authored CPU authority.
struct StratigraphyTransform
{
    math::Double3 anchorUnitDirection{0.0, 1.0, 0.0};
    f64 referenceTopRadialOffsetMeters{0.0};

    f64 tiltEastSlope{0.0};
    f64 tiltNorthSlope{0.0};

    f64 foldAmplitudeMeters{0.0};
    f64 foldWavelengthMeters{1'000.0};
    f64 foldAzimuthRadians{0.0};
    f64 foldPhaseRadians{0.0};

    f64 warpAmplitudeMeters{0.0};
    f64 warpWavelengthMeters{1'000.0};
    f64 warpPhaseRadians{0.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Project-authoritative virtual stack. Finite layers sit above one infinite
// basement material. Conceptual layer count affects only profile-level storage;
// no per-texel/per-voxel copy of this stack exists.
struct StratigraphyProfile
{
    StratigraphyProfileId id{};
    std::string name;
    std::vector<StratigraphyLayer> layers;
    RockTypeId basementMaterial{};
    StratigraphyTransform transform{};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Fixed-size result of evaluating one physical surface point. A transition is
// represented by two RockTypeIds plus a secondary weight rather than by
// creating another physical layer/voxel representation.
struct StratigraphySample
{
    RockTypeId primaryMaterial{};
    RockTypeId secondaryMaterial{};
    f32 secondaryWeight{0.0F};

    u32 layerIndex{0};
    bool basement{false};

    f64 depthBelowTopMeters{0.0};
    f64 depthIntoLayerMeters{0.0};
    f64 deformedTopRadialOffsetMeters{0.0};

    [[nodiscard]] bool HasTransition() const noexcept
    {
        return
            secondaryMaterial.IsValid() &&
            secondaryWeight > 0.0F;
    }
};

[[nodiscard]] bool ReferencesKnownMaterials(
    const StratigraphyProfile& profile,
    const GeologicalMaterialLibrary& materials) noexcept;

// Derived CPU evaluator. It precomputes only cumulative profile boundaries;
// runtime surface queries allocate nothing and require no terrain-page stack.
class CompiledStratigraphyProfile
{
public:
    CompiledStratigraphyProfile(
        StratigraphyProfile profile,
        const GeologicalMaterialLibrary& materials);

    [[nodiscard]] const StratigraphyProfile& Profile()
        const noexcept;
    [[nodiscard]] std::span<const f64> LayerBottomDepths()
        const noexcept;
    [[nodiscard]] std::size_t LayerCount() const noexcept;

    [[nodiscard]] f64 LayerTopDepthMeters(
        std::size_t layerIndex) const;
    [[nodiscard]] f64 LayerBottomDepthMeters(
        std::size_t layerIndex) const;

    // Returns the deformed radial offset of the virtual stratigraphic top at
    // this canonical body-space position. Tile/face/clipmap identity is never
    // an input to this calculation.
    [[nodiscard]] f64 DeformedTopRadialOffsetMeters(
        const world::PlanetDefinition& planet,
        const terrain::PlanetSurfacePosition& position) const noexcept;

    // position.radialOffsetMeters is the currently exposed bedrock surface.
    // Lowering it (erosion/incision) walks down the virtual stack and can expose
    // a different M02 RockTypeId without materializing deeper voxels.
    [[nodiscard]] StratigraphySample SampleExposedLayer(
        const world::PlanetDefinition& planet,
        const terrain::PlanetSurfacePosition& position) const noexcept;

private:
    StratigraphyProfile profile_;
    std::vector<f64> layerBottomDepths_;
};

// Project-authority codec for .orbitstratigraphy profiles.
[[nodiscard]] StratigraphyProfile ParseStratigraphyProfileToml(
    std::string_view text);
[[nodiscard]] std::string SerializeStratigraphyProfileToml(
    const StratigraphyProfile& profile);
[[nodiscard]] StratigraphyProfile LoadStratigraphyProfileFile(
    const std::filesystem::path& path);
} // namespace orbit::terrain_geology
