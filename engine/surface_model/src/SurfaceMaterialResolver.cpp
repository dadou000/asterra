#include <orbit/surface_model/SurfaceMaterialResolver.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::surface_model
{
namespace
{
[[nodiscard]] f32 SmoothStep01(
    const f32 value) noexcept
{
    const f32 t =
        std::clamp(
            value,
            0.0F,
            1.0F);

    return
        t * t *
        (3.0F -
         2.0F * t);
}

[[nodiscard]] f32 RangeMask(
    const f32 value,
    const f32 minimum,
    const f32 maximum,
    const f32 falloff) noexcept
{
    if (value >= minimum &&
        value <= maximum)
    {
        return 1.0F;
    }

    if (falloff <= 0.0F)
    {
        return 0.0F;
    }

    if (value < minimum)
    {
        if (value <=
            minimum -
                falloff)
        {
            return 0.0F;
        }

        return
            SmoothStep01(
                (value -
                 (minimum -
                  falloff)) /
                falloff);
    }

    if (value >=
        maximum +
            falloff)
    {
        return 0.0F;
    }

    return
        1.0F -
        SmoothStep01(
            (value -
             maximum) /
            falloff);
}

[[nodiscard]] RenderedSurfaceMaterialKind PhysicalKind(
    const terrain_material_column::ExposedSurfaceKind kind) noexcept
{
    using Physical =
        terrain_material_column::
            ExposedSurfaceKind;

    switch (kind)
    {
    case Physical::Bedrock:
        return
            RenderedSurfaceMaterialKind::
                Bedrock;
    case Physical::Regolith:
        return
            RenderedSurfaceMaterialKind::
                Regolith;
    case Physical::Soil:
        return
            RenderedSurfaceMaterialKind::
                Soil;
    case Physical::Sand:
        return
            RenderedSurfaceMaterialKind::
                Sand;
    case Physical::Debris:
        return
            RenderedSurfaceMaterialKind::
                Debris;
    }

    return
        RenderedSurfaceMaterialKind::
            Bedrock;
}

[[nodiscard]] terrain_biome::BiomeExposedMaterialMask PhysicalMask(
    const terrain_material_column::ExposedSurfaceKind kind) noexcept
{
    using Physical =
        terrain_material_column::
            ExposedSurfaceKind;

    using Mask =
        terrain_biome::
            BiomeExposedMaterialMask;

    switch (kind)
    {
    case Physical::Bedrock:
        return Mask::Bedrock;
    case Physical::Regolith:
        return Mask::Regolith;
    case Physical::Soil:
        return Mask::Soil;
    case Physical::Sand:
        return Mask::Sand;
    case Physical::Debris:
        return Mask::Debris;
    }

    return Mask::None;
}

[[nodiscard]] RenderedSurfaceMaterialKind OverlayKind(
    const terrain_biome::BiomeSurfaceLayerKind kind) noexcept
{
    using Layer =
        terrain_biome::
            BiomeSurfaceLayerKind;

    switch (kind)
    {
    case Layer::Snow:
        return
            RenderedSurfaceMaterialKind::
                Snow;
    case Layer::Moss:
        return
            RenderedSurfaceMaterialKind::
                Moss;
    case Layer::Litter:
        return
            RenderedSurfaceMaterialKind::
                Litter;
    case Layer::Dust:
        return
            RenderedSurfaceMaterialKind::
                Dust;
    }

    return
        RenderedSurfaceMaterialKind::
            Dust;
}

[[nodiscard]] f32 ProcessMask(
    const terrain_biome::BiomeSurfaceLayerKind kind,
    const SurfaceMaterialFeatureMasks& features) noexcept
{
    using Layer =
        terrain_biome::
            BiomeSurfaceLayerKind;

    switch (kind)
    {
    case Layer::Snow:
        return
            features.snowCoverage;
    case Layer::Moss:
        return
            features.mossPotential;
    case Layer::Litter:
        return
            features.litterAvailability;
    case Layer::Dust:
        return
            features.dustAvailability;
    }

    return 0.0F;
}

void AddOrMerge(
    ResolvedSurfaceMaterialBlend& blend,
    const SurfaceMaterialContribution contribution)
{
    for (auto& current :
         blend.contributions)
    {
        if (current.kind ==
                contribution.kind &&
            current.rock ==
                contribution.rock)
        {
            current.weight +=
                contribution.weight;
            return;
        }
    }

    blend.contributions.push_back(
        contribution);
}

void CompositeOverlay(
    ResolvedSurfaceMaterialBlend& blend,
    const RenderedSurfaceMaterialKind kind,
    const f32 opacity)
{
    const f32 alpha =
        std::clamp(
            opacity,
            0.0F,
            1.0F);

    if (alpha <= 0.0F)
    {
        return;
    }

    const f32 keep =
        1.0F -
        alpha;

    for (auto& contribution :
         blend.contributions)
    {
        contribution.weight *=
            keep;
    }

    AddOrMerge(
        blend,
        {
            .kind = kind,
            .weight = alpha
        });
}
} // namespace

bool SurfaceMaterialFeatureMasks::IsValid() const noexcept
{
    const auto unit =
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0F &&
                value <= 1.0F;
        };

    return
        std::isfinite(
            slopeDegrees) &&
        slopeDegrees >= 0.0F &&
        slopeDegrees <= 90.0F &&
        std::isfinite(
            curvature) &&
        unit(snowCoverage) &&
        unit(mossPotential) &&
        unit(litterAvailability) &&
        unit(dustAvailability);
}

