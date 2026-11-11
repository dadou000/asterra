#include "StudioViewportInternals.hpp"
#include <orbit/core/Log.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <utility>

namespace orbit::studio_ui
{
std::shared_ptr<rhi::Buffer> StudioViewportRenderer::EnsureTerrainSunShadow(
    const std::string_view viewportId, const universe::BodyId body,
    const std::shared_ptr<const terrain::TerrainSource>& source, const f64 radius)
{
    static_cast<void>(viewportId);
    // Camera/viewport identity is deliberately absent: one authoritative
    // body-fixed field is shared by ground, orbit and auxiliary viewports.
    auto found = std::find_if(terrainSunShadowPresentations_.begin(),
        terrainSunShadowPresentations_.end(), [&](const TerrainSunShadowPresentation& candidate)
        {
            return candidate.keyBody == body && candidate.keySource == source &&
                candidate.keyRadius == radius;
        });
    if (found == terrainSunShadowPresentations_.end())
    {
        // Bound old source generations. Evicted jobs receive cancellation;
        // their promise futures never make the render thread join a worker.
        constexpr std::size_t kMaximumResidentFields = 8U;
        if (terrainSunShadowPresentations_.size() >= kMaximumResidentFields)
        {
            terrainSunShadowPresentations_.front().cancelBuild.request_stop();
            terrainSunShadowPresentations_.erase(terrainSunShadowPresentations_.begin());
        }
        terrainSunShadowPresentations_.emplace_back();
        found = terrainSunShadowPresentations_.end() - 1;
        found->keyBody = body;
        found->keySource = source;
        found->keyRadius = radius;
    }
    auto& cache = *found;
    const auto revision = source->Revision();

    // A promise-backed future does not join its worker on destruction.
    // Poll with zero timeout; the render thread never waits for terrain CPU
    // generation and can continue showing an already published generation.
    if (cache.pending.valid() &&
        cache.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            auto field = cache.pending.get();
            if (cache.pendingBody == body && cache.pendingSource == source &&
                cache.pendingRadius == radius && field.sourceRevision == revision)
            {
                // Keep CPU storage alive until every chunk has been transferred.
                // The incomplete GPU buffer is deliberately never published.
                cache.stagedField = std::make_unique<lighting::TerrainSunShadowField>(
                    std::move(field));
                cache.stagedBuffer.reset();
                cache.uploadedSamples = 0U;
            }
        }
        catch (const std::exception& e)
        {
            orbit::log::Warning(std::string("Terrain sun shadow generation failed: ") + e.what());
            cache.retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
        if (!cache.stagedField)
            cache.pendingSource.reset();
    }

    if (cache.stagedField)
    {
        // The source may have changed while the job was running or uploading.
        // Never attach an obsolete world-fixed shadow to the current planet.
        if (cache.pendingBody != body || cache.pendingSource != source ||
            cache.pendingRadius != radius || cache.stagedField->sourceRevision != revision)
        {
            cache.stagedField.reset();
            cache.stagedBuffer.reset();
            cache.uploadedSamples = 0U;
            cache.pendingSource.reset();
        }
        else
        {
            try
            {
                if (!cache.stagedBuffer)
                    cache.stagedBuffer = lighting::AllocateTerrainSunShadowFieldBuffer(
                        *device_, *cache.stagedField);

                // 256 KiB of height data per frame at most. A 1024x512 field
                // needs eight visits; only the final completed buffer is used
                // by the render graph. Buffer creation remains an RHI call.
                constexpr std::size_t kMaximumSamplesPerFrame = 65'536U;
                cache.uploadedSamples = lighting::UploadTerrainSunShadowFieldChunk(
                    *cache.stagedBuffer, *cache.stagedField,
                    cache.uploadedSamples, kMaximumSamplesPerFrame);

                if (cache.uploadedSamples == cache.stagedField->elevations.size() &&
                    source->Revision() == revision)
                {
                    cache.buffer = std::move(cache.stagedBuffer);
                    cache.body = body;
                    cache.source = source;
                    cache.radius = radius;
                    cache.revision = revision;
                    cache.stagedField.reset();
                    cache.pendingSource.reset();
                    cache.uploadedSamples = 0U;
                    orbit::log::Info("Body-fixed terrain sun shadow field activated.");
                }
            }
            catch (const std::exception& e)
            {
                orbit::log::Warning(std::string("Terrain sun shadow upload failed: ") + e.what());
                cache.stagedField.reset();
                cache.stagedBuffer.reset();
                cache.pendingSource.reset();
                cache.uploadedSamples = 0U;
                cache.retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
        }
    }

    const bool activeMatches = cache.body == body && cache.radius == radius &&
        cache.source == source && cache.revision == revision;
    if (!activeMatches && !cache.pending.valid() && !cache.stagedField &&
        std::chrono::steady_clock::now() >= cache.retryAfter)
    {
        // One bounded low-priority background worker per viewport renderer,
        // rather than spawning an unbounded std::async thread per request.
        if (!terrainSunShadowPool_)
            terrainSunShadowPool_ = std::make_unique<jobs::JobSystem>(1U, "TerrainSunShadow");
        auto completion = std::make_shared<std::promise<lighting::TerrainSunShadowField>>();
        cache.pending = completion->get_future();
        cache.pendingSource = source;
        cache.pendingBody = body;
        cache.pendingRadius = radius;
        cache.cancelBuild = std::stop_source{};
        const auto cancellation = cache.cancelBuild.get_token();
        try
        {
            terrainSunShadowPool_->Submit(jobs::JobPriority::Low,
                [source, radius, completion, cancellation]
                {
                    try
                    {
                        completion->set_value(
                            lighting::BuildTerrainSunShadowField(*source, radius,
                                1024U, 512U, cancellation));
                    }
                    catch (...)
                    {
                        completion->set_exception(std::current_exception());
                    }
                });
        }
        catch (const std::exception& e)
        {
            cache.pending = {};
            cache.pendingSource.reset();
            cache.retryAfter = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            orbit::log::Warning(std::string("Terrain sun shadow scheduling failed: ") + e.what());
        }
    }

    // This must be the *complete* candidate for this exact terrain revision.
    // A different body, source or revision receives the analytic fallback,
    // never stale terrain self-shadowing.
    return activeMatches ? cache.buffer : nullptr;
}
} // namespace orbit::studio_ui
