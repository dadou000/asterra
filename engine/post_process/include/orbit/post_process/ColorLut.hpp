#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::post_process
{
enum class ColorLutDomain : u8
{
    DisplayLinear = 0U,
    ShapedSceneLinear = 1U
};

enum class ColorLutShaper : u8
{
    None = 0U,
    Log2 = 1U
};

struct ColorLutMetadata
{
    ColorLutDomain domain{
        ColorLutDomain::DisplayLinear};
    ColorLutShaper shaper{
        ColorLutShaper::None};
    f32 domainMinimum{0.0F};
    f32 domainMaximum{1.0F};
    std::string title;
    bool explicitOrbitMetadata{false};
};

// Orbit stores a 3D LUT as N horizontal N x N slices in one 2D texture.
// This keeps the post-process path backend-agnostic while the RHI currently
// exposes 2D textures only. The shader performs the blue-slice interpolation
// explicitly, so correction is trilinear even with the packed representation.
struct ColorLutData
{
    u32 size{32U};
    std::vector<u8> rgba8;
    ColorLutMetadata metadata{};
};

struct ColorLutImportResult
{
    ColorLutData lut;
    std::string canonicalCube;
};

[[nodiscard]] ColorLutData BuildIdentityColorLut(
    u32 size = 32U);

[[nodiscard]] ColorLutImportResult
ParseCubeColorLut(
    std::string_view text);

[[nodiscard]] std::string
SerializeCubeColorLut(
    const ColorLutData& lut);

[[nodiscard]] bool
IsDisplayLutCompatible(
    const ColorLutData& lut) noexcept;

[[nodiscard]] std::string_view
ColorLutDomainName(
    ColorLutDomain domain) noexcept;

[[nodiscard]] std::string_view
ColorLutShaperName(
    ColorLutShaper shaper) noexcept;

struct ColorLutSettings
{
    bool enabled{true};
    f32 strength{1.0F};
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

// Display-referred 3D LUT correction. Exposure and tone mapping are owned by
// the presentation pipeline. This pass only grades display-linear data and
// therefore rejects scene-referred/shaped LUT assets before GPU upload.
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
