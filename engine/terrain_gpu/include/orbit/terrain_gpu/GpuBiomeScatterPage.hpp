#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain_scatter/DeterministicScatter.hpp>

#include <memory>
#include <span>
#include <vector>

namespace orbit::terrain_gpu
{
// Persistent fixed-capacity M22 GPU scatter page.
//
// Each dispatch writes one 48-byte slot per planting-grid cell. Consumers may
// compact active slots later for indirect rendering, but the generation pass
// itself has no atomics/append order and is therefore deterministic.
class GpuBiomeScatterPage
{
public:
    GpuBiomeScatterPage(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        u32 maxResolution);

    ~GpuBiomeScatterPage();

    GpuBiomeScatterPage(
        const GpuBiomeScatterPage&) = delete;
    GpuBiomeScatterPage& operator=(
        const GpuBiomeScatterPage&) = delete;

    void Dispatch(
        rhi::CommandList& commandList,
        const terrain_scatter::ScatterPageRequest& request,
        std::span<const terrain_scatter::ScatterCellInput> cells);

    // Record after Dispatch. The caller must wait for the submission fence
    // before ApplyReadback().
    void RecordReadback(
        rhi::CommandList& commandList);

    [[nodiscard]] std::vector<
        terrain_scatter::DerivedScatterInstance>
    ApplyReadback();

    [[nodiscard]] rhi::Buffer&
    InstanceSlots() noexcept;

    [[nodiscard]] u32 ActiveResolution() const noexcept;

private:
    u32 maxResolution_{0};
    u32 activeResolution_{0};

    std::unique_ptr<rhi::ComputePipeline>
        scatterPipeline_;

    std::unique_ptr<rhi::Buffer>
        fieldInput_;

    std::unique_ptr<rhi::Buffer>
        instanceSlots_;

    std::unique_ptr<rhi::Buffer>
        instanceReadback_;

    bool dispatched_{false};
    bool readbackRecorded_{false};
};
} // namespace orbit::terrain_gpu
