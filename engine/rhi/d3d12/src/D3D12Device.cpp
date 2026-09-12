#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <orbit/core/Log.hpp>
#include <orbit/rhi/d3d12/D3D12Backend.hpp>

#include <array>
#include <cstddef>
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

D3D12_COMMAND_LIST_TYPE ToD3D12QueueType(const QueueType type)
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

class D3D12Queue final : public Queue
{
public:
    D3D12Queue(
        const QueueType type,
        ComPtr<ID3D12CommandQueue> nativeQueue)
        : type_(type),
          nativeQueue_(std::move(nativeQueue))
    {
    }

    [[nodiscard]] QueueType Type() const noexcept override
    {
        return type_;
    }

    [[nodiscard]] ID3D12CommandQueue* Native() const noexcept
    {
        return nativeQueue_.Get();
    }

private:
    QueueType type_;
    ComPtr<ID3D12CommandQueue> nativeQueue_;
};

class D3D12Swapchain final : public Swapchain
{
public:
    D3D12Swapchain(
        ComPtr<IDXGISwapChain4> nativeSwapchain,
        const SwapchainDesc& desc,
        const bool tearingEnabled)
        : nativeSwapchain_(std::move(nativeSwapchain)),
          width_(desc.width),
          height_(desc.height),
          bufferCount_(desc.bufferCount),
          tearingEnabled_(tearingEnabled)
    {
    }

    void Present(const bool verticalSync) override
    {
        const UINT syncInterval = verticalSync ? 1U : 0U;
        const UINT flags =
            (!verticalSync && tearingEnabled_) ? DXGI_PRESENT_ALLOW_TEARING : 0U;

        const HRESULT result = nativeSwapchain_->Present(syncInterval, flags);
        if (FAILED(result))
        {
            throw std::runtime_error("Orbit failed to present the D3D12 swapchain.");
        }
    }

    [[nodiscard]] u32 Width() const noexcept override
    {
        return width_;
    }

    [[nodiscard]] u32 Height() const noexcept override
    {
        return height_;
    }

    [[nodiscard]] u32 BufferCount() const noexcept override
    {
        return bufferCount_;
    }

private:
    ComPtr<IDXGISwapChain4> nativeSwapchain_;
    u32 width_{};
    u32 height_{};
    u32 bufferCount_{};
    bool tearingEnabled_{false};
};

class D3D12Device final : public Device
{
public:
    D3D12Device(
        ComPtr<ID3D12Device> nativeDevice,
        ComPtr<IDXGIFactory6> factory,
        std::string adapterName,
        DeviceCapabilities capabilities)
        : nativeDevice_(std::move(nativeDevice)),
          factory_(std::move(factory)),
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

    [[nodiscard]] std::unique_ptr<Queue> CreateQueue(const QueueType type) override
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = ToD3D12QueueType(type);
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        ComPtr<ID3D12CommandQueue> nativeQueue;
        if (FAILED(nativeDevice_->CreateCommandQueue(
                &queueDesc,
                IID_PPV_ARGS(&nativeQueue))))
        {
            throw std::runtime_error("Orbit failed to create a D3D12 command queue.");
        }

        return std::make_unique<D3D12Queue>(type, std::move(nativeQueue));
    }

    [[nodiscard]] std::unique_ptr<Swapchain> CreateSwapchain(
        Queue& queue,
        const SwapchainDesc& desc) override
    {
        if (desc.nativeWindow == nullptr)
        {
            throw std::runtime_error("Orbit cannot create a swapchain without a native window.");
        }

        if (desc.width == 0 || desc.height == 0)
        {
            throw std::runtime_error("Orbit cannot create a zero-sized swapchain.");
        }

        if (desc.bufferCount < 2)
        {
            throw std::runtime_error("Orbit requires at least two swapchain buffers.");
        }

        auto* d3dQueue = dynamic_cast<D3D12Queue*>(&queue);
        if (d3dQueue == nullptr || d3dQueue->Type() != QueueType::Graphics)
        {
            throw std::runtime_error("A D3D12 swapchain requires an Orbit D3D12 graphics queue.");
        }

        const bool tearingEnabled =
            desc.allowTearing && capabilities_.presentTearing;

        DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
        swapchainDesc.Width = desc.width;
        swapchainDesc.Height = desc.height;
        swapchainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapchainDesc.Stereo = FALSE;
        swapchainDesc.SampleDesc.Count = 1;
        swapchainDesc.SampleDesc.Quality = 0;
        swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapchainDesc.BufferCount = desc.bufferCount;
        swapchainDesc.Scaling = DXGI_SCALING_STRETCH;
        swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swapchainDesc.Flags = tearingEnabled
            ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
            : 0U;

        const HWND hwnd = static_cast<HWND>(desc.nativeWindow);

        ComPtr<IDXGISwapChain1> swapchain1;
        if (FAILED(factory_->CreateSwapChainForHwnd(
                d3dQueue->Native(),
                hwnd,
                &swapchainDesc,
                nullptr,
                nullptr,
                &swapchain1)))
        {
            throw std::runtime_error("Orbit failed to create the D3D12 swapchain.");
        }

        factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        ComPtr<IDXGISwapChain4> swapchain4;
        if (FAILED(swapchain1.As(&swapchain4)))
        {
            throw std::runtime_error("Orbit failed to acquire IDXGISwapChain4.");
        }

        return std::make_unique<D3D12Swapchain>(
            std::move(swapchain4),
            desc,
            tearingEnabled
        );
    }

private:
    ComPtr<ID3D12Device> nativeDevice_;
    ComPtr<IDXGIFactory6> factory_;
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

    const auto capabilities = QueryCapabilities(*nativeDevice.Get(), *factory.Get());
    auto adapterName = WideToUtf8(adapterDesc.Description);

    log::Info("D3D12 device created.");

    return std::make_unique<D3D12Device>(
        std::move(nativeDevice),
        std::move(factory),
        std::move(adapterName),
        capabilities
    );
}
} // namespace orbit::rhi::d3d12