bool SurfaceMaterialContribution::IsValid() const noexcept
{
    if (!std::isfinite(weight) ||
        weight < 0.0F ||
        weight > 1.0F)
    {
        return false;
    }

    if (kind ==
        RenderedSurfaceMaterialKind::
            Bedrock)
    {
        return rock.IsValid();
    }

    return
        !rock.IsValid();
}

f32 ResolvedSurfaceMaterialBlend::Weight(
    const RenderedSurfaceMaterialKind kind) const noexcept
{
    f32 result = 0.0F;

    for (const auto& contribution :
         contributions)
    {
        if (contribution.kind ==
            kind)
        {
            result +=
                contribution.weight;
        }
    }

    return result;
}

f32 ResolvedSurfaceMaterialBlend::TotalWeight() const noexcept
{
    f32 result = 0.0F;

    for (const auto& contribution :
         contributions)
    {
        result +=
            contribution.weight;
    }

    return result;
}

bool ResolvedSurfaceMaterialBlend::IsValid() const noexcept
{
    if (contributions.empty())
    {
        return false;
    }

    for (const auto& contribution :
         contributions)
    {
        if (!contribution.IsValid())
        {
            return false;
        }
    }

    return
        std::abs(
            TotalWeight() -
            1.0F) <=
        2.0e-5F;
}

ResolvedSurfaceMaterialBlend ResolveSurfaceMaterialBlend(
    const terrain_material_column::ExposedSurfaceState& physicalSurface,
    const terrain_biome::BiomeService& biomeService,
    const std::span<const terrain_biome::ResolvedBiomeWeight> biomeWeights,
    const SurfaceMaterialFeatureMasks& features)
{
    if (!physicalSurface.IsValid() ||
        !features.IsValid())
    {
        throw std::invalid_argument(
            "Orbit M21 surface material resolver received invalid shared state.");
    }

    ResolvedSurfaceMaterialBlend result;

    result.contributions.push_back({
        .kind =
            PhysicalKind(
                physicalSurface.material),
        .weight = 1.0F,
        .rock =
            physicalSurface.BedrockExposed()
                ? physicalSurface.
                    exposedRock
                : terrain_geology::
                    RockTypeId{}
    });

    std::vector<
        terrain_biome::
            ResolvedBiomeWeight>
        ordered(
            biomeWeights.begin(),
            biomeWeights.end());

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const auto& a,
           const auto& b)
        {
            if (a.base != b.base)
            {
                return a.base;
            }

            return
                a.id <
                b.id;
        });

    const auto exposedMask =
        PhysicalMask(
            physicalSurface.material);

    for (const auto& resolved :
         ordered)
    {
        if (!resolved.id.IsValid() ||
            !std::isfinite(
                resolved.weight) ||
            resolved.weight <= 0.0F)
        {
            continue;
        }

        const auto* biome =
            biomeService.Find(
                resolved.id);

        if (biome == nullptr)
        {
            continue;
        }

        for (const auto& layer :
             biome->surface.layers)
        {
            if (!layer.enabled)
            {
                continue;
            }

            const u32 compatible =
                static_cast<u32>(
                    layer.
                        compatibleExposed);

            if ((compatible &
                 static_cast<u32>(
                     exposedMask)) ==
                0U)
            {
                continue;
            }

            const f32 slope =
                RangeMask(
                    features.
                        slopeDegrees,
                    layer.
                        minimumSlopeDegrees,
                    layer.
                        maximumSlopeDegrees,
                    layer.
                        slopeFalloffDegrees);

            const f32 curvature =
                RangeMask(
                    features.
                        curvature,
                    layer.
                        minimumCurvature,
                    layer.
                        maximumCurvature,
                    layer.
                        curvatureFalloff);

            const f32 moisture =
                RangeMask(
                    physicalSurface.
                        moisture,
                    layer.
                        minimumMoisture,
                    layer.
                        maximumMoisture,
                    layer.
                        moistureFalloff);

            const f32 process =
                ProcessMask(
                    layer.kind,
                    features);

            const f32 opacity =
                std::clamp(
                    resolved.weight *
                        biome->surface.
                            materialInfluence *
                        layer.strength *
                        slope *
                        curvature *
                        moisture *
                        process,
                    0.0F,
                    1.0F);

            CompositeOverlay(
                result,
                OverlayKind(
                    layer.kind),
                opacity);
        }
    }

    result.contributions.erase(
        std::remove_if(
            result.contributions.begin(),
            result.contributions.end(),
            [](const auto& contribution)
            {
                return
                    contribution.weight <=
                    1.0e-7F;
            }),
        result.contributions.end());

    if (!result.IsValid())
    {
        throw std::logic_error(
            "Orbit M21 produced an invalid deterministic material blend.");
    }

    return result;
}
} // namespace orbit::surface_model
