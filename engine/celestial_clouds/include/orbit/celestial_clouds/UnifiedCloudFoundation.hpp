#pragma once

// System B foundations. This header must not include or call CloudRenderer (A).
// Both systems may read immutable CloudField/atmosphere/terrain inputs, but they
// never share mutable histories, GPU light caches, or output render targets.
#include <orbit/core/Types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <map>
#include <string>
#include <string_view>

namespace orbit::celestial_clouds::unified
{
// Explicit backend identity: the existing shader-define split compares two A
// variants; it must never be reported as an A-versus-B comparison.
enum class CloudBackend : u8 { BaselineA = 0, UnifiedB = 1 };

// Only mark B available after it owns a full draw, history, lighting and
// compositor path. Do not silently render A when B was requested.
[[nodiscard]] inline CloudBackend RequireAvailableBackend(
    const CloudBackend requested, const bool unifiedReady)
{
    if (requested == CloudBackend::BaselineA) return requested;
    if (requested == CloudBackend::UnifiedB && unifiedReady) return requested;
    throw std::logic_error(
        "Cloud System B is not render-ready; refusing an invalid A/B result.");
}

// Immutable identity of inputs consumed by B. The host must use stable
// identifiers and content revisions, not pointer addresses or camera-origin
// floats. Sun/atmosphere revisions represent physical input changes.
struct FrameSignature
{
    u64 worldGeneration{0};
    u64 bodyKey{0};
    u64 terrainRevision{0};
    u64 cloudFieldFingerprint{0};
    u64 stormRevision{0};
    u64 atmosphereRevision{0};
    u64 sunRevision{0};
    u64 qualityRevision{0};
    u32 width{0};
    u32 height{0};
    // True on teleport, time discontinuity, world load or history-unsafe jump.
    bool discontinuity{false};

    [[nodiscard]] constexpr bool operator==(const FrameSignature&) const noexcept = default;
};

// Separate invalidation domains: never rebuild storm density because only
// the Sun moved, and never keep stale radiance after a lightning/time jump.
struct Invalidation
{
    bool density{false};
    bool sunlight{false};
    bool aerialPerspective{false};
    bool temporalHistory{false};
    bool renderTargets{false};
    bool receiverShadows{false};

