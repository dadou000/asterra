#include "VulkanObjects.hpp"

#include <orbit/core/Log.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>

#include <array>
#include <cstring>
#include <format>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <mutex>

namespace orbit::rhi::vulkan
{
namespace detail
{
namespace
{
constexpr std::array kRequiredDeviceExtensions{
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME
};

[[nodiscard]] bool SupportsExtension(
    const std::vector<VkExtensionProperties>& available,
    const char* name)
{
    for (const auto& extension : available)
    {
        if (std::strcmp(extension.extensionName, name) == 0)
        {
            return true;
        }
    }

    return false;
}

[[nodiscard]] std::vector<VkExtensionProperties>
EnumerateDeviceExtensions(const VkPhysicalDevice physicalDevice)
{
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &count, nullptr);

    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(
        physicalDevice, nullptr, &count, extensions.data());

    return extensions;
}

// The one platform-specific check needed at physical-device-selection
// time, before any VkSurfaceKHR exists: Main.cpp creates the RHI
// device before the swapchain (the window already exists, but its
// surface doesn't yet), so presentation support has to be queried
// against the Win32 platform directly rather than a real surface --
// this is exactly what vkGetPhysicalDeviceWin32PresentationSupportKHR
// is for.
[[nodiscard]] std::optional<u32> FindGraphicsPresentQueueFamily(
    const VkPhysicalDevice physicalDevice)
{
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &count, nullptr);

    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physicalDevice, &count, families.data());

    for (u32 index = 0; index < count; ++index)
    {
        const bool hasGraphics =
            (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;

        const VkBool32 canPresent =
            vkGetPhysicalDeviceWin32PresentationSupportKHR(
                physicalDevice, index);

        if (hasGraphics && canPresent == VK_TRUE)
        {
            return index;
        }
    }

    return std::nullopt;
}

struct Candidate
{
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    u32 graphicsFamilyIndex{0};
    VkPhysicalDeviceProperties properties{};
};

[[nodiscard]] Candidate SelectPhysicalDevice(const VkInstance instance)
{
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);

    if (count == 0)
    {
        throw std::runtime_error(
            "Orbit found no Vulkan-capable physical devices.");
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    std::optional<Candidate> bestDiscrete;
    std::optional<Candidate> bestAny;

    for (const VkPhysicalDevice physicalDevice : devices)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);

        // Dynamic rendering and synchronization2 are mandatory
        // features of any Vulkan-1.3-conformant device, and timeline
        // semaphores are mandatory as of 1.2 -- so requiring apiVersion
        // >= 1.3 here guarantees all three are available to enable
        // below without a separate support query for each.
        if (properties.apiVersion < VK_API_VERSION_1_3)
        {
            continue;
        }

        const auto graphicsFamily =
            FindGraphicsPresentQueueFamily(physicalDevice);

        if (!graphicsFamily.has_value())
        {
            continue;
        }

        const auto extensions =
            EnumerateDeviceExtensions(physicalDevice);

        bool hasAllExtensions = true;
        for (const char* required : kRequiredDeviceExtensions)
        {
            if (!SupportsExtension(extensions, required))
            {
                hasAllExtensions = false;
                break;
            }
        }

        if (!hasAllExtensions)
        {
            continue;
        }

        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(physicalDevice, &features);

        if (features.fillModeNonSolid != VK_TRUE)
        {
            continue;
        }

        // Every terrain/planet surface shader declares SV_ClipDistance0
        // for a horizon cull plane, which SPIR-V lowers to the
        // ClipDistance capability -- an optional feature that must be
        // both supported and explicitly enabled (see
        // CreateLogicalDevice below).
        if (features.shaderClipDistance != VK_TRUE)
        {
            continue;
        }

        // The field-generation compute shader (engine/terrain_gpu) needs
        // real 64-bit integer hashing to match the CPU terrain
        // generator's noise bit-for-bit -- unlike the GPU-side hydrology
        // relaxation passes (a deliberately different parallel
        // algorithm), the base elevation/climate noise field has to
        // agree between the CPU-generated map/hydrology and the
        // GPU-generated clipmap, or they'd show a different planet.
        // Broadly supported on desktop GPUs (unlike shaderFloat64, which
        // this codebase deliberately avoids requiring).
        if (features.shaderInt64 != VK_TRUE)
        {
            continue;
        }

        Candidate candidate{
            physicalDevice, *graphicsFamily, properties};

        if (properties.deviceType ==
            VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            bestDiscrete = candidate;
        }

