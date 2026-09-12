#pragma once

#include <orbit/core/Types.hpp>

#include <memory>
#include <string_view>

namespace orbit::rhi
{
class CommandList;
class Device;
class Texture;
}

namespace orbit::shader
{
class Compiler;
}

namespace orbit::debug_render
{
struct VersionOverlayConfig
{
    u32 pixelScale{2};
    u32 paddingPixels{6};
    u32 marginPixels{10};
};

class VersionOverlayRenderer
{
public:
    VersionOverlayRenderer(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        std::string_view text,
        VersionOverlayConfig config = {});

    ~VersionOverlayRenderer();

    VersionOverlayRenderer(
        const VersionOverlayRenderer&) = delete;

    VersionOverlayRenderer& operator=(
        const VersionOverlayRenderer&) = delete;

    void Draw(
        rhi::CommandList& commandList,
        rhi::Texture& target,
        u32 targetWidth,
        u32 targetHeight);

    [[nodiscard]] std::string_view Text() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::debug_render
