#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/LightingView.hpp>
#include <orbit/math/Vector.hpp>

#include <array>
#include <cstddef>
#include <vector>

namespace orbit::lighting
{
// First-order spherical-harmonic representation of diffuse irradiance.
// The four RGB coefficients encode one constant term plus X/Y/Z directional
// gradients. This is deliberately low-frequency: high-frequency visible-scene
// detail belongs to M13 final gather, while this cache remains stable/reusable.
struct DirectionalIrradianceL1
{
    math::Float3 l0{};
    math::Float3 l1x{};
    math::Float3 l1y{};
    math::Float3 l1z{};
};

[[nodiscard]] math::Float3 EvaluateIrradiance(
    const DirectionalIrradianceL1& irradiance,
    math::Float3 unitDirection) noexcept;

// Sky-only irradiance of a cell, kept apart from the one-bounce L1 above so
// the sky is applied at full strength and independently of how confident the
// screen-space gather is. For a surface normal n:
//
//     E(n) = l0 * (1 + dot(gradient, n))
//
// With l0 = 0.5 * skyIrradiance * visibleFraction and gradient = the unit
// direction toward the open part of the sky, an unoccluded cell gives the sky
// irradiance on an up-facing surface, half of it on a vertical one, none on a
// down-facing one. Terrain and authored proxies occlude it through the cell's
// sky-visibility traces.
struct SkyIrradianceL1
{
    math::Float3 l0{};
    math::Float3 gradient{};
};

[[nodiscard]] math::Float3 EvaluateSkyIrradiance(
    const SkyIrradianceL1& sky,
    math::Float3 unitDirection) noexcept;

struct RadianceCellKey
{
    frames::FrameId frame{};
    universe::BodyId body{};
    u32 level{0U};
    i64 x{0};
    i64 y{0};
    i64 z{0};

    [[nodiscard]] constexpr bool operator==(
        const RadianceCellKey&) const noexcept = default;
};

// GPU storage contract: 96 bytes/cell.
//
// irradiance0.rgb = L0
// irradiance0.w   = validity [0,1]
// irradianceX.rgb = L1 X
// irradianceX.w   = update age in seconds
// irradianceY.rgb = L1 Y
// irradianceY.w   = sample count
// irradianceZ.rgb = L1 Z
// irradianceZ.w   = revision encoded as float-compatible low 24 bits.
//
// skyIrradiance.rgb = sky-only L0 (see SkyIrradianceL1)
// skyIrradiance.w   = reserved
// skyGradient.xyz   = sky-only gradient direction
// skyGradient.w     = one-bounce transport the cell's L1 was estimated with,
//                     so the reflection shaders can reproduce the sky the L1
//                     used to carry
//
// Full CPU revision identity stays outside the packed GPU cell and is checked
// by the residency/update layer before upload. The packed revision is only a
// debug/stale-detection aid, not semantic authority.
struct GpuRadianceCell
{
    math::Float4 irradiance0{};
    math::Float4 irradianceX{};
    math::Float4 irradianceY{};
    math::Float4 irradianceZ{};
    math::Float4 skyIrradiance{};
    math::Float4 skyGradient{};
};

static_assert(sizeof(GpuRadianceCell) == 96U);

struct RadianceCell
{
    DirectionalIrradianceL1 irradiance{};
    SkyIrradianceL1 sky{};
    f32 skyTransport{0.0F};
    u64 revision{0U};
    f32 updateAgeSeconds{0.0F};
    u32 sampleCount{0U};
    bool valid{false};
};

[[nodiscard]] GpuRadianceCell EncodeGpuRadianceCell(
    const RadianceCell& cell) noexcept;

struct RadianceClipmapConfig
{
    // Cell size at level 0. Higher levels multiply by levelScale.
    f64 baseCellSizeMeters{2.0};
    f64 levelScale{4.0};

    u32 levelCount{6U};

    // Dense logical cube edge length for each level. M12 controls scrolling
    // and physical residency; M11 only freezes addressing/storage semantics.
    u32 cellsPerAxis{32U};
};

struct RadianceClipmapLevelLayout
{
    u32 level{0U};
    f64 cellSizeMeters{0.0};
    u32 cellsPerAxis{0U};
    u64 cellCount{0U};
    u64 byteSize{0U};
};

struct RadianceClipmapMemoryLayout
{
    u64 bytesPerCell{sizeof(GpuRadianceCell)};
    u64 totalCells{0U};
    u64 totalBytes{0U};
    std::vector<RadianceClipmapLevelLayout> levels;
};

[[nodiscard]] f64 RadianceCellSizeMeters(
    const RadianceClipmapConfig& config,
    u32 level);

[[nodiscard]] RadianceCellKey RadianceCellForPoint(
    const math::Double3& pointInFrameMeters,
    const RadianceClipmapConfig& config,
    u32 level,
    const LightingView& view);

[[nodiscard]] math::Double3 RadianceCellCenterInFrame(
    const RadianceCellKey& key,
    const RadianceClipmapConfig& config);

[[nodiscard]] math::Float3 RadianceCellGpuCenter(
    const RadianceCellKey& key,
    const RadianceClipmapConfig& config,
    const LightingView& view);

[[nodiscard]] RadianceClipmapMemoryLayout
BuildRadianceClipmapMemoryLayout(
    const RadianceClipmapConfig& config);
} // namespace orbit::lighting
