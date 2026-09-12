#pragma once

#include <orbit/core/Types.hpp>

namespace orbit::rhi
{
class Fence
{
public:
    virtual ~Fence() = default;

    Fence(const Fence&) = delete;
    Fence& operator=(const Fence&) = delete;

    [[nodiscard]] virtual u64 CompletedValue() const noexcept = 0;
    virtual void Wait(u64 value) = 0;

protected:
    Fence() = default;
};
} // namespace orbit::rhi
