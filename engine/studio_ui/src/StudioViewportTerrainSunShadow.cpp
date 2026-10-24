#include <orbit/jobs/JobSystem.hpp>
#include "StudioViewportInternals.hpp"
#include <orbit/core/Log.hpp>

namespace orbit::studio_ui
{
std::shared_ptr<rhi::Buffer> StudioViewportRenderer::EnsureTerrainSunShadow(
    const std::string_view viewportId, const universe::BodyId body,
    const std::shared_ptr<const terrain::TerrainSource>& source, const f64 radius)
{
    auto& cache = terrainSunShadowPresentations_[std::string(viewportId)];
    const auto revision = source->Revision();
    if (cache.pending.valid() &&
        cache.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            const auto field = cache.pending.get();
            // A world/body/recipe change may have overtaken the worker.
            if (cache.pendingBody == body && cache.pendingSource == source &&
                cache.pendingRadius == radius && field.sourceRevision == revision)
            {
                auto candidate = lighting::UploadTerrainSunShadowField(*device_, field);
                cache.buffer = std::move(candidate);
                cache.body = body;
                cache.source = source;
                cache.radius = radius;
                cache.revision = revision;
                orbit::log::Info("Body-fixed terrain sun shadow field activated.");
            }
        }
        catch (const std::exception& e)
        {
            orbit::log::Warning(std::string("Terrain sun shadow candidate failed; keeping active field: ") + e.what());
            cache.retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
        cache.pendingSource.reset();
    }
    if (!cache.pending.valid() && std::chrono::steady_clock::now() >= cache.retryAfter &&
        (cache.source != source || cache.body != body || cache.radius != radius || cache.revision != revision))
    {
        auto admitted = jobs::TrySubmitBackgroundCpu([source, radius]
        {
            return lighting::BuildTerrainSunShadowField(*source, radius);
        }, jobs::JobPriority::Normal);
        if (admitted)
        {
            cache.pendingSource = source;
            cache.pendingBody = body;
            cache.pendingRadius = radius;
            cache.pending = std::move(*admitted);
        }
    }
    // Old same-body data remains usable while a new recipe builds. A field
    // from another body/radius is never applied to the replacement target.
    // Changed terrain revisions must not re-use old mountain shadows.
    return cache.body == body && cache.radius == radius &&
        cache.source == source && cache.revision == revision ? cache.buffer : nullptr;
}
} // namespace orbit::studio_ui
