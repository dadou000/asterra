#include <orbit/lighting/Visibility.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] VisibilityCapability RequiredCapabilities(
    const VisibilityRequirements& requirements) noexcept
{
    VisibilityCapability result =
        VisibilityCapability::None;

    if (requirements.requireOffscreenCoverage)
    {
        result =
            result |
            VisibilityCapability::Offscreen;
    }

    if (requirements.requireExactGeometry)
    {
        result =
            result |
            VisibilityCapability::ExactGeometry;
    }

    if (requirements.requirePlanetaryRange)
    {
        result =
            result |
            VisibilityCapability::PlanetaryRange;
    }

    if (requirements.requireSurfaceMaterial)
    {
        result =
            result |
            VisibilityCapability::SurfaceMaterial;
    }

    return result;
}

[[nodiscard]] f32 ClampConfidence(
    const f32 confidence) noexcept
{
    if (!std::isfinite(confidence))
    {
        return 0.0F;
    }

    return std::clamp(
        confidence,
        0.0F,
        1.0F);
}
} // namespace

GpuVisibilityQuery EncodeGpuVisibilityQuery(
    const VisibilityQuery& query,
    const LightingView& view) noexcept
{
    math::Double3 origin =
        query.originInFrameMeters;

    if (query.frame == view.frame)
    {
        origin =
            origin -
            view.gpuOriginInFrameMeters;
    }

    math::Float3 direction =
        query.direction;

    const f32 lengthSquared =
        math::LengthSquared(direction);

    if (std::isfinite(lengthSquared) &&
        lengthSquared > 1.0e-12F)
    {
        direction =
            math::Normalize(direction);
    }
    else
    {
        direction =
            {0.0F, 0.0F, 1.0F};
    }

    return {
        .originMinimumDistance = {
            static_cast<f32>(origin.x),
            static_cast<f32>(origin.y),
            static_cast<f32>(origin.z),
            std::max(
                query.minimumDistanceMeters,
                0.0F)
        },
        .directionMaximumDistance = {
            direction.x,
            direction.y,
            direction.z,
            std::max(
                query.maximumDistanceMeters,
                query.minimumDistanceMeters)
        },
        .requirements = {
            query.requirements.maximumNominalErrorMeters,
            query.requirements.minimumConfidence,
            query.importance,
            std::bit_cast<f32>(
                (query.requirements.requireOffscreenCoverage ? 1U : 0U) |
                (query.requirements.requireExactGeometry ? 2U : 0U) |
                (query.requirements.requirePlanetaryRange ? 4U : 0U) |
                (query.requirements.requireSurfaceMaterial ? 8U : 0U))
        }
    };
}

bool ValidateVisibilityQuery(
    const VisibilityQuery& query) noexcept
{
    if (!query.frame ||
        !std::isfinite(query.minimumDistanceMeters) ||
        !std::isfinite(query.maximumDistanceMeters) ||
        query.minimumDistanceMeters < 0.0F ||
        query.maximumDistanceMeters <=
            query.minimumDistanceMeters ||
        !std::isfinite(query.importance) ||
        query.importance < 0.0F ||
        !std::isfinite(
            query.requirements.minimumConfidence) ||
        query.requirements.minimumConfidence < 0.0F ||
        query.requirements.minimumConfidence > 1.0F ||
        std::isnan(
            query.requirements.
                maximumNominalErrorMeters) ||
        query.requirements.
                maximumNominalErrorMeters <
            0.0F)
    {
        return false;
    }

    const f32 directionLengthSquared =
        math::LengthSquared(
            query.direction);

    return
        std::isfinite(directionLengthSquared) &&
        directionLengthSquared > 1.0e-12F &&
        std::isfinite(query.originInFrameMeters.x) &&
        std::isfinite(query.originInFrameMeters.y) &&
        std::isfinite(query.originInFrameMeters.z);
}

void VisibilityRegistry::Register(
    VisibilityProvider& provider)
{
    const auto& desc =
        provider.Description();

    if (desc.providerId == 0U ||
        desc.name.empty())
    {
        throw std::invalid_argument(
            "Visibility provider requires stable non-zero ID and name.");
    }

    const auto duplicate =
        std::find_if(
            providers_.begin(),
            providers_.end(),
            [&](const VisibilityProvider* existing)
            {
                return
                    existing != nullptr &&
                    existing->Description().
                        providerId ==
                    desc.providerId;
            });

    if (duplicate != providers_.end())
    {
        throw std::invalid_argument(
            "Visibility provider ID is already registered.");
    }

    providers_.push_back(
        &provider);

    std::stable_sort(
        providers_.begin(),
        providers_.end(),
        [](const VisibilityProvider* a,
           const VisibilityProvider* b)
        {
            return
                a->Description().priority >
                b->Description().priority;
        });
}

void VisibilityRegistry::Unregister(
    const u64 providerId) noexcept
{
    std::erase_if(
        providers_,
        [providerId](
            const VisibilityProvider* provider)
        {
            return
                provider == nullptr ||
                provider->Description().
                    providerId ==
                    providerId;
        });
}

