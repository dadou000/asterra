#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace orbit::terrain
{
struct BakedGeologyProcessTexel
{
    f32 excavationDepthMeters{0.0F};
    f32 ejectaThicknessMeters{0.0F};
    f32 debrisField{0.0F};
    f32 rayField{0.0F};
    f32 meltThicknessMeters{0.0F};
    f32 brecciaField{0.0F};
    f32 resurfacedMaterialFraction{0.0F};
    f32 resurfacingThicknessMeters{0.0F};
    f32 microImpactRoughnessMeters{0.0F};
    f32 microImpactCoverage{0.0F};
    f32 excavationCoverage{0.0F};
    f32 formationAgeYears{0.0F};
    f32 exposureAgeYears{0.0F};
    u64 formationAgeOrder{0U};
    u64 exposureAgeOrder{0U};
    u32 affectingImpacts{0U};
    f32 iceDamage{0.0F};
    f32 fractureCoverage{0.0F};
    u32 nearbySegments{0U};
};

// GPU-friendly regional compilation of the authored impact and ice relief.
// Impact and ice relief channels are stored with cube-face gutters; full M07
// process channels remain available from the analytic source for M08 builds.
class BakedGeologyRasters
{
public:
    static constexpr u32 kFormatVersion = 4U;
    static constexpr u32 kFaces = 6U;

    [[nodiscard]] static BakedGeologyRasters Build(
        u32 resolution,
        u64 recipeHash,
        std::vector<f32> impactReliefGutter,
        std::vector<f32> iceReliefGutter = {},
        std::vector<BakedGeologyProcessTexel> processGutter = {});

    [[nodiscard]] u32 Resolution() const noexcept { return resolution_; }
    [[nodiscard]] u64 RecipeHash() const noexcept { return recipeHash_; }
    [[nodiscard]] u64 ContentHash() const noexcept { return contentHash_; }
    [[nodiscard]] bool HasProcessChannels() const noexcept
    {
        return processChannelsValid_;
    }
    [[nodiscard]] const std::vector<f32>& ReliefGutter() const noexcept
    {
        return impactReliefGutter_;
    }
    [[nodiscard]] const std::vector<f32>& IceReliefGutter() const noexcept
    {
        return iceReliefGutter_;
    }
    [[nodiscard]] const std::vector<BakedGeologyProcessTexel>& ProcessGutter() const noexcept
    {
        static const std::vector<BakedGeologyProcessTexel> empty;
        return processLevels_.empty() ? empty : processLevels_.front();
    }
    [[nodiscard]] f32 ImpactDeltaMeters(
        const math::Double3& direction,
        f64 footprintMeters,
        f64 planetRadiusMeters) const noexcept;
    [[nodiscard]] f32 IceDeltaMeters(
        const math::Double3& direction,
        f64 footprintMeters,
        f64 planetRadiusMeters) const noexcept;
    [[nodiscard]] f32 ReliefDeltaMeters(
        const math::Double3& direction,
        f64 footprintMeters = 0.0,
        f64 planetRadiusMeters = 6'371'000.0) const noexcept;
    [[nodiscard]] BakedGeologyProcessTexel SampleProcesses(
        const math::Double3& direction,
        f64 footprintMeters = 0.0,
        f64 planetRadiusMeters = 6'371'000.0) const noexcept;
    [[nodiscard]] const std::vector<f32>& BuildGpuRelief() const noexcept
    {
        return gpuReliefPyramid_;
    }
    [[nodiscard]] u32 LevelCount() const noexcept
    {
        return static_cast<u32>(levelResolutions_.size());
    }
    [[nodiscard]] std::span<const u32> LevelResolutions() const noexcept
    {
        return levelResolutions_;
    }
    [[nodiscard]] std::size_t ByteSize() const noexcept
    {
        std::size_t bytes =
            (impactReliefGutter_.size() + iceReliefGutter_.size() +
                gpuReliefPyramid_.size()) * sizeof(f32);
        for (const auto& level : impactLevels_) bytes += level.size() * sizeof(f32);
        for (const auto& level : iceLevels_) bytes += level.size() * sizeof(f32);
        for (const auto& level : processLevels_)
            bytes += level.size() * sizeof(BakedGeologyProcessTexel);
        return bytes;
    }

private:
    u32 resolution_{0U};
    u64 recipeHash_{0U};
    u64 contentHash_{0U};
    bool processChannelsValid_{false};
    std::vector<f32> impactReliefGutter_;
    std::vector<f32> iceReliefGutter_;
    std::vector<std::vector<BakedGeologyProcessTexel>> processLevels_;
    std::vector<u32> levelResolutions_;
    std::vector<std::vector<f32>> impactLevels_;
    std::vector<std::vector<f32>> iceLevels_;
    std::vector<f32> gpuReliefPyramid_;
};

[[nodiscard]] math::Double3 BakedGeologyTexelDirection(
    u32 face,
    i32 x,
    i32 y,
    u32 resolution) noexcept;
} // namespace orbit::terrain
