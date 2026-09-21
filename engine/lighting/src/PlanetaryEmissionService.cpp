#include <orbit/lighting/PlanetaryEmissionService.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] bool SameCenter(
    const math::Double3& a,
    const math::Double3& b) noexcept
{
    return
        math::LengthSquared(a - b) <=
        1.0e-12;
}

void AppendLeafEmitters(
    const EmissiveHierarchy& hierarchy,
    std::vector<EmissiveSampledEmitter>& output)
{
    for (u32 index = 0U;
         index < hierarchy.nodes.size();
         ++index)
    {
        const auto& node =
            hierarchy.nodes[index];

        if (!node.IsLeaf() ||
            node.radiantImportance <= 0.0 ||
            node.areaMetersSquared <= 0.0)
        {
            continue;
        }

        output.push_back({
            .sourceStableId =
                hierarchy.stableId,
            .nodeIndex = index,
            .positionInFrameMeters =
                node.energyCentroidInFrameMeters,
            .normalInFrame =
                hierarchy.surfaceNormal,
            .averageRadiance =
                node.averageRadiance,
            .integratedRadianceArea =
                node.integratedRadianceArea,
            .areaMetersSquared =
                node.areaMetersSquared,
            .radiantImportance =
                node.radiantImportance,
            .projectedPixels = 0.0F,
            .samplingProbability = 0.0F,
            .estimatorWeight = 1.0F,
            .promotedSmallEmitter = false
        });
    }
}
} // namespace

PlanetaryEmissionService::
PlanetaryEmissionService(
    PlanetaryEmissionFieldConfig config)
    : config_(config)
{
    if (config_.baseWidth == 0U ||
        config_.baseHeight == 0U ||
        config_.maximumLevels == 0U)
    {
        throw std::invalid_argument(
            "Planetary emission service requires non-zero field dimensions.");
    }
}

u64 PlanetaryEmissionService::MixRevision(
    const u64 seed,
    const u64 value) noexcept
{
    return
        seed ^
        (value +
         0x9e3779b97f4a7c15ULL +
         (seed << 6U) +
         (seed >> 2U));
}

u64 PlanetaryEmissionService::ComputeBodyRevision(
    const BodyState& state) const noexcept
{
    u64 revision =
        0x504c414e454d4954ULL;

    for (const auto& [stableId, source] :
         state.sources)
    {
        revision =
            MixRevision(
                revision,
                stableId);
        revision =
            MixRevision(
                revision,
                source.surface.
                    contentRevision);
    }

    return revision != 0U
        ? revision
        : 1U;
}

bool PlanetaryEmissionService::Upsert(
    RuntimeEmissiveSurface surface)
{
    const auto& geometry =
        surface.geometry;

    if (!geometry.frame ||
        !geometry.body ||
        geometry.stableId == 0U ||
        surface.width == 0U ||
        surface.height == 0U ||
        surface.giRadiance.size() !=
            static_cast<std::size_t>(
                surface.width) *
            surface.height)
    {
        throw std::invalid_argument(
            "Planetary emission service received an invalid emissive surface.");
    }

    auto& state =
        bodies_[geometry.body];

    if (state.body &&
        (state.frame != geometry.frame ||
         state.body != geometry.body))
    {
        throw std::invalid_argument(
            "Planetary emission body sources must share one frame authority.");
    }

    state.frame =
        geometry.frame;
    state.body =
        geometry.body;

    const auto found =
        state.sources.find(
            geometry.stableId);

    if (found != state.sources.end())
    {
        const auto& existing =
            found->second.surface;

        if (existing.contentRevision ==
                surface.contentRevision &&
            existing.geometry.originInFrameMeters ==
                surface.geometry.originInFrameMeters &&
            existing.geometry.axisUInFrameMeters ==
                surface.geometry.axisUInFrameMeters &&
            existing.geometry.axisVInFrameMeters ==
                surface.geometry.axisVInFrameMeters)
        {
            return false;
        }
    }

    state.sources.insert_or_assign(
        geometry.stableId,
        Source{
            .surface =
                std::move(surface)
        });

    state.sourceRevision =
        ComputeBodyRevision(state);
    state.dirty = true;
    return true;
}

bool PlanetaryEmissionService::Remove(
    const universe::BodyId body,
    const u64 stableId)
{
    const auto found =
        bodies_.find(body);

    if (found == bodies_.end())
    {
        return false;
    }

    auto& state =
        found->second;

    if (state.sources.erase(
            stableId) == 0U)
    {
        return false;
    }

    state.sourceRevision =
        ComputeBodyRevision(state);
    state.dirty = true;

    if (state.sources.empty())
    {
        state.field.reset();
    }

    return true;
}

void PlanetaryEmissionService::Clear() noexcept
{
    bodies_.clear();
}

const PlanetaryEmissionField*
PlanetaryEmissionService::Resolve(
    const frames::FrameId frame,
    const universe::BodyId body,
    const math::Double3& bodyCenterInFrameMeters)
{
    const auto found =
        bodies_.find(body);

    if (found == bodies_.end() ||
        found->second.sources.empty())
    {
        return nullptr;
    }

    auto& state =
        found->second;

    if (frame != state.frame)
    {
        throw std::invalid_argument(
            "Planetary emission resolve frame does not match source authority.");
    }

    if (!state.dirty &&
        state.field.has_value() &&
        state.hasCenter &&
        SameCenter(
            state.lastBodyCenter,
            bodyCenterInFrameMeters))
    {
        return &*state.field;
    }

    std::vector<EmissiveSampledEmitter>
        emitters;

    for (const auto& [stableId, source] :
         state.sources)
    {
        static_cast<void>(stableId);

        const auto hierarchy =
            BuildEmissiveHierarchy(
                source.surface.Grid());

        AppendLeafEmitters(
            hierarchy,
            emitters);
    }

    state.field =
        BuildPlanetaryEmissionField(
            frame,
            body,
            bodyCenterInFrameMeters,
            state.sourceRevision,
            emitters,
            config_);

    state.lastBodyCenter =
        bodyCenterInFrameMeters;
    state.hasCenter = true;
    state.dirty = false;
    ++rebuildCount_;

    return &*state.field;
}

PlanetaryEmissionServiceStats
PlanetaryEmissionService::Stats() const noexcept
{
    PlanetaryEmissionServiceStats result{
        .bodyCount =
            static_cast<u32>(
                bodies_.size()),
        .rebuildCount =
            rebuildCount_
    };

    for (const auto& [body, state] :
         bodies_)
    {
        static_cast<void>(body);
        result.sourceCount +=
            static_cast<u32>(
                state.sources.size());
    }

    return result;
}
} // namespace orbit::lighting
