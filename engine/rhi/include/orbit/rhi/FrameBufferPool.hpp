#pragma once

#include <orbit/rhi/Device.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::rhi
{
// Reusable host-upload buffers keyed to the host application's completed GPU
// frame slot. BeginFrame(slot) MUST only be called after the timeline/fence for
// that slot has finished. An active buffer never leaves its slot's ownership
// while command lists submitted for that slot may still refer to it.
//
// Unlike round-robin allocation or an arbitrary N-frame delay, this avoids
// calling Buffer::Map() on an in-use allocation and therefore avoids the
// blocking Vulkan host-hazard wait in the normal resource reuse path.
class FrameBufferPool final
{
public:
    FrameBufferPool(Device& device, BufferUsage usage = BufferUsage::Structured,
                    ResourceState state = ResourceState::ShaderResource)
        : device_(device), usage_(usage), initialState_(state)
    {
    }

    FrameBufferPool(const FrameBufferPool&) = delete;
    FrameBufferPool& operator=(const FrameBufferPool&) = delete;

    void BeginFrame(const u32 completedSlot, const u32 framesInFlight)
    {
        if (framesInFlight == 0U || framesInFlight > 64U ||
            completedSlot >= framesInFlight)
            throw std::invalid_argument("Invalid completed upload buffer slot.");
        if (slots_.size() != framesInFlight)
        {
            if (!slots_.empty())
                throw std::logic_error(
                    "Cannot resize active upload buffer slots without GPU drain.");
            slots_.resize(framesInFlight);
        }
        activeSlot_ = completedSlot;
        for (auto& buffer : slots_[completedSlot])
        {
            // Recycle only completed-slot buffers. Bound the pool's long-term
            // allocation footprint so an unusually large upload is retired.
            constexpr u64 kMaximumFreeBytes = 32U * 1024U * 1024U;
            if (buffer && available_.size() < 32U &&
                buffer->SizeBytes() <= kMaximumFreeBytes - cachedBytes_)
            {
                cachedBytes_ += buffer->SizeBytes();
                available_.push_back(std::move(buffer));
            }
        }
        slots_[completedSlot].clear();
    }

    [[nodiscard]] Buffer& Acquire(const u64 bytes)
    {
        if (!activeSlot_)
            throw std::logic_error(
                "Upload pool requires BeginFrame after completed GPU fence.");
        if (bytes == 0U)
            throw std::invalid_argument("Cannot allocate an empty upload buffer.");

        auto choice = available_.end();
        for (auto it = available_.begin(); it != available_.end(); ++it)
        {
            if ((*it)->SizeBytes() >= bytes &&
                (choice == available_.end() ||
                 (*it)->SizeBytes() < (*choice)->SizeBytes()))
                choice = it;
        }
        std::unique_ptr<Buffer> buffer;
        if (choice != available_.end())
        {
            cachedBytes_ -= (*choice)->SizeBytes();
            buffer = std::move(*choice);
            available_.erase(choice);
        }
        else
        {
            constexpr u64 kMaximumRoundedSize = u64{1} << 62U;
            const u64 capacity = bytes <= kMaximumRoundedSize
                ? std::bit_ceil(std::max<u64>(bytes, 256U))
                : bytes;
            buffer = device_.CreateBuffer({
                .sizeBytes = capacity,
                .usage = usage_,
                .memory = MemoryUsage::HostVisible,
                .initialState = initialState_
            });
        }
        if (!buffer)
            throw std::runtime_error("Failed to acquire host upload buffer.");
        auto& inFlight = slots_[*activeSlot_];
        inFlight.push_back(std::move(buffer));
        return *inFlight.back();
    }

    [[nodiscard]] std::size_t CachedCount() const noexcept
    {
        return available_.size();
    }

private:
    Device& device_;
    BufferUsage usage_;
    ResourceState initialState_;
    std::vector<std::vector<std::unique_ptr<Buffer>>> slots_;
    std::vector<std::unique_ptr<Buffer>> available_;
    u64 cachedBytes_{0U};
    std::optional<u32> activeSlot_;
};
} // namespace orbit::rhi
