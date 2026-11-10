#include "StudioViewportInternals.hpp"

#include <algorithm>
#include <array>
#include <bit>

namespace orbit::studio_ui::viewport_detail
{

[[nodiscard]] u64 CombineFingerprint(
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

[[nodiscard]] u64 StableViewportHash(
    const std::string_view value) noexcept
{
    u64 hash =
        1469598103934665603ULL;

    for (const unsigned char ch :
         value)
    {
        hash ^= static_cast<u64>(ch);
        hash *=
            1099511628211ULL;
    }

    return hash;
}

[[nodiscard]]
celestial_scheduler::WorkKey
CelestialWorkKeyFor(
    const std::string_view viewportId,
    const universe::BodyId body,
    const celestial_scheduler::WorkKind kind) noexcept
{
    const u64 viewportHash =
        StableViewportHash(
            viewportId);

    return {
        .subjectHigh =
            body.high ^
            viewportHash,
        .subjectLow =
            body.low ^
            (viewportHash << 1U),
        .kind = kind
    };
}

[[nodiscard]] u64 QuantizedLightingFingerprintValue(
    const f32 value,
    const f32 quantum) noexcept
{
    if (!std::isfinite(value) ||
        quantum <= 0.0F)
    {
        return 0U;
    }

    const i64 quantized =
        static_cast<i64>(
            std::llround(
                static_cast<f64>(value) /
                static_cast<f64>(quantum)));

    return static_cast<u64>(quantized);
}

[[nodiscard]] math::Float3 AtmosphereSkyIrradianceSummary(
    const celestial_atmosphere::AtmosphereSkyView* sky) noexcept
{
    if (sky == nullptr ||
        sky->skyView.texels.empty())
    {
        return {};
    }

    // Cosine-weighted sky irradiance on an up-facing surface, from the sky
    // radiance's spherical-harmonic projection, normalized by Orbit's solar
    // reference irradiance. The sky table is in the sky frame (+Z = zenith).
    constexpr f64 kReferenceIrradiance =
        1361.0;

    // The projection walks the whole table (trig per texel) and the table only
    // changes when the sky is rebuilt, so remember the result per sky view,
    // keyed by the table's storage and a hash of strided texels.
    struct CachedSummary
    {
        const celestial_atmosphere::AtmosphereSkyView* sky{nullptr};
        const void* storage{nullptr};
        std::size_t count{0U};
        u64 hash{0U};
        math::Float3 value{};
    };
    thread_local std::array<CachedSummary, 8U> cache{};
    thread_local std::size_t cacheNext = 0U;

    const auto& texels = sky->skyView.texels;
    u64 hash = 1469598103934665603ULL;
    const std::size_t stride =
        std::max<std::size_t>(1U, texels.size() / 509U);
    for (std::size_t i = 0U; i < texels.size(); i += stride)
    {
        for (const f32 component : {texels[i].x, texels[i].y, texels[i].z})
        {
            hash = (hash ^ std::bit_cast<u32>(component)) *
                1099511628211ULL;
        }
    }

    for (const auto& entry : cache)
    {
        if (entry.sky == sky && entry.storage == texels.data() &&
            entry.count == texels.size() && entry.hash == hash)
        {
            return entry.value;
        }
    }

    const auto irradiance =
        celestial_atmosphere::EvaluateSkyIrradiance(
            celestial_atmosphere::
                ProjectSkyViewToSphericalHarmonics(
                    sky->skyView),
            {0.0, 0.0, 1.0});

    const math::Float3 value{
        static_cast<f32>(irradiance.x / kReferenceIrradiance),
        static_cast<f32>(irradiance.y / kReferenceIrradiance),
        static_cast<f32>(irradiance.z / kReferenceIrradiance)
    };
    cache[cacheNext] = {sky, texels.data(), texels.size(), hash, value};
    cacheNext = (cacheNext + 1U) % cache.size();
    return value;
}
// The sky-view table depends smoothly on the observer's altitude, but it is keyed
// by an exact hash of the radius and rebuilt on the CPU (about 22 ms), so a climb
// or descent rebuilt it on every frame. Snapping the altitude to ~2% steps keeps
// the sky within a fraction of a pixel of exact while rebuilding it only a few
// times per altitude decade.
[[nodiscard]] f64 QuantizedSkyObserverRadius(
    const f64 observerRadiusMeters,
    const f64 bottomRadiusMeters) noexcept
{
    const f64 altitude =
        std::max(observerRadiusMeters - bottomRadiusMeters, 1.0);
    constexpr f64 kLogStep = 0.02;
    const f64 snapped = std::exp(
        std::round(std::log(altitude) / kLogStep) * kLogStep);
    return bottomRadiusMeters + snapped;
}

} // namespace orbit::studio_ui::viewport_detail
