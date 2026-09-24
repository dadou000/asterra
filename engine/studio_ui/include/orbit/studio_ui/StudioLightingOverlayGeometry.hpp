#pragma once

#include <orbit/editor_ui/PathPreviewRenderer.hpp>
#include <orbit/lighting/EmissiveInvalidation.hpp>
#include <orbit/lighting/RadianceClipmap.hpp>
#include <orbit/lighting/RadianceClipmapResidency.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

namespace orbit::studio_ui
{
namespace detail
{
inline void AppendBox(
    std::vector<editor_ui::PreviewLine>& lines,
    const math::Float3 center,
    const f32 halfExtent,
    const math::Float4 color)
{
    const std::array<math::Float3, 8> p{{
        center + math::Float3{-halfExtent, -halfExtent, -halfExtent},
        center + math::Float3{ halfExtent, -halfExtent, -halfExtent},
        center + math::Float3{ halfExtent,  halfExtent, -halfExtent},
        center + math::Float3{-halfExtent,  halfExtent, -halfExtent},
        center + math::Float3{-halfExtent, -halfExtent,  halfExtent},
        center + math::Float3{ halfExtent, -halfExtent,  halfExtent},
        center + math::Float3{ halfExtent,  halfExtent,  halfExtent},
        center + math::Float3{-halfExtent,  halfExtent,  halfExtent}
    }};

    constexpr std::array<std::array<u32, 2>, 12> edges{{
        {{0U,1U}},{{1U,2U}},{{2U,3U}},{{3U,0U}},
        {{4U,5U}},{{5U,6U}},{{6U,7U}},{{7U,4U}},
        {{0U,4U}},{{1U,5U}},{{2U,6U}},{{3U,7U}}
    }};

    for (const auto& edge : edges)
    {
        lines.push_back({
            .start = p[edge[0]],
            .end = p[edge[1]],
            .color = color
        });
    }
}

inline void AppendSphere(
    std::vector<editor_ui::PreviewLine>& lines,
    const math::Float3 center,
    const f32 radius,
    const math::Float4 color)
{
    constexpr u32 segments = 32U;

    for (u32 axis = 0U; axis < 3U; ++axis)
    {
        for (u32 segment = 0U; segment < segments; ++segment)
        {
            const f32 a = static_cast<f32>(
                2.0 * std::numbers::pi_v<f64> *
                static_cast<f64>(segment) /
                static_cast<f64>(segments));
            const f32 b = static_cast<f32>(
                2.0 * std::numbers::pi_v<f64> *
                static_cast<f64>(segment + 1U) /
                static_cast<f64>(segments));

            math::Float3 pa = center;
            math::Float3 pb = center;

            if (axis == 0U)
            {
                pa.y += std::cos(a) * radius;
                pa.z += std::sin(a) * radius;
                pb.y += std::cos(b) * radius;
                pb.z += std::sin(b) * radius;
            }
            else if (axis == 1U)
            {
                pa.x += std::cos(a) * radius;
                pa.z += std::sin(a) * radius;
                pb.x += std::cos(b) * radius;
                pb.z += std::sin(b) * radius;
            }
            else
            {
                pa.x += std::cos(a) * radius;
                pa.y += std::sin(a) * radius;
                pb.x += std::cos(b) * radius;
                pb.y += std::sin(b) * radius;
            }

            lines.push_back({
                .start = pa,
                .end = pb,
                .color = color
            });
        }
    }
}
} // namespace detail

[[nodiscard]] inline std::vector<editor_ui::PreviewLine>
BuildGiUpdateCellOverlayLines(
    std::span<const lighting::RadianceUpdateCandidate> candidates,
    const lighting::RadianceClipmapConfig& config,
    const lighting::LightingView& view,
    const u32 maximumCells = 48U)
{
    std::vector<editor_ui::PreviewLine> lines;
    const u32 count = std::min<u32>(
        static_cast<u32>(candidates.size()),
        std::max(maximumCells, 1U));
    lines.reserve(static_cast<std::size_t>(count) * 12U);

    for (u32 index = 0U; index < count; ++index)
    {
        const auto& candidate = candidates[index];
        const f32 halfExtent = static_cast<f32>(
            lighting::RadianceCellSizeMeters(config, candidate.key.level) * 0.5);
        const auto center = lighting::RadianceCellGpuCenter(
            candidate.key,
            config,
            view);

        const f32 priority = std::clamp(candidate.priority, 0.0F, 4.0F) / 4.0F;
        detail::AppendBox(
            lines,
            center,
            halfExtent,
            {1.0F, 0.35F + 0.55F * priority, 0.10F, 0.85F});
    }

    return lines;
}

[[nodiscard]] inline std::vector<editor_ui::PreviewLine>
BuildRadianceCacheRegionOverlayLines(
    const math::Double3 observerInFrameMeters,
    const lighting::RadianceClipmapConfig& config,
    const lighting::LightingView& view,
    const u32 maximumLevels = 3U)
{
    std::vector<editor_ui::PreviewLine> lines;
    const u32 levelCount = std::min(
        config.levelCount,
        std::max(maximumLevels, 1U));
    lines.reserve(static_cast<std::size_t>(levelCount) * 12U);

    for (u32 level = 0U; level < levelCount; ++level)
    {
        const auto key = lighting::RadianceCellForPoint(
            observerInFrameMeters,
            config,
            level,
            view);
        const auto center = lighting::RadianceCellGpuCenter(key, config, view);
        const f32 halfExtent = static_cast<f32>(
            lighting::RadianceCellSizeMeters(config, level) *
            static_cast<f64>(config.cellsPerAxis) * 0.5);
        const f32 t = levelCount > 1U
            ? static_cast<f32>(level) / static_cast<f32>(levelCount - 1U)
            : 0.0F;

        detail::AppendBox(
            lines,
            center,
            halfExtent,
            {0.12F + 0.35F * t, 0.72F, 1.0F - 0.35F * t, 0.55F});
    }

    return lines;
}

[[nodiscard]] inline std::vector<editor_ui::PreviewLine>
BuildEmissiveInfluenceOverlayLines(
    std::span<const lighting::DynamicEmissiveSourceState> sources,
    const lighting::LightingView& view,
    const u32 maximumSources = 32U)
{
    std::vector<editor_ui::PreviewLine> lines;
    const u32 count = std::min<u32>(
        static_cast<u32>(sources.size()),
        std::max(maximumSources, 1U));
    lines.reserve(static_cast<std::size_t>(count) * 96U);

    for (u32 index = 0U; index < count; ++index)
    {
        const auto& source = sources[index];
        const auto center = lighting::ToLightingCameraRelative(
            source.centerInFrameMeters,
            view);
        const f32 radius = static_cast<f32>(
            std::max(source.influenceRangeMeters, source.sourceRadiusMeters));

        if (radius <= 0.0F)
        {
            continue;
        }

        detail::AppendSphere(
            lines,
            center,
            radius,
            {1.0F, 0.22F, 0.82F, 0.65F});
    }

    return lines;
}
} // namespace orbit::studio_ui
