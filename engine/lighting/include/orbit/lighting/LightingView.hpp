#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <cstdint>

namespace orbit::lighting
{
enum class LightingViewChange : u32
{
    None = 0U,
    CameraCut = 1U << 0U,
    GpuOriginRebase = 1U << 1U,
    FrameChanged = 1U << 2U,
    BodyChanged = 1U << 3U,
    ProjectionChanged = 1U << 4U
};

[[nodiscard]] constexpr LightingViewChange operator|(
    const LightingViewChange a,
    const LightingViewChange b) noexcept
{
    return static_cast<LightingViewChange>(
        static_cast<u32>(a) |
        static_cast<u32>(b));
}

[[nodiscard]] constexpr LightingViewChange& operator|=(
    LightingViewChange& a,
    const LightingViewChange b) noexcept
{
    a = a | b;
    return a;
}

[[nodiscard]] constexpr bool HasChange(
    const LightingViewChange value,
    const LightingViewChange test) noexcept
{
    return
        (static_cast<u32>(value) &
         static_cast<u32>(test)) != 0U;
}

struct LightingView
{
    frames::FrameId frame{};
    universe::BodyId body{};

    // Stable, double-precision semantic coordinates.
    math::Double3 cameraPositionInFrameMeters{};

    // Presentation origin used to produce float GPU positions. This may move
    // without changing any stable cache key.
    math::Double3 gpuOriginInFrameMeters{};
    u64 gpuOriginRevision{0U};

    math::Float3 forward{0.0F, 0.0F, 1.0F};
    math::Float3 up{0.0F, 1.0F, 0.0F};
    f32 verticalFovRadians{1.0F};

    LightingViewChange change{
        LightingViewChange::None};
};

struct StableLightingCell
{
    frames::FrameId frame{};
    universe::BodyId body{};
    i64 x{0};
    i64 y{0};
    i64 z{0};

    [[nodiscard]] constexpr bool operator==(
        const StableLightingCell&) const noexcept = default;
};

[[nodiscard]] math::Float3 ToLightingCameraRelative(
    const math::Double3& pointInFrameMeters,
    const LightingView& view) noexcept;

[[nodiscard]] StableLightingCell StableCellForPoint(
    const math::Double3& pointInFrameMeters,
    f64 cellSizeMeters,
    const LightingView& view);

[[nodiscard]] LightingViewChange ClassifyLightingViewChange(
    const LightingView& previous,
    const LightingView& current,
    bool explicitCameraCut = false,
    bool explicitProjectionChange = false) noexcept;
} // namespace orbit::lighting
