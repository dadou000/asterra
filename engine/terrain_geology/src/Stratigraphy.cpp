#include <orbit/terrain_geology/Stratigraphy.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_geology
{
namespace
{
[[nodiscard]] bool Finite(const f64 value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool FiniteDirection(
    const math::Double3& value) noexcept
{
    return
        Finite(value.x) &&
        Finite(value.y) &&
        Finite(value.z) &&
        math::LengthSquared(value) > 0.0;
}
} // namespace

bool StratigraphyLayer::IsValid() const noexcept
{
    return
        material.IsValid() &&
        Finite(thicknessMeters) &&
        thicknessMeters > 0.0 &&
        Finite(transitionBandMeters) &&
        transitionBandMeters >= 0.0 &&
        transitionBandMeters <= thicknessMeters;
}

bool StratigraphyTransform::IsValid() const noexcept
{
    return
        FiniteDirection(anchorUnitDirection) &&
        Finite(referenceTopRadialOffsetMeters) &&
        Finite(tiltEastSlope) &&
        Finite(tiltNorthSlope) &&
        Finite(foldAmplitudeMeters) &&
        foldAmplitudeMeters >= 0.0 &&
        Finite(foldWavelengthMeters) &&
        foldWavelengthMeters > 0.0 &&
        Finite(foldAzimuthRadians) &&
        Finite(foldPhaseRadians) &&
        Finite(warpAmplitudeMeters) &&
        warpAmplitudeMeters >= 0.0 &&
        Finite(warpWavelengthMeters) &&
        warpWavelengthMeters > 0.0 &&
        Finite(warpPhaseRadians);
}

bool StratigraphyProfile::IsValid() const noexcept
{
    if (!id.IsValid() ||
        name.empty() ||
        !basementMaterial.IsValid() ||
        !transform.IsValid())
    {
        return false;
    }

    for (const StratigraphyLayer& layer : layers)
    {
        if (!layer.IsValid())
        {
            return false;
        }
    }

    return true;
}

bool ReferencesKnownMaterials(
    const StratigraphyProfile& profile,
    const GeologicalMaterialLibrary& materials) noexcept
{
    if (!profile.IsValid() ||
        materials.Find(profile.basementMaterial) == nullptr)
    {
        return false;
    }

    for (const StratigraphyLayer& layer : profile.layers)
    {
        if (materials.Find(layer.material) == nullptr)
        {
            return false;
        }
    }

    return true;
}

CompiledStratigraphyProfile::CompiledStratigraphyProfile(
    StratigraphyProfile profile,
    const GeologicalMaterialLibrary& materials)
    : profile_(std::move(profile))
{
    if (!profile_.IsValid())
    {
        throw std::invalid_argument(
            "Cannot compile an invalid stratigraphy profile.");
    }

    if (!ReferencesKnownMaterials(profile_, materials))
    {
        throw std::invalid_argument(
            "Stratigraphy profile references unknown geological materials.");
    }

    layerBottomDepths_.reserve(profile_.layers.size());

    f64 cumulativeDepth = 0.0;
    for (const StratigraphyLayer& layer : profile_.layers)
    {
        cumulativeDepth += layer.thicknessMeters;
        if (!Finite(cumulativeDepth))
        {
            throw std::overflow_error(
                "Stratigraphy cumulative layer depth overflowed.");
        }

        layerBottomDepths_.push_back(cumulativeDepth);
    }
}

const StratigraphyProfile&
CompiledStratigraphyProfile::Profile() const noexcept
{
    return profile_;
}

std::span<const f64>
CompiledStratigraphyProfile::LayerBottomDepths() const noexcept
{
    return layerBottomDepths_;
}

std::size_t
CompiledStratigraphyProfile::LayerCount() const noexcept
{
    return profile_.layers.size();
}

f64 CompiledStratigraphyProfile::LayerTopDepthMeters(
    const std::size_t layerIndex) const
{
    if (layerIndex >= profile_.layers.size())
    {
        throw std::out_of_range(
            "Stratigraphy layer index is out of range.");
    }

    return layerIndex == 0
        ? 0.0
        : layerBottomDepths_[layerIndex - 1];
}

f64 CompiledStratigraphyProfile::LayerBottomDepthMeters(
    const std::size_t layerIndex) const
{
    if (layerIndex >= layerBottomDepths_.size())
    {
        throw std::out_of_range(
            "Stratigraphy layer index is out of range.");
    }

    return layerBottomDepths_[layerIndex];
}

f64 CompiledStratigraphyProfile::DeformedTopRadialOffsetMeters(
    const world::PlanetDefinition& planet,
    const terrain::PlanetSurfacePosition& position) const noexcept
{
    if (!planet.id.IsValid() ||
        !position.IsValid() ||
        position.planet != planet.id)
    {
        return std::numeric_limits<f64>::quiet_NaN();
    }

    const terrain::PlanetSurfacePosition canonical =
        terrain::CanonicalizeSurfacePosition(position);

    const terrain::PlanetSurfacePosition anchor{
        .planet = planet.id,
        .unitDirection = math::Normalize(
            profile_.transform.anchorUnitDirection),
        .radialOffsetMeters = 0.0
    };

    const world::SurfaceFrame anchorFrame =
        terrain::SurfaceTangentFrame(anchor);

    const math::Double2 local =
        terrain::SurfaceOffsetBetweenPositions(
            planet,
            anchor,
            anchorFrame,
            canonical);

    if (!Finite(local.x) ||
        !Finite(local.y))
    {
        return std::numeric_limits<f64>::quiet_NaN();
    }

    constexpr f64 twoPi =
        2.0 * std::numbers::pi_v<f64>;
    constexpr f64 halfPi =
        0.5 * std::numbers::pi_v<f64>;

    const f64 warpWaveNumber =
        twoPi /
        profile_.transform.warpWavelengthMeters;

    const f64 warpedEast =
        local.x +
        profile_.transform.warpAmplitudeMeters *
            std::sin(
                warpWaveNumber * local.y +
                profile_.transform.warpPhaseRadians);

    const f64 warpedNorth =
        local.y +
        profile_.transform.warpAmplitudeMeters *
            std::sin(
                warpWaveNumber * local.x +
                profile_.transform.warpPhaseRadians +
                halfPi);

    const f64 tiltOffset =
        profile_.transform.tiltEastSlope *
            warpedEast +
        profile_.transform.tiltNorthSlope *
            warpedNorth;

    const f64 foldDirectionEast =
        std::cos(
            profile_.transform.foldAzimuthRadians);
    const f64 foldDirectionNorth =
        std::sin(
            profile_.transform.foldAzimuthRadians);

    const f64 foldCoordinate =
        warpedEast * foldDirectionEast +
        warpedNorth * foldDirectionNorth;

    const f64 foldOffset =
        profile_.transform.foldAmplitudeMeters *
        std::sin(
            twoPi * foldCoordinate /
                profile_.transform.foldWavelengthMeters +
            profile_.transform.foldPhaseRadians);

    return
        profile_.transform.referenceTopRadialOffsetMeters +
        tiltOffset +
        foldOffset;
}

StratigraphySample
CompiledStratigraphyProfile::SampleExposedLayer(
    const world::PlanetDefinition& planet,
    const terrain::PlanetSurfacePosition& position) const noexcept
{
    const f64 topRadialOffset =
        DeformedTopRadialOffsetMeters(
            planet,
            position);

    if (!Finite(topRadialOffset))
    {
        return {};
    }

    const terrain::PlanetSurfacePosition canonical =
        terrain::CanonicalizeSurfacePosition(position);

    const f64 depthBelowTop =
        std::max(
            0.0,
            topRadialOffset -
                canonical.radialOffsetMeters);

    const auto found =
        std::lower_bound(
            layerBottomDepths_.begin(),
            layerBottomDepths_.end(),
            depthBelowTop);

    if (found == layerBottomDepths_.end())
    {
        const f64 basementTop =
            layerBottomDepths_.empty()
                ? 0.0
                : layerBottomDepths_.back();

        return {
            .primaryMaterial =
                profile_.basementMaterial,
            .secondaryMaterial = {},
            .secondaryWeight = 0.0F,
            .layerIndex =
                static_cast<u32>(
                    profile_.layers.size()),
            .basement = true,
            .depthBelowTopMeters = depthBelowTop,
            .depthIntoLayerMeters =
                std::max(
                    0.0,
                    depthBelowTop -
                        basementTop),
            .deformedTopRadialOffsetMeters =
                topRadialOffset
        };
    }

    const std::size_t layerIndex =
        static_cast<std::size_t>(
            std::distance(
                layerBottomDepths_.begin(),
                found));

    const f64 layerTop =
        layerIndex == 0
            ? 0.0
            : layerBottomDepths_[layerIndex - 1];
    const f64 layerBottom =
        layerBottomDepths_[layerIndex];
    const StratigraphyLayer& layer =
        profile_.layers[layerIndex];

    RockTypeId secondary{};
    f32 secondaryWeight = 0.0F;

    if (layer.transitionBandMeters > 0.0)
    {
        const f64 transitionStart =
            layerBottom -
            layer.transitionBandMeters;

        if (depthBelowTop >= transitionStart)
        {
            secondary =
                layerIndex + 1 <
                        profile_.layers.size()
                    ? profile_.layers[
                          layerIndex + 1].material
                    : profile_.basementMaterial;

            if (secondary != layer.material)
            {
                const f64 weight =
                    std::clamp(
                        (depthBelowTop -
                         transitionStart) /
                            layer.transitionBandMeters,
                        0.0,
                        1.0);

                secondaryWeight =
                    static_cast<f32>(weight);
            }
            else
            {
                secondary = {};
            }
        }
    }

    return {
        .primaryMaterial = layer.material,
        .secondaryMaterial = secondary,
        .secondaryWeight = secondaryWeight,
        .layerIndex =
            static_cast<u32>(layerIndex),
        .basement = false,
        .depthBelowTopMeters = depthBelowTop,
        .depthIntoLayerMeters =
            std::max(
                0.0,
                depthBelowTop -
                    layerTop),
        .deformedTopRadialOffsetMeters =
            topRadialOffset
    };
}
} // namespace orbit::terrain_geology
