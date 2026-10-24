#pragma once

#include <orbit/rhi/Device.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::rhi
{
// A frame-owned handle for driver pipelines. HLSL compilation happens before
// construction; the Vulkan driver compile happens on its dedicated worker.
// Every input span is copied so a delayed/rejected job can safely retry after
// the caller's shader binaries and vertex arrays have gone out of scope.
//
// Poll() MUST be called from the owning rendering thread; it never waits.
// Destroying an unresolved packaged-task future does not join the worker.
// As with other GPU objects, the Device must outlive the renderer.
class DeferredGraphicsPipeline final
{
public:
    DeferredGraphicsPipeline(Device& device, const GraphicsPipelineDesc& desc)
        : device_(device), desc_(desc)
    {
        if (!desc.vertexShader.data || !desc.pixelShader.data ||
            desc.vertexShader.size == 0U || desc.pixelShader.size == 0U)
            throw std::invalid_argument("Deferred graphics pipeline requires shader bytes.");
        vertex_.assign(desc.vertexShader.data,
                       desc.vertexShader.data + desc.vertexShader.size);
        pixel_.assign(desc.pixelShader.data,
                      desc.pixelShader.data + desc.pixelShader.size);
        attributes_.assign(desc.vertexAttributes.begin(), desc.vertexAttributes.end());
        desc_.vertexShader = {vertex_.data(), vertex_.size()};
        desc_.pixelShader = {pixel_.data(), pixel_.size()};
        desc_.vertexAttributes = attributes_;
        Queue();
    }

    DeferredGraphicsPipeline(const DeferredGraphicsPipeline&) = delete;
    DeferredGraphicsPipeline& operator=(const DeferredGraphicsPipeline&) = delete;
    DeferredGraphicsPipeline(DeferredGraphicsPipeline&&) = delete;
    DeferredGraphicsPipeline& operator=(DeferredGraphicsPipeline&&) = delete;

    [[nodiscard]] GraphicsPipeline* Poll()
    {
        if (ready_) return ready_.get();
        if (!pending_.valid()) Queue();
        if (pending_.valid() &&
            pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            ready_ = pending_.get();
        return ready_.get();
    }

private:
    void Queue()
    {
        // Saturated driver-compile queues are retried on subsequent frames.
        // Never fall back to a synchronous cold Vulkan pipeline compilation.
        auto accepted = device_.TryCreateGraphicsPipelineAsync(desc_);
        if (accepted) pending_ = std::move(*accepted);
    }

    Device& device_;
    GraphicsPipelineDesc desc_;
    std::vector<u8> vertex_;
    std::vector<u8> pixel_;
    std::vector<VertexAttribute> attributes_;
    std::future<std::unique_ptr<GraphicsPipeline>> pending_;
    std::unique_ptr<GraphicsPipeline> ready_;
};

class DeferredComputePipeline final
{
public:
    DeferredComputePipeline(Device& device, const ComputePipelineDesc& desc)
        : device_(device), desc_(desc)
    {
        if (!desc.computeShader.data || desc.computeShader.size == 0U)
            throw std::invalid_argument("Deferred compute pipeline requires shader bytes.");
        bytecode_.assign(desc.computeShader.data,
                         desc.computeShader.data + desc.computeShader.size);
        desc_.computeShader = {bytecode_.data(), bytecode_.size()};
        Queue();
    }

    DeferredComputePipeline(const DeferredComputePipeline&) = delete;
    DeferredComputePipeline& operator=(const DeferredComputePipeline&) = delete;
    DeferredComputePipeline(DeferredComputePipeline&&) = delete;
    DeferredComputePipeline& operator=(DeferredComputePipeline&&) = delete;

    [[nodiscard]] ComputePipeline* Poll()
    {
        if (ready_) return ready_.get();
        if (!pending_.valid()) Queue();
        if (pending_.valid() &&
            pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            ready_ = pending_.get();
        return ready_.get();
    }

private:
    void Queue()
    {
        auto accepted = device_.TryCreateComputePipelineAsync(desc_);
        if (accepted) pending_ = std::move(*accepted);
    }

    Device& device_;
    ComputePipelineDesc desc_;
    std::vector<u8> bytecode_;
    std::future<std::unique_ptr<ComputePipeline>> pending_;
    std::unique_ptr<ComputePipeline> ready_;
};
} // namespace orbit::rhi
