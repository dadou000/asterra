#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/shading/ShadingContract.hpp>

#include <string>
#include <string_view>

// CPU-only assembly and compilation of a Shading-tab shader. The GPU pipeline
// that consumes the result lives in ShaderPreviewRenderer, so this can be unit
// tested and driven from RPC without a device.
namespace orbit::shading
{
// The number of dwords of push constants the preview pipelines use. Kept equal
// to the largest existing pipeline in the engine (160 bytes).
inline constexpr u32 kPreviewPushDwords = 40U;

struct ShadingProgram
{
    bool ok{false};
    // Compiler output (DXC's text, with `#line` mapping locations onto the
    // user's own file) and parameter-annotation problems. Empty when ok.
    std::string diagnostics;
    shader::Binary pixel;
    // Non-empty only when layout.HasDisplacement(): a vertex stage that
    // samples the `height` texture and displaces along the normal, compiled
    // from the same layout alongside the pixel stage. Empty otherwise -- the
    // renderer then uses its plain fixed vertex shader instead.
    shader::Binary vertex;
    ShaderParameterLayout layout;
    u64 sourceHash{0};
    f64 compileMilliseconds{0.0};
};

// Assembles prelude + parameter accessors + user source + wrapper and compiles
// the pixel stage. Never throws for a shader error: the failure is reported in
// `diagnostics` so the caller can keep the last good program alive.
[[nodiscard]] ShadingProgram CompileShadingProgram(
    const shader::Compiler& compiler,
    std::string_view assetName,
    std::string_view userSource);

// Exposed for tests and for the docs' worked example.
[[nodiscard]] std::string BuildObjectPixelSource(
    std::string_view assetName,
    std::string_view userSource,
    const ShaderParameterLayout& layout);

// The displacement vertex stage compiled alongside the pixel stage when
// layout.HasDisplacement(). Throws if it doesn't (no `height` texture2d
// parameter to sample).
[[nodiscard]] std::string BuildObjectVertexSource(
    const ShaderParameterLayout& layout);

// Fixed engine stages, compiled once by the renderer.
[[nodiscard]] std::string_view PreviewVertexSource() noexcept;
[[nodiscard]] std::string BackgroundPixelSource();
[[nodiscard]] std::string_view BackgroundVertexSource() noexcept;
// Shown when no shader is selected or none has ever compiled successfully.
[[nodiscard]] std::string_view ErrorShaderSource() noexcept;

[[nodiscard]] u64 HashSource(std::string_view text) noexcept;
} // namespace orbit::shading
