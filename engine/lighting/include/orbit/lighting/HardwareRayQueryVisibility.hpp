#pragma once

#include <orbit/lighting/SoftwareProxyVisibility.hpp>
#include <orbit/lighting/Visibility.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::lighting
{
class HardwareRayQueryVisibilityBatch
{
public:
    HardwareRayQueryVisibilityBatch(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void RebuildScene(
        const SoftwareProxyScene& scene);

    [[nodiscard]] bool Supported() const noexcept;
    [[nodiscard]] bool Ready() const noexcept;
    [[nodiscard]] u32 PrimitiveCount() const noexcept;

    void Dispatch(
        rhi::CommandList& commands,
        rhi::Buffer& queries,
        rhi::Buffer& results,
        u32 queryCount);

private:
    rhi::Device* device_{nullptr};
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
    std::unique_ptr<rhi::AccelerationStructure> accelerationStructure_;
    std::unique_ptr<rhi::Buffer> primitiveBuffer_;
    u32 primitiveCount_{0U};
    bool supported_{false};
};
} // namespace orbit::lighting
