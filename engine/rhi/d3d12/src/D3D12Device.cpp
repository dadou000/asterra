#include "D3D12Objects.hpp"

#include <orbit/core/Log.hpp>
#include <orbit/rhi/d3d12/D3D12Backend.hpp>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::rhi::d3d12
{
namespace detail
{
std::string WideToUtf8(const wchar_t* text)
{
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);

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
        nullptr);

    if (written <= 1)
    {
        return {};
    }

    result.resize(static_cast<std::size_t>(written - 1));
    return result;
}

D3D12_COMMAND_LIST_TYPE ToNativeQueueType(const QueueType type)
{
    switch (type)
    {
    case QueueType::Graphics:
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    case QueueType::Compute:
        return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    case QueueType::Copy:
        return D3D12_COMMAND_LIST_TYPE_COPY;
    }

    throw std::runtime_error("Orbit received an invalid RHI queue type.");
}

ComPtr<IDXGIAdapter1> SelectHardwareAdapter(IDXGIFactory6& factory)
{
    for (u32 index = 0;; ++index)
    {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory.EnumAdapterByGpuPreference(
            index,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&adapter));

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

bool QueryPresentTearingSupport(IDXGIFactory6& factory)
{
    BOOL allowTearing = FALSE;

    if (FAILED(factory.CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allowTearing,
            sizeof(allowTearing))))
    {
        return false;
    }

    return allowTearing == TRUE;
}

DeviceCapabilities QueryCapabilities(
    ID3D12Device& device,
    IDXGIFactory6& factory)
{
    DeviceCapabilities capabilities{};
    capabilities.presentTearing = QueryPresentTearingSupport(factory);

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
            options6.VariableShadingRateTier !=
            D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
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
            const u32 encoded =
                static_cast<u32>(shaderModel.HighestShaderModel);
            capabilities.shaderModelMajor = (encoded >> 4U) & 0xFU;
            capabilities.shaderModelMinor = encoded & 0xFU;
            break;
        }
    }

    return capabilities;
}

D3D12Device::D3D12Device(
    ComPtr<ID3D12Device> nativeDevice,
    ComPtr<IDXGIFactory6> factory,
    std::string adapterName,
    const DeviceCapabilities capabilities)
    : nativeDevice_(std::move(nativeDevice)),
      factory_(std::move(factory)),
      adapterName_(std::move(adapterName)),
      capabilities_(capabilities)
{
}

Backend D3D12Device::GetBackend() const noexcept
{
    return Backend::D3D12;
}

std::string_view D3D12Device::AdapterName() const noexcept
{
    return adapterName_;
}

const DeviceCapabilities& D3D12Device::Capabilities() const noexcept
{
    return capabilities_;
}

std::unique_ptr<Queue> D3D12Device::CreateQueue(const QueueType type)
{
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = ToNativeQueueType(type);
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(nativeDevice_->CreateCommandQueue(
            &desc,
            IID_PPV_ARGS(&queue))))
    {
        throw std::runtime_error("Orbit failed to create a D3D12 command queue.");
    }

    return std::make_unique<D3D12Queue>(type, std::move(queue));
}

std::unique_ptr<Fence> D3D12Device::CreateFence(const u64 initialValue)
{
    ComPtr<ID3D12Fence> fence;
    if (FAILED(nativeDevice_->CreateFence(
            initialValue,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&fence))))
    {
        throw std::runtime_error("Orbit failed to create a D3D12 fence.");
    }

    const HANDLE eventHandle = CreateEventExA(
        nullptr,
        nullptr,
        0,
        EVENT_ALL_ACCESS);

    if (eventHandle == nullptr)
    {
        throw std::runtime_error("Orbit failed to create a D3D12 fence event.");
    }

    return std::make_unique<D3D12Fence>(
        std::move(fence),
        eventHandle);
}

std::unique_ptr<CommandAllocator> D3D12Device::CreateCommandAllocator(
    const QueueType type)
{
    ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(nativeDevice_->CreateCommandAllocator(
            ToNativeQueueType(type),
            IID_PPV_ARGS(&allocator))))
    {
        throw std::runtime_error(
            "Orbit failed to create a D3D12 command allocator.");
    }

    return std::make_unique<D3D12CommandAllocator>(
        type,
        std::move(allocator));
}

