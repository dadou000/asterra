#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/math/Vector.hpp>

#include <span>
#include <vector>

namespace orbit::lighting
{
enum class LocalLightType : u8
{
    Point,
    Spot
};

struct LocalLight
{
    LocalLightType type{LocalLightType::Point};

    // Stable semantic position in LightingView::frame coordinates.
    math::Double3 positionInFrameMeters{};

    // Direction light points toward, expressed in the same frame.
    math::Float3 direction{0.0F, -1.0F, 0.0F};

    math::Float3 colorLinear{1.0F, 1.0F, 1.0F};

    // Authored luminous flux. Shading converts this to an approximate
    // isotropic/candela scale according to light type.
    f32 luminousFluxLumens{800.0F};

    f32 rangeMeters{12.0F};
    f32 innerConeRadians{0.39269908F};
    f32 outerConeRadians{0.61086524F};

    u64 stableId{0U};
};

struct ResolvedLocalLight
{
    LocalLightType type{LocalLightType::Point};
    math::Float3 positionCameraRelativeMeters{};
    math::Float3 direction{0.0F, -1.0F, 0.0F};
    math::Float3 colorLinear{1.0F, 1.0F, 1.0F};
    f32 luminousFluxLumens{0.0F};
    f32 rangeMeters{0.0F};
    f32 innerConeCosine{1.0F};
    f32 outerConeCosine{0.0F};
    u64 stableId{0U};
};

struct GpuLocalLight
{
    math::Float4 positionType{};
    math::Float4 directionRange{};
    math::Float4 colorFlux{};
    math::Float4 cone{};
};

static_assert(sizeof(GpuLocalLight) == 64U);

[[nodiscard]] GpuLocalLight EncodeGpuLocalLight(
    const ResolvedLocalLight& light) noexcept;

struct TiledLightGridConfig
{
    u32 tileSizePixels{16U};
    u32 maximumLightsPerTile{64U};

    // When a tile is covered by more lights than fit, keep the strongest ones
    // (by flux over squared distance, ties in submission order) and replace the
    // rest with ONE proxy point light carrying their summed flux at the
    // flux-weighted centroid, so a crowd of individually weak lights keeps its
    // total energy. false restores plain first-N truncation.
    bool aggregateOverflow{true};
};

struct TiledLightGrid
{
    u32 viewportWidth{0U};
    u32 viewportHeight{0U};
    u32 tileSizePixels{16U};
    u32 tilesX{0U};
    u32 tilesY{0U};

    std::vector<ResolvedLocalLight> lights;

    // CSR-style tile lists. offsets.size() == tileCount + 1.
    std::vector<u32> offsets;
    std::vector<u32> lightIndices;

    // Light-to-tile assignments that did not get an individual slot. With
    // aggregateOverflow their flux is carried by a proxy; without it it is lost.
    u32 droppedAssignments{0U};

    // Proxy lights appended to `lights` (dropped sets shared by neighbouring
    // tiles share one proxy). Proxy stableId has kProxyLightIdBit set.
    u32 proxyLights{0U};
};

inline constexpr u64 kProxyLightIdBit = 1ULL << 63U;

[[nodiscard]] ResolvedLocalLight ResolveLocalLight(
    const LocalLight& light,
    const LightingView& view) noexcept;

[[nodiscard]] TiledLightGrid BuildTiledLightGrid(
    std::span<const LocalLight> lights,
    const LightingView& view,
    u32 viewportWidth,
    u32 viewportHeight,
    const TiledLightGridConfig& config = {});
} // namespace orbit::lighting
