#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <array>
#include <memory>
#include <string_view>

namespace orbit::post_process
{
enum class AntiAliasingMode : u8
{
    Off,
    // Spatial, single frame: used for the debug-friendly mode and as the
    // fallback whenever TAA has no usable history.
    Fxaa,
    Taa
};

[[nodiscard]] std::string_view AntiAliasingModeName(
    AntiAliasingMode mode) noexcept;

// Parses "off" / "fxaa" / "taa" (case-insensitive); false for anything else.
[[nodiscard]] bool ParseAntiAliasingMode(
    std::string_view text,
    AntiAliasingMode& mode) noexcept;

// Sub-pixel jitter in pixels, each component in (-0.5, 0.5): an 8-sample
// Halton(2, 3) sequence indexed by frame counter.
[[nodiscard]] std::array<f32, 2> TaaJitterPixels(u32 frameCounter) noexcept;

// Applies a jitter of `jitterPixels` to a camera by a tiny rotation, so every
// pass that builds rays or a projection from forward/up/fov renders with the
// shifted sampling pattern without knowing about it. The history resolve
// reprojects with the (jittered) camera pairs, so the shift never shows as
// motion. `up` is re-orthogonalised against the new forward.
void ApplyCameraJitter(
    const math::Float3& forward,
    const math::Float3& up,
    f32 verticalFovRadians,
    u32 height,
    const std::array<f32, 2>& jitterPixels,
    math::Float3& jitteredForward,
    math::Float3& jitteredUp) noexcept;

// What the resolve needs to know about a camera to reproject with it.
struct TaaCamera
{
    math::Double3 positionMeters{};
    math::Float3 forward{0.0F, 0.0F, 1.0F};
    math::Float3 up{0.0F, 1.0F, 0.0F};
    f32 verticalFovRadians{1.0F};
    f32 nearPlaneMeters{0.05F};
    f32 farPlaneMeters{1.0e7F};
};

struct TaaSettings
{
    // Weight of the (clipped) history at rest; lowered with screen motion.
    f32 feedback{0.975F};
    // Strength of the post-resolve sharpen that counters temporal blur. It adds
    // back part of the raw (jittered) frame's local contrast, so large values
    // make the jitter itself visible as shimmer.
    f32 sharpen{0.08F};
};

// True when `previous` can be reprojected into `current`: no teleport, no
// field-of-view jump, no near/far change.
[[nodiscard]] bool TaaHistoryUsable(
    const TaaCamera& previous,
    const TaaCamera& current) noexcept;

// Post-lighting, pre-tone-mapping anti-aliasing on the HDR scene colour.
// Both modes work on a Karis-compressed copy of the colour so a single bright
// pixel cannot dominate the filter.
class AntiAliasingRenderer
{
public:
    AntiAliasingRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    // `destination` is an RGBA16F storage texture in UnorderedAccess; `source`
    // is in ShaderResource. Source and destination must differ.
    void Fxaa(
        rhi::CommandList& commands,
        rhi::Texture& source,
        rhi::Texture& destination,
        u32 width,
        u32 height);

    // Temporal resolve. `history` is the previous resolved frame; `depth` is
    // the reverse-Z scene depth (DepthRead). When `historyValid` is false the
    // history is ignored and the current colour passes through unchanged (the
    // caller runs Fxaa instead in that case).
    void Taa(
        rhi::CommandList& commands,
        rhi::Texture& color,
        rhi::Texture& depth,
        rhi::Texture& history,
        rhi::Texture& destination,
        u32 width,
        u32 height,
        const TaaCamera& current,
        const TaaCamera& previous,
        bool historyValid,
        const TaaSettings& settings = {});

private:
    std::unique_ptr<rhi::ComputePipeline> fxaaPipeline_;
    std::unique_ptr<rhi::ComputePipeline> taaPipeline_;
};
} // namespace orbit::post_process