std::unique_ptr<CommandList> D3D12Device::CreateCommandList(
    CommandAllocator& allocator)
{
    auto* d3dAllocator = dynamic_cast<D3D12CommandAllocator*>(&allocator);
    if (d3dAllocator == nullptr)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a command allocator from another backend.");
    }

    ComPtr<ID3D12GraphicsCommandList> commandList;
    if (FAILED(nativeDevice_->CreateCommandList(
            0,
            ToNativeQueueType(allocator.Type()),
            d3dAllocator->Native(),
            nullptr,
            IID_PPV_ARGS(&commandList))))
    {
        throw std::runtime_error(
            "Orbit failed to create a D3D12 command list.");
    }

    if (FAILED(commandList->Close()))
    {
        throw std::runtime_error(
            "Orbit failed to close a new D3D12 command list.");
    }

    return std::make_unique<D3D12CommandList>(
        allocator.Type(),
        std::move(commandList));
}

std::unique_ptr<Swapchain> D3D12Device::CreateSwapchain(
    Queue& queue,
    const SwapchainDesc& desc)
{
    if (desc.nativeWindow == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot create a swapchain without a native window.");
    }

    if (desc.width == 0 || desc.height == 0)
    {
        throw std::runtime_error(
            "Orbit cannot create a zero-sized swapchain.");
    }

    if (desc.bufferCount < 2)
    {
        throw std::runtime_error(
            "Orbit requires at least two swapchain buffers.");
    }

    auto* d3dQueue = dynamic_cast<D3D12Queue*>(&queue);
    if (d3dQueue == nullptr || d3dQueue->Type() != QueueType::Graphics)
    {
        throw std::runtime_error(
            "A D3D12 swapchain requires an Orbit D3D12 graphics queue.");
    }

    const bool tearingEnabled =
        desc.allowTearing && capabilities_.presentTearing;

    DXGI_SWAP_CHAIN_DESC1 nativeDesc{};
    nativeDesc.Width = desc.width;
    nativeDesc.Height = desc.height;
    nativeDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nativeDesc.Stereo = FALSE;
    nativeDesc.SampleDesc.Count = 1;
    nativeDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    nativeDesc.BufferCount = desc.bufferCount;
    nativeDesc.Scaling = DXGI_SCALING_STRETCH;
    nativeDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    nativeDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    nativeDesc.Flags =
        tearingEnabled ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0U;

    const HWND hwnd = static_cast<HWND>(desc.nativeWindow);

    ComPtr<IDXGISwapChain1> swapchain1;
    if (FAILED(factory_->CreateSwapChainForHwnd(
            d3dQueue->Native(),
            hwnd,
            &nativeDesc,
            nullptr,
            nullptr,
            &swapchain1)))
    {
        throw std::runtime_error(
            "Orbit failed to create the D3D12 swapchain.");
    }

    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<IDXGISwapChain4> swapchain4;
    if (FAILED(swapchain1.As(&swapchain4)))
    {
        throw std::runtime_error(
            "Orbit failed to acquire IDXGISwapChain4.");
    }

    return std::make_unique<D3D12Swapchain>(
        std::move(swapchain4),
        *nativeDevice_.Get(),
        desc,
        tearingEnabled);
}
} // namespace detail

std::unique_ptr<Device> CreateDevice(const DeviceDesc& desc)
{
    u32 factoryFlags = 0;

    if (desc.enableValidation)
    {
        detail::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(
                IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            log::Info("D3D12 validation layer enabled.");
        }
        else
        {
            log::Warning(
                "D3D12 validation requested but unavailable.");
        }
    }

    detail::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(
            factoryFlags,
            IID_PPV_ARGS(&factory))))
    {
        throw std::runtime_error(
            "Orbit failed to create the DXGI factory.");
    }

    auto adapter = detail::SelectHardwareAdapter(*factory.Get());

    DXGI_ADAPTER_DESC1 adapterDesc{};
    if (FAILED(adapter->GetDesc1(&adapterDesc)))
    {
        throw std::runtime_error(
            "Orbit failed to query the selected DXGI adapter.");
    }

    detail::ComPtr<ID3D12Device> nativeDevice;
    if (FAILED(D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&nativeDevice))))
    {
        throw std::runtime_error(
            "Orbit failed to create the D3D12 device.");
    }

    const auto capabilities =
        detail::QueryCapabilities(*nativeDevice.Get(), *factory.Get());

    auto adapterName = detail::WideToUtf8(adapterDesc.Description);

    log::Info("D3D12 device created.");

    return std::make_unique<detail::D3D12Device>(
        std::move(nativeDevice),
        std::move(factory),
        std::move(adapterName),
        capabilities);
}
} // namespace orbit::rhi::d3d12
