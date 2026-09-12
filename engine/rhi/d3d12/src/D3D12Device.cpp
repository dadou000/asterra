#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <orbit/core/Log.hpp>
#include <orbit/rhi/d3d12/D3D12Backend.hpp>

#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::rhi::d3d12
{
namespace
{
using Microsoft::WRL::ComPtr;

std::string WideToUtf8(const wchar_t* text)
{
    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        text,
        -1,
        nullptr,
        0,
        nullptr,
        nullptr
    );

    if (required <= 1)
    {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');

    const int written = WideCharToMultiByte(
        CP_UTF8,
        0,
        text,
        -1,
        result.data(),
        required,
        nullptr,
        nullptr
    );

    if (written <= 1)
    {
        return {};
    }

    result.resize(static_cast<std::size_t>(written - 1));
    return result;
}

ComPtr<IDXGIAdapter1> SelectHardwareAdapter(IDXGIFactory6& factory)
{
    for (u32 index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory.EnumAdapterByGpuPreference(
            index,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&adapter)
        );

        if (result == DXGI_ERROR_NOT_FOUND)
        {
            break;
        }

        if (FAILED(result))
        {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)))
        {
            continue;
        }

        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(
                adapter.Get(),
                D3D_FEATURE_LEVEL_12_0,
                __uuidof(ID3D12Device),
                nullptr)))
        {
            return adapter;
        }
    }

    throw std::runtime_error("Orbit could not find a hardware D3D12 adapter.");
}

DeviceCapabilities QueryCapabilities(ID3D12Device& device)
{
    DeviceCapabilities capabilities{};

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    if (SUCCEEDED(device.CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS5,
            &options5,
            sizeof(options5))))
    {
        capabilities.rayTracing =
            options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
    if (SUCCEEDED(device.CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS6,
            &options6,
            sizeof(options6))))
    {
        capabilities.variableRateShading =
            options6.VariableShadingRateTier != D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(device.CheckFeatureSupport(
            D3D12_FEATURE_D3D12_OPTIONS7,
            &options7,
            sizeof(options7))))
    {
        capabilities.meshShaders =
            options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    }

    constexpr std::array shaderModels{
        D3D_SHADER_MODEL_6_7,
        D3D_SHADER_MODEL_6_6,
        D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4,
        D3D_SHADER_MODEL_6_3,
        D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1,
        D3D_SHADER_MODEL_6_0,
        D3D_SHADER_MODEL_5_1
    };

    for (const D3D_SHADER_MODEL model : shaderModels)
    {
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{model};
        if (SUCCEEDED(device.CheckFeatureSupport(
                D3D12_FEATURE_SHADER_MODEL,
                &shaderModel,
                sizeof(shaderModel))))
        {
            const u32 encoded = static_cast<u32>(shaderModel.HighestShaderModel);
            capabilities.shaderModelMajor = (encoded >> 4U) & 0xFU;
            capabilities.shaderModelMinor = encoded & 0xFU;
            break;
        }
    }

    return capabilities;
}

class D3D12Device final : public Device
{
public:
    D3D12Device(
        ComPtr<ID3D12Device> nativeDevice,
        std::string adapterName,
        DeviceCapabilities capabilities)
        : nativeDevice_(std::move(nativeDevice)),
          adapterName_(std::move(adapterName)),
          capabilities_(capabilities)
    {
    }

    [[nodiscard]] Backend GetBackend() const noexcept override
    {
        return Backend::D3D12;
    }

    [[nodiscard]] std::string_view AdapterName() const noexcept override
    {
        return adapterName_;
    }

    [[nodiscard]] const DeviceCapabilities& Capabilities() const noexcept override
    {
        return capabilities_;
    }

private:
    ComPtr<ID3D12Device> nativeDevice_;
    std::string adapterName_;
    DeviceCapabilities capabilities_{};
};
} // namespace

std::unique_ptr<Device> CreateDevice(const DeviceDesc& desc)
{
    u32 factoryFlags = 0;

    if (desc.enableValidation)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            log::Info("D3D12 validation layer enabled.");
        }
        else
        {
            log::Warning("D3D12 validation requested but unavailable.");
        }
    }

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory))))
    {
        throw std::runtime_error("Orbit failed to create the DXGI factory.");
    }

    ComPtr<IDXGIAdapter1> adapter = SelectHardwareAdapter(*factory.Get());

    DXGI_ADAPTER_DESC1 adapterDesc{};
    if (FAILED(adapter->GetDesc1(&adapterDesc)))
    {
        throw std::runtime_error("Orbit failed to query the selected DXGI adapter.");
    }

    ComPtr<ID3D12Device> nativeDevice;
    if (FAILED(D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&nativeDevice))))
    {
        throw std::runtime_error("Orbit failed to create the D3D12 device.");
    }

    const auto capabilities = QueryCapabilities(*nativeDevice.Get());
    auto adapterName = WideToUtf8(adapterDesc.Description);

    log::Info("D3D12 device created.");

    return std::make_unique<D3D12Device>(
        std::move(nativeDevice),
        std::move(adapterName),
        capabilities
    );
}
} // namespace orbit::rhi::d3d12
