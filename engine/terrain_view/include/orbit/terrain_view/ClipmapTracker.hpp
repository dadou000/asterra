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
    math::Double3 centerDirection{};
    world::SurfaceFrame surfaceFrame{};
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

    [[nodiscard]] const ClipmapConfig& Config() const noexcept;

private:
    struct LevelState
    {
        bool initialized{false};
        math::Double3 centerDirection{};
        world::SurfaceFrame frame{};
    };

    world::PlanetDefinition planet_;
    ClipmapConfig config_;
    std::vector<LevelState> levels_;
};
} // namespace orbit::terrain_view
