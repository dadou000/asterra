#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_view/ClipmapLayout.hpp>
#include <orbit/world/Planet.hpp>
#include <orbit/world/WorldPosition.hpp>

#include <vector>

namespace orbit::terrain_view
{
struct ClipmapLevelMotion
{
    u32 levelIndex{0};
    i64 cellShiftX{0};
    i64 cellShiftY{0};

    // Snapped center of this level on the shared spherical lattice.
    math::Double3 centerDirection{};

    // Stable anchor frame shared by every active LOD. Samples are addressed
    // from this frame plus centerOffsetMeters; the frame does not rotate on
    // ordinary cell shifts, so retained toroidal samples keep the exact same
    // world-space address.
    world::SurfaceFrame surfaceFrame{};
    math::Double2 centerOffsetMeters{};

    bool fullRefresh{false};
};

struct ClipmapMotionUpdate
{
    std::vector<ClipmapLevelMotion> levels;
};

class ClipmapTracker
{
public:
    ClipmapTracker(
        world::PlanetDefinition planet,
        ClipmapConfig config);

    [[nodiscard]] ClipmapMotionUpdate Update(
        const world::WorldPosition& observer);

    void Reset() noexcept;
    // Refresh data without relocating the stable sampling lattice.
    void InvalidateSamples() noexcept;
    [[nodiscard]] ClipmapTracker Reconfigured(ClipmapConfig config) const;

    [[nodiscard]] const ClipmapConfig& Config() const noexcept;

private:
    struct LevelState
    {
        bool initialized{false};
        bool samplesInvalidated{false};
        math::Double2 centerOffsetMeters{};
        math::Double3 centerDirection{};
    };

    world::PlanetDefinition planet_;
    ClipmapConfig config_;
    std::vector<LevelState> levels_;

    bool latticeInitialized_{false};
    world::SurfaceFrame latticeFrame_{};
};
} // namespace orbit::terrain_view
