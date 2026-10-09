#include <orbit/terrain/BakedGeology.hpp>

#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace orbit::terrain
{
namespace
{
[[nodiscard]] std::size_t GutterIndex(
    const u32 resolution,
    const u32 face,
    const i32 x,
    const i32 y) noexcept
{
    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    return (static_cast<std::size_t>(face) * stride +
        static_cast<std::size_t>(y + 1)) * stride +
        static_cast<std::size_t>(x + 1);
}

[[nodiscard]] f32 SampleRaster(
    const std::vector<f32>& raster,
    const u32 resolution,
    const math::Double3& direction) noexcept
{
    if (resolution == 0U) return 0.0F;
    const world::CubeCoordinate cube = world::UnitDirectionToCube(direction);
    const u32 face = static_cast<u32>(cube.face);
    const f64 r = static_cast<f64>(resolution);
    const f64 fx = (cube.uv.x + 1.0) * 0.5 * r - 0.5;
    const f64 fy = (cube.uv.y + 1.0) * 0.5 * r - 0.5;
    const i32 last = static_cast<i32>(resolution);
    const i32 x0 = std::clamp(static_cast<i32>(std::floor(fx)), -1, last - 1);
    const i32 y0 = std::clamp(static_cast<i32>(std::floor(fy)), -1, last - 1);
    const f32 tx = static_cast<f32>(std::clamp(fx - x0, 0.0, 1.0));
    const f32 ty = static_cast<f32>(std::clamp(fy - y0, 0.0, 1.0));
    const f32 a = raster[GutterIndex(resolution, face, x0, y0)];
    const f32 b = raster[GutterIndex(resolution, face, x0 + 1, y0)];
    const f32 c = raster[GutterIndex(resolution, face, x0, y0 + 1)];
    const f32 d = raster[GutterIndex(resolution, face, x0 + 1, y0 + 1)];
    const f32 top = a + (b - a) * tx;
    return top + ((c + (d - c) * tx) - top) * ty;
}

[[nodiscard]] std::size_t ProcessIndex(
    const u32 resolution,
    const u32 face,
    const i32 x,
    const i32 y) noexcept
{
    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    return (static_cast<std::size_t>(face) * stride + static_cast<std::size_t>(y + 1)) *
        stride + static_cast<std::size_t>(x + 1);
}

[[nodiscard]] BakedGeologyProcessTexel SampleProcessRaster(
    const std::vector<BakedGeologyProcessTexel>& raster,
    const u32 resolution,
    const math::Double3& direction) noexcept
{
    if (resolution == 0U || raster.empty()) return {};
    const world::CubeCoordinate cube = world::UnitDirectionToCube(direction);
    const u32 face = static_cast<u32>(cube.face);
    const f64 r = static_cast<f64>(resolution);
    const f64 fx = (cube.uv.x + 1.0) * 0.5 * r - 0.5;
    const f64 fy = (cube.uv.y + 1.0) * 0.5 * r - 0.5;
    const i32 last = static_cast<i32>(resolution);
    const i32 x0 = std::clamp(static_cast<i32>(std::floor(fx)), -1, last - 1);
    const i32 y0 = std::clamp(static_cast<i32>(std::floor(fy)), -1, last - 1);
    const f32 tx = static_cast<f32>(std::clamp(fx - x0, 0.0, 1.0));
    const f32 ty = static_cast<f32>(std::clamp(fy - y0, 0.0, 1.0));
    const auto& a = raster[ProcessIndex(resolution, face, x0, y0)];
    const auto& b = raster[ProcessIndex(resolution, face, x0 + 1, y0)];
    const auto& c = raster[ProcessIndex(resolution, face, x0, y0 + 1)];
    const auto& d = raster[ProcessIndex(resolution, face, x0 + 1, y0 + 1)];
    const auto scalar = [&](const f32 av, const f32 bv, const f32 cv, const f32 dv)
    {
        const f32 top = av + (bv - av) * tx;
        return top + ((cv + (dv - cv) * tx) - top) * ty;
    };
    const auto& nearest = raster[ProcessIndex(resolution, face,
        x0 + (tx >= 0.5F ? 1 : 0), y0 + (ty >= 0.5F ? 1 : 0))];
    return {
        .excavationDepthMeters = scalar(a.excavationDepthMeters, b.excavationDepthMeters, c.excavationDepthMeters, d.excavationDepthMeters),
        .ejectaThicknessMeters = scalar(a.ejectaThicknessMeters, b.ejectaThicknessMeters, c.ejectaThicknessMeters, d.ejectaThicknessMeters),
        .debrisField = scalar(a.debrisField, b.debrisField, c.debrisField, d.debrisField),
        .rayField = scalar(a.rayField, b.rayField, c.rayField, d.rayField),
        .meltThicknessMeters = scalar(a.meltThicknessMeters, b.meltThicknessMeters, c.meltThicknessMeters, d.meltThicknessMeters),
        .brecciaField = scalar(a.brecciaField, b.brecciaField, c.brecciaField, d.brecciaField),
        .resurfacedMaterialFraction = scalar(a.resurfacedMaterialFraction, b.resurfacedMaterialFraction, c.resurfacedMaterialFraction, d.resurfacedMaterialFraction),
        .resurfacingThicknessMeters = scalar(a.resurfacingThicknessMeters, b.resurfacingThicknessMeters, c.resurfacingThicknessMeters, d.resurfacingThicknessMeters),
        .microImpactRoughnessMeters = scalar(a.microImpactRoughnessMeters, b.microImpactRoughnessMeters, c.microImpactRoughnessMeters, d.microImpactRoughnessMeters),
        .microImpactCoverage = scalar(a.microImpactCoverage, b.microImpactCoverage, c.microImpactCoverage, d.microImpactCoverage),
        .excavationCoverage = scalar(a.excavationCoverage, b.excavationCoverage, c.excavationCoverage, d.excavationCoverage),
        .formationAgeYears = scalar(a.formationAgeYears, b.formationAgeYears, c.formationAgeYears, d.formationAgeYears),
        .exposureAgeYears = scalar(a.exposureAgeYears, b.exposureAgeYears, c.exposureAgeYears, d.exposureAgeYears),
        .formationAgeOrder = nearest.formationAgeOrder,
        .exposureAgeOrder = nearest.exposureAgeOrder,
        .affectingImpacts = nearest.affectingImpacts,
        .iceDamage = scalar(a.iceDamage, b.iceDamage, c.iceDamage, d.iceDamage),
        .fractureCoverage = scalar(a.fractureCoverage, b.fractureCoverage, c.fractureCoverage, d.fractureCoverage),
        .nearbySegments = nearest.nearbySegments};
}

[[nodiscard]] BakedGeologyProcessTexel AverageProcess(
    const BakedGeologyProcessTexel& a,
    const BakedGeologyProcessTexel& b,
    const BakedGeologyProcessTexel& c,
    const BakedGeologyProcessTexel& d) noexcept
{
    const auto average = [](const f32 av, const f32 bv, const f32 cv, const f32 dv)
    {
        return (av + bv + cv + dv) * 0.25F;
    };
    return {
        .excavationDepthMeters = average(a.excavationDepthMeters, b.excavationDepthMeters, c.excavationDepthMeters, d.excavationDepthMeters),
        .ejectaThicknessMeters = average(a.ejectaThicknessMeters, b.ejectaThicknessMeters, c.ejectaThicknessMeters, d.ejectaThicknessMeters),
        .debrisField = average(a.debrisField, b.debrisField, c.debrisField, d.debrisField),
        .rayField = average(a.rayField, b.rayField, c.rayField, d.rayField),
        .meltThicknessMeters = average(a.meltThicknessMeters, b.meltThicknessMeters, c.meltThicknessMeters, d.meltThicknessMeters),
        .brecciaField = average(a.brecciaField, b.brecciaField, c.brecciaField, d.brecciaField),
        .resurfacedMaterialFraction = average(a.resurfacedMaterialFraction, b.resurfacedMaterialFraction, c.resurfacedMaterialFraction, d.resurfacedMaterialFraction),
        .resurfacingThicknessMeters = average(a.resurfacingThicknessMeters, b.resurfacingThicknessMeters, c.resurfacingThicknessMeters, d.resurfacingThicknessMeters),
        .microImpactRoughnessMeters = average(a.microImpactRoughnessMeters, b.microImpactRoughnessMeters, c.microImpactRoughnessMeters, d.microImpactRoughnessMeters),
        .microImpactCoverage = average(a.microImpactCoverage, b.microImpactCoverage, c.microImpactCoverage, d.microImpactCoverage),
        .excavationCoverage = average(a.excavationCoverage, b.excavationCoverage, c.excavationCoverage, d.excavationCoverage),
        .formationAgeYears = average(a.formationAgeYears, b.formationAgeYears, c.formationAgeYears, d.formationAgeYears),
        .exposureAgeYears = average(a.exposureAgeYears, b.exposureAgeYears, c.exposureAgeYears, d.exposureAgeYears),
        .formationAgeOrder = b.formationAgeOrder,
        .exposureAgeOrder = b.exposureAgeOrder,
        .affectingImpacts = b.affectingImpacts,
        .iceDamage = average(a.iceDamage, b.iceDamage, c.iceDamage, d.iceDamage),
        .fractureCoverage = average(a.fractureCoverage, b.fractureCoverage, c.fractureCoverage, d.fractureCoverage),
        .nearbySegments = b.nearbySegments};
}
} // namespace

math::Double3 BakedGeologyTexelDirection(
    const u32 face,
    const i32 x,
    const i32 y,
    const u32 resolution) noexcept
{
    const f64 u = (static_cast<f64>(x) + 0.5) /
        static_cast<f64>(resolution) * 2.0 - 1.0;
    const f64 v = (static_cast<f64>(y) + 0.5) /
        static_cast<f64>(resolution) * 2.0 - 1.0;
    math::Double3 cube{};
    switch (static_cast<world::CubeFace>(face))
    {
    case world::CubeFace::PositiveX: cube = {1.0, v, -u}; break;
    case world::CubeFace::NegativeX: cube = {-1.0, v, u}; break;
    case world::CubeFace::PositiveY: cube = {u, 1.0, -v}; break;
    case world::CubeFace::NegativeY: cube = {u, -1.0, v}; break;
    case world::CubeFace::PositiveZ: cube = {u, v, 1.0}; break;
    case world::CubeFace::NegativeZ: cube = {-u, v, -1.0}; break;
    }
    return math::Normalize(cube);
}

BakedGeologyRasters BakedGeologyRasters::Build(
    const u32 resolution,
    const u64 recipeHash,
    std::vector<f32> impactReliefGutter,
    std::vector<f32> iceReliefGutter,
    std::vector<BakedGeologyProcessTexel> processGutter)
{
    if (resolution < 2U || resolution > 4096U || recipeHash == 0U)
        throw std::invalid_argument("Baked geology header is invalid.");
    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    const std::size_t expected = static_cast<std::size_t>(kFaces) * stride * stride;
    if (impactReliefGutter.size() != expected ||
        !std::all_of(impactReliefGutter.begin(), impactReliefGutter.end(),
            [](const f32 value) { return std::isfinite(value); }))
        throw std::invalid_argument("Baked impact relief raster is invalid.");
    if (iceReliefGutter.empty()) iceReliefGutter.assign(expected, 0.0F);
    if (iceReliefGutter.size() != expected ||
        !std::all_of(iceReliefGutter.begin(), iceReliefGutter.end(),
            [](const f32 value) { return std::isfinite(value); }))
        throw std::invalid_argument("Baked ice relief raster is invalid.");
    const bool hasProcessChannels = !processGutter.empty();
    if (processGutter.empty()) processGutter.resize(expected);
    if (processGutter.size() != expected ||
        !std::all_of(processGutter.begin(), processGutter.end(), [](const auto& value)
        {
            return std::isfinite(value.excavationDepthMeters) &&
                std::isfinite(value.ejectaThicknessMeters) &&
                std::isfinite(value.debrisField) && std::isfinite(value.rayField) &&
                std::isfinite(value.meltThicknessMeters) && std::isfinite(value.brecciaField) &&
                std::isfinite(value.resurfacedMaterialFraction) &&
                std::isfinite(value.resurfacingThicknessMeters) &&
                std::isfinite(value.microImpactRoughnessMeters) &&
                std::isfinite(value.microImpactCoverage) &&
                std::isfinite(value.excavationCoverage) &&
                std::isfinite(value.formationAgeYears) && std::isfinite(value.exposureAgeYears) &&
                std::isfinite(value.iceDamage) && std::isfinite(value.fractureCoverage);
        }))
        throw std::invalid_argument("Baked geology process raster is invalid.");

    BakedGeologyRasters result;
    result.resolution_ = resolution;
    result.recipeHash_ = recipeHash;
    result.processChannelsValid_ = hasProcessChannels;
    result.impactReliefGutter_ = std::move(impactReliefGutter);
    result.iceReliefGutter_ = std::move(iceReliefGutter);
    result.processLevels_.push_back(std::move(processGutter));

    const auto buildPyramid = [&](std::vector<f32> base)
    {
        std::vector<std::vector<f32>> levels;
        levels.push_back(std::move(base));
        u32 levelResolution = resolution;
        while (levelResolution > 16U && levelResolution % 2U == 0U)
        {
            const u32 nextResolution = levelResolution / 2U;
            if (nextResolution < 16U) break;
            const std::size_t nextStride = static_cast<std::size_t>(nextResolution) + 2U;
            std::vector<f32> next(static_cast<std::size_t>(kFaces) * nextStride * nextStride);
            for (u32 face = 0U; face < kFaces; ++face)
            {
                for (i32 y = -1; y <= static_cast<i32>(nextResolution); ++y)
                {
                    for (i32 x = -1; x <= static_cast<i32>(nextResolution); ++x)
                    {
                        const std::size_t target =
                            (static_cast<std::size_t>(face) * nextStride + static_cast<std::size_t>(y + 1)) *
                            nextStride + static_cast<std::size_t>(x + 1);
                        const auto& previous = levels.back();
                        const i32 fineX = x * 2;
                        const i32 fineY = y * 2;
                        next[target] = 0.25F * (
                            SampleRaster(previous, levelResolution,
                                BakedGeologyTexelDirection(face, fineX, fineY, levelResolution)) +
                            SampleRaster(previous, levelResolution,
                                BakedGeologyTexelDirection(face, fineX + 1, fineY, levelResolution)) +
                            SampleRaster(previous, levelResolution,
                                BakedGeologyTexelDirection(face, fineX, fineY + 1, levelResolution)) +
                            SampleRaster(previous, levelResolution,
                                BakedGeologyTexelDirection(face, fineX + 1, fineY + 1, levelResolution)));
                    }
                }
            }
            levels.push_back(std::move(next));
            levelResolution = nextResolution;
        }
        return levels;
    };
    result.impactLevels_ = buildPyramid(result.impactReliefGutter_);
    result.iceLevels_ = buildPyramid(result.iceReliefGutter_);
    for (u32 level = 0U; level < static_cast<u32>(result.impactLevels_.size()); ++level)
    {
        result.levelResolutions_.push_back(
            level == 0U ? resolution : result.levelResolutions_.back() / 2U);
        const auto& impact = result.impactLevels_[level];
        const auto& ice = result.iceLevels_[level];
        result.gpuReliefPyramid_.insert(
            result.gpuReliefPyramid_.end(), impact.begin(), impact.end());
        result.gpuReliefPyramid_.insert(
            result.gpuReliefPyramid_.end(), ice.begin(), ice.end());
    }
    u32 processResolution = resolution;
    for (std::size_t level = 1U; level < result.levelResolutions_.size(); ++level)
    {
        const u32 nextResolution = result.levelResolutions_[level];
        const std::size_t nextStride = static_cast<std::size_t>(nextResolution) + 2U;
        std::vector<BakedGeologyProcessTexel> nextProcess(
            static_cast<std::size_t>(kFaces) * nextStride * nextStride);
        const auto& previous = result.processLevels_.back();
        for (u32 face = 0U; face < kFaces; ++face)
        {
            for (i32 y = -1; y <= static_cast<i32>(nextResolution); ++y)
            {
                for (i32 x = -1; x <= static_cast<i32>(nextResolution); ++x)
                {
                    const i32 fineX = x * 2;
                    const i32 fineY = y * 2;
                    nextProcess[ProcessIndex(nextResolution, face, x, y)] = AverageProcess(
                        SampleProcessRaster(previous, processResolution,
                            BakedGeologyTexelDirection(face, fineX, fineY, processResolution)),
                        SampleProcessRaster(previous, processResolution,
                            BakedGeologyTexelDirection(face, fineX + 1, fineY, processResolution)),
                        SampleProcessRaster(previous, processResolution,
                            BakedGeologyTexelDirection(face, fineX, fineY + 1, processResolution)),
                        SampleProcessRaster(previous, processResolution,
                            BakedGeologyTexelDirection(face, fineX + 1, fineY + 1, processResolution)));
                }
            }
        }
        result.processLevels_.push_back(std::move(nextProcess));
        processResolution = nextResolution;
    }
    u64 hash = StableCombine64(0x42414B4547454F31ULL, recipeHash);
    hash = StableCombine64(hash, resolution);
    hash = StableCombine64(hash, hasProcessChannels ? 1U : 0U);
    for (const f32 value : result.impactReliefGutter_)
        hash = StableCombine64(hash, std::bit_cast<u32>(value));
    for (const f32 value : result.iceReliefGutter_)
        hash = StableCombine64(hash, std::bit_cast<u32>(value));
    for (const auto& value : result.processLevels_.front())
    {
        hash = StableCombine64(hash, std::bit_cast<u32>(value.excavationDepthMeters));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.ejectaThicknessMeters));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.debrisField));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.rayField));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.meltThicknessMeters));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.brecciaField));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.resurfacedMaterialFraction));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.resurfacingThicknessMeters));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.microImpactRoughnessMeters));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.microImpactCoverage));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.excavationCoverage));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.formationAgeYears));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.exposureAgeYears));
        hash = StableCombine64(hash, value.formationAgeOrder);
        hash = StableCombine64(hash, value.exposureAgeOrder);
        hash = StableCombine64(hash, value.affectingImpacts);
        hash = StableCombine64(hash, std::bit_cast<u32>(value.iceDamage));
        hash = StableCombine64(hash, std::bit_cast<u32>(value.fractureCoverage));
        hash = StableCombine64(hash, value.nearbySegments);
    }
    result.contentHash_ = hash == 0U ? 1U : hash;
    return result;
}

