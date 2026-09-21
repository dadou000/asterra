#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>
#include <string_view>

namespace orbit::post_process
{
enum class OutputMode : u8
{
    Auto = 0U,
    Sdr = 1U,
    Hdr10 = 2U
};

enum class OutputTestPattern : u8
{
    None = 0U,
    LinearRamp = 1U,
    ReferenceWhite = 2U,
    PeakWhite = 3U
};

struct OutputDisplayCapabilities
{
    bool hdr10Supported{false};
    f32 reportedPeakNits{0.0F};
};

struct OutputTransformSettings
{
    OutputMode mode{OutputMode::Auto};
    f32 referenceWhiteNits{203.0F};
    f32 requestedPeakNits{1000.0F};
    OutputTestPattern testPattern{
        OutputTestPattern::None};
};

struct OutputTransformDiagnostics
{
    OutputMode requestedMode{OutputMode::Auto};
    OutputMode resolvedMode{OutputMode::Sdr};
    bool hdrCapabilityAvailable{false};
    bool fellBackToSdr{false};
    f32 referenceWhiteNits{203.0F};
    f32 requestedPeakNits{1000.0F};
    f32 resolvedPeakNits{203.0F};
};

[[nodiscard]] OutputTransformDiagnostics
ResolveOutputTransform(
    const OutputTransformSettings& settings,
    const OutputDisplayCapabilities& capabilities) noexcept;

[[nodiscard]] f32
EncodeSrgbChannel(
    f32 linear) noexcept;

[[nodiscard]] f32
EncodeSt2084FromNits(
    f32 nits) noexcept;

[[nodiscard]] f32
DecodeSt2084ToNits(
    f32 encoded) noexcept;

[[nodiscard]] std::string_view
OutputModeName(
    OutputMode mode) noexcept;

[[nodiscard]] std::string_view
OutputTestPatternName(
    OutputTestPattern pattern) noexcept;

class OutputTransformRenderer
{
public:
    OutputTransformRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& sourceDisplayLinear,
        rhi::Texture& targetEncoded,
        u32 width,
        u32 height,
        const OutputTransformDiagnostics& diagnostics,
        OutputTestPattern pattern =
            OutputTestPattern::None);

private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
};
} // namespace orbit::post_process
