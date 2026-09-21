#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>
#include <vector>

namespace orbit::post_process
{
// Orbit stores a 3D LUT as N horizontal N x N slices in one 2D texture.
// This keeps the post-process path backend-agnostic while the RHI currently
// exposes 2D textures only. The shader performs the blue-slice interpolation
// explicitly, so correction is trilinear even with the packed representation.
struct ColorLutData
{
    u32 size{32U};
    std::vector<u8> rgba8;
};

[[nodiscard]] ColorLutData BuildIdentityColorLut(
    u32 size = 32U);

struct ColorLutSettings
{
    bool enabled{true};
    f32 strength{1.0F};
    bool toneMapEnabled{true};
    f32 exposureScale{1.0F};
};

class GpuColorLut
{
public:
    GpuColorLut(
        rhi::Device& device,
        const ColorLutData& data);

    void EnsureUploaded(
        rhi::CommandList& commands);

    [[nodiscard]] rhi::Texture& Texture() noexcept;
    [[nodiscard]] u32 Size() const noexcept;

private:
    std::unique_ptr<rhi::Buffer> staging_;
    std::unique_ptr<rhi::Texture> texture_;
    u32 size_{0U};
    bool uploaded_{false};
};

// Display-referred 3D LUT correction. It intentionally does not own exposure,
// tone mapping, bloom or output transfer. Those stages decide where this pass
// sits in the final presentation chain. In the current 0.0.6 viewport it runs
// after the existing renderer output; the HDR pipeline can later keep the same
// renderer and place a scene-referred LUT behind an explicit HDR shaper.
class ColorLutRenderer
{
public:
    ColorLutRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& source,
        rhi::Texture& target,
        u32 width,
        u32 height,
        GpuColorLut& lut,
        const ColorLutSettings& settings = {});

private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
};
} // namespace orbit::post_process
