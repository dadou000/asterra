#pragma once

#include <orbit/core/Types.hpp>

namespace orbit::rhi
{
enum class ResourceState : u8
{
    Present,
    RenderTarget,
    DepthWrite,
    DepthRead,
    ShaderResource,
    UnorderedAccess,
    CopySource,
    CopyDestination
};

class Texture
{
public:
    virtual ~Texture() = default;

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    [[nodiscard]] virtual u32 Width() const noexcept = 0;
    [[nodiscard]] virtual u32 Height() const noexcept = 0;

protected:
    Texture() = default;
};
} // namespace orbit::rhi
