#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/rhi/Resource.hpp>

namespace orbit::rhi
{
struct ClearColor
{
    f32 red{0.0F};
    f32 green{0.0F};
    f32 blue{0.0F};
    f32 alpha{1.0F};
};

class CommandAllocator
{
public:
    virtual ~CommandAllocator() = default;

    CommandAllocator(const CommandAllocator&) = delete;
    CommandAllocator& operator=(const CommandAllocator&) = delete;

    [[nodiscard]] virtual QueueType Type() const noexcept = 0;
    virtual void Reset() = 0;

protected:
    CommandAllocator() = default;
};

class CommandList
{
public:
    virtual ~CommandList() = default;

    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;

    [[nodiscard]] virtual QueueType Type() const noexcept = 0;

    virtual void Reset(CommandAllocator& allocator) = 0;
    virtual void Transition(
        Texture& texture,
        ResourceState before,
        ResourceState after) = 0;
    virtual void ClearColorTarget(Texture& texture, const ClearColor& color) = 0;
    virtual void Close() = 0;

protected:
    CommandList() = default;
};
} // namespace orbit::rhi