    [[nodiscard]] constexpr bool Any() const noexcept
    {
        return density || sunlight || aerialPerspective || temporalHistory ||
            renderTargets || receiverShadows;
    }
};

[[nodiscard]] constexpr Invalidation AllInvalid() noexcept
{
    return {true, true, true, true, true, true};
}

[[nodiscard]] inline Invalidation Invalidate(
    const std::optional<FrameSignature>& previous,
    const FrameSignature& next)
{
    if (next.width == 0 || next.height == 0)
        throw std::invalid_argument("Unified clouds require a positive viewport extent.");
    if (!previous || next.discontinuity ||
        previous->worldGeneration != next.worldGeneration ||
        previous->bodyKey != next.bodyKey)
        return AllInvalid();

    Invalidation result{};
    if (previous->width != next.width || previous->height != next.height)
    {
        result.renderTargets = true;
        result.temporalHistory = true;
    }
    if (previous->cloudFieldFingerprint != next.cloudFieldFingerprint ||
        previous->stormRevision != next.stormRevision)
    {
        result.density = true;
        result.sunlight = true;
        result.receiverShadows = true;
        // Ordinary weather advection is continuous: reproject history using
        // motion vectors. A discontinuous storm/time jump sets discontinuity.
    }
    if (previous->terrainRevision != next.terrainRevision)
    {
        result.sunlight = true;
        result.receiverShadows = true;
        result.temporalHistory = true;
    }
    if (previous->sunRevision != next.sunRevision)
    {
        result.sunlight = true;
        result.aerialPerspective = true;
        result.receiverShadows = true;
        // The Sun moves continuously; invalidating history here every frame
        // would disable temporal reconstruction in normal gameplay.
    }
    if (previous->atmosphereRevision != next.atmosphereRevision)
    {
        result.sunlight = true;
        result.aerialPerspective = true;
        result.temporalHistory = true;
    }
    if (previous->qualityRevision != next.qualityRevision)
    {
        result.temporalHistory = true;
        result.renderTargets = true;
    }
    return result;
}

// Shared *radiometric output contract*, not a shared renderer. Radiance is
// scene-linear, already integrated over the interval. T is scalar, neutral
// extinction. More general coloured absorption is an explicit future schema
// change, not something to hide in premultiplied alpha.
struct IntegratedMedium
{
    std::array<f64, 3> radiance{0.0, 0.0, 0.0};
    f64 transmittance{1.0};
};

// For a near slab N and far slab F, the radiance reaching the eye is
// L(N) + T(N)*L(F). The same equation works for cloud and clear-air segments,
// provided each physical interval is included exactly once.
[[nodiscard]] inline IntegratedMedium CompositeFrontToBack(
    const IntegratedMedium& near, const IntegratedMedium& far)
{
    const auto valid = [](const IntegratedMedium& m)
    {
        return std::isfinite(m.transmittance) &&
            m.transmittance >= 0.0 && m.transmittance <= 1.0 &&
            std::all_of(m.radiance.begin(), m.radiance.end(),
                [](const f64 v) { return std::isfinite(v) && v >= 0.0; });
    };
    if (!valid(near) || !valid(far))
        throw std::invalid_argument("Unified cloud radiance/visibility is invalid.");

    IntegratedMedium output{};
    output.transmittance = near.transmittance * far.transmittance;
    for (std::size_t channel = 0; channel < 3; ++channel)
        output.radiance[channel] =
            near.radiance[channel] + near.transmittance * far.radiance[channel];
    return output;
}

// Each B viewport owns its independent history/generation domain. Never use
// A's "studio.primary" or "studio.primary.B" cache names for B. This CPU
// registry is safe to use before a B GPU renderer is implemented and makes
// lifetime/invalidation semantics testable without graphics hardware.
class ViewRegistry
{
public:
    [[nodiscard]] Invalidation Observe(
        const std::string_view viewId, const FrameSignature& signature)
    {
        if (viewId.empty())
            throw std::invalid_argument("Unified clouds require a stable viewport ID.");
        auto& previous = views_[std::string(viewId)];
        const Invalidation invalidation = Invalidate(previous, signature);
        previous = signature;
        return invalidation;
    }

    void Forget(const std::string_view viewId)
    {
        views_.erase(std::string(viewId));
    }

    void Reset() noexcept { views_.clear(); }
    [[nodiscard]] std::size_t Size() const noexcept { return views_.size(); }

private:
    std::map<std::string, std::optional<FrameSignature>, std::less<>> views_;
};

// Memory and update limits belong to B, not to A's graphics settings. B will
// use a fixed budget per body/view and evict far-field/cached data first.
struct ResourceBudget
{
    u64 maximumResidentBytes{256ULL * 1024ULL * 1024ULL};
    u32 maximumUpdatesPerFrame{4096U};
    u32 maximumViewSteps{192U};

    // B1 partitions the limit between its two RGBA16F render targets.
    // Use division instead of multiplying dimensions to avoid u64 overflow.
    [[nodiscard]] bool FitsPreviewTarget(
        const u32 width, const u32 height, const u32 bytesPerPixel = 8U) const noexcept
    {
        return Valid() && width != 0U && height != 0U &&
            bytesPerPixel != 0U &&
            static_cast<u64>(width) <=
                (maximumResidentBytes / 2U / bytesPerPixel) /
                static_cast<u64>(height);
    }

    [[nodiscard]] bool Valid() const noexcept
    {
        return maximumResidentBytes >= 1ULL * 1024ULL * 1024ULL &&
            maximumUpdatesPerFrame > 0U && maximumViewSteps > 0U;
    }
};

// No implicit fallback to A. The GPU implementation will produce a B-owned
// medium target, light cache, history, and performance pass namespace.
struct BackendReadiness
{
    bool ownMarch{false};
    bool ownSunCache{false};
    bool ownHistory{false};
    bool ownComposite{false};
    bool ownReceiverShadow{false};

    [[nodiscard]] constexpr bool FullABReady() const noexcept
    {
        return ownMarch && ownSunCache && ownHistory &&
            ownComposite && ownReceiverShadow;
    }
};
} // namespace orbit::celestial_clouds::unified
