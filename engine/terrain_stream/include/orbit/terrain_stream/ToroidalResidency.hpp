#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain_view/ClipmapLayout.hpp>
#include <orbit/terrain_view/ClipmapTracker.hpp>

#include <vector>

namespace orbit::terrain_stream
{
struct PhysicalRegion
{
    u32 x{0};
    u32 y{0};
    u32 width{0};
    u32 height{0};
};

struct LevelResidencyUpdate
{
    u32 levelIndex{0};
    u32 originX{0};
    u32 originY{0};
    bool fullRefresh{false};
    std::vector<PhysicalRegion> refreshRegions;
};

struct ResidencyUpdate
{
    std::vector<LevelResidencyUpdate> levels;
};

class ToroidalResidency
{
public:
    explicit ToroidalResidency(
        terrain_view::ClipmapConfig config);

    [[nodiscard]] ResidencyUpdate Apply(
        const terrain_view::ClipmapMotionUpdate& motion);

    void Reset() noexcept;

    [[nodiscard]] const terrain_view::ClipmapConfig&
    Config() const noexcept;

private:
    struct LevelState
    {
        u32 originX{0};
        u32 originY{0};
    };

    terrain_view::ClipmapConfig config_;
    std::vector<LevelState> levels_;
};
} // namespace orbit::terrain_stream
