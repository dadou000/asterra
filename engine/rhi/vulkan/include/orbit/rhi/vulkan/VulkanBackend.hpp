#pragma once

#include <orbit/rhi/Device.hpp>

#include <memory>

namespace orbit::rhi::vulkan
{
struct DeviceDesc
{
    bool enableValidation{false};
};

[[nodiscard]] std::unique_ptr<Device> CreateDevice(const DeviceDesc& desc = {});
} // namespace orbit::rhi::vulkan
