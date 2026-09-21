#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <span>

namespace orbit::rhi
{
// Narrow V0.0.7 M09 contract for proxy visibility acceleration. The public RHI
// deliberately exposes AABBs rather than Vulkan-specific BLAS/TLAS handles;
// triangle/mesh AS creation can be added as a sibling path later without
// changing lighting's visibility-query contract.
struct AccelerationAabb
{
    math::Float3 minimum{};
    math::Float3 maximum{};
    u32 primitiveId{0U};
};

class AccelerationStructure
{
public:
    virtual ~AccelerationStructure() = default;

    AccelerationStructure(const AccelerationStructure&) = delete;
    AccelerationStructure& operator=(const AccelerationStructure&) = delete;

    [[nodiscard]] virtual u32 PrimitiveCount() const noexcept = 0;

protected:
    AccelerationStructure() = default;
};
} // namespace orbit::rhi
