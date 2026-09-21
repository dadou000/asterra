#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::lighting
{
enum class VisibilityPurpose : u8
{
    DiffuseGi,
    Reflection,
    Shadow,
    SkyVisibility,
    ProbeUpdate,
    Diagnostic
};

enum class VisibilityResolution : u8
{
    Unresolved,
    Hit,
    Miss
};

enum class VisibilityBackendKind : u8
{
    Unknown,
    ScreenSpace,
    Analytic,
    TerrainHeightfield,
    SoftwareProxy,
    HardwareRayQuery
};

enum class VisibilityCapability : u32
{
    None = 0U,
    ViewDependent = 1U << 0U,
    Offscreen = 1U << 1U,
    ExactGeometry = 1U << 2U,
    PlanetaryRange = 1U << 3U,
    DynamicGeometry = 1U << 4U,
    SurfaceMaterial = 1U << 5U
};

[[nodiscard]] constexpr VisibilityCapability operator|(
    const VisibilityCapability a,
    const VisibilityCapability b) noexcept
{
    return static_cast<VisibilityCapability>(
        static_cast<u32>(a) |
        static_cast<u32>(b));
}

[[nodiscard]] constexpr bool HasCapability(
    const VisibilityCapability value,
    const VisibilityCapability required) noexcept
{
    return
        (static_cast<u32>(value) &
         static_cast<u32>(required)) ==
        static_cast<u32>(required);
}

struct VisibilityRequirements
{
    // Callers state what the answer must guarantee, never which backend
    // should provide it.
    bool requireOffscreenCoverage{false};
    bool requireExactGeometry{false};
    bool requirePlanetaryRange{false};
    bool requireSurfaceMaterial{false};

    f32 maximumNominalErrorMeters{
        std::numeric_limits<f32>::infinity()};
    f32 minimumConfidence{0.0F};
};

struct VisibilityQuery
{
    VisibilityPurpose purpose{
        VisibilityPurpose::DiffuseGi};

    frames::FrameId frame{};
    universe::BodyId body{};

    math::Double3 originInFrameMeters{};
    math::Float3 direction{0.0F, 0.0F, 1.0F};

    f32 minimumDistanceMeters{0.01F};
    f32 maximumDistanceMeters{100.0F};
    f32 importance{1.0F};

    VisibilityRequirements requirements{};
};

struct VisibilityProviderDesc
{
    u64 providerId{0U};
    std::string name;
    VisibilityBackendKind kind{
        VisibilityBackendKind::Unknown};
    VisibilityCapability capabilities{
        VisibilityCapability::None};

    // Used only as a selection hint. A provider may still report lower
    // per-query confidence or an unresolved result.
    f32 nominalErrorMeters{
        std::numeric_limits<f32>::infinity()};

    i32 priority{0};
};

struct VisibilityHit
{
    f32 distanceMeters{0.0F};
    math::Double3 positionInFrameMeters{};
    math::Float3 geometricNormal{0.0F, 1.0F, 0.0F};
    math::Float3 shadingNormal{0.0F, 1.0F, 0.0F};
    u32 materialId{0U};
    u32 instanceId{0U};
};

struct VisibilityResult
{
    VisibilityResolution resolution{
        VisibilityResolution::Unresolved};

    VisibilityHit hit{};

    f32 confidence{0.0F};

    // True means the provider can prove that no acceptable hit exists for
    // this query domain. A screen-space miss, for example, is not terminal.
    bool terminal{false};

    VisibilityBackendKind backend{
        VisibilityBackendKind::Unknown};
    u64 providerId{0U};
    std::string providerName;
};

struct VisibilityAttempt
{
    u64 providerId{0U};
    std::string providerName;
    VisibilityBackendKind backend{
        VisibilityBackendKind::Unknown};
    VisibilityResolution resolution{
        VisibilityResolution::Unresolved};
    f32 confidence{0.0F};
    bool terminal{false};
};

struct VisibilityTraceDiagnostics
{
    std::vector<VisibilityAttempt> attempts;
};

struct GpuVisibilityQuery
{
    // Camera-relative origin and normalized direction. Batch GPU providers
    // operate in the active LightingView frame; arbitrary-frame/double
    // transforms happen before encoding.
    math::Float4 originMinimumDistance{};
    math::Float4 directionMaximumDistance{};

    // x = maximum nominal proxy error (infinity allowed)
    // y = minimum confidence
    // z = importance
    // w = requirement flags encoded as float bits
    math::Float4 requirements{};
};

struct GpuVisibilityResult
{
    u32 resolution{0U};
    u32 backend{0U};
    f32 confidence{0.0F};
    f32 distanceMeters{0.0F};

    // xyz = provider-relative hit position.
    // w bit pattern carries materialId for GPU/readback consumers.
    math::Float4 position{};

    // xyz = geometric normal.
    // w bit pattern carries instanceId.
    math::Float4 normal{};
};

static_assert(sizeof(GpuVisibilityQuery) == 48U);
static_assert(sizeof(GpuVisibilityResult) == 48U);

[[nodiscard]] GpuVisibilityQuery EncodeGpuVisibilityQuery(
    const VisibilityQuery& query,
    const LightingView& view) noexcept;


class VisibilityProvider
{
public:
    virtual ~VisibilityProvider() = default;

    VisibilityProvider(const VisibilityProvider&) = delete;
    VisibilityProvider& operator=(const VisibilityProvider&) = delete;

    [[nodiscard]] virtual const VisibilityProviderDesc&
    Description() const noexcept = 0;

    [[nodiscard]] virtual bool SupportsPurpose(
        VisibilityPurpose purpose) const noexcept = 0;

    [[nodiscard]] virtual VisibilityResult Trace(
        const VisibilityQuery& query) = 0;

protected:
    VisibilityProvider() = default;
};

class VisibilityRegistry
{
public:
    void Register(VisibilityProvider& provider);
    void Unregister(u64 providerId) noexcept;

    [[nodiscard]] std::vector<VisibilityProviderDesc>
    CandidateProviders(
        const VisibilityQuery& query) const;

    [[nodiscard]] VisibilityResult Trace(
        const VisibilityQuery& query,
        VisibilityTraceDiagnostics* diagnostics = nullptr) const;

    // Occlusion/nearest-surface mode: evaluates every qualifying provider and
    // returns the closest sufficiently confident hit across representations.
    // Unlike Trace(), a terminal miss from one provider does not suppress a
    // hit from another representation.
    [[nodiscard]] VisibilityResult TraceNearest(
        const VisibilityQuery& query,
        VisibilityTraceDiagnostics* diagnostics = nullptr) const;

private:
    [[nodiscard]] bool Qualifies(
        const VisibilityProviderDesc& provider,
        const VisibilityQuery& query) const noexcept;

    std::vector<VisibilityProvider*> providers_;
};

[[nodiscard]] bool ValidateVisibilityQuery(
    const VisibilityQuery& query) noexcept;
} // namespace orbit::lighting
