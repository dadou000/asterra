#pragma once

#include <orbit/core/Types.hpp>

namespace orbit::rhi
{
class CommandList;
class Fence;

enum class QueueType : u8
{
    Graphics,
    Compute,
    Copy
};

class Queue
{
public:
    virtual ~Queue() = default;

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    [[nodiscard]] virtual QueueType Type() const noexcept = 0;

    virtual void Submit(CommandList& commandList) = 0;
    virtual void Signal(Fence& fence, u64 value) = 0;

protected:
    Queue() = default;
};
} // namespace orbit::rhi
