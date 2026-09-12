#pragma once

#include <orbit/rhi/Device.hpp>

#include <memory>

namespace orbit::rhi::d3d12
{
struct DeviceDesc
{
    bool enableValidation{false};
};

[[nodiscard]] std::unique_ptr<Device> CreateDevice(const DeviceDesc& desc = {});
} // namespace orbit::rhi::d3d12