f32 BakedGeologyRasters::ReliefDeltaMeters(
    const math::Double3& direction,
    const f64 footprintMeters,
    const f64 planetRadiusMeters) const noexcept
{
    return ImpactDeltaMeters(direction, footprintMeters, planetRadiusMeters) +
        IceDeltaMeters(direction, footprintMeters, planetRadiusMeters);
}

namespace
{
[[nodiscard]] std::size_t SelectGeologyLevel(
    const std::vector<u32>& levels,
    const f64 footprintMeters,
    const f64 planetRadiusMeters) noexcept
{
    std::size_t level = 0U;
    while (level + 1U < levels.size() && footprintMeters > 0.0 &&
        footprintMeters >= 4.0 * planetRadiusMeters / static_cast<f64>(levels[level]))
        ++level;
    return level;
}
} // namespace

f32 BakedGeologyRasters::ImpactDeltaMeters(
    const math::Double3& direction,
    const f64 footprintMeters,
    const f64 planetRadiusMeters) const noexcept
{
    if (impactLevels_.empty()) return 0.0F;
    const std::size_t level = SelectGeologyLevel(
        levelResolutions_, footprintMeters, planetRadiusMeters);
    return SampleRaster(impactLevels_[level], levelResolutions_[level], direction);
}

f32 BakedGeologyRasters::IceDeltaMeters(
    const math::Double3& direction,
    const f64 footprintMeters,
    const f64 planetRadiusMeters) const noexcept
{
    if (iceLevels_.empty()) return 0.0F;
    const std::size_t level = SelectGeologyLevel(
        levelResolutions_, footprintMeters, planetRadiusMeters);
    return SampleRaster(iceLevels_[level], levelResolutions_[level], direction);
}

BakedGeologyProcessTexel BakedGeologyRasters::SampleProcesses(
    const math::Double3& direction,
    const f64 footprintMeters,
    const f64 planetRadiusMeters) const noexcept
{
    if (resolution_ == 0U || !processChannelsValid_ || processLevels_.empty()) return {};
    const std::size_t level = SelectGeologyLevel(
        levelResolutions_, footprintMeters, planetRadiusMeters);
    return SampleProcessRaster(
        processLevels_[level], levelResolutions_[level], direction);
}
} // namespace orbit::terrain