        if (!bestAny.has_value())
        {
            bestAny = candidate;
        }
    }

    if (bestDiscrete.has_value())
    {
        return *bestDiscrete;
    }

    if (bestAny.has_value())
    {
        return *bestAny;
    }

    throw std::runtime_error(
        "Orbit could not find a suitable Vulkan device (needs API "
        "1.3, graphics+present support, VK_KHR_push_descriptor, and "
        "fillModeNonSolid).");
}

[[nodiscard]] bool QueryPresentTearingSupport(
    const VkPhysicalDevice physicalDevice)
{
    // There is no real VkSurfaceKHR yet at device-creation time (see
    // FindGraphicsPresentQueueFamily above), and present-mode support
    // is only reported per-surface. VK_PRESENT_MODE_IMMEDIATE_KHR --
    // the closest Vulkan equivalent to DXGI's allow-tearing flag -- is
    // in practice a property of the platform's presentation engine
    // rather than any specific surface, so approximate it here by
    // checking whether the extension exists at all and defer the
    // authoritative answer to swapchain creation; conservatively
    // report support so the caller's SwapchainDesc::allowTearing
    // request is honored where the driver allows it.
    (void)physicalDevice;
    return true;
}

[[nodiscard]] DeviceCapabilities QueryCapabilities(
    const VkPhysicalDevice physicalDevice)
{
    DeviceCapabilities capabilities{};

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    capabilities.shaderModelMajor =
        VK_API_VERSION_MAJOR(properties.apiVersion);
    capabilities.shaderModelMinor =
        VK_API_VERSION_MINOR(properties.apiVersion);

    capabilities.presentTearing =
        QueryPresentTearingSupport(physicalDevice);

    const auto extensions =
        EnumerateDeviceExtensions(physicalDevice);

    const bool hasAccelerationExtensions =
        SupportsExtension(
            extensions,
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
        SupportsExtension(
            extensions,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

    const bool hasRayQueryExtension =
        SupportsExtension(
            extensions,
            VK_KHR_RAY_QUERY_EXTENSION_NAME);

    const bool hasRayPipelineExtension =
        SupportsExtension(
            extensions,
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR
        accelerationFeatures{};
    accelerationFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;

    VkPhysicalDeviceRayQueryFeaturesKHR
        rayQueryFeatures{};
    rayQueryFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR
        rayPipelineFeatures{};
    rayPipelineFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;

    if (hasAccelerationExtensions)
    {
        features12.pNext =
            &accelerationFeatures;

        if (hasRayQueryExtension)
        {
            accelerationFeatures.pNext =
                &rayQueryFeatures;

            if (hasRayPipelineExtension)
            {
                rayQueryFeatures.pNext =
                    &rayPipelineFeatures;
            }
        }
        else if (hasRayPipelineExtension)
        {
            accelerationFeatures.pNext =
                &rayPipelineFeatures;
        }
    }

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext =
        &features12;

    vkGetPhysicalDeviceFeatures2(
        physicalDevice,
        &features2);

    capabilities.accelerationStructures =
        hasAccelerationExtensions &&
        features12.bufferDeviceAddress == VK_TRUE &&
        accelerationFeatures.accelerationStructure == VK_TRUE;

    capabilities.rayQuery =
        capabilities.accelerationStructures &&
        hasRayQueryExtension &&
        rayQueryFeatures.rayQuery == VK_TRUE;

    capabilities.rayTracingPipeline =
        capabilities.accelerationStructures &&
        hasRayPipelineExtension &&
        rayPipelineFeatures.rayTracingPipeline == VK_TRUE;

    capabilities.rayTracing =
        capabilities.rayTracingPipeline;

    capabilities.meshShaders =
        SupportsExtension(
            extensions,
            VK_EXT_MESH_SHADER_EXTENSION_NAME);

    capabilities.variableRateShading =
        SupportsExtension(
            extensions,
            VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);

    capabilities.calibratedTimestamps =
        SupportsExtension(
            extensions,
            VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

    return capabilities;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessengerCallback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* /*userData*/)
{
    const std::string message =
        std::format(
            "Vulkan validation: {}",
            callbackData->pMessage != nullptr
                ? callbackData->pMessage
                : "");

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        log::Error(message);
    }
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        log::Warning(message);
    }

    return VK_FALSE;
}

struct ValidationFeatureRequest
{
    bool bestPractices{false};
    bool synchronization{false};
    bool gpuAssisted{false};
};

[[nodiscard]] VkInstance CreateInstance(
    const bool enableValidation,
    const ValidationFeatureRequest& extraFeatures,
    bool& validationActuallyEnabled)
{
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Orbit";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
    appInfo.pEngineName = "Orbit";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 0, 1);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> instanceExtensions{
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };

    std::vector<const char*> layers;
    validationActuallyEnabled = false;

    if (enableValidation)
    {
        u32 layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(
            &layerCount, availableLayers.data());

        constexpr const char* kValidationLayer =
            "VK_LAYER_KHRONOS_validation";

        bool layerAvailable = false;
        for (const auto& layer : availableLayers)
        {
            if (std::strcmp(
                    layer.layerName, kValidationLayer) == 0)
            {
                layerAvailable = true;
                break;
            }
        }

        if (layerAvailable)
        {
            layers.push_back(kValidationLayer);
            instanceExtensions.push_back(
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            validationActuallyEnabled = true;
        }
        else
        {
            log::Warning(
                "Vulkan validation requested but "
                "VK_LAYER_KHRONOS_validation is unavailable.");
        }
    }

    // VK_EXT_validation_features toggles extra VK_LAYER_KHRONOS_validation
    // behavior beyond plain error/warning messages -- see DeviceDesc's
    // comment for why each is a separate, independently-toggleable
    // knob rather than always-on.
    std::vector<VkValidationFeatureEnableEXT> enabledValidationFeatures;

    if (validationActuallyEnabled)
    {
        if (extraFeatures.bestPractices)
        {
            enabledValidationFeatures.push_back(
                VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
        }

        if (extraFeatures.synchronization)
        {
            enabledValidationFeatures.push_back(
                VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
        }

        if (extraFeatures.gpuAssisted)
        {
            enabledValidationFeatures.push_back(
                VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
        }

        if (!enabledValidationFeatures.empty())
        {
            instanceExtensions.push_back(
                VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        }
    }

    VkValidationFeaturesEXT validationFeaturesInfo{};
    validationFeaturesInfo.sType =
        VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeaturesInfo.enabledValidationFeatureCount =
        static_cast<u32>(enabledValidationFeatures.size());
    validationFeaturesInfo.pEnabledValidationFeatures =
        enabledValidationFeatures.data();

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount =
        static_cast<u32>(instanceExtensions.size());
    createInfo.ppEnabledExtensionNames = instanceExtensions.data();
    createInfo.enabledLayerCount =
        static_cast<u32>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();

    if (!enabledValidationFeatures.empty())
    {
        createInfo.pNext = &validationFeaturesInfo;

        log::Info(
            std::format(
                "Vulkan extra validation features: best-practices={} "
                "synchronization={} gpu-assisted={}",
                extraFeatures.bestPractices,
                extraFeatures.synchronization,
                extraFeatures.gpuAssisted));
    }

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create the Vulkan instance.");
    }

    return instance;
}

[[nodiscard]] VkDebugUtilsMessengerEXT CreateDebugMessenger(
    const VkInstance instance)
{
    const auto createFunction =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(
                instance, "vkCreateDebugUtilsMessengerEXT"));

    if (createFunction == nullptr)
    {
        return VK_NULL_HANDLE;
    }

    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType =
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = &DebugMessengerCallback;

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    createFunction(instance, &createInfo, nullptr, &messenger);
    return messenger;
}

[[nodiscard]] VkDevice CreateLogicalDevice(
    const VkPhysicalDevice physicalDevice,
    const u32 graphicsFamilyIndex,
    const DeviceCapabilities& capabilities)
{
    constexpr f32 queuePriority = 1.0F;

    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType =
        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = graphicsFamilyIndex;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    features13.shaderDemoteToHelperInvocation = VK_TRUE;
    features13.pNext = &features12;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features.fillModeNonSolid = VK_TRUE;
    features2.features.shaderClipDistance = VK_TRUE;
    features2.features.shaderInt64 = VK_TRUE;
    features2.pNext = &features13;

    std::vector<const char*> enabledExtensions(
        kRequiredDeviceExtensions.begin(),
        kRequiredDeviceExtensions.end());

    // GPU profiling: lets GPU timestamps be placed on the CPU profiler timeline.
    if (capabilities.calibratedTimestamps)
    {
        enabledExtensions.push_back(
            VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    }

    VkPhysicalDeviceAccelerationStructureFeaturesKHR
        accelerationFeatures{};
    accelerationFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;

    VkPhysicalDeviceRayQueryFeaturesKHR
        rayQueryFeatures{};
    rayQueryFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR
        rayPipelineFeatures{};
    rayPipelineFeatures.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;

    if (capabilities.accelerationStructures)
    {
        features12.bufferDeviceAddress = VK_TRUE;

        accelerationFeatures.accelerationStructure =
            VK_TRUE;

        enabledExtensions.push_back(
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        enabledExtensions.push_back(
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);

        features12.pNext =
            &accelerationFeatures;

        if (capabilities.rayQuery)
        {
            rayQueryFeatures.rayQuery =
                VK_TRUE;
            accelerationFeatures.pNext =
                &rayQueryFeatures;

            enabledExtensions.push_back(
                VK_KHR_RAY_QUERY_EXTENSION_NAME);
        }

        if (capabilities.rayTracingPipeline)
        {
            rayPipelineFeatures.rayTracingPipeline =
                VK_TRUE;

            if (capabilities.rayQuery)
            {
                rayQueryFeatures.pNext =
                    &rayPipelineFeatures;
            }
            else
            {
                accelerationFeatures.pNext =
                    &rayPipelineFeatures;
            }

            enabledExtensions.push_back(
                VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        }
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &features2;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueCreateInfo;
    createInfo.enabledExtensionCount =
        static_cast<u32>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames =
        enabledExtensions.data();

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(
            physicalDevice, &createInfo, nullptr, &device) !=
        VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create the Vulkan logical device.");
    }

    return device;
}

[[nodiscard]] VmaAllocator CreateAllocator(
    const VkInstance instance,
    const VkPhysicalDevice physicalDevice,
    const VkDevice device,
    const DeviceCapabilities& capabilities)
{
    VmaAllocatorCreateInfo createInfo{};
    createInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    createInfo.instance = instance;
    createInfo.physicalDevice = physicalDevice;
    createInfo.device = device;

    if (capabilities.accelerationStructures)
    {
        createInfo.flags |=
            VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

    VmaAllocator allocator = nullptr;
    if (vmaCreateAllocator(&createInfo, &allocator) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create the Vulkan memory allocator.");
    }

    return allocator;
}
} // namespace

// Cache content and path are tied to the Vulkan implementation and device.
// Vulkan performs the definitive header check as well; we reject obviously
// incompatible or corrupted files before invoking the driver.
void VulkanDevice::InitializePipelineCache() noexcept
{
    try
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice_, &props);
        std::array<wchar_t, 32768> home{};
        const DWORD chars = GetEnvironmentVariableW(
            L"LOCALAPPDATA", home.data(), static_cast<DWORD>(home.size()));
        const auto base = chars > 0U && chars < home.size()
            ? std::filesystem::path(home.data())
            : std::filesystem::temp_directory_path();
        const auto directory = base / "Orbit" / "PipelineCache";
        std::error_code fsError;
        std::filesystem::create_directories(directory, fsError);
        if (!fsError)
        {
            pipelineCachePath_ = directory / std::format(
                "v1_{:08x}_{:08x}_{:08x}.bin",
                props.vendorID, props.deviceID, props.driverVersion);
        }

        constexpr std::size_t kMaxCacheBytes = 64U * 1024U * 1024U;
        std::vector<u8> saved;
        if (!pipelineCachePath_.empty())
        {
            std::ifstream file(pipelineCachePath_, std::ios::binary | std::ios::ate);
            if (file)
            {
                const auto end = file.tellg();
                if (end > 0 && end <= static_cast<std::streamoff>(kMaxCacheBytes))
                {
                    saved.resize(static_cast<std::size_t>(end));
                    file.seekg(0);
                    file.read(reinterpret_cast<char*>(saved.data()),
                        static_cast<std::streamsize>(saved.size()));
                    if (!file || saved.size() < sizeof(VkPipelineCacheHeaderVersionOne))
                        saved.clear();
                    else
                    {
                        VkPipelineCacheHeaderVersionOne header{};
                        std::memcpy(&header, saved.data(), sizeof(header));
                        if (header.headerSize < sizeof(header) ||
                            header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
                            header.vendorID != props.vendorID ||
                            header.deviceID != props.deviceID ||
                            std::memcmp(header.pipelineCacheUUID,
                                props.pipelineCacheUUID, VK_UUID_SIZE) != 0)
                            saved.clear();
                    }
                }
            }
        }

        VkPipelineCacheCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        createInfo.initialDataSize = saved.size();
        createInfo.pInitialData = saved.empty() ? nullptr : saved.data();
        if (vkCreatePipelineCache(nativeDevice_, &createInfo, nullptr,
                &pipelineCache_) != VK_SUCCESS)
        {
            // Device remains usable if a cached blob is rejected. Recreate
            // an empty cache and avoid replaying the bad data next launch.
            createInfo.initialDataSize = 0U;
            createInfo.pInitialData = nullptr;
            pipelineCache_ = VK_NULL_HANDLE;
            static_cast<void>(vkCreatePipelineCache(
                nativeDevice_, &createInfo, nullptr, &pipelineCache_));
        }
        // Give the slow compilation worker its own cache instance, seeded
        // from the same disk blob. Render-thread creation never locks behind
        // vkCreate*Pipelines on the worker; caches merge at shutdown.
        if (vkCreatePipelineCache(nativeDevice_, &createInfo, nullptr,
                &pipelineWorkerCache_) != VK_SUCCESS)
        {
            createInfo.initialDataSize = 0U;
            createInfo.pInitialData = nullptr;
            static_cast<void>(vkCreatePipelineCache(
                nativeDevice_, &createInfo, nullptr, &pipelineWorkerCache_));
        }
        log::Info(pipelineCache_ != VK_NULL_HANDLE
            ? "Vulkan persistent pipeline cache initialized."
            : "Vulkan pipeline cache unavailable; using uncached creation.");
    }
    catch (...)
    {
        // A corrupted filesystem or missing cache directory may not prevent
        // rendering. VK_NULL_HANDLE remains a valid pipeline cache argument.
        pipelineCachePath_.clear();
    }
}

void VulkanDevice::SavePipelineCache() noexcept
{
    if (pipelineCache_ == VK_NULL_HANDLE &&
        pipelineWorkerCache_ == VK_NULL_HANDLE) return;
    {
        std::scoped_lock lock(pipelineCacheMutex_);
        if (pipelineCache_ != VK_NULL_HANDLE &&
            pipelineWorkerCache_ != VK_NULL_HANDLE)
        {
            static_cast<void>(vkMergePipelineCaches(
                nativeDevice_, pipelineCache_, 1U, &pipelineWorkerCache_));
        }
        if (pipelineCache_ == VK_NULL_HANDLE)
        {
            pipelineCache_ = pipelineWorkerCache_;
            pipelineWorkerCache_ = VK_NULL_HANDLE;
        }
        try
        {
            if (!pipelineCachePath_.empty())
            {
                std::size_t size = 0U;
                if (vkGetPipelineCacheData(nativeDevice_, pipelineCache_,
                        &size, nullptr) == VK_SUCCESS &&
                    size >= sizeof(VkPipelineCacheHeaderVersionOne) &&
                    size <= 64U * 1024U * 1024U)
                {
                    std::vector<u8> data(size);
                    if (vkGetPipelineCacheData(nativeDevice_, pipelineCache_,
                            &size, data.data()) == VK_SUCCESS)
                    {
                        const auto temp = pipelineCachePath_.wstring() + L".tmp";
                        std::ofstream output(std::filesystem::path(temp),
                            std::ios::binary | std::ios::trunc);
                        if (output)
                        {
                            output.write(reinterpret_cast<const char*>(data.data()),
                                static_cast<std::streamsize>(size));
                            output.flush();
                            if (output.good())
                            {
                                output.close();
                                static_cast<void>(MoveFileExW(temp.c_str(),
                                    pipelineCachePath_.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
                            }
                        }
                    }
                }
            }
        }
        catch (...) {}
        vkDestroyPipelineCache(nativeDevice_, pipelineCache_, nullptr);
        pipelineCache_ = VK_NULL_HANDLE;
        if (pipelineWorkerCache_ != VK_NULL_HANDLE)
        {
            vkDestroyPipelineCache(nativeDevice_, pipelineWorkerCache_, nullptr);
            pipelineWorkerCache_ = VK_NULL_HANDLE;
        }
    }
}

bool VulkanDevice::TryQueuePipelineCompile(std::function<void()> work)
{
    std::scoped_lock lock(pipelineQueueMutex_);
    if (pipelineQueueStopping_ || !pipelineCompileThread_.joinable() ||
        pipelineQueue_.size() + pipelineCompilesInProgress_ >=
            kMaxOutstandingPipelineCompiles)
    {
        ++pipelineCompilesRejected_;
        return false;
    }
    pipelineQueue_.push_back(std::move(work));
    pipelineQueueCv_.notify_one();
    return true;
}

void VulkanDevice::PipelineCompileLoop() noexcept
{
    // Cold Vulkan driver compilations are allowed to spend substantial CPU
    // time, but they must not preempt the event/render thread.
    static_cast<void>(SetThreadPriority(
        GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL));
    static_cast<void>(SetThreadDescription(
        GetCurrentThread(), L"Orbit.VulkanPipeline"));
    while (true)
    {
        std::function<void()> work;
        {
            std::unique_lock lock(pipelineQueueMutex_);
            pipelineQueueCv_.wait(lock, [this] {
                return pipelineQueueStopping_ || !pipelineQueue_.empty();
            });
            if (pipelineQueue_.empty() && pipelineQueueStopping_)
                return;
            work = std::move(pipelineQueue_.front());
            pipelineQueue_.pop_front();
            ++pipelineCompilesInProgress_;
        }
        try { work(); }
        catch (...) { /* Packaged tasks own their exceptions. */ }
        {
            std::scoped_lock lock(pipelineQueueMutex_);
            --pipelineCompilesInProgress_;
            ++pipelineCompilesCompleted_;
        }
    }
}

PipelineCompilationStats VulkanDevice::PipelineCompilation() noexcept
{
    std::scoped_lock lock(pipelineQueueMutex_);
    return {
        .queued = static_cast<u64>(pipelineQueue_.size()),
        .compiling = static_cast<u64>(pipelineCompilesInProgress_),
        .completed = pipelineCompilesCompleted_,
        .rejected = pipelineCompilesRejected_
    };
}

VulkanDevice::VulkanDevice(
    const VkInstance instance,
    const VkPhysicalDevice physicalDevice,
    const VkDevice nativeDevice,
    const VmaAllocator allocator,
    const u32 graphicsFamilyIndex,
    std::string adapterName,
    const DeviceCapabilities capabilities,
    const DeviceFunctions functions,
    const bool validationEnabled,
    const VkDebugUtilsMessengerEXT debugMessenger,
    std::unique_ptr<RenderDocCapture> renderDoc)
    : instance_(instance),
      physicalDevice_(physicalDevice),
      nativeDevice_(nativeDevice),
      allocator_(allocator),
      graphicsFamilyIndex_(graphicsFamilyIndex),
      adapterName_(std::move(adapterName)),
      capabilities_(capabilities),
      functions_(functions),
      validationEnabled_(validationEnabled),
      debugMessenger_(debugMessenger),
      renderDoc_(std::move(renderDoc))
{
    progress_ = std::make_unique<GpuProgress>(nativeDevice_);

    VkSamplerCreateInfo samplerCreateInfo{};
    samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerCreateInfo.magFilter = VK_FILTER_LINEAR;
    samplerCreateInfo.minFilter = VK_FILTER_LINEAR;
    samplerCreateInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerCreateInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCreateInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerCreateInfo.maxLod = VK_LOD_CLAMP_NONE;

    if (vkCreateSampler(
            nativeDevice_, &samplerCreateInfo, nullptr, &defaultSampler_) !=
        VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create the default Vulkan sampler.");
    }
    InitializePipelineCache();
    try
    {
        pipelineCompileThread_ = std::thread([this] {
            PipelineCompileLoop();
        });
    }
    catch (const std::system_error& error)
    {
        log::Warning(std::string("Cold Vulkan pipeline worker unavailable: ") +
            error.what());
    }
}

RenderDocCapture* VulkanDevice::GetRenderDocCapture() const noexcept
{
    return renderDoc_.get();
}

VkInstance VulkanDevice::NativeInstance() const noexcept
{
    return instance_;
}

VkSampler VulkanDevice::DefaultSampler() const noexcept
{
    return defaultSampler_;
}

VulkanDevice::~VulkanDevice()
{
    // Drain pipeline driver calls before invalidating VkDevice or persisting
    // the cache. This join occurs only at device teardown, never per frame.
    {
        std::scoped_lock lock(pipelineQueueMutex_);
        pipelineQueueStopping_ = true;
    }
    pipelineQueueCv_.notify_all();
    if (pipelineCompileThread_.joinable())
        pipelineCompileThread_.join();

    if (nativeDevice_ != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(nativeDevice_);
    }
    SavePipelineCache();

    // Deferred destroys call into the allocator and device, so they run
    // (and the progress timeline goes) before either is torn down.
    if (progress_ != nullptr)
    {
        progress_->Flush();
        progress_.reset();
    }

    if (defaultSampler_ != VK_NULL_HANDLE)
    {
        vkDestroySampler(nativeDevice_, defaultSampler_, nullptr);
    }

    if (allocator_ != nullptr)
    {
        vmaDestroyAllocator(allocator_);
    }

    if (nativeDevice_ != VK_NULL_HANDLE)
    {
        vkDestroyDevice(nativeDevice_, nullptr);
    }

    if (debugMessenger_ != VK_NULL_HANDLE)
    {
        const auto destroyFunction =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(
                    instance_, "vkDestroyDebugUtilsMessengerEXT"));

        if (destroyFunction != nullptr)
        {
            destroyFunction(instance_, debugMessenger_, nullptr);
        }
    }

    if (instance_ != VK_NULL_HANDLE)
    {
        vkDestroyInstance(instance_, nullptr);
    }
}

Backend VulkanDevice::GetBackend() const noexcept
{
    return Backend::Vulkan;
}

std::string_view VulkanDevice::AdapterName() const noexcept
{
    return adapterName_;
}

const DeviceCapabilities& VulkanDevice::Capabilities() const noexcept
{
    return capabilities_;
}

std::unique_ptr<Queue> VulkanDevice::CreateQueue(const QueueType type)
{
    // This application only ever creates a single Graphics queue (see
    // Main.cpp); Compute/Copy are accepted for interface completeness
    // but are served from the same graphics-capable family rather
    // than a dedicated async queue, since nothing exercises those
    // paths today.
    VkQueue nativeQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(nativeDevice_, graphicsFamilyIndex_, 0, &nativeQueue);

    return std::make_unique<VulkanQueue>(
        nativeDevice_,
        nativeQueue,
        graphicsFamilyIndex_,
        type,
        functions_,
        progress_.get());
}

std::unique_ptr<Fence> VulkanDevice::CreateFence(const u64 initialValue)
{
    VkSemaphoreTypeCreateInfo typeCreateInfo{};
    typeCreateInfo.sType =
        VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeCreateInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeCreateInfo.initialValue = initialValue;

    VkSemaphoreCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    createInfo.pNext = &typeCreateInfo;

    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (vkCreateSemaphore(
            nativeDevice_, &createInfo, nullptr, &semaphore) !=
        VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create a Vulkan timeline semaphore.");
    }

    return std::make_unique<VulkanFence>(nativeDevice_, semaphore);
}

std::unique_ptr<CommandAllocator>
VulkanDevice::CreateCommandAllocator(const QueueType type)
{
    VkCommandPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    createInfo.flags =
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = graphicsFamilyIndex_;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(
            nativeDevice_, &createInfo, nullptr, &pool) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create a Vulkan command pool.");
    }

    return std::make_unique<VulkanCommandAllocator>(
        nativeDevice_, type, pool, progress_.get());
}

std::unique_ptr<CommandList> VulkanDevice::CreateCommandList(
    CommandAllocator& allocator)
{
    auto* vulkanAllocator =
        dynamic_cast<VulkanCommandAllocator*>(&allocator);

    if (vulkanAllocator == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a command allocator from another "
            "backend.");
    }

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = vulkanAllocator->Native();
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(
            nativeDevice_, &allocateInfo, &commandBuffer) !=
        VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to allocate a Vulkan command buffer.");
    }

    auto commandList = std::make_unique<VulkanCommandList>(
        nativeDevice_,
        allocator.Type(),
        vulkanAllocator->Native(),
        commandBuffer,
        functions_,
        defaultSampler_,
        progress_.get());
    commandList->NoteAllocator(*vulkanAllocator);
    return commandList;
}
} // namespace detail

std::unique_ptr<Device> CreateDevice(const DeviceDesc& desc)
{
    // Must happen before vkCreateInstance: RenderDoc's Vulkan capture
    // layer only activates for instances created after renderdoc.dll is
    // loaded into the process (or was already loaded because the app was
    // launched under the RenderDoc UI).
    std::unique_ptr<RenderDocCapture> renderDoc;

    if (desc.enableRenderDoc)
    {
        renderDoc = RenderDocCapture::TryLoad();

        log::Info(
            renderDoc != nullptr
                ? "RenderDoc capture support enabled."
                : "RenderDoc capture requested but renderdoc.dll was "
                  "not found (checked ORBIT_RENDERDOC_DLL, the default "
                  "install path, and an already-loaded module).");
    }

    bool validationEnabled = false;
    const VkInstance instance =
        detail::CreateInstance(
            desc.enableValidation,
            detail::ValidationFeatureRequest{
                desc.enableBestPracticesValidation,
                desc.enableSynchronizationValidation,
                desc.enableGpuAssistedValidation},
            validationEnabled);

    if (validationEnabled)
    {
        log::Info("Vulkan validation layer enabled.");
    }

    const VkDebugUtilsMessengerEXT debugMessenger =
        validationEnabled
            ? detail::CreateDebugMessenger(instance)
            : VK_NULL_HANDLE;

    const auto candidate = detail::SelectPhysicalDevice(instance);

    const auto capabilities =
        detail::QueryCapabilities(
            candidate.physicalDevice);

    const VkDevice nativeDevice =
        detail::CreateLogicalDevice(
            candidate.physicalDevice,
            candidate.graphicsFamilyIndex,
            capabilities);

    const VmaAllocator allocator =
        detail::CreateAllocator(
            instance,
            candidate.physicalDevice,
            nativeDevice,
            capabilities);

    detail::DeviceFunctions functions{};
    functions.vkCmdPushDescriptorSetKHR =
        reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(
                nativeDevice, "vkCmdPushDescriptorSetKHR"));

    if (capabilities.calibratedTimestamps)
    {
        functions.vkGetCalibratedTimestampsKHR =
            reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(
                vkGetDeviceProcAddr(
                    nativeDevice, "vkGetCalibratedTimestampsKHR"));
    }

    if (capabilities.accelerationStructures)
    {
        functions.vkCreateAccelerationStructureKHR =
            reinterpret_cast<
                PFN_vkCreateAccelerationStructureKHR>(
                    vkGetDeviceProcAddr(
                        nativeDevice,
                        "vkCreateAccelerationStructureKHR"));
        functions.vkDestroyAccelerationStructureKHR =
            reinterpret_cast<
                PFN_vkDestroyAccelerationStructureKHR>(
                    vkGetDeviceProcAddr(
                        nativeDevice,
                        "vkDestroyAccelerationStructureKHR"));
        functions.vkGetAccelerationStructureBuildSizesKHR =
            reinterpret_cast<
                PFN_vkGetAccelerationStructureBuildSizesKHR>(
                    vkGetDeviceProcAddr(
                        nativeDevice,
                        "vkGetAccelerationStructureBuildSizesKHR"));
        functions.vkCmdBuildAccelerationStructuresKHR =
            reinterpret_cast<
                PFN_vkCmdBuildAccelerationStructuresKHR>(
                    vkGetDeviceProcAddr(
                        nativeDevice,
                        "vkCmdBuildAccelerationStructuresKHR"));
        functions.vkGetAccelerationStructureDeviceAddressKHR =
            reinterpret_cast<
                PFN_vkGetAccelerationStructureDeviceAddressKHR>(
                    vkGetDeviceProcAddr(
                        nativeDevice,
                        "vkGetAccelerationStructureDeviceAddressKHR"));

        if (functions.vkCreateAccelerationStructureKHR == nullptr ||
            functions.vkDestroyAccelerationStructureKHR == nullptr ||
            functions.vkGetAccelerationStructureBuildSizesKHR == nullptr ||
            functions.vkCmdBuildAccelerationStructuresKHR == nullptr ||
            functions.vkGetAccelerationStructureDeviceAddressKHR == nullptr)
        {
            throw std::runtime_error(
                "Orbit detected Vulkan acceleration structures but failed "
                "to resolve one or more required device entry points.");
        }
    }

    if (functions.vkCmdPushDescriptorSetKHR == nullptr)
    {
        throw std::runtime_error(
            "Orbit failed to resolve vkCmdPushDescriptorSetKHR.");
    }

    log::Info(
        std::format(
            "Vulkan device created. AS={} rayQuery={} rtPipeline={}",
            capabilities.accelerationStructures,
            capabilities.rayQuery,
            capabilities.rayTracingPipeline));

    return std::make_unique<detail::VulkanDevice>(
        instance,
        candidate.physicalDevice,
        nativeDevice,
        allocator,
        candidate.graphicsFamilyIndex,
        std::string(candidate.properties.deviceName),
        capabilities,
        functions,
        validationEnabled,
        debugMessenger,
        std::move(renderDoc));
}

void SetRenderDocActiveWindow(Device& device, void* const nativeWindow)
{
    auto* vulkanDevice = dynamic_cast<detail::VulkanDevice*>(&device);

    if (vulkanDevice == nullptr ||
        vulkanDevice->GetRenderDocCapture() == nullptr)
    {
        return;
    }

    vulkanDevice->GetRenderDocCapture()->SetActiveWindow(
        vulkanDevice->NativeInstance(), nativeWindow);
}

void TriggerRenderDocCapture(Device& device)
{
    auto* vulkanDevice = dynamic_cast<detail::VulkanDevice*>(&device);

    if (vulkanDevice == nullptr ||
        vulkanDevice->GetRenderDocCapture() == nullptr)
    {
        log::Warning(
            "Orbit ignored a RenderDoc capture request: RenderDoc is "
            "not available for this device.");
        return;
    }

    vulkanDevice->GetRenderDocCapture()->TriggerCapture();
}

bool IsRenderDocAvailable(const Device& device) noexcept
{
    const auto* vulkanDevice = dynamic_cast<const detail::VulkanDevice*>(&device);
    return vulkanDevice != nullptr &&
        vulkanDevice->GetRenderDocCapture() != nullptr;
}

bool IsRenderDocCapturing(const Device& device) noexcept
{
    const auto* vulkanDevice = dynamic_cast<const detail::VulkanDevice*>(&device);

    return vulkanDevice != nullptr &&
        vulkanDevice->GetRenderDocCapture() != nullptr &&
        vulkanDevice->GetRenderDocCapture()->IsCapturing();
}

std::string LastRenderDocCapturePath(const Device& device)
{
    const auto* vulkanDevice = dynamic_cast<const detail::VulkanDevice*>(&device);

    if (vulkanDevice == nullptr ||
        vulkanDevice->GetRenderDocCapture() == nullptr)
    {
        return {};
    }

    return vulkanDevice->GetRenderDocCapture()->LastCapturePath();
}
} // namespace orbit::rhi::vulkan