bool VisibilityRegistry::Qualifies(
    const VisibilityProviderDesc& provider,
    const VisibilityQuery& query) const noexcept
{
    const auto required =
        RequiredCapabilities(
            query.requirements);

    if (!HasCapability(
            provider.capabilities,
            required))
    {
        return false;
    }

    if (std::isfinite(
            query.requirements.
                maximumNominalErrorMeters) &&
        (!std::isfinite(
             provider.nominalErrorMeters) ||
         provider.nominalErrorMeters >
             query.requirements.
                 maximumNominalErrorMeters))
    {
        return false;
    }

    return true;
}

std::vector<VisibilityProviderDesc>
VisibilityRegistry::CandidateProviders(
    const VisibilityQuery& query) const
{
    if (!ValidateVisibilityQuery(query))
    {
        throw std::invalid_argument(
            "Visibility query is invalid.");
    }

    std::vector<VisibilityProviderDesc>
        result;

    for (const auto* provider :
         providers_)
    {
        if (provider == nullptr ||
            !provider->SupportsPurpose(
                query.purpose) ||
            !Qualifies(
                provider->Description(),
                query))
        {
            continue;
        }

        result.push_back(
            provider->Description());
    }

    return result;
}

VisibilityResult VisibilityRegistry::Trace(
    const VisibilityQuery& query,
    VisibilityTraceDiagnostics* diagnostics) const
{
    if (!ValidateVisibilityQuery(query))
    {
        throw std::invalid_argument(
            "Visibility query is invalid.");
    }

    if (diagnostics != nullptr)
    {
        diagnostics->attempts.clear();
    }

    VisibilityResult last{};

    for (auto* provider :
         providers_)
    {
        if (provider == nullptr ||
            !provider->SupportsPurpose(
                query.purpose) ||
            !Qualifies(
                provider->Description(),
                query))
        {
            continue;
        }

        auto result =
            provider->Trace(query);

        const auto& desc =
            provider->Description();

        result.backend = desc.kind;
        result.providerId = desc.providerId;
        result.providerName = desc.name;
        result.confidence =
            ClampConfidence(
                result.confidence);

        if (diagnostics != nullptr)
        {
            diagnostics->attempts.push_back({
                .providerId =
                    result.providerId,
                .providerName =
                    result.providerName,
                .backend =
                    result.backend,
                .resolution =
                    result.resolution,
                .confidence =
                    result.confidence,
                .terminal =
                    result.terminal
            });
        }

        last = result;

        if (result.resolution ==
                VisibilityResolution::Hit &&
            result.confidence >=
                query.requirements.
                    minimumConfidence)
        {
            return result;
        }

        if (result.resolution ==
                VisibilityResolution::Miss &&
            result.terminal &&
            result.confidence >=
                query.requirements.
                    minimumConfidence)
        {
            return result;
        }
    }

    // No provider produced a sufficiently confident terminal answer.
    // Preserve the final provider provenance for diagnostics but make the
    // unresolved state explicit to callers.
    last.resolution =
        VisibilityResolution::Unresolved;
    last.terminal = false;
    return last;
}

VisibilityResult VisibilityRegistry::TraceNearest(
    const VisibilityQuery& query,
    VisibilityTraceDiagnostics* diagnostics) const
{
    if (!ValidateVisibilityQuery(query))
    {
        throw std::invalid_argument(
            "Visibility query is invalid.");
    }

    if (diagnostics != nullptr)
    {
        diagnostics->attempts.clear();
    }

    VisibilityResult closest{};
    bool hasClosest = false;
    bool sawUnresolved = false;
    bool allTerminalMiss = true;
    VisibilityResult last{};

    for (auto* provider : providers_)
    {
        if (provider == nullptr ||
            !provider->SupportsPurpose(query.purpose) ||
            !Qualifies(provider->Description(), query))
        {
            continue;
        }

        auto result = provider->Trace(query);
        const auto& desc = provider->Description();

        result.backend = desc.kind;
        result.providerId = desc.providerId;
        result.providerName = desc.name;
        result.confidence =
            ClampConfidence(result.confidence);

        if (diagnostics != nullptr)
        {
            diagnostics->attempts.push_back({
                .providerId = result.providerId,
                .providerName = result.providerName,
                .backend = result.backend,
                .resolution = result.resolution,
                .confidence = result.confidence,
                .terminal = result.terminal
            });
        }

        last = result;

        if (result.resolution ==
                VisibilityResolution::Hit &&
            result.confidence >=
                query.requirements.minimumConfidence &&
            (!hasClosest ||
             result.hit.distanceMeters <
                 closest.hit.distanceMeters))
        {
            closest = result;
            hasClosest = true;
        }

        if (result.resolution ==
                VisibilityResolution::Unresolved ||
            !result.terminal)
        {
            sawUnresolved = true;
        }

        if (result.resolution !=
                VisibilityResolution::Miss ||
            !result.terminal ||
            result.confidence <
                query.requirements.minimumConfidence)
        {
            allTerminalMiss = false;
        }
    }

    if (hasClosest)
    {
        closest.terminal = true;
        return closest;
    }

    if (allTerminalMiss &&
        !providers_.empty())
    {
        last.resolution =
            VisibilityResolution::Miss;
        last.terminal = true;
        return last;
    }

    last.resolution =
        VisibilityResolution::Unresolved;
    last.terminal = false;
    if (sawUnresolved)
    {
        return last;
    }

    return last;
}

} // namespace orbit::lighting
